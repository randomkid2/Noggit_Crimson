// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/ui/ShadowToolSettings.hpp>

#include <noggit/Action.hpp>
#include <noggit/ActionManager.hpp>
#include <noggit/Camera.hpp>
#include <noggit/MapTile.h>
#include <noggit/MapView.h>
#include <noggit/TileIndex.hpp>
#include <noggit/ui/DesignTokens.hpp>
#include <noggit/ui/FontNoggit.hpp>
#include <noggit/ui/tools/ToolPanel/ToolWidgetStyle.hpp>
#include <noggit/ui/tools/UiCommon/ExtendedSlider.hpp>
#include <noggit/World.h>
#include <noggit/World.inl>
#include <opengl/context.hpp>

#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QVBoxLayout>

#include <chrono>

namespace
{
  // The brush cannot usefully exceed a tile, and a shadow disc that large is not a brush stroke,
  // it is a bake with the wrong tool. Kept well under the 1000 the terrain brushes allow.
  constexpr double MAX_BRUSH_RADIUS = 200.0;

  // Green's control range and default for the sun's elevation. The lower bound is 1 rather than 0
  // because a horizontal light gives a degenerate orthographic fit -- see
  // ShadowBakeSettings::sanitized.
  constexpr double MIN_SUN_PITCH = 1.0;
  constexpr double MAX_SUN_PITCH = 90.0;
  constexpr double DEFAULT_SUN_PITCH = 47.0;

  // 0..256 with a default of 128, again Green's. 256 is one past the largest coverage a texel can
  // report and is therefore the "shadow nothing" end, so it has to stay reachable.
  constexpr int DEFAULT_THRESHOLD = 128;

  QString describeReport(Noggit::Rendering::ShadowBakeReport const& report, double seconds)
  {
    if (!report.ok())
    {
      return QString("Bake did not run: %1.").arg(QString::fromUtf8(report.failure));
    }

    // Deliberately reports the numbers rather than the word "done". A shadow bake writes a layer
    // that is invisible until the terrain is next lit, so the failure worth guarding against is
    // one that completes and writes nothing -- and the only defence is saying what it covered.
    QString text
      = QString("Baked %1 of %2 chunks in %3 s. %4 texels shadowed (%5% of the tile). "
                "%6 neighbouring tiles were loaded and able to cast.")
          .arg(report.chunks_changed)
          .arg(report.chunks_visited)
          .arg(seconds, 0, 'f', 1)
          .arg(report.texels_shadowed)
          .arg(100.0 * report.texels_shadowed / (256.0 * 4096.0), 0, 'f', 1)
          .arg(report.neighbour_tiles_loaded);

    if (report.texels_shadowed == 0 && report.chunks_changed > 0)
    {
      // The destructive case, called out separately because the two "nothing is in shadow"
      // outcomes are not equally serious. Every chunk this bake changed it changed by CLEARING,
      // so whatever was in those maps -- most plausibly shadows the user painted by hand -- has
      // just been replaced by an empty one. It is a single undo step (World::bakeTerrainShadows
      // opens one action for the whole tile), and saying so here is the difference between a
      // recoverable mistake and a silent one.
      text += QString(" NOTHING IS IN SHADOW, so this bake CLEARED the shadow maps of %1 chunks"
                      " that previously had some -- including any you painted by hand. Ctrl+Z"
                      " restores them. Lower the sun pitch, or check that the casters you expect"
                      " are on a loaded tile.").arg(report.chunks_changed);
    }
    else if (report.texels_shadowed == 0)
    {
      text += " Nothing is in shadow -- lower the sun pitch, or check that the casters you expect"
              " are on a loaded tile.";
    }

    if (!report.depthOutresolvesMcsh())
    {
      text += QString(" Depth pass resolved %1 yd of ground per texel against MCSH's %2 yd, so"
                      " shadow edges will be stepped -- raise the depth resolution or the sun.")
                .arg(report.depth_texel_yards, 0, 'f', 2)
                .arg(report.mcsh_texel_yards, 0, 'f', 2);
    }

    if (report.neighbour_tiles_loaded == 0)
    {
      text += " No neighbouring tile was loaded, so nothing outside this tile cast into it.";
    }

    return text;
  }
}

namespace Noggit
{
  namespace Ui
  {
    ShadowToolSettings::ShadowToolSettings(MapView* map_view)
      : QWidget(map_view)
      , _map_view(map_view)
    {
      auto layout(Tools::ToolPanelStyle::toolColumn(this));

      layout->addWidget
        ( Tools::ToolPanelStyle::keybindRow
            (this, FontNoggit::shift, FontNoggit::lmb, tr("Paint Shadow"))
        );
      layout->addWidget
        ( Tools::ToolPanelStyle::keybindRow
            (this, FontNoggit::ctrl, FontNoggit::lmb, tr("Erase Shadow"))
        );

      auto brush_group(Tools::ToolPanelStyle::toolSection(layout, tr("Brush")));
      auto brush_layout(Tools::ToolPanelStyle::sectionColumn(brush_group));

      _radius_slider = new Noggit::Ui::Tools::UiCommon::ExtendedSlider(this);
      _radius_slider->setPrefix("Radius:");
      _radius_slider->setRange(0.0, MAX_BRUSH_RADIUS);
      _radius_slider->setDecimals(2);
      _radius_slider->setValue(15.0);
      _radius_slider->setToolTip("Shadow is one bit per texel, so the brush has a hard edge and"
                                 " no falloff. Each texel is 0.52 yd across.");
      brush_layout->addWidget(_radius_slider);

      auto sun_group(Tools::ToolPanelStyle::toolSection(layout, tr("Sun")));
      auto sun_layout(Tools::ToolPanelStyle::sectionColumn(sun_group));

      _sun_pitch_slider = new Noggit::Ui::Tools::UiCommon::ExtendedSlider(this);
      _sun_pitch_slider->setPrefix("Pitch:");
      _sun_pitch_slider->setRange(MIN_SUN_PITCH, MAX_SUN_PITCH);
      _sun_pitch_slider->setDecimals(1);
      _sun_pitch_slider->setValue(DEFAULT_SUN_PITCH);
      _sun_pitch_slider->setToolTip("Sun elevation above the horizon. 90 is directly overhead and"
                                    " casts almost nothing; low angles throw long shadows and"
                                    " need a higher depth resolution to stay crisp.");
      sun_layout->addWidget(_sun_pitch_slider);

      _sun_yaw_slider = new Noggit::Ui::Tools::UiCommon::ExtendedSlider(this);
      _sun_yaw_slider->setPrefix("Yaw:");
      _sun_yaw_slider->setRange(0.0, 360.0);
      _sun_yaw_slider->setDecimals(1);
      _sun_yaw_slider->setValue(0.0);
      // The warning is the point of this tooltip. Nothing enforces a consistent yaw across tiles,
      // and a map baked with two different ones has shadows that change direction at a tile
      // border -- which is very hard to diagnose after the fact and impossible to fix except by
      // re-baking. Green sidesteps it by fixing the yaw outright; this exposes it and says so.
      _sun_yaw_slider->setToolTip("Sun azimuth. LEAVE THIS ALONE unless you mean to move the sun"
                                  " for the whole map: two tiles baked at different yaws have"
                                  " shadows pointing different ways across their shared border.");
      sun_layout->addWidget(_sun_yaw_slider);

      auto bake_group(Tools::ToolPanelStyle::toolSection(layout, tr("Bake")));
      auto bake_layout(Tools::ToolPanelStyle::sectionColumn(bake_group));

      auto parameter_layout(new QFormLayout);
      parameter_layout->setContentsMargins(0, 0, 0, 0);
      parameter_layout->setHorizontalSpacing(Design::S1);
      parameter_layout->setVerticalSpacing(Design::S3);

      _threshold_spin = new QSpinBox(this);
      _threshold_spin->setRange(0, 256);
      _threshold_spin->setValue(DEFAULT_THRESHOLD);
      _threshold_spin->setToolTip("How much of a texel must be in shadow before the bit is set,"
                                  " on a 0-256 scale. 128 is half. 256 shadows nothing.");
      parameter_layout->addRow("Threshold:", _threshold_spin);

      _resolution_combo = new QComboBox(this);
      // The value is carried in the item DATA, never inferred from the row index -- the same rule
      // ErosionToolSettings states for its edge-mode combo. Reading a number back out by
      // currentIndex() survives exactly until someone reorders the list.
      _resolution_combo->addItem("1024 (fast)", 1024);
      _resolution_combo->addItem("2048 (default)", 2048);
      _resolution_combo->addItem("4096 (low sun)", 4096);
      _resolution_combo->addItem("8192 (maximum)", 8192);
      _resolution_combo->setCurrentIndex(1);
      _resolution_combo->setToolTip("Edge of the square depth render. It has to out-resolve the"
                                    " 0.52 yd MCSH texel across the tile AS SEEN FROM THE SUN, so"
                                    " a low sun needs more of it. The status line reports what was"
                                    " achieved.");
      parameter_layout->addRow("Depth res:", _resolution_combo);

      _supersample_spin = new QSpinBox(this);
      _supersample_spin->setRange(1, 4);
      _supersample_spin->setValue(2);
      _supersample_spin->setToolTip("Samples per axis inside each texel, so 2 means four samples."
                                    " At 1 the coverage can only be 0 or full and the threshold"
                                    " stops doing anything.");
      parameter_layout->addRow("Supersample:", _supersample_spin);

      _caster_margin_spin = new QSpinBox(this);
      _caster_margin_spin->setRange(0, 2000);
      _caster_margin_spin->setValue(200);
      _caster_margin_spin->setSuffix(" yd");
      _caster_margin_spin->setToolTip("How far outside the tile geometry may still cast into it."
                                      " It cannot conjure a caster that is not loaded.");
      parameter_layout->addRow("Caster margin:", _caster_margin_spin);

      bake_layout->addLayout(parameter_layout);

      _bias_slider = new Noggit::Ui::Tools::UiCommon::ExtendedSlider(this);
      _bias_slider->setPrefix("Bias:");
      _bias_slider->setRange(0.0, 10.0);
      _bias_slider->setDecimals(2);
      _bias_slider->setSingleStep(0.1);
      _bias_slider->setValue(1.0);
      _bias_slider->setToolTip("How far toward the sun a point is nudged before its depth is"
                               " tested, in yards. Too little and lit slopes shadow themselves in"
                               " a moire pattern; too much and objects lose the shadow at their"
                               " base.");
      bake_layout->addWidget(_bias_slider);

      _include_models_check = new QCheckBox("Doodads (M2) cast", this);
      _include_models_check->setChecked(true);
      bake_layout->addWidget(_include_models_check);

      _include_wmos_check = new QCheckBox("Buildings (WMO) cast", this);
      _include_wmos_check->setChecked(true);
      bake_layout->addWidget(_include_wmos_check);

      _bake_button = new QPushButton("Bake shadows on this tile", this);
      _bake_button->setAutoDefault(false);
      _bake_button->setToolTip("Renders the loaded scene from the sun and writes MCSH across the"
                               " tile under the camera. One undo step. Seconds, not frames.");
      // A plain clicked connection and nothing deferred. This handler is delivered by the Qt event
      // loop, which is already the guarantee that matters: it cannot be running inside paintGL, so
      // the readback below stalls nobody's frame mid-draw and the status text it sets afterwards
      // is not a dialog raised from a paint.
      connect(_bake_button, &QPushButton::clicked, this, [this] { bakeCurrentTile(); });
      bake_layout->addWidget(_bake_button);

      _clear_button = new QPushButton("Clear shadows on this tile", this);
      _clear_button->setAutoDefault(false);
      _clear_button->setToolTip("Erases MCSH across the tile under the camera, in one undo step."
                                " The way back from a bake you do not want.");
      connect(_clear_button, &QPushButton::clicked, this, [this] { clearCurrentTile(); });
      bake_layout->addWidget(_clear_button);

      _status_label = new QLabel("No bake yet.", this);
      _status_label->setWordWrap(true);
      _status_label->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
      layout->addWidget(_status_label);
    }

    void ShadowToolSettings::changeRadius(float change)
    {
      _radius_slider->setValue(_radius_slider->value() + change);
    }

    float ShadowToolSettings::brushRadius() const
    {
      // ExtendedSlider::value() is not const in this tree, so the const_cast is here rather than
      // the accessor being non-const: every Tool::brushRadius override is const and changing that
      // signature would touch tools this round does not own.
      return static_cast<float>
        (const_cast<Noggit::Ui::Tools::UiCommon::ExtendedSlider*>(_radius_slider)->value());
    }

    Noggit::Rendering::ShadowBakeSettings ShadowToolSettings::bakeSettings() const
    {
      Noggit::Rendering::ShadowBakeSettings settings;

      auto* const pitch = const_cast<Noggit::Ui::Tools::UiCommon::ExtendedSlider*>(_sun_pitch_slider);
      auto* const yaw = const_cast<Noggit::Ui::Tools::UiCommon::ExtendedSlider*>(_sun_yaw_slider);
      auto* const bias = const_cast<Noggit::Ui::Tools::UiCommon::ExtendedSlider*>(_bias_slider);

      settings.sun_pitch_degrees = static_cast<float>(pitch->value());
      settings.sun_yaw_degrees = static_cast<float>(yaw->value());
      settings.bias_yards = static_cast<float>(bias->value());
      settings.threshold = _threshold_spin->value();
      settings.supersample = _supersample_spin->value();
      settings.caster_margin_yards = static_cast<float>(_caster_margin_spin->value());
      settings.depth_resolution = _resolution_combo->currentData().toInt();
      settings.include_models = _include_models_check->isChecked();
      settings.include_wmos = _include_wmos_check->isChecked();

      return settings.sanitized();
    }

    void ShadowToolSettings::setStatus(QString const& text)
    {
      _status_label->setText(text);
    }

    void ShadowToolSettings::bakeCurrentTile()
    {
      if (NOGGIT_CUR_ACTION)
      {
        setStatus("Another edit is still in progress. Release the mouse button, or finish the"
                  " current stroke, and bake again.");
        return;
      }

      World* world = _map_view->getWorld();
      TileIndex const tile_index (_map_view->getCamera()->position);

      if (!world->mapIndex.hasTile(tile_index) || !world->mapIndex.tileLoaded(tile_index))
      {
        setStatus("No loaded tile under the camera. Fly to the tile you want to bake.");
        return;
      }

      MapTile* tile = world->mapIndex.getTile(tile_index);

      if (!tile || !tile->finishedLoading())
      {
        setStatus("The tile under the camera is still loading.");
        return;
      }

      Noggit::Rendering::ShadowBakeSettings const settings (bakeSettings());

      auto const started = std::chrono::steady_clock::now();

      Noggit::Rendering::SunDepthMap depth_map;
      int neighbours_loaded = 0;

      {
        // The GL context has to be made current explicitly: this is a button handler, not a
        // paint, so nothing has done it for us. Same two lines MinimapTool uses around
        // saveMinimap (MinimapTool.cpp:154-155), and the scoped_setter is what points the global
        // `gl` wrapper at the right context for the duration.
        OpenGL::context::scoped_setter const context_setter (::gl, _map_view->context());
        _map_view->makeCurrent();

        if (!world->renderer()->renderSunDepth(tile_index, settings, depth_map, &neighbours_loaded))
        {
          setStatus("The sun depth render failed. The most likely cause is a depth resolution the"
                    " driver would not allocate -- try a smaller one.");
          return;
        }
      }

      // Everything from here down is CPU only, and deliberately outside the scoped_setter: it
      // touches no GL and holding the context current across it would serve no purpose.
      Noggit::Rendering::ShadowBakeReport report
        (world->bakeTerrainShadows(_map_view, tile, depth_map, settings));

      report.neighbour_tiles_loaded = neighbours_loaded;

      // Closed only if bakeTerrainShadows actually opened one, which it does lazily on the first
      // chunk that changes -- so a re-bake that finds nothing to do leaves the undo stack alone
      // and this closes nothing. See the note on World::bakeTerrainShadows.
      if (NOGGIT_CUR_ACTION)
      {
        try
        {
          NOGGIT_ACTION_MGR->endAction();
        }
        catch (std::exception const& e)
        {
          // endAction -> Action::finish() allocates the redo snapshot, one 4 KB array per changed
          // chunk, and is a plausible place for bad_alloc on a 32-bit build. ActionManager clears
          // _cur_action before anything that can throw (ActionManager.cpp:103-118), so undo
          // survives; the redo half does not, and saying so beats a silent half-recorded step.
          setStatus(QString("The bake was applied but its redo data could not be stored: %1."
                            " Undo still works.").arg(QString::fromUtf8(e.what())));
          return;
        }
      }

      double const seconds
        = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();

      setStatus(describeReport(report, seconds));
    }

    void ShadowToolSettings::clearCurrentTile()
    {
      if (NOGGIT_CUR_ACTION)
      {
        setStatus("Another edit is still in progress. Release the mouse button and try again.");
        return;
      }

      World* world = _map_view->getWorld();
      glm::vec3 const camera (_map_view->getCamera()->position);
      TileIndex const tile_index (camera);

      if (!world->mapIndex.hasTile(tile_index) || !world->mapIndex.tileLoaded(tile_index))
      {
        setStatus("No loaded tile under the camera.");
        return;
      }

      // World::clear_shadows walks the tile under a position and registers every chunk, so this
      // is one action and one ctrl-Z exactly as the bake is. Opened here because clear_shadows
      // dereferences NOGGIT_CUR_ACTION without checking it (World.cpp:3089).
      NOGGIT_ACTION_MGR->beginAction(_map_view, Noggit::ActionFlags::eCHUNK_SHADOWS);
      world->clear_shadows(camera);

      try
      {
        NOGGIT_ACTION_MGR->endAction();
      }
      catch (std::exception const& e)
      {
        setStatus(QString("Shadows were cleared but redo data could not be stored: %1.")
                    .arg(QString::fromUtf8(e.what())));
        return;
      }

      setStatus("Cleared MCSH across the tile. One undo step.");
    }

    QSize ShadowToolSettings::sizeHint() const
    {
      return QSize(250, height());
    }
  }
}
