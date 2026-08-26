// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#include "hole_tool.hpp"

#include <noggit/ui/tools/ToolPanel/ToolWidgetStyle.hpp>
#include <noggit/ui/tools/UiCommon/ExtendedSlider.hpp>

#include <QtWidgets/QFormLayout>
#include <QtWidgets/QGroupBox>

namespace Noggit
{
  namespace Ui
  {
    hole_tool::hole_tool(QWidget* parent) : QWidget(parent)
    {
      // THIS TOOL HAD NO SECTION AND NO SHARED GUTTER. It was a bare QFormLayout put straight
      // on the tool widget with no contents margins of its own, so it took
      // QStyle::PM_LayoutLeftMargin (13px on windowsvista here) on top of ToolPanel's own 12px,
      // and it was one of only two tools in the dock -- the other was ShaderTool -- that
      // presented its controls with no titled section around them at all. Pressing the key that
      // swaps the terrain tool for this one replaced a panel of framed sections with two
      // unframed rows. See ToolWidgetStyle.hpp.
      auto layout (Tools::ToolPanelStyle::toolColumn (this));

      auto* const brush_section (Tools::ToolPanelStyle::toolSection (layout, tr ("Brush")));
      auto* const brush_layout (Tools::ToolPanelStyle::sectionForm (brush_section));

      // ONE RADIUS CONTROL, the same one every other brush tool in the dock uses.
      //
      // This was the only radius in the editor built as a bare QDoubleSpinBox on one form row
      // plus a separate QSlider on the next, wired together by a pair of hand-written
      // QSignalBlocker lambdas. Every other tool -- terrain, flatten, erosion, texturing,
      // shader, ground effects -- uses ExtendedSlider, which is that same pairing plus a prefix
      // label, a right-aligned value and the tablet-pressure menu, in two lines.
      //
      // THE RANGE IS THE SPIN BOX'S, and that is the one judgement call here. The two widgets
      // this replaces disagreed: the spin box was 0..1000 and the slider 0..250, so dragging
      // the slider to its right-hand end produced 250 while typing into the box, or calling
      // setRadius/changeRadius -- which both wrote to the SPIN BOX -- could reach 1000. The
      // tool's exposed range has always been the spin box's, because brushRadius() reads the
      // value those two setters produce; 0..1000 is therefore the range that is preserved here,
      // and the 250 was the proxy widget's private clamp rather than a limit of the tool.
      // Nothing else moves: same decimals, same initial value, same brushRadius() contract.
      _radius_slider = new Noggit::Ui::Tools::UiCommon::ExtendedSlider (this);
      _radius_slider->setPrefix ("Radius:");
      _radius_slider->setRange (0.0, 1000.0);
      _radius_slider->setDecimals (2);
      _radius_slider->setValue (_radius);

      brush_layout->addRow (_radius_slider);

      connect ( _radius_slider, &Noggit::Ui::Tools::UiCommon::ExtendedSlider::valueChanged
              , [&] (double v) { _radius = static_cast<float> (v); }
              );
    }

    void hole_tool::changeRadius(float change)
    {
      _radius_slider->setValue (_radius + change);
    }

    float hole_tool::brushRadius() const
    {
      return _radius;
    }

    void hole_tool::setRadius(float radius)
    {
      _radius_slider->setValue(radius);
    }
  }
}
