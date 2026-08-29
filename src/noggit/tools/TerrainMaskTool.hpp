// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_TOOLS_TERRAINMASKTOOL_HPP
#define NOGGIT_TOOLS_TERRAINMASKTOOL_HPP

#include <noggit/Tool.hpp>

namespace Noggit
{
    namespace Ui
    {
        class TerrainMaskToolSettings;
    }

    // THE HAND ON A MASK.
    //
    // A mask can already be derived from slope, height, curvature, layer alpha, area id and
    // noise, combined into a stack, named, persisted to a project sidecar and used to clip every
    // brush in the editor. Everything except the last ten per cent, which is a human deciding
    // that the quarry wall is not a cliff. TerrainMaskPainter was written for exactly that and
    // had no caller; this is the caller.
    //
    // IT WRITES NOTHING INTO ADT DATA, and that property is not incidental -- it is the reason
    // the whole feature is free of client risk. A stroke touches two in-memory fields of one
    // NamedTerrainMask: the paint layer, which is saved as a run-length-encoded sidecar under the
    // project, and the composited field, which is not saved at all. No MCNK, no MCAL, no chunk
    // flag ever learns that a mask exists. Consequently this tool NEVER opens a Noggit Action:
    // see TerrainMaskHistory.hpp for the argument, which is the substantive design decision in
    // this file.
    //
    // Shaped after ShadowTool: this class translates input into calls and holds no parameters,
    // the settings widget holds the parameters and performs anything slow. The one slow thing
    // here is baking a tile -- 256 chunks, and up to 1'048'576 evaluations of the filter stack --
    // and it is reached ONLY from onMousePress and onMouseMove, which the Qt event loop delivers.
    // It must never move into onTick: tick runs inside paintGL (MapView.cpp:5139), and this
    // project has twice been bitten by work that stalls or raises a dialog from inside a paint.
    class TerrainMaskTool final : public Tool
    {
    public:
        TerrainMaskTool(MapView* mapView);
        ~TerrainMaskTool();

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

        void onSelected() override;
        void onDeselected() override;

        void onTick(float deltaTime, TickParameters const& params) override;

        void onMousePress(MousePressParameters const& params) override;
        void onMouseRelease(MouseReleaseParameters const& params) override;
        void onMouseMove(MouseMoveParameters const& params) override;

    private:
        // Closes the stroke in the mask's own history. Called from every path that can end one:
        // the button coming up, the modifier being released mid-drag, and the tool losing focus.
        void endStroke();

        // Asks the panel to bake the tile under `position` if it needs it, at most once per tile
        // per gesture. The panel refuses politely when auto-bake is off, and that refusal writes
        // a status line -- which is why this is throttled rather than called on every mouse move.
        void bakeTileOnce(glm::vec3 const& position);

        Ui::TerrainMaskToolSettings* _settings = nullptr;

        // -1 means "no tile checked yet". Reset when a gesture ends so the next stroke on the
        // same tile can report again.
        int _checked_tile_x = -1;
        int _checked_tile_z = -1;
    };
}

#endif // NOGGIT_TOOLS_TERRAINMASKTOOL_HPP
