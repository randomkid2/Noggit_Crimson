// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_TOOLS_EROSIONTOOL_HPP
#define NOGGIT_TOOLS_EROSIONTOOL_HPP

#include <noggit/Tool.hpp>

namespace Noggit
{
    namespace Ui
    {
        class ErosionToolSettings;
    }

    // Thermal-erosion brush over ErosionKernel.
    //
    // Shaped after FlattenBlurTool rather than ScriptingTool: the settings widget holds the
    // parameters and does the world edit, this class does nothing but translate input into calls.
    class ErosionTool final : public Tool
    {
    public:
        ErosionTool(MapView* mapView);
        ~ErosionTool();

        [[nodiscard]]
        virtual char const* name() const override;

        [[nodiscard]]
        virtual editing_mode editingMode() const override;

        [[nodiscard]]
        virtual Ui::FontNoggit::Icons icon() const override;

        void setupUi(Ui::Tools::ToolPanel* toolPanel) override;

        [[nodiscard]]
        ToolDrawParameters drawParameters() const override;

        void onTick(float deltaTime, TickParameters const& params) override;

        void onMouseMove(MouseMoveParameters const& params) override;

    private:
        Ui::ErosionToolSettings* _erosionTool = nullptr;
    };
}

#endif // NOGGIT_TOOLS_EROSIONTOOL_HPP
