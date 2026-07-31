// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include "ErosionTool.hpp"

#include <noggit/ActionManager.hpp>
#include <noggit/Input.hpp>
#include <noggit/MapView.h>
#include <noggit/ui/ErosionToolSettings.hpp>
#include <noggit/ui/tools/ToolPanel/ToolPanel.hpp>
#include <noggit/World.h>

namespace Noggit
{
    ErosionTool::ErosionTool(MapView* mapView)
        : Tool{ mapView }
    {
        // MapView dispatches these to the ACTIVE tool only (MapView.cpp:5583), so the mode check
        // is redundant; the terrain brushes carry it anyway and the !NOGGIT_CUR_ACTION half is
        // not redundant -- it stops the radius changing underneath a stroke that is already open.
        //
        // The step is 1.0f, not the 0.01f the other terrain brushes pass. Radius here is a world
        // distance, so 0.01 is a hundred keypresses per yard.
        addHotkey("increaseRadius"_hash, Hotkey{
            .onPress = [this] { _erosionTool->changeRadius(1.0f); },
            .condition = [mapView] { return mapView->get_editing_mode() == editing_mode::erosion && !NOGGIT_CUR_ACTION; },
            });

        addHotkey("decreaseRadius"_hash, Hotkey{
            .onPress = [this] { _erosionTool->changeRadius(-1.0f); },
            .condition = [mapView] { return mapView->get_editing_mode() == editing_mode::erosion && !NOGGIT_CUR_ACTION; },
            });
    }

    ErosionTool::~ErosionTool()
    {
        delete _erosionTool;
    }

    char const* ErosionTool::name() const
    {
        return "Erosion";
    }

    editing_mode ErosionTool::editingMode() const
    {
        return editing_mode::erosion;
    }

    Ui::FontNoggit::Icons ErosionTool::icon() const
    {
        // No erosion glyph exists in the icon font, and inventing one is not a code change.
        // INFO is what the tree already uses for a tool without its own icon
        // (ScriptingTool.cpp:38, ChunkTool.cpp:34).
        return Ui::FontNoggit::INFO;
    }

    void ErosionTool::setupUi(Ui::Tools::ToolPanel* toolPanel)
    {
        _erosionTool = new Noggit::Ui::ErosionToolSettings(mapView());
        toolPanel->registerTool(this, _erosionTool);
    }

    ToolDrawParameters ErosionTool::drawParameters() const
    {
        // This is the whole of the 3D brush cursor integration. The renderer reads radius and
        // inner_radius off these parameters every frame (CursorRender.cpp:29-31), so a tool that
        // fills them in correctly gets the ring for free and one that computes its own falloff
        // from different numbers gets a cursor that lies about where the brush ends.
        return
        {
            .radius = _erosionTool->brushRadius(),
            .inner_radius = _erosionTool->innerRadiusRatio(),
        };
    }

    void ErosionTool::onTick(float deltaTime, TickParameters const& params)
    {
        // Gated on left_mouse, and that gate is safe HERE.
        //
        // ScriptingTool.cpp:56-74 records the opposite case at length: gating its onTick on
        // left_mouse broke the scripting brush outright, because sendBrushEvent drives an edge
        // detector that has to observe the button-up state to fire its release callbacks. Skipping
        // the call while the button is up meant it never saw "up" again. Nothing here is an edge
        // detector -- erode() reads the current settings, does one relaxation, and keeps no state
        // between ticks -- so not being called is exactly equivalent to being called with nothing
        // to do, and the cheap gate is the right one.
        //
        // deltaTime is deliberately unused. Every other terrain brush scales its per-tick effect
        // by it, because raising or flattening has no fixed point and a fast machine would
        // otherwise move the ground further per second. Thermal erosion does have a fixed point:
        // once the surface is at the angle of repose, further iterations move nothing (the panel
        // reports "at rest"). Frame rate therefore changes how QUICKLY a stroke settles and not
        // what it settles to, and multiplying the correction by deltaTime would only add a
        // framerate-dependent overshoot the kernel's strength clamp was designed to avoid.
        (void)deltaTime;

        if (!mapView()->getWorld()->has_selection() || !params.left_mouse)
        {
            return;
        }

        if (params.displayMode != display_mode::in_3D || params.underMap)
        {
            return;
        }

        if (!params.mod_shift_down)
        {
            return;
        }

        // One stroke, one undo step. beginAction returns the action already in flight if there is
        // one (ActionManager.cpp:64-65), so calling it every tick opens exactly one; the modality
        // controllers are what close it, when shift or the left button is released. The action has
        // to exist before erode() runs, because registerChunkTerrainChange inside it is what
        // snapshots the pre-edit vertices.
        NOGGIT_ACTION_MGR->beginAction(mapView(), Noggit::ActionFlags::eCHUNKS_TERRAIN,
            Noggit::ActionModalityControllers::eSHIFT
            | Noggit::ActionModalityControllers::eLMB);

        _erosionTool->erode(mapView()->getWorld(), mapView()->cursorPosition());
    }

    void ErosionTool::onMouseMove(MouseMoveParameters const& params)
    {
        // Same bindings as the other terrain brushes: alt-drag on the left button is the outer
        // radius (FlattenBlurTool.cpp:149-152), alt-drag on the right is the inner one
        // (RaiseLowerTool.cpp:159-170).
        if (params.left_mouse)
        {
            if (params.mod_alt_down && !params.mod_shift_down && !params.mod_ctrl_down)
            {
                _erosionTool->changeRadius(params.relative_movement.dx() / XSENS);
            }
        }

        if (params.right_mouse)
        {
            if (params.mod_alt_down && !params.mod_shift_down && !params.mod_ctrl_down)
            {
                _erosionTool->changeInnerRadius(params.relative_movement.dx() / 100.0f);
            }
        }
    }
}
