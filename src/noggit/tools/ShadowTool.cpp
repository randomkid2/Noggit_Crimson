// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include "ShadowTool.hpp"

#include <noggit/ActionManager.hpp>
#include <noggit/Input.hpp>
#include <noggit/MapView.h>
#include <noggit/ui/ShadowToolSettings.hpp>
#include <noggit/ui/tools/ToolPanel/ToolPanel.hpp>
#include <noggit/World.h>

namespace Noggit
{
    ShadowTool::ShadowTool(MapView* mapView)
        : Tool{ mapView }
    {
        // The !NOGGIT_CUR_ACTION half is the load-bearing one: it stops the radius changing
        // underneath a stroke that is already open, which would leave part of the stroke recorded
        // against one radius and part against another. Copied from ErosionTool.cpp:22-31, which
        // carries the longer note.
        addHotkey("increaseRadius"_hash, Hotkey{
            .onPress = [this] { _settings->changeRadius(1.0f); },
            .condition = [mapView] { return mapView->get_editing_mode() == editing_mode::shadow && !NOGGIT_CUR_ACTION; },
            });

        addHotkey("decreaseRadius"_hash, Hotkey{
            .onPress = [this] { _settings->changeRadius(-1.0f); },
            .condition = [mapView] { return mapView->get_editing_mode() == editing_mode::shadow && !NOGGIT_CUR_ACTION; },
            });
    }

    ShadowTool::~ShadowTool()
    {
        delete _settings;
    }

    char const* ShadowTool::name() const
    {
        return "Terrain Shadows";
    }

    editing_mode ShadowTool::editingMode() const
    {
        return editing_mode::shadow;
    }

    Ui::FontNoggit::Icons ShadowTool::icon() const
    {
        // No shadow glyph exists in the icon font. INFO is what this tree already uses for a tool
        // without one of its own (ScriptingTool.cpp:38, ChunkTool.cpp:34, ErosionTool.cpp:54).
        return Ui::FontNoggit::INFO;
    }

    void ShadowTool::setupUi(Ui::Tools::ToolPanel* toolPanel)
    {
        _settings = new Noggit::Ui::ShadowToolSettings(mapView());
        toolPanel->registerTool(this, _settings);
    }

    ToolDrawParameters ShadowTool::drawParameters() const
    {
        // inner_radius stays 0. The shadow brush has no falloff and cannot have one: MCSH is one
        // bit per texel, so a texel is either shadowed or it is not and there is nothing for a
        // soft edge to interpolate. Drawing an inner ring would promise a gradient the tool
        // cannot deliver -- CursorRender reads these numbers directly (CursorRender.cpp:29-31).
        return
        {
            .radius = _settings->brushRadius(),
            .inner_radius = 0.0f,
        };
    }

    float ShadowTool::brushRadius() const
    {
        return _settings->brushRadius();
    }

    void ShadowTool::onTick(float deltaTime, TickParameters const& params)
    {
        // deltaTime is unused and that is correct here rather than an oversight. The other
        // brushes scale their per-tick effect by it because raising or smoothing has no fixed
        // point, so a faster machine would move the ground further per second. Setting a bit has
        // a fixed point after one tick: the texel is already 85, and setting it again is exactly
        // the no-op it looks like. Frame rate cannot change what a stroke converges to.
        (void)deltaTime;

        if (!mapView()->getWorld()->has_selection() || !params.left_mouse)
        {
            return;
        }

        if (params.displayMode != display_mode::in_3D || params.underMap)
        {
            return;
        }

        bool const add = params.mod_shift_down;
        bool const erase = params.mod_ctrl_down;

        // Neither, or both. Both would otherwise be resolved by whichever branch is written
        // first, which is an arbitrary answer to an ambiguous gesture.
        if (add == erase)
        {
            return;
        }

        // One stroke, one undo step. beginAction returns the action already in flight when there
        // is one (ActionManager.cpp:64-65), so calling it every tick opens exactly one; the
        // modality controllers close it when the modifier or the button is released. It has to
        // exist before setShadow runs, because registerChunkShadowChange inside it is what
        // snapshots the pre-stroke map.
        NOGGIT_ACTION_MGR->beginAction( mapView()
                                      , Noggit::ActionFlags::eCHUNK_SHADOWS
                                      , (add ? Noggit::ActionModalityControllers::eSHIFT
                                             : Noggit::ActionModalityControllers::eCTRL)
                                        | Noggit::ActionModalityControllers::eLMB
                                      );

        mapView()->getWorld()->setShadow(mapView()->cursorPosition(), _settings->brushRadius(), add);
    }

    void ShadowTool::onMouseMove(MouseMoveParameters const& params)
    {
        // Alt-drag on the left button is the radius, the same binding the terrain brushes use
        // (FlattenBlurTool.cpp:149-152). There is no inner-radius drag because there is no inner
        // radius; see drawParameters.
        if (params.left_mouse
            && params.mod_alt_down
            && !params.mod_shift_down
            && !params.mod_ctrl_down)
        {
            _settings->changeRadius(params.relative_movement.dx() / XSENS);
        }
    }
}
