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
        // Gated on the left button, as every other brush tool is (RaiseLowerTool.cpp:122,
        // VertexPainterTool.cpp:85). Without it the brush fired on every tick for as long as a
        // chunk stayed SELECTED, which outlives the click -- so a scatter script kept placing
        // models after the user had stopped painting, and the only thing holding it back was
        // whatever spacing check the script happened to implement.
        if (!params.left_mouse)
        {
            return;
        }

        auto mv = mapView();
        auto world = mv->getWorld();

        auto currentSelection = world->current_selection();
        if (world->has_selection())
        {
            for (auto& selection : currentSelection)
            {
                if (selection.index() == eEntry_MapChunk)
                {
                    // One undo step per STROKE, not per tick.
                    //
                    // eLMB is what makes that work: the manager keeps the same action open while
                    // the button is held and closes it on release, so a scatter brush that placed
                    // two hundred models is a single Ctrl+Z rather than two hundred of them.
                    //
                    // Scripts add and remove objects and edit terrain, so the flags cover all
                    // three -- a script is not restricted to one kind of edit and the action has
                    // to record whatever it touches.
                    NOGGIT_ACTION_MGR->beginAction(mv
                        , Noggit::ActionFlags::eOBJECTS_ADDED
                        | Noggit::ActionFlags::eOBJECTS_REMOVED
                        | Noggit::ActionFlags::eCHUNKS_TERRAIN
                        , Noggit::ActionModalityControllers::eLMB);

                    _scriptingTool->sendBrushEvent(mv->cursorPosition(), 7.5f * deltaTime);
                }
            }
        }
    }
}
