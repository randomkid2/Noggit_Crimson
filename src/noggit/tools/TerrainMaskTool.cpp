// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include "TerrainMaskTool.hpp"

#include <noggit/ActionManager.hpp>
#include <noggit/Input.hpp>
#include <noggit/MapView.h>
#include <noggit/TileIndex.hpp>
#include <noggit/World.h>
#include <noggit/terrain/TerrainMaskHistory.hpp>
#include <noggit/terrain/TerrainMaskPainter.hpp>
#include <noggit/terrain/TerrainMaskStore.hpp>
#include <noggit/ui/TerrainMaskToolSettings.hpp>
#include <noggit/ui/tools/ToolPanel/ToolPanel.hpp>

namespace Noggit
{
    TerrainMaskTool::TerrainMaskTool(MapView* mapView)
        : Tool{ mapView }
    {
        // The !NOGGIT_CUR_ACTION half is inherited from ErosionTool.cpp:18-31 and kept even
        // though this tool never opens an Action of its own: the condition is about the EDITOR
        // being mid-stroke, and changing a mask brush's radius while a terrain stroke is open
        // would move the clip region under an edit that is already recorded against the old one.
        addHotkey("increaseRadius"_hash, Hotkey{
            .onPress = [this] { _settings->changeRadius(1.0f); },
            .condition = [mapView] { return mapView->get_editing_mode() == editing_mode::terrain_mask && !NOGGIT_CUR_ACTION; },
            });

        addHotkey("decreaseRadius"_hash, Hotkey{
            .onPress = [this] { _settings->changeRadius(-1.0f); },
            .condition = [mapView] { return mapView->get_editing_mode() == editing_mode::terrain_mask && !NOGGIT_CUR_ACTION; },
            });

        // MASK UNDO IS NOT Ctrl+Z, AND THAT IS THE POINT. Ctrl+Z is a QAction shortcut on the
        // Edit menu (MapView.cpp:2747) and drives ActionManager, which restores MCNK data; a
        // mask is not MCNK data and must not be reachable from the same key. See the argument in
        // TerrainMaskHistory.hpp. Alt+Z mirrors the shape of the editor's binding with a
        // different modifier, and the condition below confines it to this tool so no other mode
        // loses a key.
        addHotkey("maskUndoStroke"_hash, Hotkey{
            .onPress = [this] { _settings->undoStroke(); },
            .condition = [mapView] { return mapView->get_editing_mode() == editing_mode::terrain_mask && !NOGGIT_CUR_ACTION; },
            });

        addHotkey("maskRedoStroke"_hash, Hotkey{
            .onPress = [this] { _settings->redoStroke(); },
            .condition = [mapView] { return mapView->get_editing_mode() == editing_mode::terrain_mask && !NOGGIT_CUR_ACTION; },
            });
    }

    TerrainMaskTool::~TerrainMaskTool()
    {
        delete _settings;
    }

    char const* TerrainMaskTool::name() const
    {
        return "Mask Painter";
    }

    editing_mode TerrainMaskTool::editingMode() const
    {
        return editing_mode::terrain_mask;
    }

    Ui::FontNoggit::Icons TerrainMaskTool::icon() const
    {
        // NOT FontNoggit::INFO. Measured with
        // `grep -rn "return Ui::FontNoggit::INFO" src/noggit/tools/*.cpp`, three tools in this
        // eighteen-button strip return it TODAY -- ErosionTool.cpp:54, ScriptingTool.cpp:38 and
        // ShadowTool.cpp:51 -- and it is also the Details-info toolbar toggle. The note on
        // FontNoggit::TOOL_SCRIPTING records the mistake, though it is itself stale: it names
        // ChunkTool, which has since moved to TOOL_CHUNK (ChunkTool.cpp:101). A fourth identical
        // "i" button is exactly the defect this brief named.
        //
        // TEXTURE_PALETTE instead, on two measured grounds. It has a real glyph in the shipped
        // font -- codepoint 0xf8c3, well below the 0xf8e2 end-of-font boundary FontNoggit.hpp
        // documents -- so it renders without needing theme artwork, which the three enumerators
        // past that boundary do not. And a grep for constructions of it across src/ found ZERO
        // before this line and finds exactly this one after: the enumerator existed and nothing
        // in the tree ever built an icon from it, so it collides with no tool, no toolbar button
        // and no menu icon anywhere. A palette is also the nearest available metaphor for what
        // this tool does -- pick one of several named channels and paint into it.
        //
        // The permanent fix is a TOOL_TERRAIN_MASK enumerator of its own, which needs a row in
        // FontNoggit.hpp and one in FontAwesome.cpp's noggitIcons() table. Neither file is owned
        // this round, so it is reported rather than done.
        return Ui::FontNoggit::TEXTURE_PALETTE;
    }

    void TerrainMaskTool::setupUi(Ui::Tools::ToolPanel* toolPanel)
    {
        _settings = new Noggit::Ui::TerrainMaskToolSettings(mapView());
        toolPanel->registerTool(this, _settings);
    }

    ToolDrawParameters TerrainMaskTool::drawParameters() const
    {
        // inner_radius carries the hardness as a RATIO, which is what CursorRender multiplies the
        // outer radius by to draw the second ring (CursorRender.cpp:29-32) and what TexturingTool
        // already passes (TexturingTool.cpp:381). The two rings therefore bracket the feather:
        // full strength inside the inner one, linearly falling to nothing at the outer.
        return
        {
            .radius = _settings->brushRadius(),
            .inner_radius = _settings->hardness(),
            .cursor_color = _settings->cursorColor(),
        };
    }

    float TerrainMaskTool::brushRadius() const
    {
        return _settings->brushRadius();
    }

    void TerrainMaskTool::onSelected()
    {
        // The Terrain Masks dialog may have created, renamed or deleted a mask while another
        // tool was in use, and the store has no change signal by design.
        _settings->onToolSelected();
    }

    void TerrainMaskTool::onDeselected()
    {
        endStroke();
    }

    void TerrainMaskTool::onTick(float deltaTime, TickParameters const& params)
    {
        // deltaTime is unused, and that is correct rather than an oversight. The other brushes
        // scale by it because raising or smoothing has no fixed point, so a faster machine would
        // move the ground further per second. A mask stroke folds with Max at strength * falloff
        // (TerrainMaskPainter::strokeInto), so the second tick over a texel writes the value that
        // is already there. Frame rate cannot change what the stroke converges to.
        (void)deltaTime;

        bool const add = params.mod_shift_down;
        bool const erase = params.mod_ctrl_down;

        // Neither, or both. Both would otherwise be resolved by whichever branch is written
        // first, which is an arbitrary answer to an ambiguous gesture.
        bool const painting
          = params.left_mouse
         && (add != erase)
         && params.displayMode == display_mode::in_3D
         && !params.underMap
         && mapView()->getWorld()->has_selection();

        if (!painting)
        {
            // THIS is where a stroke ends most of the time -- the frame after Shift or the button
            // comes up -- which is the same shape as ActionManager::endActionOnModalityMismatch
            // closing a terrain stroke (MapView.cpp:5408). onMouseRelease closes it too, because
            // paintGL early-returns when nothing needs redrawing and this tick may not run.
            endStroke();
            return;
        }

        NamedTerrainMask* const mask = TerrainMaskStore::instance()->active();

        if (!mask)
        {
            return;
        }

        glm::vec3 const cursor = mapView()->cursorPosition();

        float const radius = _settings->brushRadius();

        // BEFORE the paint, always. capture() reads the pre-stroke bytes of every chunk the
        // circle can reach and stores each one once per stroke, so a drag that crosses the same
        // chunk fifty times costs one 8 KiB snapshot. A failed capture -- the only cause is a
        // failed allocation, which it swallows -- makes this stroke un-undoable and is not a
        // reason to refuse the edit.
        TerrainMaskHistory* const history = TerrainMaskHistory::instance();

        history->beginStroke(mask->name);
        history->capture(*mask, cursor.x, cursor.z, radius);

        // ONE CALL, BOTH FIELDS. TerrainMaskPainter exists so that the paint layer and the
        // composited field cannot be written apart: writing only the paint layer leaves the
        // stroke invisible until the next bake, and writing only the composite makes it vanish
        // at the next bake.
        TerrainMaskPainter::stroke( cursor.x
                                  , cursor.z
                                  , radius
                                  , _settings->hardness()
                                  , _settings->strength()
                                  , add ? TerrainMaskPainter::StrokeMode::Add
                                        : TerrainMaskPainter::StrokeMode::Erase
                                  );

        // Flags only. Anything that touched a QLabel here would run a layout pass inside paintGL.
        _settings->noteStrokePainted();
    }

    void TerrainMaskTool::onMousePress(MousePressParameters const& params)
    {
        if (params.button != Qt::LeftButton)
        {
            return;
        }

        if (params.mod_shift_down == params.mod_ctrl_down)
        {
            return;
        }

        // A new gesture, so the once-per-tile bake report is armed again.
        _checked_tile_x = -1;
        _checked_tile_z = -1;

        bakeTileOnce(mapView()->cursorPosition());
    }

    void TerrainMaskTool::onMouseRelease(MouseReleaseParameters const& params)
    {
        if (params.button == Qt::LeftButton)
        {
            endStroke();
        }
    }

    void TerrainMaskTool::onMouseMove(MouseMoveParameters const& params)
    {
        // The same three drags the texture brush uses, deliberately: alt on the left button is
        // the radius, alt on the right button is the hardness, space on the left button is the
        // strength (TexturingTool.cpp:536-567). XSENS and the 300 divisor are that tool's
        // constants, not new ones, so the gestures feel identical.
        if (params.left_mouse
            && params.mod_alt_down
            && !params.mod_shift_down
            && !params.mod_ctrl_down)
        {
            _settings->changeRadius(params.relative_movement.dx() / XSENS);
        }

        if (params.right_mouse
            && params.mod_alt_down
            && !params.mod_shift_down
            && !params.mod_ctrl_down)
        {
            _settings->changeHardness(params.relative_movement.dx() / 300.0f);
        }

        if (params.left_mouse && params.mod_space_down)
        {
            _settings->changeStrength(params.relative_movement.dx() / 300.0f);
        }

        // A drag that crosses a tile border needs the new tile baked too, and this handler is
        // where that can happen safely: mouseMoveEvent is delivered by the Qt event loop, not
        // from inside paintGL.
        if (params.left_mouse && (params.mod_shift_down != params.mod_ctrl_down))
        {
            bakeTileOnce(mapView()->cursorPosition());
        }
    }

    void TerrainMaskTool::endStroke()
    {
        TerrainMaskHistory::instance()->endStroke();

        _checked_tile_x = -1;
        _checked_tile_z = -1;
    }

    void TerrainMaskTool::bakeTileOnce(glm::vec3 const& position)
    {
        // TileIndex casts to std::size_t, so a cursor that missed the terrain -- which arrives as
        // NaN, and fails every ordered comparison below -- must not reach it.
        if (!(position.x >= 0.0f) || !(position.z >= 0.0f))
        {
            return;
        }

        TileIndex const index(position);

        int const tile_x = static_cast<int>(index.x);
        int const tile_z = static_cast<int>(index.z);

        if (tile_x == _checked_tile_x && tile_z == _checked_tile_z)
        {
            return;
        }

        _checked_tile_x = tile_x;
        _checked_tile_z = tile_z;

        _settings->ensureTileBaked(position);
    }
}
