// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include "ScriptingTool.hpp"

#include <noggit/ActionManager.hpp>
#include <noggit/Action.hpp>
#include <noggit/MapView.h>
#include <noggit/Input.hpp>
#include <noggit/scripting/scripting_tool.hpp>
#include <noggit/scripting/script_settings.hpp>
#include <noggit/ui/tools/ToolPanel/ToolPanel.hpp>
#include <noggit/World.h>

namespace Noggit
{
    ScriptingTool::ScriptingTool(MapView* mapView)
        : Tool{ mapView }
    {
    }

    ScriptingTool::~ScriptingTool()
    {
        delete _scriptingTool;
    }

    char const* ScriptingTool::name() const
    {
        return "Scripting";
    }

    editing_mode ScriptingTool::editingMode() const
    {
        return editing_mode::scripting;
    }

    Ui::FontNoggit::Icons ScriptingTool::icon() const
    {
        return Ui::FontNoggit::INFO;
    }

    void ScriptingTool::setupUi(Ui::Tools::ToolPanel* toolPanel)
    {
        _scriptingTool = new Noggit::Scripting::scripting_tool(mapView(), mapView(), mapView()->settings());
        toolPanel->registerTool(this, _scriptingTool);
    }

    ToolDrawParameters ScriptingTool::drawParameters() const
    {
        return
        {
            .radius = _scriptingTool->get_settings()->brushRadius(),
            .inner_radius = _scriptingTool->get_settings()->innerRadius(),
        };
    }

    void ScriptingTool::onTick(float deltaTime, TickParameters const& params)
    {
        // Called on EVERY tick, deliberately, and exactly once.
        //
        // An earlier version gated this on params.left_mouse the way the terrain brushes do. That
        // broke the scripting brush outright: sendBrushEvent drives an edge detector
        // (scripting_tool.cpp) that compares the current mouse state against the previous one, so
        // returning early whenever the button is up meant it never observed the "up" state --
        // on_left_click fired once per session, and on_left_release and every right-button
        // callback never fired at all.
        //
        // sendBrushEvent self-gates instead: idle with nothing pending it dispatches nothing,
        // allocates nothing and opens no undo action, and it opens its own action with the
        // eLMB/eRMB modality only when a script callback is about to run -- so a stroke is still
        // one undo step rather than one per frame.
        (void)params;

        _scriptingTool->sendBrushEvent(mapView()->cursorPosition(), 7.5f * deltaTime);
    }
}
