// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once
#include <noggit/tool_enums.hpp>

#include <glm/vec3.hpp>

#include <QJsonObject>
#include <QtWidgets/QWidget>

namespace Noggit::Ui::Tools::UiCommon
{
  class ExtendedSlider;
}

class World;

class QButtonGroup;
class QCheckBox;
class QLabel;
class QDial;
class QDoubleSpinBox;
class QGroupBox;
class QPushButton;
class QSlider;

namespace Noggit
{
  namespace Ui
  {
    class flatten_blur_tool : public QWidget
    {
    public:
      flatten_blur_tool(QWidget* parent = nullptr);

      void flatten (World* world, glm::vec3 const& cursor_pos, float dt);
      void blur (World* world, glm::vec3 const& cursor_pos, float dt);

      // THE RAMP TOOL.
      //
      // Two terrain picks and a width, graded at a constant slope between them. The idea and the
      // gesture come from haloreach252's feature/ramp_tool branch
      // (gitlab.com/haloreach252/noggit-red, commits b5d34291 and 883492a8, GPL-3.0); the
      // geometry below and in World::buildRamp is this fork's own.
      //
      // A checkable group and NOT a fifth entry in the Type picker, which is where upstream put
      // it. Every existing eFlattenType is a falloff curve that MapChunk::flattenTerrain switches
      // on, and the ramp never reaches MapChunk::flattenTerrain -- it goes to World::buildRamp,
      // which has its own geometry -- so a fifth enumerator would be a value that is invalid
      // everywhere the other four are used. "Angled mode" and "Lock mode" in this same panel are
      // already checkable groups sitting beside the type picker rather than inside it, which is
      // the shape this belongs in. It also leaves all four curves available to the blur that
      // usually follows a ramp.

      //! QButtonGroup ids for the falloff curve. Ids and not an enum because that is what
      //! QButtonGroup::addButton and checkedId() deal in, and the pair never leaves this widget:
      //! buildRamp turns them into World::buildRamp's `smooth_falloff` bool before anything else
      //! sees them.
      static constexpr int RAMP_CURVE_LINEAR = 0;
      static constexpr int RAMP_CURVE_SMOOTH = 1;

      [[nodiscard]] bool rampMode() const;

      void setRampStart (glm::vec3 const& pos);
      void setRampEnd (glm::vec3 const& pos);
      void clearRampPoints();

      [[nodiscard]] bool hasRampStart() const;
      [[nodiscard]] bool hasRampEnd() const;
      [[nodiscard]] glm::vec3 rampStart() const;
      [[nodiscard]] glm::vec3 rampEnd() const;

      //! Half the flat core's width, and the falloff band outside it, both in yards. The renderer
      //! needs these to outline the footprint, which is why they are public.
      [[nodiscard]] float rampHalfWidth() const;
      [[nodiscard]] float rampFalloffWidth() const;

      //! Grade the ramp. Does NOT open an action -- the caller brackets it, so that the whole
      //! ramp, however many chunks it crosses, is one undo step. Returns what World::buildRamp
      //! returned.
      bool buildRamp (World* world);

      //! The panel's Build button, handed out so the tool that owns the action stack can connect
      //! it to the same code path shift + right click takes. Same idiom as getRadiusSlider below:
      //! this widget has no MapView and must not open an action itself, or the ramp would land in
      //! a different undo step depending on which of the two ways it was started.
      [[nodiscard]] QPushButton* rampBuildButton();

      void nextFlattenType();
      void nextFlattenMode();
      void toggleFlattenAngle();
      void toggleFlattenLock();
      void lockPos (glm::vec3 const& cursor_pos);

      void changeRadius(float change);
      void changeSpeed(float change);
      void changeOrientation(float change);
      void changeAngle(float change);
      void changeHeight(float change);

      void setRadius(float radius);
      void setSpeed(float speed);
      void setOrientation(float orientation);

      float brushRadius() const;
      float angle() const;
      float orientation() const;
      bool angled_mode() const;
      bool use_ref_pos() const;
      glm::vec3 ref_pos() const;

      Noggit::Ui::Tools::UiCommon::ExtendedSlider* getRadiusSlider();;
      Noggit::Ui::Tools::UiCommon::ExtendedSlider* getSpeedSlider();;

      QSize sizeHint() const override;
      flatten_mode _flatten_mode;

      QJsonObject toJSON();
      void fromJSON(QJsonObject const& json);

    private:
      //! Rewrites the coordinate, length and grade read-out under the ramp controls.
      void updateRampReadout();

      float _angle;
      float _orientation;

      glm::vec3 _lock_pos;

      int _flatten_type;

      glm::vec3 _ramp_start;
      glm::vec3 _ramp_end;
      bool _has_ramp_start;
      bool _has_ramp_end;

    private:
      QButtonGroup* _type_button_box;
      Noggit::Ui::Tools::UiCommon::ExtendedSlider* _radius_slider;
      Noggit::Ui::Tools::UiCommon::ExtendedSlider* _speed_slider;

      QGroupBox* _angle_group;
      QSlider* _angle_slider;
      QDial* _orientation_dial;
      QLabel* _orientation_info;
      QLabel* _angle_info;

      QGroupBox* _lock_group;
      QDoubleSpinBox* _lock_x;
      QDoubleSpinBox* _lock_z;
      QDoubleSpinBox* _lock_h;

      QCheckBox* _lock_up_checkbox;
      QCheckBox* _lock_down_checkbox;
      QCheckBox* _snap_m2_objects_chkbox;
      QCheckBox* _snap_wmo_objects_chkbox;

      //! Opt-in, and off at every start, for the reason spelled out on the identical box in
      //! TerrainTool: re-tilting overwrites a rotation somebody placed by hand.
      QCheckBox* _follow_ground_normals_chkbox;

      QGroupBox* _ramp_group;
      Noggit::Ui::Tools::UiCommon::ExtendedSlider* _ramp_width_slider;
      Noggit::Ui::Tools::UiCommon::ExtendedSlider* _ramp_falloff_slider;
      QButtonGroup* _ramp_curve_button_box;
      QLabel* _ramp_readout;
      QPushButton* _ramp_build_button;
      QPushButton* _ramp_clear_button;
    };
  }
}
