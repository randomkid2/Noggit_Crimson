// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/ui/TerrainMaskToolSettings.hpp>

// Alphamap.hpp and texture_set.hpp are here for TerrainMaskBaker, not for this file's own code.
// bakeTile instantiates TextureLayerAlphaProbe::readLayerAlpha<MapChunk>, whose body dereferences
// the TextureSet that MapChunk.h only forward-declares -- see the contract at the top of
// TextureLayerAlphaProbe.hpp, which lists exactly these three headers.
#include <noggit/Alphamap.hpp>
#include <noggit/MapChunk.h>
#include <noggit/MapTile.h>
#include <noggit/MapView.h>
#include <noggit/TileIndex.hpp>
#include <noggit/World.h>
#include <noggit/texture_set.hpp>
#include <noggit/project/CurrentProject.hpp>
#include <noggit/terrain/TerrainMask.hpp>
#include <noggit/terrain/TerrainMaskBaker.hpp>
#include <noggit/terrain/TerrainMaskHistory.hpp>
#include <noggit/terrain/TerrainMaskStore.hpp>
#include <noggit/ui/DesignTokens.hpp>
#include <noggit/ui/TerrainMaskDialog.hpp>
#include <noggit/ui/TerrainMaskPreview.hpp>
#include <noggit/ui/tools/ToolPanel/ToolWidgetStyle.hpp>
#include <noggit/ui/tools/UiCommon/ExtendedSlider.hpp>

#include <QtCore/QString>
#include <QtCore/QTimer>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QInputDialog>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QVBoxLayout>

#include <chrono>
#include <string>
#include <vector>

namespace
{
  // A mask brush larger than this is not a brush, it is a fill, and the loupe cannot show it:
  // 200 yd of radius is 400 yd across against the widest 533.33 yd window. The same ceiling
  // ShadowToolSettings uses, and for the same reason.
  constexpr double MAX_BRUSH_RADIUS = 200.0;

  constexpr double DEFAULT_BRUSH_RADIUS = 15.0;
  constexpr double DEFAULT_HARDNESS = 0.5;
  constexpr double DEFAULT_STRENGTH = 1.0;

  // Only the two folds TerrainMaskStore's own note names as the ninety-per-cent cases. Replace,
  // Add, Subtract and Multiply are reachable from the dialog for a user who wants them; putting
  // all six on the brush panel would make the important distinction -- does my brush add to the
  // derived mask or carve out of it -- one option among six instead of a two-way switch.
  constexpr Noggit::MaskCombine FOLD_ORDER[] =
    { Noggit::MaskCombine::Max
    , Noggit::MaskCombine::Min
    };

  QString describeBytes(std::size_t bytes)
  {
    if (bytes < 1024u)
    {
      return QString("%1 B").arg(bytes);
    }

    if (bytes < 1024u * 1024u)
    {
      return QString("%1 KiB").arg(static_cast<double>(bytes) / 1024.0, 0, 'f', 1);
    }

    return QString("%1 MiB").arg(static_cast<double>(bytes) / (1024.0 * 1024.0), 0, 'f', 2);
  }

  // TileIndex casts straight to std::size_t (TileIndex.cpp:11-12), so a non-finite or negative
  // coordinate has to be rejected BEFORE one is built. Non-finite is not hypothetical: it is what
  // an unprojection that missed the terrain produces, which is the same case TerrainMaskQuery.hpp
  // fails open on.
  bool usableCursor(glm::vec3 const& position)
  {
    constexpr float map_extent
      = static_cast<float>(Noggit::MASK_MAP_TILE_SIDE) * Noggit::MASK_TILE_SIZE;

    return position.x >= 0.0f && position.z >= 0.0f
        && position.x < map_extent
        && position.z < map_extent;
  }

  std::string currentProjectPath()
  {
    // Safe from a tool panel for the reason TerrainMaskDialog::projectPath gives: this widget is
    // built by a MapView and a MapView cannot exist without an open project. The null test is
    // still here because CurrentProject::get() asserts on one in a debug build and an assert is
    // a worse diagnostic than an empty path.
    auto* const project = Noggit::Project::CurrentProject::get();
    return project ? project->ProjectPath : std::string();
  }
}

namespace Noggit::Ui
{
  TerrainMaskToolSettings::TerrainMaskToolSettings(MapView* map_view)
    : QWidget(map_view)
    , _map_view(map_view)
    , _poll(new QTimer(this))
  {
    auto* const layout(Tools::ToolPanelStyle::toolColumn(this));

    layout->addWidget
      ( Tools::ToolPanelStyle::keybindRow
          (this, FontNoggit::shift, FontNoggit::lmb, tr("Paint into mask"))
      );
    layout->addWidget
      ( Tools::ToolPanelStyle::keybindRow
          (this, FontNoggit::ctrl, FontNoggit::lmb, tr("Erase from mask"))
      );

    // === The mask ===============================================================

    auto* const mask_group(Tools::ToolPanelStyle::toolSection(layout, tr("Mask")));
    auto* const mask_layout(Tools::ToolPanelStyle::sectionColumn(mask_group));

    _mask_combo = new QComboBox(mask_group);
    // No angle brackets in this string. Qt::AutoText guesses rich text from the first tag it
    // sees, and a literal "<project>" would be parsed as an unknown element and dropped.
    _mask_combo->setToolTip("Which named mask the brush writes into. Masks are project sidecars"
                            " in the project's noggit_masks folder and never touch an ADT.");
    mask_layout->addWidget(_mask_combo);

    // THE IDENTITY ROW IS THE ANSWER TO "WHICH MASK AM I PAINTING". A combo alone is not enough:
    // it is one line of small text among four other combo boxes on this panel, and the cost of
    // getting it wrong is a stroke into the wrong region that nothing on screen contradicts. The
    // swatch is the same colour as the loupe's fill and the brush ring in the 3D view, so all
    // three agree at a glance.
    _identity_label = new QLabel(mask_group);
    _identity_label->setTextFormat(Qt::RichText);
    _identity_label->setWordWrap(true);
    mask_layout->addWidget(_identity_label);

    auto* const mask_buttons(new QHBoxLayout());
    _new_button = new QPushButton(tr("New..."), mask_group);
    _new_button->setAutoDefault(false);
    _save_button = new QPushButton(tr("Save"), mask_group);
    _save_button->setAutoDefault(false);
    _save_button->setToolTip("Writes every mask to the project sidecar. Paint lives in memory"
                             " until this is pressed, and closing the map loses it.");
    mask_buttons->addWidget(_new_button);
    mask_buttons->addWidget(_save_button);
    mask_layout->addLayout(mask_buttons);

    _dialog_button = new QPushButton(tr("Terrain Masks..."), mask_group);
    _dialog_button->setAutoDefault(false);
    _dialog_button->setToolTip("The full editor: filter stacks, deriving a mask from slope,"
                               " height, curvature, layer alpha, area id and noise, and baking"
                               " it over every loaded tile.");
    mask_layout->addWidget(_dialog_button);

    _clip_enabled = new QCheckBox(tr("Clip every brush to this mask"), mask_group);
    _clip_enabled->setToolTip("The same master switch the Terrain Masks dialog carries. It does"
                              " not affect this brush -- it decides whether the OTHER tools are"
                              " clipped by what you are painting.");
    mask_layout->addWidget(_clip_enabled);

    _paint_combine = new QComboBox(mask_group);
    // The value is carried in the item DATA, never inferred from the row index -- the same rule
    // ShadowToolSettings and ErosionToolSettings state for their combos.
    _paint_combine->addItem(tr("Paint adds to the derived mask"), static_cast<int>(MaskCombine::Max));
    _paint_combine->addItem(tr("Paint carves out of it"), static_cast<int>(MaskCombine::Min));
    mask_layout->addWidget(_paint_combine);

    _fold_note = new QLabel(mask_group);
    _fold_note->setWordWrap(true);
    _fold_note->setStyleSheet(QString("color: %1;").arg(QString::fromLatin1(Design::TEXT_DIM)));
    mask_layout->addWidget(_fold_note);

    // === The brush ==============================================================

    auto* const brush_group(Tools::ToolPanelStyle::toolSection(layout, tr("Brush")));
    auto* const brush_layout(Tools::ToolPanelStyle::sectionColumn(brush_group));

    _radius_slider = new Noggit::Ui::Tools::UiCommon::ExtendedSlider(this);
    _radius_slider->setPrefix("Radius:");
    _radius_slider->setRange(0.0, MAX_BRUSH_RADIUS);
    _radius_slider->setDecimals(2);
    _radius_slider->setValue(DEFAULT_BRUSH_RADIUS);
    _radius_slider->setToolTip("In yards, like every other brush. The mask stores one byte per"
                               " 0.52 yd texel, which is exactly the alphamap texel the texture"
                               " brush paints, so a mask edge is never coarser than the paint it"
                               " clips.");
    brush_layout->addWidget(_radius_slider);

    _hardness_slider = new Noggit::Ui::Tools::UiCommon::ExtendedSlider(this);
    _hardness_slider->setPrefix("Hardness:");
    _hardness_slider->setRange(0.0, 1.0);
    _hardness_slider->setDecimals(2);
    _hardness_slider->setSingleStep(0.05);
    _hardness_slider->setValue(DEFAULT_HARDNESS);
    _hardness_slider->setToolTip("Fraction of the radius that is fully weighted. The feather"
                                 " outside it is linear -- the same profile Brush::getValue gives"
                                 " the texture brush, so a mask painted at a radius and hardness"
                                 " has the same edge as a texture stroke with those numbers.");
    brush_layout->addWidget(_hardness_slider);

    _strength_slider = new Noggit::Ui::Tools::UiCommon::ExtendedSlider(this);
    _strength_slider->setPrefix("Strength:");
    _strength_slider->setRange(0.0, 1.0);
    _strength_slider->setDecimals(2);
    _strength_slider->setSingleStep(0.05);
    _strength_slider->setValue(DEFAULT_STRENGTH);
    _strength_slider->setToolTip("What the stroke converges ON, not how fast it gets there. A"
                                 " stroke folds with Max, so holding the brush still at strength"
                                 " 0.5 settles at half and does not creep to full.");
    brush_layout->addWidget(_strength_slider);

    // === Seeing it ==============================================================

    auto* const view_group(Tools::ToolPanelStyle::toolSection(layout, tr("What is there")));
    auto* const view_layout(Tools::ToolPanelStyle::sectionColumn(view_group));

    _preview = new TerrainMaskPreview(map_view, view_group);
    view_layout->addWidget(_preview);

    _window_combo = new QComboBox(view_group);
    _window_combo->addItem(tr("64 yd window"), 64.0);
    _window_combo->addItem(tr("128 yd window"), 128.0);
    _window_combo->addItem(tr("256 yd window"), 256.0);
    _window_combo->addItem(tr("533 yd window (one tile)"), static_cast<double>(MASK_TILE_SIZE));
    _window_combo->setCurrentIndex(1);
    view_layout->addWidget(_window_combo);

    _paint_only = new QCheckBox(tr("Show the paint layer alone"), view_group);
    _paint_only->setToolTip("Off shows the composited field, which is the filter stack with your"
                            " paint folded on top and is what the brushes actually read. On shows"
                            " only what you painted, which is the part nothing can recompute.");
    view_layout->addWidget(_paint_only);

    _readout_label = new QLabel(view_group);
    _readout_label->setWordWrap(true);
    view_layout->addWidget(_readout_label);

    // === History ================================================================

    auto* const history_group(Tools::ToolPanelStyle::toolSection(layout, tr("Stroke history")));
    auto* const history_layout(Tools::ToolPanelStyle::sectionColumn(history_group));

    auto* const history_buttons(new QHBoxLayout());
    _undo_button = new QPushButton(tr("Undo (Alt+Z)"), history_group);
    _undo_button->setAutoDefault(false);
    _redo_button = new QPushButton(tr("Redo (Alt+Shift+Z)"), history_group);
    _redo_button->setAutoDefault(false);
    history_buttons->addWidget(_undo_button);
    history_buttons->addWidget(_redo_button);
    history_layout->addLayout(history_buttons);

    _history_label = new QLabel(history_group);
    _history_label->setWordWrap(true);
    _history_label->setStyleSheet(QString("color: %1;").arg(QString::fromLatin1(Design::TEXT_DIM)));
    // Says the thing a user will otherwise discover by pressing Ctrl+Z and watching a hillside
    // move. The reasoning is in TerrainMaskHistory.hpp; the panel only has to state the rule.
    _history_label->setToolTip("Mask strokes have their own history and are NOT on the editor's"
                               " undo stack. Ctrl+Z still undoes terrain, textures and objects"
                               " and never touches a mask.");
    history_layout->addWidget(_history_label);

    // === Bake ===================================================================

    auto* const bake_group(Tools::ToolPanelStyle::toolSection(layout, tr("Bake")));
    auto* const bake_layout(Tools::ToolPanelStyle::sectionColumn(bake_group));

    _auto_bake = new QCheckBox(tr("Bake a tile on the first stroke"), bake_group);
    _auto_bake->setChecked(true);
    // WHY THIS IS ON BY DEFAULT. TerrainMaskStore::factorAt returns 1.0 on a tile the mask has
    // never been baked over, so without a bake a hand-painted mask clips nothing at all and the
    // brush appears to do nothing. Baking one tile on the first click is what makes the tool
    // work on its own; the switch exists for a heavy filter stack, where the bake is a visible
    // hitch and the user may prefer to do the whole map from the dialog.
    _auto_bake->setToolTip("A mask does not clip anything on a tile it has not been baked over."
                           " With this on, the first stroke on a tile bakes it. Turn it off if"
                           " your filter stack is expensive and you would rather bake from the"
                           " Terrain Masks dialog.");
    bake_layout->addWidget(_auto_bake);

    _bake_button = new QPushButton(tr("Bake the tile under the cursor"), bake_group);
    _bake_button->setAutoDefault(false);
    bake_layout->addWidget(_bake_button);

    _status_label = new QLabel(bake_group);
    _status_label->setWordWrap(true);
    bake_layout->addWidget(_status_label);

    // === Wiring =================================================================

    connect(_mask_combo, qOverload<int>(&QComboBox::currentIndexChanged), this
           , [this] (int index) { onMaskChosen(index); });

    connect(_new_button, &QPushButton::clicked, this, [this] { onNewMask(); });
    connect(_save_button, &QPushButton::clicked, this, [this] { onSaveMasks(); });

    connect(_dialog_button, &QPushButton::clicked, this
           , [this]
             {
               // Modeless, exactly as the Assist menu opens it: the dialog authors the stack and
               // this panel paints into the result, and both have to be usable at once.
               auto* const dialog = new TerrainMaskDialog(_map_view, _map_view);
               dialog->setAttribute(Qt::WA_DeleteOnClose);
               dialog->show();
             });

    connect(_clip_enabled, &QCheckBox::toggled, this
           , [this] (bool enabled)
             {
               if (_loading_widgets)
               {
                 return;
               }

               TerrainMaskStore::instance()->setClippingEnabled(enabled);
             });

    connect(_paint_combine, qOverload<int>(&QComboBox::currentIndexChanged), this
           , [this] (int index)
             {
               if (_loading_widgets || index < 0)
               {
                 return;
               }

               NamedTerrainMask* const mask = TerrainMaskStore::instance()->active();

               if (mask)
               {
                 mask->paint_combine = FOLD_ORDER[index];
               }

               refreshVolatileState();
             });

    connect(_window_combo, qOverload<int>(&QComboBox::currentIndexChanged), this
           , [this] (int index)
             {
               _preview->setWindowYards
                 (static_cast<float>(_window_combo->itemData(index).toDouble()));
             });

    connect(_paint_only, &QCheckBox::toggled, this
           , [this] (bool on) { _preview->setShowPaintOnly(on); });

    connect(_radius_slider, &Noggit::Ui::Tools::UiCommon::ExtendedSlider::valueChanged, this
           , [this] (double) { pushBrushToPreview(); });
    connect(_hardness_slider, &Noggit::Ui::Tools::UiCommon::ExtendedSlider::valueChanged, this
           , [this] (double) { pushBrushToPreview(); });

    connect(_undo_button, &QPushButton::clicked, this, [this] { undoStroke(); });
    connect(_redo_button, &QPushButton::clicked, this, [this] { redoStroke(); });

    connect(_bake_button, &QPushButton::clicked, this
           , [this]
             {
               // The button ignores the auto-bake switch AND re-bakes a tile that is already
               // baked, because "bake this tile" is also how a user picks up a terrain change the
               // mask has not seen. ensureTileBaked is the incremental path; this is not.
               glm::vec3 const cursor = _map_view->cursorPosition();
               TileIndex const index(cursor);

               NamedTerrainMask* const mask = TerrainMaskStore::instance()->active();

               if (mask)
               {
                 // Drops the composited field for this tile and clears its baked flag. The paint
                 // layer is untouched, which is the whole point of NamedTerrainMask::releaseTile.
                 mask->releaseTile(static_cast<int>(index.x), static_cast<int>(index.z));
               }

               performTileBake(cursor);
             });

    // 250 ms. Slow enough to cost nothing -- it reads a handful of scalars off the store -- and
    // fast enough that a change made in the dialog is reflected here before the user's hand gets
    // back to the mouse.
    _poll->setInterval(250);
    connect(_poll, &QTimer::timeout, this, [this] { refreshVolatileState(); });

    refreshMaskCombo();
    pushBrushToPreview();

    // The combo's index was set before the connect above, so nothing pushed the initial window
    // size through. The preview's own default happens to be the same 128 yd, and this line is
    // what stops that coincidence from being load-bearing.
    _preview->setWindowYards(static_cast<float>(_window_combo->currentData().toDouble()));

    refreshVolatileState();
    setStatus(tr("A mask decides where the OTHER tools apply. Nothing here writes to an ADT."));
  }

  float TerrainMaskToolSettings::brushRadius() const
  {
    // ExtendedSlider::value() is not const in this tree, so the const_cast is here rather than in
    // the accessor -- Tool::brushRadius is const and changing that signature would touch tools
    // this round does not own. Copied verbatim from ShadowToolSettings::brushRadius.
    return static_cast<float>
      (const_cast<Noggit::Ui::Tools::UiCommon::ExtendedSlider*>(_radius_slider)->value());
  }

  float TerrainMaskToolSettings::hardness() const
  {
    return static_cast<float>
      (const_cast<Noggit::Ui::Tools::UiCommon::ExtendedSlider*>(_hardness_slider)->value());
  }

  float TerrainMaskToolSettings::strength() const
  {
    return static_cast<float>
      (const_cast<Noggit::Ui::Tools::UiCommon::ExtendedSlider*>(_strength_slider)->value());
  }

  void TerrainMaskToolSettings::changeRadius(float change)
  {
    _radius_slider->setValue(_radius_slider->value() + change);
  }

  void TerrainMaskToolSettings::changeHardness(float change)
  {
    _hardness_slider->setValue(_hardness_slider->value() + change);
  }

  void TerrainMaskToolSettings::changeStrength(float change)
  {
    _strength_slider->setValue(_strength_slider->value() + change);
  }

  glm::vec4 TerrainMaskToolSettings::cursorColor() const
  {
    NamedTerrainMask const* const mask = TerrainMaskStore::instance()->active();

    if (!mask)
    {
      // White, which is MapView's own default (MapView.cpp:4747). A tool with no mask selected
      // must not paint the ring some arbitrary colour that means nothing.
      return glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);
    }

    QColor const identity = maskIdentityColor(mask->name);

    float const value = _preview->valueUnderCursor();

    // 0.35 floor so the ring never goes so dark it reads as absent over masked-out ground; the
    // ring is also the cursor, and losing the cursor is worse than losing the readout.
    float const brightness = 0.35f + 0.65f * (value < 0.0f ? 0.0f : value);

    return glm::vec4( static_cast<float>(identity.redF()) * brightness
                    , static_cast<float>(identity.greenF()) * brightness
                    , static_cast<float>(identity.blueF()) * brightness
                    , 1.0f
                    );
  }

  void TerrainMaskToolSettings::onToolSelected()
  {
    loadProjectMasksOnce();
    refreshMaskCombo();
    refreshVolatileState();
  }

  void TerrainMaskToolSettings::loadProjectMasksOnce()
  {
    if (_tried_project_load)
    {
      return;
    }

    _tried_project_load = true;

    if (!TerrainMaskStore::instance()->names().empty())
    {
      // Something already put masks here -- almost certainly the dialog's own load -- and load()
      // would throw them away and re-read the disk.
      return;
    }

    std::string const path = currentProjectPath();

    if (path.empty())
    {
      return;
    }

    // Failure is the normal case for a project that has never had a mask, so it is not reported:
    // there is no directory to read and nothing has gone wrong. A real error is still available
    // through the dialog's Reload, which asks and reports.
    if (TerrainMaskStore::instance()->load(path))
    {
      // load() replaced the set, so any recorded stroke now names a mask that either no longer
      // exists or merely shares a name with the one it was captured from.
      TerrainMaskHistory::instance()->clear();
    }
  }

  void TerrainMaskToolSettings::noteStrokePainted()
  {
    // Flags only. This is reached from Tool::onTick, which runs inside paintGL, and setting a
    // QLabel's text from there would run a layout pass in the middle of a frame.
    _preview->markDirty();
  }

  void TerrainMaskToolSettings::undoStroke()
  {
    if (TerrainMaskHistory::instance()->undo(*TerrainMaskStore::instance()))
    {
      _preview->markDirty();
      setStatus(tr("Undid one mask stroke. This history is the mask's own -- Ctrl+Z is still the"
                   " editor's and still undoes terrain."));
    }
    else
    {
      setStatus(tr("Nothing to undo in this mask's stroke history."));
    }

    refreshVolatileState();
  }

  void TerrainMaskToolSettings::redoStroke()
  {
    if (TerrainMaskHistory::instance()->redo(*TerrainMaskStore::instance()))
    {
      _preview->markDirty();
      setStatus(tr("Redid one mask stroke."));
    }
    else
    {
      setStatus(tr("Nothing to redo in this mask's stroke history."));
    }

    refreshVolatileState();
  }

  void TerrainMaskToolSettings::ensureTileBaked(glm::vec3 const& position)
  {
    NamedTerrainMask* const mask = TerrainMaskStore::instance()->active();

    if (!mask || !usableCursor(position))
    {
      return;
    }

    TileIndex const index(position);
    int const tile_x = static_cast<int>(index.x);
    int const tile_z = static_cast<int>(index.z);

    if (mask->tileIsBaked(tile_x, tile_z))
    {
      return;
    }

    if (!_auto_bake->isChecked())
    {
      setStatus(tr("Tile %1, %2 is not baked, so the stroke is stored but clips nothing yet."
                   " Bake it, or switch the first-stroke bake back on.").arg(tile_x).arg(tile_z));
      return;
    }

    performTileBake(position);
  }

  void TerrainMaskToolSettings::performTileBake(glm::vec3 const& position)
  {
    NamedTerrainMask* const mask = TerrainMaskStore::instance()->active();

    if (!mask)
    {
      setStatus(tr("No mask selected, so there is nothing to bake."));
      return;
    }

    if (!usableCursor(position))
    {
      setStatus(tr("The cursor is not on the map."));
      return;
    }

    TileIndex const index(position);
    int const tile_x = static_cast<int>(index.x);
    int const tile_z = static_cast<int>(index.z);

    World* const world = _map_view->getWorld();

    if (!world->mapIndex.hasTile(index) || !world->mapIndex.tileLoaded(index))
    {
      setStatus(tr("No loaded tile under the cursor."));
      return;
    }

    MapTile* const tile = world->mapIndex.getTile(index);

    if (!tile)
    {
      setStatus(tr("No loaded tile under the cursor."));
      return;
    }

    // The one validate() complaint that has to block rather than warn, restated from
    // TerrainMaskBaker::bakeLoadedTiles: a stack mixing curvature scales rebuilds the curvature
    // grid per texel and takes minutes, which from a mouse press looks exactly like a hang.
    for (std::string const& problem : mask->stack.validate())
    {
      if (problem.find("different scales") != std::string::npos)
      {
        setStatus(tr("Not baking: %1.").arg(QString::fromStdString(problem)));
        return;
      }
    }

    auto const started = std::chrono::steady_clock::now();

    std::size_t const written
      = TerrainMaskBaker::bakeTile<MapTile, MapChunk>(*mask, tile, tile_x, tile_z);

    double const seconds
      = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();

    // After the bake, not before: enforceBudget drops composited fields when the store is over
    // its 192 MiB soft cap, and it never touches the active mask or any paint layer.
    std::size_t const dropped = TerrainMaskStore::instance()->enforceBudget();

    if (written == 0)
    {
      setStatus(tr("Tile %1, %2 produced no chunks -- it is still loading, or the stack needs a"
                   " height field the tile could not supply.").arg(tile_x).arg(tile_z));
      return;
    }

    QString text = tr("Baked tile %1, %2: %3 chunks in %4 s.")
                     .arg(tile_x).arg(tile_z).arg(written).arg(seconds, 0, 'f', 2);

    if (dropped > 0)
    {
      text += tr(" %1 other mask(s) dropped their composited field to stay inside the memory"
                 " budget; re-bake them when you need them.").arg(dropped);
    }

    setStatus(text);
    _preview->markDirty();
  }

  QSize TerrainMaskToolSettings::sizeHint() const
  {
    return QSize(250, height());
  }

  void TerrainMaskToolSettings::showEvent(QShowEvent* event)
  {
    QWidget::showEvent(event);
    refreshMaskCombo();
    refreshVolatileState();
    _poll->start();
  }

  void TerrainMaskToolSettings::hideEvent(QHideEvent* event)
  {
    _poll->stop();
    QWidget::hideEvent(event);
  }

  void TerrainMaskToolSettings::pushBrushToPreview()
  {
    _preview->setBrush(brushRadius(), hardness());
  }

  void TerrainMaskToolSettings::refreshMaskCombo()
  {
    _loading_widgets = true;

    std::string const active = TerrainMaskStore::instance()->activeName();

    _mask_combo->clear();
    _mask_combo->addItem(tr("(no mask selected)"), QString());

    int select = 0;

    for (std::string const& name : TerrainMaskStore::instance()->names())
    {
      QString const text = QString::fromStdString(name);
      _mask_combo->addItem(text, text);

      if (name == active)
      {
        select = _mask_combo->count() - 1;
      }
    }

    _mask_combo->setCurrentIndex(select);

    _loading_widgets = false;
  }

  void TerrainMaskToolSettings::onMaskChosen(int index)
  {
    if (_loading_widgets || index < 0)
    {
      return;
    }

    // A stroke in flight belongs to the mask it started in. Closing it here is what stops one
    // history entry from holding chunks of two different masks -- see TerrainMaskHistory.
    TerrainMaskHistory::instance()->endStroke();

    TerrainMaskStore::instance()->setActive(_mask_combo->itemData(index).toString().toStdString());

    _preview->markDirty();
    refreshVolatileState();
  }

  void TerrainMaskToolSettings::onNewMask()
  {
    bool accepted = false;

    QString const name = QInputDialog::getText( this
                                              , tr("New mask")
                                              , tr("Name:")
                                              , QLineEdit::Normal
                                              , QString()
                                              , &accepted
                                              );

    if (!accepted)
    {
      return;
    }

    NamedTerrainMask* const mask = TerrainMaskStore::instance()->create(name.toStdString());

    if (!mask)
    {
      setStatus(tr("Could not create \"%1\": the name is empty, already taken, or has nothing in"
                   " it that can be used as a filename.").arg(name));
      return;
    }

    TerrainMaskStore::instance()->setActive(mask->name);

    refreshMaskCombo();
    refreshVolatileState();

    setStatus(tr("Created \"%1\" with no filter stack, so it is empty and everything you paint"
                 " into it is yours. Press Save to put it in the project.").arg(name));
  }

  void TerrainMaskToolSettings::onSaveMasks()
  {
    std::string const path = currentProjectPath();

    if (path.empty())
    {
      setStatus(tr("No project path, so there is nowhere to save to."));
      return;
    }

    if (!TerrainMaskStore::instance()->save(path))
    {
      setStatus(tr("Save failed: %1")
                  .arg(QString::fromStdString(TerrainMaskStore::instance()->lastError())));
      return;
    }

    setStatus(tr("Saved every mask to the project sidecar. Filter stacks as JSON and paint layers"
                 " run-length encoded; the composited field is not saved because it is a pure"
                 " function of the two and rebaking it is cheap."));
  }

  void TerrainMaskToolSettings::refreshVolatileState()
  {
    _loading_widgets = true;

    TerrainMaskStore* const store = TerrainMaskStore::instance();
    NamedTerrainMask const* const mask = store->active();

    // --- Which mask ---

    if (mask)
    {
      QColor const identity = maskIdentityColor(mask->name);

      _identity_label->setText
        (QString("<span style=\"color:%1; font-size:16px;\">&#9632;</span> "
                 "<b style=\"color:%2;\">%3</b>")
           .arg(identity.name())
           .arg(QString::fromLatin1(Design::TEXT_HI))
           .arg(QString::fromStdString(mask->name).toHtmlEscaped()));
    }
    else
    {
      _identity_label->setText
        (QString("<b style=\"color:%1;\">%2</b>")
           .arg(QString::fromLatin1(Design::WARN))
           .arg(tr("No mask selected - the brush does nothing")));
    }

    // The combo can go stale under a rename or a delete made in the dialog. Rebuilding it every
    // poll tick would fight the user's own drop-down, so it is rebuilt only when the store and
    // the combo disagree about how many masks there are or about which one is active.
    std::vector<std::string> const names = store->names();

    bool const count_matches
      = _mask_combo->count() == static_cast<int>(names.size()) + 1;
    bool const active_matches
      = _mask_combo->itemData(_mask_combo->currentIndex()).toString().toStdString()
        == store->activeName();

    if (!count_matches || !active_matches)
    {
      _loading_widgets = false;
      refreshMaskCombo();
      _loading_widgets = true;
    }

    // --- The two shared switches ---

    _clip_enabled->setChecked(store->clippingEnabled());

    // The dialog offers all six combinators and this panel offers two, so a mask can arrive here
    // folding with Replace, Add, Subtract or Multiply -- and a two-item combo cannot show that.
    // It says so and disables itself rather than displaying "Paint adds" over a mask that
    // multiplies, which would be a control that lies about the value it is bound to.
    int fold_index = -1;

    if (mask)
    {
      for (int i = 0; i < static_cast<int>(sizeof(FOLD_ORDER) / sizeof(FOLD_ORDER[0])); ++i)
      {
        if (FOLD_ORDER[i] == mask->paint_combine)
        {
          fold_index = i;
        }
      }
    }

    _paint_combine->setEnabled(fold_index >= 0);

    if (fold_index >= 0)
    {
      _paint_combine->setCurrentIndex(fold_index);

      // THE TRAP THIS LABEL EXISTS FOR. The fold is applied at BAKE time, and a stroke is applied
      // at once. With the fold on Max, an erase stroke is visible immediately and then quietly
      // reverts the next time the tile is baked, because Max(derived, a low painted value) is
      // derived. Nothing else in the interface says so.
      if (mask->paint_combine == MaskCombine::Max)
      {
        _fold_note->setText(tr("Erase strokes show at once but are dropped at the next bake,"
                               " because a fold of Max keeps whichever is higher."));
      }
      else
      {
        _fold_note->setText(tr("Paint strokes show at once but are dropped at the next bake,"
                               " because a fold of Min keeps whichever is lower."));
      }
    }
    else if (mask)
    {
      _fold_note->setText(tr("This mask folds paint with \"%1\", which is set in the Terrain"
                             " Masks dialog and cannot be shown here.")
                            .arg(QString::fromLatin1(maskCombineName(mask->paint_combine))));
    }
    else
    {
      _fold_note->setText(QString());
    }

    // --- The readout ---

    glm::vec3 const cursor = _map_view->cursorPosition();
    float const value = _preview->valueUnderCursor();

    QString readout;

    if (value < 0.0f)
    {
      readout = tr("Mask under cursor: -");
    }
    else
    {
      readout = tr("Mask under cursor: %1%").arg(static_cast<int>(value * 100.0f + 0.5f));
    }

    if (cursor.x >= 0.0f && cursor.z >= 0.0f)
    {
      readout += tr("   tile %1, %2")
                   .arg(static_cast<int>(cursor.x / MASK_TILE_SIZE))
                   .arg(static_cast<int>(cursor.z / MASK_TILE_SIZE));
    }

    readout += store->clippingEnabled()
                 ? tr("\nClipping is ON: every terrain and texture brush is scaled by this mask.")
                 : tr("\nClipping is OFF: the other brushes ignore this mask entirely.");

    _readout_label->setText(readout);

    // --- History ---

    TerrainMaskHistory const* const history = TerrainMaskHistory::instance();

    _undo_button->setEnabled(history->canUndo());
    _redo_button->setEnabled(history->canRedo());

    _history_label->setText
      (tr("%1 undo, %2 redo, %3. Not on the editor's undo stack -- Ctrl+Z is unchanged.")
         .arg(history->undoDepth())
         .arg(history->redoDepth())
         .arg(describeBytes(history->bytes())));

    _loading_widgets = false;
  }

  void TerrainMaskToolSettings::setStatus(QString const& text)
  {
    _status_label->setText(text);
  }
}
