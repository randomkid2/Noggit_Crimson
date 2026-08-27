// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_TOOLS_CHUNKTOOL_HPP
#define NOGGIT_TOOLS_CHUNKTOOL_HPP

#include <noggit/Tool.hpp>

namespace Noggit
{
    namespace Ui::Tools::ChunkManipulator
    {
        class ChunkManipulatorPanel;
    }

    class ChunkTool final : public Tool
    {
    public:
        ChunkTool(MapView* mapView);
        ~ChunkTool();

        [[nodiscard]]
        virtual char const* name() const override;

        [[nodiscard]]
        virtual editing_mode editingMode() const override;

        [[nodiscard]]
        virtual Ui::FontNoggit::Icons icon() const override;

        void setupUi(Ui::Tools::ToolPanel* toolPanel) override;

        [[nodiscard]]
        virtual ToolDrawParameters drawParameters() const override;

        [[nodiscard]]
        virtual float brushRadius() const override;

        virtual void onMousePress(MousePressParameters const& params) override;
        virtual void onMouseMove(MouseMoveParameters const& params) override;
        virtual void onMouseWheel(MouseWheelParameters const& params) override;

    private:
        //! Shift-select and ctrl-deselect share everything except the mode, and both are reachable
        //! from a press and from a drag, so all four routes go through one function.
        void applySelection(bool deselect);

        Ui::Tools::ChunkManipulator::ChunkManipulatorPanel* _chunkManipulator = nullptr;
    };
}

#endif // NOGGIT_TOOLS_CHUNKTOOL_HPP
