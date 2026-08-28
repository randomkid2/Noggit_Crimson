// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/ui/FlattenTool.hpp>
#include <noggit/ui/DesignTokens.hpp>
#include <noggit/ui/FontNoggit.hpp>
#include <noggit/ui/tools/ToolPanel/ToolWidgetStyle.hpp>
#include <noggit/ui/tools/UiCommon/ExtendedSlider.hpp>
#include <noggit/World.h>

#include <math/trig.hpp>

#include <util/qt/overload.hpp>

#include <QtCore/QSettings>
#include <QtWidgets/QButtonGroup>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QDial>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSlider>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace Noggit
{
  namespace Ui
  {
    flatten_blur_tool::flatten_blur_tool(QWidget* parent)
      : QWidget(parent)
      , _angle(45.0f)
      , _orientation(0.0f)
      , _flatten_type(eFlattenType_Linear)
      , _ramp_start(0.0f, 0.0f, 0.0f)
      , _ramp_end(0.0f, 0.0f, 0.0f)
      , _has_ramp_start(false)
      , _has_ramp_end(false)
      , _flatten_mode(true, true)
    {
      // The dock's shared shell -- zero margins, S3 between sections, 250px floor. This layout
      // set no margins, so it took QStyle::PM_LayoutLeftMargin (13px on windowsvista here) on
      // top of ToolPanel's own 12px. See ToolWidgetStyle.hpp.
      auto layout (Tools::ToolPanelStyle::toolColumn (this));

      // THE KEYBIND LEGEND, and the three layout-parent defects it used to carry.
      //
      // Both rows were built as `new QHBoxLayout(this)` on a widget that ALREADY owned the
      // column above, so Qt logged "Attempting to add QLayout to QWidget which already has a
      // layout" into log.txt twice on every flatten tool constructed, and neither row was
      // installed where the code meant. Row 1 was then rescued by setLayout on a container, and
      // ALSO handed to layout->addLayout, which QLayout::addChildLayout refuses outright
      // because the layout already has a parent -- a third warning, and a dead call. Each row
      // is now built by one function that constructs the layout directly on a fresh container
      // widget, so nothing is ever handed a widget that already owns a layout.
      //
      // ErosionToolSettings already carried the corrected shape of this block and says in its
      // own comment that it copies this file. This is the file that was never updated.
      layout->addWidget
        ( Tools::ToolPanelStyle::keybindRow
            (this, FontNoggit::shift, FontNoggit::lmb, tr ("Flatten Terrain"))
        );

      layout->addWidget
        ( Tools::ToolPanelStyle::keybindRow
            (this, FontNoggit::ctrl, FontNoggit::lmb, tr ("Blur Terrain"))
        );

      // THE TYPE PICKER, now the same segmented control TerrainTool uses.
      //
      // These two tools occupy the same dock slot and swap on a keypress, and they built the
      // same concept -- the brush's falloff type -- two different ways: a 3x3 grid of checkable
      // chips in one, a 2x2 block of QRadioButtons in the other. The user watched one idiom
      // turn into the other.
      //
      // Nothing about the group changed. Same QButtonGroup, same four eFlattenType_* ids, same
      // exclusivity, same idClicked connection below, and the id -> button lookup other code
      // uses is QAbstractButton API that does not care about the concrete class. An exclusive
      // QButtonGroup refuses to uncheck its checked member on a second click for push buttons
      // exactly as it does for radios.
      _type_button_box = new QButtonGroup (this);

      QPushButton* radio_flat (Tools::ToolPanelStyle::segmentButton (this, tr ("Flat")));
      QPushButton* radio_linear (Tools::ToolPanelStyle::segmentButton (this, tr ("Linear")));
      QPushButton* radio_smooth (Tools::ToolPanelStyle::segmentButton (this, tr ("Smooth")));
      QPushButton* radio_origin (Tools::ToolPanelStyle::segmentButton (this, tr ("Origin")));

      _type_button_box->addButton (radio_flat, (int)eFlattenType_Flat);
      _type_button_box->addButton (radio_linear, (int)eFlattenType_Linear);
      _type_button_box->addButton (radio_smooth, (int)eFlattenType_Smooth);
      _type_button_box->addButton (radio_origin, (int)eFlattenType_Origin);

      radio_linear->toggle();

      auto* const flatten_type_group
        (Tools::ToolPanelStyle::segmentedSection (layout, tr ("Type")));
      auto* const flatten_type_layout
        (Tools::ToolPanelStyle::segmentGrid (flatten_type_group));
      flatten_type_layout->addWidget (radio_flat, 0, 0);
      flatten_type_layout->addWidget (radio_linear, 0, 1);
      flatten_type_layout->addWidget (radio_smooth, 1, 0);
      flatten_type_layout->addWidget (radio_origin, 1, 1);
      flatten_type_layout->setColumnStretch (0, 1);
      flatten_type_layout->setColumnStretch (1, 1);

      auto* const settings_group
        (Tools::ToolPanelStyle::toolSection (layout, tr ("Settings")));
      auto settings_layout (Tools::ToolPanelStyle::sectionColumn (settings_group));


      _radius_slider = new Noggit::Ui::Tools::UiCommon::ExtendedSlider(this);
      _radius_slider->setPrefix("Radius:");
      _radius_slider->setRange (0, 1000);
      _radius_slider->setDecimals (2);
      _radius_slider->setValue (10.0f);

      _speed_slider = new Noggit::Ui::Tools::UiCommon::ExtendedSlider(this);
      _speed_slider->setPrefix("Speed:");
      _speed_slider->setRange (0, 10);
      _speed_slider->setSingleStep (1);
      _speed_slider->setValue(2.0f);

      _snap_m2_objects_chkbox = new QCheckBox("Snap M2 objects", this);
      _snap_m2_objects_chkbox->setChecked(true);

      _snap_wmo_objects_chkbox = new QCheckBox("Snap WMO objects", this);
      _snap_wmo_objects_chkbox->setChecked(true);

      // THE SECOND HALF OF "OBJECTS FOLLOW THE GROUND", the same box TerrainTool grew and for the
      // same reason: the two boxes above keep a prop the right distance above a surface that is
      // moving under it and say nothing about which way it points, so flattening a decorated
      // hillside leaves every prop perpendicular to the slope it used to sit on.
      //
      // Off by default and not serialised into a brush preset, so it cannot arrive switched on:
      // re-tilting overwrites a rotation somebody placed by hand.
      _follow_ground_normals_chkbox = new QCheckBox("Rotate objects to ground normal", this);
      _follow_ground_normals_chkbox->setChecked(false);
      _follow_ground_normals_chkbox->setToolTip
        (tr ("Re-align snapped objects to the slope as it changes under them.\n"
             "Uses the same alignment as the Object Editor, so it replaces the object's placed\n"
             "rotation with one derived from the ground normal. Has no effect unless at least\n"
             "one of the snap boxes above is ticked."));

      settings_layout->addWidget(_radius_slider);
      settings_layout->addWidget(_speed_slider);
      settings_layout->addWidget(_snap_m2_objects_chkbox);
      settings_layout->addWidget(_snap_wmo_objects_chkbox);
      settings_layout->addWidget(_follow_ground_normals_chkbox);

      // THE RAMP TOOL. Why it is a checkable group beside the Type picker instead of a fifth chip
      // inside it is on the declaration in FlattenTool.hpp; the geometry is on World::buildRamp.
      //
      // Added straight to the tool column rather than wrapped in a toolSection, because a
      // checkable QGroupBox IS the section header here -- the same shape "Angled mode" and "Lock
      // mode" already take below, and it buys the affordance that matters: Qt disables every
      // child of an unchecked checkable group, so the width and falloff controls grey out
      // whenever the ramp is not the thing the mouse buttons are doing.
      _ramp_group = new QGroupBox("Ramp mode", this);
      _ramp_group->setCheckable(true);
      _ramp_group->setChecked(false);
      _ramp_group->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
      _ramp_group->setToolTip
        (tr ("Grade a constant slope between two points instead of flattening under the brush.\n"
             "While this is on, shift + left and shift + right set the two ends and shift +\n"
             "right also builds. Ctrl + left still blurs, with the falloff type chosen above."));

      auto* const ramp_layout (new QVBoxLayout (_ramp_group));
      Tools::ToolPanelStyle::dressSectionLayout (ramp_layout);

      ramp_layout->addWidget
        ( Tools::ToolPanelStyle::keybindRow
            (_ramp_group, FontNoggit::shift, FontNoggit::lmb, tr ("Set ramp start"))
        );

      ramp_layout->addWidget
        ( Tools::ToolPanelStyle::keybindRow
            (_ramp_group, FontNoggit::shift, FontNoggit::rmb, tr ("Set ramp end and build"))
        );

      _ramp_width_slider = new Noggit::Ui::Tools::UiCommon::ExtendedSlider(_ramp_group);
      _ramp_width_slider->setPrefix("Width:");
      _ramp_width_slider->setRange (1, 200);
      _ramp_width_slider->setDecimals (2);
      _ramp_width_slider->setValue (12.0f);

      _ramp_falloff_slider = new Noggit::Ui::Tools::UiCommon::ExtendedSlider(_ramp_group);
      _ramp_falloff_slider->setPrefix("Falloff width:");
      _ramp_falloff_slider->setRange (0, 200);
      _ramp_falloff_slider->setDecimals (2);
      _ramp_falloff_slider->setValue (8.0f);

      ramp_layout->addWidget(_ramp_width_slider);
      ramp_layout->addWidget(_ramp_falloff_slider);

      // The falloff curve, as the same segmented control the Type picker above uses, because it
      // is the same kind of choice. segmentedSection takes the column it is appended to and
      // parents the new group to that column's widget, so passing ramp_layout nests it inside the
      // ramp group and it inherits the enable/disable with everything else in there.
      _ramp_curve_button_box = new QButtonGroup (this);

      QPushButton* ramp_curve_linear
        (Tools::ToolPanelStyle::segmentButton (_ramp_group, tr ("Linear")));
      QPushButton* ramp_curve_smooth
        (Tools::ToolPanelStyle::segmentButton (_ramp_group, tr ("Smooth")));

      _ramp_curve_button_box->addButton (ramp_curve_linear, RAMP_CURVE_LINEAR);
      _ramp_curve_button_box->addButton (ramp_curve_smooth, RAMP_CURVE_SMOOTH);

      // Smooth is the default because the linear band leaves a slope discontinuity where it meets
      // the untouched terrain, and a visible fold down both sides of a road is the exact seam
      // this tool exists to get rid of. See rampFalloff in World.cpp.
      ramp_curve_smooth->toggle();

      auto* const ramp_curve_group
        (Tools::ToolPanelStyle::segmentedSection (ramp_layout, tr ("Falloff curve")));
      auto* const ramp_curve_layout
        (Tools::ToolPanelStyle::segmentGrid (ramp_curve_group));
      ramp_curve_layout->addWidget (ramp_curve_linear, 0, 0);
      ramp_curve_layout->addWidget (ramp_curve_smooth, 0, 1);
      ramp_curve_layout->setColumnStretch (0, 1);
      ramp_curve_layout->setColumnStretch (1, 1);

      // The read-out is the only place the run and the grade are ever stated, and the grade is
      // the number the tool exists to hold constant, so it is worth the two lines it costs.
      _ramp_readout = new QLabel(_ramp_group);
      _ramp_readout->setWordWrap(true);
      _ramp_readout->setTextInteractionFlags(Qt::TextSelectableByMouse);
      ramp_layout->addWidget(_ramp_readout);

      _ramp_build_button = new QPushButton(tr ("Build ramp"), _ramp_group);
      _ramp_build_button->setAutoDefault(false);
      _ramp_build_button->setToolTip
        (tr ("Grade the ramp again with the current width and falloff.\n"
             "Each build blends from the terrain as it is NOW, so building twice over the same\n"
             "ramp pulls the falloff band further toward the ramp plane a second time. Undo the\n"
             "first build before rebuilding if that is not what you want."));

      _ramp_clear_button = new QPushButton(tr ("Clear points"), _ramp_group);
      _ramp_clear_button->setAutoDefault(false);

      auto* const ramp_button_layout (new QHBoxLayout);
      ramp_button_layout->setContentsMargins (0, 0, 0, 0);
      ramp_button_layout->setSpacing (Design::S1);
      ramp_button_layout->addWidget(_ramp_build_button);
      ramp_button_layout->addWidget(_ramp_clear_button);
      ramp_layout->addLayout(ramp_button_layout);

      layout->addWidget(_ramp_group);

      auto* const flatten_blur_group
        (Tools::ToolPanelStyle::toolSection (layout, tr ("Flatten/Blur")));
      auto flatten_blur_layout (new QGridLayout (flatten_blur_group));
      Tools::ToolPanelStyle::dressSectionLayout (flatten_blur_layout);

      flatten_blur_layout->addWidget(_lock_up_checkbox = new QCheckBox(this), 0, 0);
      flatten_blur_layout->addWidget(_lock_down_checkbox = new QCheckBox(this), 0, 1);

      _lock_up_checkbox->setChecked(_flatten_mode.raise);
      _lock_up_checkbox->setText("Raise");
      _lock_up_checkbox->setToolTip("Raise the terrain when using the tool");
      _lock_down_checkbox->setChecked(_flatten_mode.lower);
      _lock_down_checkbox->setText("Lower");
      _lock_down_checkbox->setToolTip("Lower the terrain when using the tool");

      QSettings settings;
      bool use_classic_ui = settings.value("classicUI", true).toBool();
      if (use_classic_ui)
          flatten_blur_group->show();
      else
          flatten_blur_group->hide();

      auto* const flatten_only_group
        (Tools::ToolPanelStyle::toolSection (layout, tr ("Flatten only")));
      auto flatten_only_layout (Tools::ToolPanelStyle::sectionColumn (flatten_only_group));

      _angle_group = new QGroupBox("Angled mode", this);
      _angle_group->setCheckable(true);
      _angle_group->setChecked(false);
      _angle_group->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);

      QGridLayout* angle_layout(new QGridLayout(_angle_group));

      angle_layout->addWidget(_orientation_dial = new QDial(this), 0, 0);
      _orientation_dial->setRange(0, 360);
      _orientation_dial->setWrapping(true);
      _orientation_dial->setSliderPosition(_orientation - 90); // to get ingame orientation
      _orientation_dial->setToolTip("Orientation");
      _orientation_dial->setSingleStep(10);

      _angle_slider = new QSlider(this);
      _angle_slider->setRange(0, 89);
      _angle_slider->setSliderPosition(_angle);
      _angle_slider->setToolTip("Angle");
      _angle_slider->setMinimumHeight(80);
      angle_layout->addWidget(_angle_slider, 0, 1);

      _angle_info = new QLabel(this);
      _angle_info->setText(QString::number(_angle_slider->value()));
      angle_layout->addWidget(new QLabel(tr("Angle : ")), 1, 0);
      angle_layout->addWidget(_angle_info, 1, 1);

      _orientation_info = new QLabel(this);
      _orientation_info->setText(QString::number(_orientation_dial->value()));
      angle_layout->addWidget(new QLabel(tr("Orientation : ")), 2, 0);
      angle_layout->addWidget(_orientation_info, 2, 1);
      
      flatten_only_layout->addWidget(_angle_group);

      _lock_group = new QGroupBox("Lock mode", this);
      _lock_group->setCheckable(true);
      _lock_group->setChecked(false);

      QFormLayout* lock_layout(new QFormLayout(_lock_group));

      lock_layout->addRow("X:", _lock_x = new QDoubleSpinBox(this));
      lock_layout->addRow("Z:", _lock_z = new QDoubleSpinBox(this));
      lock_layout->addRow("H:", _lock_h = new QDoubleSpinBox(this));

      _lock_x->setRange(0.0, 34133.0);
      _lock_x->setDecimals(3);
      _lock_z->setRange(0.0, 34133.0);
      _lock_z->setDecimals(3);
      _lock_h->setRange (std::numeric_limits<float>::lowest(), std::numeric_limits<float>::max());
      _lock_h->setDecimals(3);
      _lock_h->setMinimumWidth(30);

      flatten_only_layout->addWidget(_lock_group);

      connect ( _ramp_clear_button, &QPushButton::clicked
              , [this] { clearRampPoints(); }
              );

      // Build is wired by the tool that owns the action stack, not here: this widget has no
      // MapView and must not open an action of its own, or the ramp would land in a different
      // undo step depending on which of the two ways it was started. FlattenBlurTool connects it.

      connect ( _type_button_box, qOverload<int> (&QButtonGroup::idClicked)
              , [&] (int id)
                {
                  _flatten_type = id;
                }
              );

      connect(_lock_up_checkbox, &QCheckBox::stateChanged
          , [&](int state)
          {
              _flatten_mode.raise = state;
          }
      );

      connect(_lock_down_checkbox, &QCheckBox::stateChanged
          , [&](int state)
          {
              _flatten_mode.lower = state;
          }
      );

      connect ( _angle_slider, &QSlider::valueChanged
                , [&] (int v)
                  {
                    _angle = v;
                    _angle_info->setText(QString::number(_angle));
                  }
                );

      connect ( _orientation_dial, &QDial::valueChanged
                , [this] (int v)
                  {
                    setOrientation(v + 90.0f);
                    _orientation_info->setText(QString::number(v));
                  }
                );

      connect ( _lock_x, qOverload<double> (&QDoubleSpinBox::valueChanged)
                , [&] (double v)
                  {
                    _lock_pos.x = v;
                  }
                );

      connect ( _lock_h, qOverload<double> (&QDoubleSpinBox::valueChanged)
                , [&] (double v)
                  {
                    _lock_pos.y = v;
                  }
              );

      connect ( _lock_z, qOverload<double> (&QDoubleSpinBox::valueChanged)
                , [&] (double v)
                  {
                    _lock_pos.z = v;
                  }
              );

      // Last, because it reads _ramp_readout and _ramp_build_button and both have to exist. It
      // is what puts "No points set." in the label and leaves Build disabled at startup, rather
      // than an empty label and a button that does nothing when pressed.
      updateRampReadout();
    }

    void flatten_blur_tool::flatten (World* world, glm::vec3 const& cursor_pos, float dt)
    {
      // TODO : objects snapping may be optimized by reusing the flatten code for objects instead of ray intersection for ground distance
      // store the ground height diff at center of all objects hit before editing it
      std::vector<std::pair<SceneObject*, float>> objects_ground_distance = world->getObjectsGroundDistance(cursor_pos, _radius_slider->value()
          , _snap_wmo_objects_chkbox->isChecked(), _snap_m2_objects_chkbox->isChecked());


      world->flattenTerrain ( cursor_pos
                            , 1.f - pow (0.5f, dt *_speed_slider->value())
                            , _radius_slider->value()
                            , _flatten_type
                            , _flatten_mode
                            , use_ref_pos() ? _lock_pos : cursor_pos
                            , math::degrees (angled_mode() ? _angle : 0.0f)
                            , math::degrees (angled_mode() ? _orientation : 0.0f)
                            );

      // re apply the ground height diff to the objects, and re-tilt them if asked
      world->reseatObjectsOnGround(objects_ground_distance, _follow_ground_normals_chkbox->isChecked());
    }

    void flatten_blur_tool::blur (World* world, glm::vec3 const& cursor_pos, float dt)
    {
      // store the ground height diff at center of all objects hit before editing it
      std::vector<std::pair<SceneObject*, float>> objects_ground_distance = world->getObjectsGroundDistance(cursor_pos, _radius_slider->value()
          , _snap_wmo_objects_chkbox->isChecked(), _snap_m2_objects_chkbox->isChecked());


      world->blurTerrain ( cursor_pos
                         , 1.f - pow (0.5f, dt * _speed_slider->value())
                         , _radius_slider->value()
                         , _flatten_type
                         , _flatten_mode
                         );

      // re apply the ground height diff to the objects, and re-tilt them if asked
      world->reseatObjectsOnGround(objects_ground_distance, _follow_ground_normals_chkbox->isChecked());
    }

    bool flatten_blur_tool::rampMode() const
    {
      return _ramp_group->isChecked();
    }

    void flatten_blur_tool::setRampStart (glm::vec3 const& pos)
    {
      _ramp_start = pos;
      _has_ramp_start = true;
      updateRampReadout();
    }

    void flatten_blur_tool::setRampEnd (glm::vec3 const& pos)
    {
      _ramp_end = pos;
      _has_ramp_end = true;
      updateRampReadout();
    }

    void flatten_blur_tool::clearRampPoints()
    {
      _has_ramp_start = false;
      _has_ramp_end = false;
      updateRampReadout();
    }

    bool flatten_blur_tool::hasRampStart() const
    {
      return _has_ramp_start;
    }

    bool flatten_blur_tool::hasRampEnd() const
    {
      return _has_ramp_end;
    }

    glm::vec3 flatten_blur_tool::rampStart() const
    {
      return _ramp_start;
    }

    glm::vec3 flatten_blur_tool::rampEnd() const
    {
      return _ramp_end;
    }

    float flatten_blur_tool::rampHalfWidth() const
    {
      return _ramp_width_slider->value() * 0.5f;
    }

    float flatten_blur_tool::rampFalloffWidth() const
    {
      return _ramp_falloff_slider->value();
    }

    bool flatten_blur_tool::buildRamp (World* world)
    {
      if (!_has_ramp_start || !_has_ramp_end)
      {
        return false;
      }

      float const width (_ramp_width_slider->value());
      float const falloff (_ramp_falloff_slider->value());

      float const dx (_ramp_end.x - _ramp_start.x);
      float const dz (_ramp_end.z - _ramp_start.z);
      float const run (std::sqrt (dx * dx + dz * dz));

      // The objects are measured before the terrain moves and put back after, which is the same
      // two-step the flatten and blur strokes above use and the reason getObjectsGroundDistance
      // exists. The radius is the ramp's own bounding CIRCLE -- half the run, plus the half width,
      // plus the falloff band -- because a circle is all getObjectsGroundDistance can search.
      std::vector<std::pair<SceneObject*, float>> objects_ground_distance
        ( world->getObjectsGroundDistance
            ( (_ramp_start + _ramp_end) * 0.5f
            , run * 0.5f + width * 0.5f + falloff
            , _snap_wmo_objects_chkbox->isChecked()
            , _snap_m2_objects_chkbox->isChecked()
            )
        );

      // AND THEN NARROWED TO THE RAMP ITSELF, because a long thin ramp is nothing like its own
      // bounding circle: a 200 yard run at the default 12 yard width and 8 yard band has a search
      // radius of 100 + 6 + 8 = 114 yards, so it hands back every object within 114 yards of the
      // midpoint to grade a strip 28 yards across. Re-SEATING those is harmless -- the
      // ground under them has not moved, so the height they are given back is the height they
      // had -- but re-TILTING them is not, and "rotate objects to ground normal" would spin every
      // prop on the hillside to a slope the ramp never graded under it.
      //
      // World::projectOntoRamp is the same footprint the grading itself uses, called per object
      // instead of per vertex, so "inside the ramp" cannot come to mean two different things.
      // The kept condition mirrors rampFalloff exactly: inside the core, or inside the band.
      objects_ground_distance.erase
        ( std::remove_if
            ( objects_ground_distance.begin()
            , objects_ground_distance.end()
            , [&] (std::pair<SceneObject*, float> const& entry)
              {
                float const distance
                  ( World::projectOntoRamp (_ramp_start, _ramp_end, width, entry.first->pos)
                      .distance_to_core
                  );

                return distance > 0.0f && (falloff <= 0.0f || distance >= falloff);
              }
            )
        , objects_ground_distance.end()
        );

      bool const built
        ( world->buildRamp
            ( _ramp_start
            , _ramp_end
            , width
            , falloff
            , _ramp_curve_button_box->checkedId() == RAMP_CURVE_SMOOTH
            )
        );

      if (built)
      {
        world->reseatObjectsOnGround
          (objects_ground_distance, _follow_ground_normals_chkbox->isChecked());
      }

      return built;
    }

    QPushButton* flatten_blur_tool::rampBuildButton()
    {
      return _ramp_build_button;
    }

    void flatten_blur_tool::updateRampReadout()
    {
      _ramp_build_button->setEnabled(_has_ramp_start && _has_ramp_end);

      if (!_has_ramp_start && !_has_ramp_end)
      {
        _ramp_readout->setText(tr ("No points set."));
        return;
      }

      if (!_has_ramp_start || !_has_ramp_end)
      {
        glm::vec3 const& point (_has_ramp_start ? _ramp_start : _ramp_end);

        _ramp_readout->setText
          ( tr ("%1 at %2, %3, %4. Set the other end to build.")
              .arg (_has_ramp_start ? tr ("Start") : tr ("End"))
              .arg (point.x, 0, 'f', 2)
              .arg (point.y, 0, 'f', 2)
              .arg (point.z, 0, 'f', 2)
          );
        return;
      }

      float const dx (_ramp_end.x - _ramp_start.x);
      float const dz (_ramp_end.z - _ramp_start.z);
      float const run (std::sqrt (dx * dx + dz * dz));
      float const rise (_ramp_end.y - _ramp_start.y);

      // The same thousandth of a unit World::buildRamp refuses, stated here so the user finds
      // out before pressing Build rather than by nothing happening.
      if (run < 0.001f)
      {
        _ramp_readout->setText(tr ("The two ends are on top of each other."));
        _ramp_build_button->setEnabled(false);
        return;
      }

      float const grade (rise / run);

      // Percentage and angle both, because a grade is quoted both ways and neither reads as the
      // other at a glance. The percent sign is concatenated rather than left in the format
      // string, so QString::arg can never mistake it for a placeholder.
      _ramp_readout->setText
        ( tr ("Run %1 yd, rise %2 yd. Grade %3 (%4 deg).")
            .arg (run, 0, 'f', 2)
            .arg (rise, 0, 'f', 2)
            .arg (QString::number (grade * 100.0f, 'f', 1) + QLatin1String ("%"))
            .arg (math::degrees (math::radians (std::atan (grade)))._, 0, 'f', 1)
        );
    }

    void flatten_blur_tool::nextFlattenMode()
    {
        _flatten_mode.next();

        QSignalBlocker const up_lock(_lock_up_checkbox);
        QSignalBlocker const down_lock(_lock_down_checkbox);
        _lock_up_checkbox->setChecked(_flatten_mode.raise);
        _lock_down_checkbox->setChecked(_flatten_mode.lower);
    }

    void flatten_blur_tool::nextFlattenType()
    {
      _flatten_type = ( ++_flatten_type ) % eFlattenType_Count;
      _type_button_box->button (_flatten_type)->toggle();
    }

    void flatten_blur_tool::toggleFlattenAngle()
    {
      _angle_group->setChecked(!angled_mode());
    }

    void flatten_blur_tool::toggleFlattenLock()
    {
      _lock_group->setChecked(!use_ref_pos());
    }

    void flatten_blur_tool::lockPos (glm::vec3 const& cursor_pos)
    {
      _lock_pos = cursor_pos;
      _lock_x->setValue (_lock_pos.x);
      _lock_h->setValue (_lock_pos.y);
      _lock_z->setValue (_lock_pos.z);

      if (!use_ref_pos())
      {
        toggleFlattenLock();
      }
    }

    void flatten_blur_tool::changeRadius(float change)
    {
      _radius_slider->setValue (_radius_slider->value() + change);
    }

    void flatten_blur_tool::changeSpeed(float change)
    {
      _speed_slider->setValue(_speed_slider->value() + change);
    }

    void flatten_blur_tool::setSpeed(float speed)
    {
      _speed_slider->setValue(speed);
    }

    void flatten_blur_tool::changeOrientation(float change)
    {
      setOrientation(_orientation + change);
    }

    void flatten_blur_tool::setOrientation (float orientation)
    {
      QSignalBlocker const blocker (_orientation_dial);

      _orientation = orientation;
      while (_orientation >= 360.0f)
      {
        _orientation -= 360.0f;
      }
      while (_orientation < 0.0f)
      {
        _orientation += 360.0f;
      }
      _orientation_dial->setSliderPosition(_orientation - 90.0f);
      _orientation_info->setText(QString::number(_orientation_dial->value()));
    }

    float flatten_blur_tool::brushRadius() const
    {
      return _radius_slider->value();
    }

    float flatten_blur_tool::angle() const
    {
      return _angle;
    }

    float flatten_blur_tool::orientation() const
    {
      return _orientation;
    }

    bool flatten_blur_tool::angled_mode() const
    {
      return _angle_group->isChecked();
    }

    bool flatten_blur_tool::use_ref_pos() const
    {
      return _lock_group->isChecked();
    }

    glm::vec3 flatten_blur_tool::ref_pos() const
    {
      return _lock_pos;
    }

    Noggit::Ui::Tools::UiCommon::ExtendedSlider* flatten_blur_tool::getRadiusSlider()
    {
      return _radius_slider;
    }

    Noggit::Ui::Tools::UiCommon::ExtendedSlider* flatten_blur_tool::getSpeedSlider()
    {
      return _speed_slider;
    }

    void flatten_blur_tool::changeAngle(float change)
    {
      _angle = std::min(89.0f, std::max(0.0f, _angle + change));
      _angle_slider->setSliderPosition(_angle);
    }

    void flatten_blur_tool::changeHeight(float change)
    {
      _lock_h->setValue(_lock_pos.y + change);
    }

    void flatten_blur_tool::setRadius(float radius)
    {
      _radius_slider->setValue(radius);
    }

    QSize flatten_blur_tool::sizeHint() const
    {
      return QSize(250, height());
    }

    QJsonObject flatten_blur_tool::toJSON()
    {
      QJsonObject json;

      json["brush_action_type"] = "FLATTEN_BLUR";

      json["speed"] = _speed_slider->rawValue();
      json["radius"] = _radius_slider->rawValue();

      int flag = 0;
      flag |= _flatten_mode.raise ? 0x1 : 0x0;
      flag |= _flatten_mode.lower ? 0x2 : 0x0;

      json["flatten_mode"] = flag;
      json["flatten_type"] = _flatten_type;
      json["angle"] = _angle_slider->value();
      json["use_angle"] = _angle_group->isChecked();
      json["orientation"] = _orientation_dial->value();
      json["use_angle"] = _angle_group->isChecked();
      json["use_lock"] = _lock_group->isChecked();
      json["lock_x"] = _lock_x->value();
      json["lock_z"] = _lock_z->value();
      json["lock_h"] = _lock_h->value();

      // The ramp's two points are deliberately absent: they are a transient aim, not a setting,
      // and a preset that restored somebody else's endpoints would build a ramp across terrain
      // that has nothing to do with them. Its shape IS a setting, so that travels.
      json["ramp_mode"] = _ramp_group->isChecked();
      json["ramp_width"] = _ramp_width_slider->rawValue();
      json["ramp_falloff_width"] = _ramp_falloff_slider->rawValue();
      json["ramp_curve"] = _ramp_curve_button_box->checkedId();

      return json;
    }

    void flatten_blur_tool::fromJSON(const QJsonObject& json)
    {
      _speed_slider->setValue(json["speed"].toDouble());
      _radius_slider->setValue(json["radius"].toDouble());

      int flag = json["flatten_mode"].toInt();
      _flatten_mode.raise = flag & 0x1;
      _flatten_mode.lower = flag & 0x2;

      _flatten_type = json["flatten_type"].toInt();
      _angle_slider->setValue(json["angle"].toDouble());
      _angle_group->setChecked(json["use_angle"].toBool());
      _orientation_dial->setValue(json["orientation"].toDouble());
      _angle_group->setChecked(json["use_angle"].toBool());
      _lock_group->setChecked(json["use_lock"].toBool());
      _lock_x->setValue(json["lock_x"].toDouble());
      _lock_z->setValue(json["lock_z"].toDouble());
      _lock_h->setValue(json["lock_h"].toDouble());

      // Guarded with contains(), because every preset written before the ramp existed has none of
      // these keys and QJsonValue::toInt on a missing key returns 0 -- which is a valid button id
      // here (RAMP_CURVE_LINEAR) and would silently flip the falloff curve of an old preset.
      if (json.contains("ramp_mode"))
      {
        _ramp_group->setChecked(json["ramp_mode"].toBool());
        _ramp_width_slider->setValue(json["ramp_width"].toDouble());
        _ramp_falloff_slider->setValue(json["ramp_falloff_width"].toDouble());

        if (auto* const curve = _ramp_curve_button_box->button(json["ramp_curve"].toInt()))
        {
          curve->setChecked(true);
        }
      }
    }
  }
}
