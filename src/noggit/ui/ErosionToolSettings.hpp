// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_UI_EROSIONTOOLSETTINGS_HPP
#define NOGGIT_UI_EROSIONTOOLSETTINGS_HPP

#include <glm/vec3.hpp>

#include <QtWidgets/QWidget>

class World;

class QComboBox;
class QLabel;
class QSpinBox;

namespace Noggit::Ui::Tools::UiCommon
{
  class ExtendedSlider;
}

namespace Noggit
{
  namespace Ui
  {
    // The settings panel for ErosionTool, shaped after flatten_blur_tool: the Tool owns the
    // widget, the widget owns the parameters AND the call into World. Splitting it the other way
    // -- Tool reads the widgets and drives the world itself -- is what the rest of this tree
    // deliberately does not do, and following it keeps the whole edit in one readable function.
    //
    // ErosionKernel.hpp is NOT included here. It pulls in nothing but the STL, so the cost would
    // be small, but every type it would bring in is used only inside erode(); the header stays
    // free of it and the .cpp does the including.
    class ErosionToolSettings : public QWidget
    {
    public:
      ErosionToolSettings(QWidget* parent = nullptr);

      // One brush tick: sample the lattice under the cursor from `world`, relax it toward the
      // angle of repose, and add the resulting correction back to the terrain vertices.
      //
      // Opens no action of its own -- the caller must already be inside one, exactly as
      // World::blurTerrain requires, because the undo snapshot has to be taken before the first
      // vertex moves and the stroke is one action across many ticks.
      void erode(World* world, glm::vec3 const& cursor_pos);

      void changeRadius(float change);
      void changeInnerRadius(float change);

      [[nodiscard]]
      float brushRadius() const;

      // A RATIO in [0,1], not a world distance. ToolDrawParameters::inner_radius is fed straight
      // to CursorRender::draw as inner_radius_ratio (CursorRender.cpp:31, where it multiplies the
      // radius), so the brush cursor only draws its inner ring correctly if this stays a fraction.
      // The kernel wants the absolute distance instead; erode() is where the two are reconciled.
      [[nodiscard]]
      float innerRadiusRatio() const;

      QSize sizeHint() const override;

    private:
      Noggit::Ui::Tools::UiCommon::ExtendedSlider* _radius_slider;
      Noggit::Ui::Tools::UiCommon::ExtendedSlider* _inner_radius_slider;
      Noggit::Ui::Tools::UiCommon::ExtendedSlider* _repose_angle_slider;
      Noggit::Ui::Tools::UiCommon::ExtendedSlider* _strength_slider;
      QSpinBox* _iterations_spin;
      QComboBox* _edge_mode_combo;
      QLabel* _evidence_label;
    };
  }
}

#endif // NOGGIT_UI_EROSIONTOOLSETTINGS_HPP
