// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include "ChunkTool.hpp"

#include <noggit/ActionManager.hpp>
#include <noggit/Input.hpp>
#include <noggit/MapView.h>
#include <noggit/ui/tools/ChunkManipulator/ChunkClipboard.hpp>
#include <noggit/ui/tools/ChunkManipulator/ChunkManipulatorPanel.hpp>
#include <noggit/ui/tools/ToolPanel/ToolPanel.hpp>

#include <QtGui/QWheelEvent>

namespace Noggit
{
    ChunkTool::ChunkTool(MapView* mapView)
        : Tool{ mapView }
    {
        // Every hotkey is gated on chunk mode AND on no action being open. The second half is
        // not decoration: MapView dispatches a hotkey to whichever registration matches the key,
        // the modifiers and the condition first (MapView.cpp:5646-5656), and several of these
        // keys are already claimed by other tools -- C by the vertex selection, F by the area
        // designator and the cursor lock, R by the brush level. Refusing when the condition is
        // false is what lets those keep working everywhere else. The !NOGGIT_CUR_ACTION half
        // matches AreaTool.cpp:26 and stops a paste from being folded into somebody's stroke.
        auto const inChunkMode
            ( [mapView] { return mapView->get_editing_mode() == editing_mode::chunk
                              && !NOGGIT_CUR_ACTION; }
            );

        addHotkey("chunkCopy"_hash, Hotkey{
            .onPress = [this] { if (_chunkManipulator) _chunkManipulator->doCopy(); },
            .condition = inChunkMode
            });

        addHotkey("chunkPaste"_hash, Hotkey{
            .onPress = [this] { if (_chunkManipulator) _chunkManipulator->doPaste(); },
            .condition = inChunkMode
            });

        addHotkey("chunkClearSelection"_hash, Hotkey{
            .onPress = [this] { if (_chunkManipulator) _chunkManipulator->doClearSelection(); },
            .condition = inChunkMode
            });

        addHotkey("chunkRotate90"_hash, Hotkey{
            .onPress = [this]
                {
                    if (_chunkManipulator)
                    {
                        _chunkManipulator->applyTransform(Ui::Tools::ChunkManipulator::ChunkGridOp::ROTATE_90);
                    }
                },
            .condition = inChunkMode
            });

        addHotkey("chunkMirrorHorizontal"_hash, Hotkey{
            .onPress = [this]
                {
                    if (_chunkManipulator)
                    {
                        _chunkManipulator->applyTransform(Ui::Tools::ChunkManipulator::ChunkGridOp::MIRROR_X);
                    }
                },
            .condition = inChunkMode
            });

        addHotkey("chunkMirrorVertical"_hash, Hotkey{
            .onPress = [this]
                {
                    if (_chunkManipulator)
                    {
                        _chunkManipulator->applyTransform(Ui::Tools::ChunkManipulator::ChunkGridOp::MIRROR_Z);
                    }
                },
            .condition = inChunkMode
            });
    }

    ChunkTool::~ChunkTool()
    {
        delete _chunkManipulator;
    }

    char const* ChunkTool::name() const
    {
        return "Chunk Manipulator";
    }

    editing_mode ChunkTool::editingMode() const
    {
        return editing_mode::chunk;
    }

    Ui::FontNoggit::Icons ChunkTool::icon() const
    {
        // Was FontNoggit::INFO, which is also what ScriptingTool and ErosionTool return and what
        // the "Details info" toolbar toggle uses -- four identical glyphs in one window, three of
        // them in the same sixteen-button strip. FontNoggit.hpp:160 already declares TOOL_CHUNK
        // for exactly this and nothing constructed it.
        return Ui::FontNoggit::TOOL_CHUNK;
    }

    void ChunkTool::setupUi(Ui::Tools::ToolPanel* toolPanel)
    {
        _chunkManipulator = new Noggit::Ui::Tools::ChunkManipulator::ChunkManipulatorPanel(mapView(), mapView());
        toolPanel->registerTool(this, _chunkManipulator);
    }

    ToolDrawParameters ChunkTool::drawParameters() const
    {
        return
        {
            .radius = brushRadius(),
            .cursor_type = CursorType::CIRCLE,
        };
    }

    float ChunkTool::brushRadius() const
    {
        return _chunkManipulator ? _chunkManipulator->selectionRadius() : 0.0f;
    }

    void ChunkTool::applySelection(bool deselect)
    {
        if (!_chunkManipulator)
        {
            return;
        }

        using namespace Noggit::Ui::Tools::ChunkManipulator;

        ChunkClipboard* const clipboard(_chunkManipulator->clipboard());
        ChunkSelectionMode const mode(deselect ? ChunkSelectionMode::DESELECT
                                               : ChunkSelectionMode::SELECT);

        if (_chunkManipulator->eyedropperActive())
        {
            clipboard->selectChunk(mapView()->cursorPosition(), mode);
            return;
        }

        clipboard->selectRange(mapView()->cursorPosition(), _chunkManipulator->selectionRadius(), mode);
    }

    void ChunkTool::onMousePress(MousePressParameters const& params)
    {
        if (params.button != Qt::MouseButton::LeftButton)
        {
            return;
        }

        // Shift adds, ctrl removes. Alt is the radius drag and is handled in onMouseMove only,
        // so a bare alt-click cannot also select the chunk the drag started on.
        if (params.mod_shift_down && !params.mod_ctrl_down && !params.mod_alt_down)
        {
            applySelection(false);
        }
        else if (params.mod_ctrl_down && !params.mod_shift_down && !params.mod_alt_down)
        {
            applySelection(true);
        }
    }

    void ChunkTool::onMouseMove(MouseMoveParameters const& params)
    {
        if (!params.left_mouse || !_chunkManipulator)
        {
            return;
        }

        // The radius drag, copied verbatim from StampTool.cpp:135-138 including the XSENS
        // divisor, so that sizing a brush feels the same in every tool that offers it.
        if (params.mod_alt_down && !params.mod_shift_down && !params.mod_ctrl_down)
        {
            _chunkManipulator->changeSelectionRadius(params.relative_movement.dx() / XSENS);
            return;
        }

        // Dragging with shift or ctrl held keeps painting the selection, which is how a
        // non-circular region gets picked without clicking 200 times.
        if (params.mod_shift_down && !params.mod_ctrl_down)
        {
            applySelection(false);
        }
        else if (params.mod_ctrl_down && !params.mod_shift_down)
        {
            applySelection(true);
        }
    }

    void ChunkTool::onMouseWheel(MouseWheelParameters const& params)
    {
        if (!params.mod_shift_down || !_chunkManipulator)
        {
            return;
        }

        // angleDelta is in eighths of a degree and a detent is 120 of them, so this is one unit
        // of height per notch.
        float const notches(static_cast<float>(params.event.angleDelta().y()) / 120.0f);
        _chunkManipulator->changeHeightOffset(notches);
        params.event.accept();
    }
}
