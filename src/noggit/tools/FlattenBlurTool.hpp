// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

#include <noggit/rendering/Primitives.hpp>
#include <noggit/Tool.hpp>

#include <vector>

namespace Noggit
{
    namespace Ui
    {
        class flatten_blur_tool;
    }

    class FlattenBlurTool final : public Tool
    {
    public:
        FlattenBlurTool(MapView* mapView);
        ~FlattenBlurTool();

        [[nodiscard]]
        char const* name() const override;

        [[nodiscard]]
        editing_mode editingMode() const override;

        [[nodiscard]]
        Ui::FontNoggit::Icons icon() const override;

        void setupUi(Ui::Tools::ToolPanel* toolPanel) override;

        void postUiSetup() override;

        void onTick(float deltaTime, TickParameters const& params) override;

        [[nodiscard]]
        ToolDrawParameters drawParameters() const override;

        void onMousePress(MousePressParameters const& params) override;

        void onMouseMove(MouseMoveParameters const& params) override;

        void onMouseWheel(MouseWheelParameters const& params) override;

        void postRender() override;

    private:
        //! Open one action, grade the ramp, run Live Auto Texture, close it. See the comment on
        //! the definition for why the bracketing lives here and not in World::buildRamp.
        void buildRamp();

        //! The boundary of the region within `corner_radius` of the ramp's flat core, as a closed
        //! line strip in world space. `corner_radius` 0 gives the core rectangle itself.
        [[nodiscard]]
        std::vector<glm::vec3> rampOutline(float corner_radius) const;

        Ui::flatten_blur_tool* _flattenTool = nullptr;

        // Drawn from postRender, which is how AreaTriggerTool draws its trigger volumes: the tool
        // owns its primitive renderers and takes the matrices off MapView. That route is what
        // keeps the ramp overlay out of ToolDrawParameters and WorldRender, neither of which this
        // change owns -- and it is not a workaround, it is the cheaper of the two: a ramp is a
        // polyline of arbitrary length, and ToolDrawParameters is a fixed struct of scalars
        // copied every frame for every tool.
        Noggit::Rendering::Primitives::Sphere _rampPointRenderer;
        Noggit::Rendering::Primitives::Line _rampOutlineRenderer;
    };
}
