// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_TOOLS_SHADOWTOOL_HPP
#define NOGGIT_TOOLS_SHADOWTOOL_HPP

#include <noggit/Tool.hpp>

namespace Noggit
{
    namespace Ui
    {
        class ShadowToolSettings;
    }

    // Terrain shadow (MCSH) painting and baking.
    //
    // Two halves that share one panel. The brush paints the 64x64 one-bit shadow mask directly,
    // shift to add and ctrl to erase, which is the same pairing every other chunk-flag tool in
    // this tree uses. The bake renders the whole loaded scene once from the sun and thresholds
    // the result into every chunk of one tile.
    //
    // Shaped after ErosionTool: this class translates input into calls and holds no parameters,
    // the settings widget holds the parameters and drives the world.
    //
    // The bake deliberately has NO path through this class. It is a slow operation that ends in a
    // glReadPixels stall and it must not run from onTick, preRender or postRender -- all three are
    // reached from paintGL (MapView.cpp:5071 and :5086 for the render hooks), and the project has
    // twice been bitten by work that raises a dialog or throws from inside a paint. The bake runs
    // from the panel's own button handler, which the Qt event loop delivers on its own.
    class ShadowTool final : public Tool
    {
    public:
        ShadowTool(MapView* mapView);
        ~ShadowTool();

        [[nodiscard]]
        virtual char const* name() const override;

        [[nodiscard]]
        virtual editing_mode editingMode() const override;

        [[nodiscard]]
        virtual Ui::FontNoggit::Icons icon() const override;

        void setupUi(Ui::Tools::ToolPanel* toolPanel) override;

        [[nodiscard]]
        ToolDrawParameters drawParameters() const override;

        [[nodiscard]]
        virtual float brushRadius() const override;

        void onTick(float deltaTime, TickParameters const& params) override;

        void onMouseMove(MouseMoveParameters const& params) override;

    private:
        Ui::ShadowToolSettings* _settings = nullptr;
    };
}

#endif // NOGGIT_TOOLS_SHADOWTOOL_HPP
