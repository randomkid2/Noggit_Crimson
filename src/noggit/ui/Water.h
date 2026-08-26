// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

#include <noggit/BoolToggleProperty.hpp>
#include <noggit/TileIndex.hpp>
#include <noggit/tool_enums.hpp>

class QDoubleSpinBox;
class QGroupBox;
class QLabel;
class QPushButton;
class QSpinBox;
class World;
class QComboBox;
class QRadioButton;
class QButtonGroup;

namespace Noggit::Ui::Tools::UiCommon
{
  class ExtendedSlider;
}

namespace Noggit
{
  struct unsigned_int_property;

  namespace Ui
  {
    class water : public QWidget
    {
      Q_OBJECT

    public:
      // What Shift+LMB and Ctrl+LMB do. Paint is the tool's original behaviour and stays the
      // default; the other three drive the per-vertex height brushes and leave the subchunk
      // mask alone, so they reshape water that is already there rather than creating any.
      enum water_brush_mode
      {
        water_brush_paint = 0,
        water_brush_raise_lower,
        water_brush_flatten,
        water_brush_smooth,
        water_brush_count,
      };

      water ( unsigned_int_property* current_layer
            , BoolToggleProperty* display_all_layers
            , QWidget* parent = nullptr
            );

      void updatePos(TileIndex const& newTile);
      void updateData();

      void changeWaterType(int waterint);

      void paintLiquid (World*, glm::vec3 const& pos, bool add);

      // The height brushes. delta_time is the tick length, so a stroke covers the same ground
      // per second whatever the frame rate -- the rate constants are lifted from the terrain
      // tools so that a liquid raise and a ground raise at the same strength move at the same
      // speed. Each of them welds the seams it touched afterwards when the box is ticked.
      void raiseLowerLiquid (World*, glm::vec3 const& pos, float delta_time, bool raise);
      void flattenLiquid (World*, glm::vec3 const& pos, float delta_time);
      void smoothLiquid (World*, glm::vec3 const& pos, float delta_time);

      int brushMode() const;
      float innerRadius() const;

      void changeRadius(float change);
      void setRadius(float radius);
      void changeOrientation(float change);
      void changeAngle(float change);
      void change_height(float change);

      void lockPos(glm::vec3 const& cursor_pos);
      void toggle_lock();
      void toggle_angled_mode();

      float brushRadius() const;
      float angle() const;
      float orientation() const;
      bool angled_mode() const;
      bool use_ref_pos() const;
      glm::vec3 ref_pos() const;

      QSize sizeHint() const override;

    signals:
      void regenerate_water_opacity (float factor);
      void crop_water();

    private:
      static constexpr float RIVER_OPACITY_VALUE = 0.0337f;
      static constexpr float OCEAN_OPACITY_VALUE = 0.007f;

      float get_opacity_factor() const;

      // Where a flatten aims. Locked: the Lock group's X/Z/H. Unlocked: the liquid surface
      // under the cursor, NOT the cursor position itself -- the cursor sits on the terrain the
      // ray hit, which under water is the lake bed, and flattening a lake to its own bed is
      // never what was meant.
      glm::vec3 flatten_origin (World* world, glm::vec3 const& pos) const;

      flatten_mode current_flatten_mode() const;

      // The frame-rate-independent blend weight the flatten and smooth passes take.
      float blend_for (float delta_time) const;

      void weld_if_enabled (World* world, glm::vec3 const& pos);

      int _liquid_id;
      liquid_basic_types _liquid_type;
      float _radius;

      float _angle;
      float _orientation;

      BoolToggleProperty _locked;
      BoolToggleProperty _angled_mode;

      BoolToggleProperty _override_liquid_id;
      BoolToggleProperty _override_height;

      int _brush_mode;

      // One control drives two enums. eFlattenType_Flat/_Linear/_Smooth are 0, 1 and 2, and so
      // are eTerrainType_Flat/_Linear/_Smooth, so the same stored value is a valid brush type
      // for changeTerrainProcessVertex and for the flatten/smooth blend without a mapping
      // table. That is why the falloff picker offers exactly those three and not the four
      // further terrain curves, which have no counterpart on the flatten side.
      int _falloff_type;

      BoolToggleProperty _flatten_raise;
      BoolToggleProperty _flatten_lower;
      BoolToggleProperty _weld_seams;

      int _opacity_mode;
      float _custom_opacity_factor;

      glm::vec3 _lock_pos;

      QDoubleSpinBox* _radius_spin;
      QDoubleSpinBox* _angle_spin;
      QDoubleSpinBox* _orientation_spin;

      QDoubleSpinBox* _x_spin;
      QDoubleSpinBox* _z_spin;
      QDoubleSpinBox* _h_spin;

      QRadioButton* river_button;
      QRadioButton* ocean_button;
      QRadioButton* custom_button;
      QButtonGroup* transparency_toggle;

      QComboBox* waterType;
      QSpinBox* waterLayer;

      QComboBox* _brush_mode_combo;
      QComboBox* _falloff_combo;
      Tools::UiCommon::ExtendedSlider* _strength_slider;
      Tools::UiCommon::ExtendedSlider* _inner_radius_slider;

      TileIndex tile;
    };
  }
}
