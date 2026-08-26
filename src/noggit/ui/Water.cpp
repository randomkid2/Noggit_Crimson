// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/DBC.h>
#include <noggit/MapHeaders.h>
#include <noggit/ui/Checkbox.hpp>
#include <noggit/ui/pushbutton.hpp>
#include <noggit/ui/tools/ToolPanel/ToolWidgetStyle.hpp>
#include <noggit/ui/tools/UiCommon/ExtendedSlider.hpp>
#include <noggit/ui/Water.h>
#include <noggit/unsigned_int_property.hpp>
#include <noggit/World.h>

#include <QtWidgets/QButtonGroup>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QLabel>
#include <QtWidgets/QRadioButton>

#include <cmath>

namespace Noggit
{
  namespace Ui
  {
    water::water ( unsigned_int_property* current_layer
                 , BoolToggleProperty* display_all_layers
                 , QWidget* parent
                 )
      : QWidget (parent)
      , _liquid_id(5)
      , _liquid_type(liquid_basic_types_water)
      , _radius(10.0f)
      , _angle(10.0f)
      , _orientation(0.0f)
      , _locked(false)
      , _angled_mode(false)
      , _override_liquid_id(true)
      , _override_height(true)
      , _brush_mode(water_brush_paint)
      , _falloff_type(eFlattenType_Linear)
      , _flatten_raise(true)
      , _flatten_lower(true)
      , _weld_seams(true)
      , _opacity_mode(auto_opacity)
      , _custom_opacity_factor(RIVER_OPACITY_VALUE)
      , _lock_pos(glm::vec3(0.0f, 0.0f, 0.0f))
      , tile(0, 0)
    {
      // The dock's shared shell -- zero margins, S3 down the column, 250px floor. This layout
      // set no margins, so it took QStyle::PM_LayoutLeftMargin (13px on windowsvista here) on
      // top of ToolPanel's own 12px, and the six group boxes below each added a second inset
      // inside the padding the theme already gives a QGroupBox. See ToolWidgetStyle.hpp.
      auto layout (Tools::ToolPanelStyle::toolForm (this));

      auto brush_group(new QGroupBox("Brush", this));
      auto brush_layout (Tools::ToolPanelStyle::sectionForm (brush_group));

      _radius_spin = new QDoubleSpinBox (this);
      _radius_spin->setRange (0.f, 1000.f);
      connect ( _radius_spin, qOverload<double> (&QDoubleSpinBox::valueChanged)
              , [&] (float f) { _radius = f; }
              );
      _radius_spin->setValue(_radius);
      brush_layout->addRow ("Radius", _radius_spin);

      waterType = new QComboBox(this);

      for (DBCFile::Iterator i = gLiquidTypeDB.begin(); i != gLiquidTypeDB.end(); ++i)
      {
        int liquid_id = i->getInt(LiquidTypeDB::ID);

        // filter WMO liquids
        if (liquid_id == LIQUID_WMO_Water || liquid_id == LIQUID_WMO_Ocean || liquid_id == LIQUID_WMO_Water_Interior
            || liquid_id == LIQUID_WMO_Magma || liquid_id == LIQUID_WMO_Slime)
            continue;

        std::stringstream ss;
        ss << liquid_id << "-" << LiquidTypeDB::getLiquidName(liquid_id);
        waterType->addItem (QString::fromUtf8(ss.str().c_str()), QVariant (liquid_id));

      }

      connect (waterType, qOverload<int> (&QComboBox::currentIndexChanged)
              , [&]
                {
                  changeWaterType(waterType->currentData().toInt());

                  // change auto opacity based on liquid type
                  if (_opacity_mode == custom_opacity || _opacity_mode == auto_opacity)
                      return;

                  // other liquid types shouldn't use opacity(depth)
                  int liquid_type = LiquidTypeDB::getLiquidType(_liquid_id);
                  if (liquid_type == liquid_basic_types_ocean) // ocean
                  {
                      ocean_button->setChecked(true);
                      _opacity_mode = ocean_opacity;
                  }
                  else // water. opacity doesn't matter for lava/slim
                  {
                      river_button->setChecked(true);
                      _opacity_mode = river_opacity;
                  }

                }
              );

      brush_layout->addRow (waterType);

      layout->addRow (brush_group);

      // ---- vertex height ------------------------------------------------------------------
      //
      // The liquid height field has always been one float per vertex, 81 per chunk-layer, but
      // every write before this group was an absolute assignment through an inclined plane:
      // there was no way to nudge a height, no falloff at all (the radius test was a plain
      // in/out) and nothing that read a neighbouring height. That is why shaping a slope meant
      // stepping between flat plateaus. These controls drive the same brushes the terrain tools
      // use, on the liquid grid.
      auto height_group (new QGroupBox ("Vertex height", this));
      auto height_layout (Tools::ToolPanelStyle::sectionForm (height_group));

      _brush_mode_combo = new QComboBox (this);
      _brush_mode_combo->addItem ("Paint water", QVariant (water_brush_paint));
      _brush_mode_combo->addItem ("Raise / lower", QVariant (water_brush_raise_lower));
      _brush_mode_combo->addItem ("Flatten", QVariant (water_brush_flatten));
      _brush_mode_combo->addItem ("Smooth", QVariant (water_brush_smooth));
      _brush_mode_combo->setCurrentIndex (_brush_mode);
      _brush_mode_combo->setToolTip
        ( "What Shift+LMB and Ctrl+LMB do.\n"
          "Paint water: Shift adds liquid, Ctrl removes it. The original behaviour.\n"
          "Raise / lower: Shift raises the liquid vertices under the brush, Ctrl lowers them.\n"
          "Flatten: Shift pulls them towards the target plane, Ctrl smooths.\n"
          "Smooth: both average each vertex against its neighbours.\n"
          "The three height modes never add or remove liquid.\n"
          "Flatten aims at the Angled mode plane, anchored on the Lock position when Lock is\n"
          "on and on the liquid surface under the cursor when it is off."
        );

      // Read the item's data rather than its row, so that reordering or inserting an entry
      // cannot silently repoint a mode at the wrong brush.
      connect ( _brush_mode_combo, qOverload<int> (&QComboBox::currentIndexChanged)
              , [this] { _brush_mode = _brush_mode_combo->currentData().toInt(); }
              );

      height_layout->addRow ("Mode", _brush_mode_combo);

      _falloff_combo = new QComboBox (this);
      _falloff_combo->addItem ("Flat", QVariant (eFlattenType_Flat));
      _falloff_combo->addItem ("Linear", QVariant (eFlattenType_Linear));
      _falloff_combo->addItem ("Smooth", QVariant (eFlattenType_Smooth));
      _falloff_combo->setCurrentIndex (_falloff_type);
      _falloff_combo->setToolTip
        ( "How the brush strength falls off towards the rim, using the same curves as the\n"
          "terrain tools. Flat applies the full strength everywhere inside the radius, which\n"
          "is what the water tool did before it had a falloff at all."
        );

      connect ( _falloff_combo, qOverload<int> (&QComboBox::currentIndexChanged)
              , [this] { _falloff_type = _falloff_combo->currentData().toInt(); }
              );

      height_layout->addRow ("Falloff", _falloff_combo);

      // Range 0-10 and start 2, which is flatten_blur_tool's Speed slider verbatim
      // (FlattenTool.cpp:112-114). It has to be that one and not TerrainTool's, whose range is
      // 0-1000, because this single slider feeds both the raise/lower rate and the exponent of
      // the flatten and smooth blend, and an exponent of 1000 would snap in one tick.
      _strength_slider = new Noggit::Ui::Tools::UiCommon::ExtendedSlider (this);
      _strength_slider->setPrefix ("Strength:");
      _strength_slider->setRange (0, 10);
      _strength_slider->setSingleStep (1);
      _strength_slider->setValue (2.0f);
      height_layout->addRow (_strength_slider);

      _inner_radius_slider = new Noggit::Ui::Tools::UiCommon::ExtendedSlider (this);
      _inner_radius_slider->setPrefix ("Inner radius:");
      _inner_radius_slider->setRange (0.0, 1.0);
      _inner_radius_slider->setDecimals (2);
      _inner_radius_slider->setSingleStep (0.05f);
      _inner_radius_slider->setValue (0);
      _inner_radius_slider->setToolTip
        ( "How much strength survives at the rim of the brush. The Linear curve in\n"
          "MapChunk::changeTerrainProcessVertex is 1 - dist * (1 - inner) / radius, so 0 falls\n"
          "away to nothing at the rim and 1 removes the falloff entirely. That curve is the\n"
          "only one that consults it, so it is inert on Flat and Smooth -- the same as in the\n"
          "terrain tools."
        );
      height_layout->addRow (_inner_radius_slider);

      auto* const raise_check (new CheckBox ("Raise", &_flatten_raise, this));
      raise_check->setToolTip ("Let Flatten and Smooth move a vertex upwards.");
      height_layout->addRow (raise_check);

      auto* const lower_check (new CheckBox ("Lower", &_flatten_lower, this));
      lower_check->setToolTip ("Let Flatten and Smooth move a vertex downwards.");
      height_layout->addRow (lower_check);

      auto* const weld_check (new CheckBox ("Weld chunk seams", &_weld_seams, this));
      weld_check->setToolTip
        ( "A liquid vertex on a chunk border is stored once per chunk, twice on an edge and up\n"
          "to four times at a tile corner, and a map can arrive with those copies already\n"
          "disagreeing -- which is what a tear at a water seam is. This averages every copy\n"
          "the brush touched after each stroke, so the seam closes instead of merely not\n"
          "getting worse."
        );
      height_layout->addRow (weld_check);

      layout->addRow (height_group);

      auto angle_group (new QGroupBox ("Angled mode", this));
      angle_group->setCheckable (true);
      angle_group->setChecked (_angled_mode.get());
      
      
      connect ( &_angled_mode, &BoolToggleProperty::changed
              , angle_group, &QGroupBox::setChecked
              );
      connect ( angle_group, &QGroupBox::toggled
              , &_angled_mode, &BoolToggleProperty::set
              );
      auto angle_layout (Tools::ToolPanelStyle::sectionForm (angle_group));

      _angle_spin = new QDoubleSpinBox (this);
      _angle_spin->setRange (0.00001f, 89.f);
      _angle_spin->setSingleStep (2.0f);
      connect ( _angle_spin, qOverload<double> (&QDoubleSpinBox::valueChanged)
              , [&] (float f) { _angle = f; }
              );
      _angle_spin->setValue(_angle);
      angle_layout->addRow ("Angle", _angle_spin);

      _orientation_spin = new QDoubleSpinBox (this);
      _orientation_spin->setRange (0.f, 360.f);
      _orientation_spin->setWrapping (true);
      _orientation_spin->setValue(_orientation);
      _orientation_spin->setSingleStep (5.0f);
      connect ( _orientation_spin, qOverload<double> (&QDoubleSpinBox::valueChanged)
              , [&] (float f) { _orientation = f; }
              );

      angle_layout->addRow ("Orienation", _orientation_spin);

      layout->addRow (angle_group);

      auto lock_group (new QGroupBox ("Lock", this));
      lock_group->setCheckable (true);
      lock_group->setChecked (_locked.get());
      auto lock_layout (Tools::ToolPanelStyle::sectionForm (lock_group));

      lock_layout->addRow("X:", _x_spin = new QDoubleSpinBox (this));
      lock_layout->addRow("Z:", _z_spin = new QDoubleSpinBox (this));
      lock_layout->addRow("H:", _h_spin = new QDoubleSpinBox (this));

      _x_spin->setRange (std::numeric_limits<float>::lowest(), std::numeric_limits<float>::max());
      _z_spin->setRange (std::numeric_limits<float>::lowest(), std::numeric_limits<float>::max());
      _h_spin->setRange (std::numeric_limits<float>::lowest(), std::numeric_limits<float>::max());
      _x_spin->setDecimals (2);
      _z_spin->setDecimals (2);
      _h_spin->setDecimals (2);

      connect ( _x_spin, qOverload<double> (&QDoubleSpinBox::valueChanged)
              , [&] (float f) { _lock_pos.x = f; }
              );
      connect ( _z_spin, qOverload<double> (&QDoubleSpinBox::valueChanged)
              , [&] (float f) { _lock_pos.z = f; }
              );
      connect ( _h_spin, qOverload<double> (&QDoubleSpinBox::valueChanged)
              , [&] (float f) { _lock_pos.y = f; }
              );

      connect ( &_locked, &BoolToggleProperty::changed
              , lock_group, &QGroupBox::setChecked
              );
      connect ( lock_group, &QGroupBox::toggled
              , &_locked, &BoolToggleProperty::set
              );

      layout->addRow(lock_group);

      auto override_group (new QGroupBox ("Override", this));
      auto override_layout (Tools::ToolPanelStyle::sectionForm (override_group));

      override_layout->addWidget (new CheckBox ("Liquid ID", &_override_liquid_id, this));
      override_layout->addWidget (new CheckBox ("Height", &_override_height, this));

      layout->addRow(override_group);

      auto opacity_group (new QGroupBox ("Auto opacity", this));
      auto opacity_layout (Tools::ToolPanelStyle::sectionForm (opacity_group));

      auto auto_button(new QRadioButton("Auto", this));
      auto_button->setToolTip("Automatically uses river or ocean opacity based on liquid type.");
      river_button = new QRadioButton ("River", this);
      river_button->setToolTip(std::to_string(RIVER_OPACITY_VALUE).c_str());
      ocean_button = new QRadioButton ("Ocean", this);
      ocean_button->setToolTip(std::to_string(OCEAN_OPACITY_VALUE).c_str());
      custom_button = new QRadioButton ("Custom factor:", this);

      transparency_toggle = new QButtonGroup (this);
      transparency_toggle->addButton(auto_button, auto_opacity);
      transparency_toggle->addButton (river_button, river_opacity);
      transparency_toggle->addButton (ocean_button, ocean_opacity);
      transparency_toggle->addButton (custom_button, custom_opacity);

      connect ( transparency_toggle, qOverload<int> (&QButtonGroup::idClicked)
              , [&] (int id) { _opacity_mode = id; }
              );

      opacity_layout->addRow(auto_button);
      opacity_layout->addRow (river_button);
      opacity_layout->addRow (ocean_button);
      opacity_layout->addRow (custom_button);

      transparency_toggle->button (_opacity_mode)->setChecked (true);

      QDoubleSpinBox *opacity_spin = new QDoubleSpinBox (this);
      opacity_spin->setRange (0.f, 1.f);
      opacity_spin->setDecimals (4);
      opacity_spin->setSingleStep (0.02f);
      opacity_spin->setValue(_custom_opacity_factor);
      connect ( opacity_spin, qOverload<double> (&QDoubleSpinBox::valueChanged)
              , [&] (float f) { _custom_opacity_factor = f; }
              );
      opacity_layout->addRow (opacity_spin);

      layout->addRow (opacity_group);

      layout->addRow ( new pushbutton
                            ( "Regen ADT opacity"
                            , [this]
                              {
                                emit regenerate_water_opacity
                                  (get_opacity_factor());
                              }
                            )
                        );
      layout->addRow ( new pushbutton
                            ( "Crop water"
                            , [this]
                              {
                                emit crop_water();
                              }
                            )
                        );

      auto layer_group (new QGroupBox ("Layers", this));
      auto layer_layout (Tools::ToolPanelStyle::sectionForm (layer_group));

      layer_layout->addRow (new CheckBox("Show all layers", display_all_layers));
      layer_layout->addRow (new QLabel("Current layer:", this));

      waterLayer = new QSpinBox (this);
      waterLayer->setValue (current_layer->get());
      waterLayer->setRange (0, 100);
      layer_layout->addRow (waterLayer);

      layout->addRow (layer_group);

      connect ( waterLayer, qOverload<int> (&QSpinBox::valueChanged)
              , current_layer, &unsigned_int_property::set
              );
      connect ( current_layer, &unsigned_int_property::changed
              , waterLayer, &QSpinBox::setValue
              );

      updateData();

    }

    void water::updatePos(TileIndex const& newTile)
    {
      if (newTile == tile) return;

      tile = newTile;

      updateData();
    }

    void water::updateData()
    {
      std::stringstream mt;
      mt << _liquid_id << " - " << LiquidTypeDB::getLiquidName(_liquid_id);
      waterType->setCurrentText (QString::fromStdString (mt.str()));
      _liquid_type = static_cast<liquid_basic_types>(LiquidTypeDB::getLiquidType(_liquid_id));
    }

    void water::changeWaterType(int waterint)
    {
      _liquid_id = waterint;

      updateData();
    }

    void water::changeRadius(float change)
    {
      _radius_spin->setValue(_radius + change);
    }

    void water::setRadius(float radius)
    {
      _radius_spin->setValue(radius);
    }

    void water::changeOrientation(float change)
    {
      _orientation += change;

      while (_orientation >= 360.0f)
      {
        _orientation -= 360.0f;
      }
      while (_orientation < 0.0f)
      {
        _orientation += 360.0f;
      }

      _orientation_spin->setValue(_orientation);
    }

    void water::changeAngle(float change)
    {
      _angle_spin->setValue(_angle + change);
    }

    void water::change_height(float change)
    {
      _h_spin->setValue(_lock_pos.y + change);
    }

    void water::paintLiquid (World* world, glm::vec3 const& pos, bool add)
    {
      world->paintLiquid ( pos
                         , _radius
                         , _liquid_id
                         , add
                         , math::degrees (_angled_mode.get() ? _angle : 0.0f)
                         , math::degrees (_angled_mode.get() ? _orientation : 0.0f)
                         , _locked.get()
                         , _lock_pos
                         , _override_height.get()
                         , _override_liquid_id.get()
                         , get_opacity_factor()
                         );
    }

    flatten_mode water::current_flatten_mode() const
    {
      return flatten_mode (_flatten_raise.get(), _flatten_lower.get());
    }

    glm::vec3 water::flatten_origin (World* world, glm::vec3 const& pos) const
    {
      if (_locked.get())
      {
        return _lock_pos;
      }

      float height;

      // A negative liquid id means "whatever layer has data here", which is right for an
      // anchor: the user is pointing at a water surface, not at a layer index.
      if (world->getLiquidHeight (pos.x, pos.z, -1, height))
      {
        return glm::vec3 (pos.x, height, pos.z);
      }

      return pos;
    }

    void water::weld_if_enabled (World* world, glm::vec3 const& pos)
    {
      if (_weld_seams.get())
      {
        world->weldLiquidSeams (pos, _radius, get_opacity_factor());
      }
    }

    void water::raiseLowerLiquid (World* world, glm::vec3 const& pos, float delta_time, bool raise)
    {
      // 7.5 units per second per point of strength, which is the constant RaiseLowerTool::onTick
      // passes to TerrainTool::changeTerrain before the speed slider multiplies it. Reusing it
      // means a liquid raise and a ground raise at the same strength climb together, which is
      // what you want when you are shaping a river bed and its water in the same session.
      float const change ((raise ? 7.5f : -7.5f) * delta_time
                          * static_cast<float> (_strength_slider->value()));

      world->changeLiquidHeight ( pos
                                , change
                                , _radius
                                , static_cast<float> (_inner_radius_slider->value())
                                , _falloff_type
                                , get_opacity_factor()
                                );

      weld_if_enabled (world, pos);
    }

    void water::flattenLiquid (World* world, glm::vec3 const& pos, float delta_time)
    {
      world->flattenLiquidHeight ( pos
                                 , blend_for (delta_time)
                                 , _radius
                                 , _falloff_type
                                 , current_flatten_mode()
                                 , flatten_origin (world, pos)
                                 , math::degrees (_angled_mode.get() ? _angle : 0.0f)
                                 , math::degrees (_angled_mode.get() ? _orientation : 0.0f)
                                 , get_opacity_factor()
                                 );

      weld_if_enabled (world, pos);
    }

    void water::smoothLiquid (World* world, glm::vec3 const& pos, float delta_time)
    {
      world->smoothLiquidHeight ( pos
                                , blend_for (delta_time)
                                , _radius
                                , _falloff_type
                                , current_flatten_mode()
                                , get_opacity_factor()
                                );

      weld_if_enabled (world, pos);
    }

    float water::blend_for (float delta_time) const
    {
      // 1 - 0.5^(dt * strength), the same exponential approach flatten_blur_tool uses for
      // terrain. It is the frame-rate-independent form: the fraction of the gap LEFT after a
      // tick is 0.5^(dt * strength), and two 8 ms ticks leave 0.5^(0.008 * k) squared, which is
      // 0.5^(0.016 * k) - exactly what one 16 ms tick leaves. A fixed weight per tick would
      // instead make the brush bite twice as hard at twice the frame rate.
      return 1.f - std::pow (0.5f, delta_time * static_cast<float> (_strength_slider->value()));
    }

    int water::brushMode() const
    {
      return _brush_mode;
    }

    float water::innerRadius() const
    {
      return static_cast<float> (_inner_radius_slider->value());
    }

    void water::lockPos(glm::vec3 const& cursor_pos)
    {
      QSignalBlocker const blocker_x(_x_spin);
      QSignalBlocker const blocker_z(_z_spin);
      QSignalBlocker const blocker_h(_h_spin);
      _lock_pos = cursor_pos;

      _x_spin->setValue(_lock_pos.x);
      _z_spin->setValue(_lock_pos.z);
      _h_spin->setValue(_lock_pos.y);

      if (!_locked.get())
      {
        toggle_lock();
      }
    }

    void water::toggle_lock()
    {
      _locked.toggle();
    }

    void water::toggle_angled_mode()
    {
      _angled_mode.toggle();
    }

    float water::brushRadius() const
    {
      return _radius;
    }

    float water::angle() const
    {
      return _angle;
    }

    float water::orientation() const
    {
      return _orientation;
    }

    bool water::angled_mode() const
    {
      return _angled_mode.get();
    }

    bool water::use_ref_pos() const
    {
      return _locked.get();
    }

    glm::vec3 water::ref_pos() const
    {
      return _lock_pos;
    }

    float water::get_opacity_factor() const
    {
      switch (_opacity_mode)
      {
      default:          // values found by experimenting
      case river_opacity:  return RIVER_OPACITY_VALUE;
      case ocean_opacity:  return OCEAN_OPACITY_VALUE;
      case custom_opacity: return _custom_opacity_factor;
      case auto_opacity:
      {
        switch (_liquid_type)
        {
        case 0: return RIVER_OPACITY_VALUE;
        case 1: return OCEAN_OPACITY_VALUE;
        default:  return RIVER_OPACITY_VALUE; // lava and slime, opacity isn't used
        }
      }
      break;
      }
    }

    QSize water::sizeHint() const
    {
      return QSize(250, height());
    }
  }
}
