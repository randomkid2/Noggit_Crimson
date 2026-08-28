// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include "FlattenBlurTool.hpp"

#include <noggit/ActionManager.hpp>
#include <noggit/Camera.hpp>
#include <noggit/Input.hpp>
#include <noggit/MapView.h>
#include <noggit/terrain/LiveAutoTexture.hpp>
#include <noggit/ui/FlattenTool.hpp>
#include <noggit/ui/tools/ToolPanel/ToolPanel.hpp>
#include <noggit/ui/tools/ViewToolbar/Ui/ViewToolbar.hpp>
#include <noggit/World.h>

#include <QWheelEvent>
#include <QtWidgets/QPushButton>

#include <glm/geometric.hpp>
#include <glm/mat4x4.hpp>

#include <algorithm>
#include <array>
#include <cmath>

namespace Noggit
{
    FlattenBlurTool::FlattenBlurTool(MapView* mapView)
        : Tool{ mapView }
    {
        addHotkey("nextType"_hash, Hotkey{
            .onPress = [this] { _flattenTool->nextFlattenType(); },
            .condition = [mapView] { return mapView->get_editing_mode() == editing_mode::flatten_blur && !NOGGIT_CUR_ACTION; },
            });

        addHotkey("toggleAngle"_hash, Hotkey{
            .onPress = [this] { _flattenTool->toggleFlattenAngle(); },
            .condition = [mapView] { return mapView->get_editing_mode() == editing_mode::flatten_blur && !NOGGIT_CUR_ACTION; },
            });

        addHotkey("nextMode"_hash, Hotkey{
            .onPress = [this, mv = mapView]
            {
                mv->getLeftSecondaryViewToolbar()->nextFlattenMode();
                _flattenTool->nextFlattenMode();
            },
            .condition = [mapView] { return mapView->get_editing_mode() == editing_mode::flatten_blur && !NOGGIT_CUR_ACTION; },
            });

        addHotkey("toggleLock"_hash, Hotkey{
            .onPress = [this] { _flattenTool->toggleFlattenLock(); },
            .condition = [mapView] { return mapView->get_editing_mode() == editing_mode::flatten_blur && !NOGGIT_CUR_ACTION; },
            });

        addHotkey("lockCursor"_hash, Hotkey{
            .onPress = [this, mv = mapView] { _flattenTool->lockPos(mv->cursorPosition()); },
            .condition = [mapView] { return mapView->get_editing_mode() == editing_mode::flatten_blur && !NOGGIT_CUR_ACTION; },
            });

        addHotkey("increaseRadius"_hash, Hotkey{
            .onPress = [this] { _flattenTool->changeRadius(0.01f); },
            .condition = [mapView] { return mapView->get_editing_mode() == editing_mode::flatten_blur && !NOGGIT_CUR_ACTION; },
            });

        addHotkey("decreaseRadius"_hash, Hotkey{
            .onPress = [this] { _flattenTool->changeRadius(-0.01f); },
            .condition = [mapView] { return mapView->get_editing_mode() == editing_mode::flatten_blur && !NOGGIT_CUR_ACTION; },
            });

        // CLEARING THE TWO RAMP POINTS, on C rather than on Escape.
        //
        // Upstream's ramp branch uses Escape. Escape is not available here: MapView binds it, with
        // no hotkey name, to closing the main window (MapView.cpp:4488), so claiming it means
        // editing MapView -- which this change does not own -- and means a mapper who misses the
        // ramp gesture quits the editor. "clearVertexSelection" is already bound to C with no
        // modifier (MapView.cpp:4360) and dispatches to whichever tool is active, and RaiseLowerTool
        // uses it for exactly this shape of thing: throw away the aim I have set up. Nothing in
        // flatten mode claimed it before, so this takes no key away from anyone.
        //
        // Gated on rampMode() as well as on the editing mode, so pressing C while flattening
        // normally still does nothing at all rather than silently discarding points the user set
        // ten minutes ago and can no longer see.
        addHotkey("clearVertexSelection"_hash, Hotkey{
            .onPress = [this] { _flattenTool->clearRampPoints(); },
            .condition = [this, mapView] {
                return mapView->get_editing_mode() == editing_mode::flatten_blur
                    && _flattenTool && _flattenTool->rampMode() && !NOGGIT_CUR_ACTION;
            },
            });
    }

    FlattenBlurTool::~FlattenBlurTool()
    {
        delete _flattenTool;
    }

    char const* FlattenBlurTool::name() const
    {
        return "Flatten | Blur";
    }

    editing_mode FlattenBlurTool::editingMode() const
    {
        return editing_mode::flatten_blur;
    }

    Ui::FontNoggit::Icons FlattenBlurTool::icon() const
    {
        return Ui::FontNoggit::TOOL_FLATTEN_BLUR;
    }

    void FlattenBlurTool::setupUi(Ui::Tools::ToolPanel* toolPanel)
    {
        _flattenTool = new Noggit::Ui::flatten_blur_tool(mapView());
        toolPanel->registerTool(this, _flattenTool);
    }

    void FlattenBlurTool::postUiSetup()
    {
        // The panel's Build button and shift + right click reach the same function, so the ramp
        // is one undo step whichever way it was started. The button exists because the width and
        // falloff can be changed after the two points are down, and re-clicking to rebuild would
        // mean re-aiming.
        QObject::connect(_flattenTool->rampBuildButton(), &QPushButton::clicked
            , [this] { buildRamp(); }
        );

        QObject::connect(mapView()->getLeftSecondaryViewToolbar()
            , &Ui::Tools::ViewToolbar::Ui::ViewToolbar::updateStateRaise
            , [this](bool newState)
            {
                _flattenTool->_flatten_mode.raise = newState;
            }
        );

        QObject::connect(mapView()->getLeftSecondaryViewToolbar()
            , &Ui::Tools::ViewToolbar::Ui::ViewToolbar::updateStateLower
            , [this](bool newState)
            {
                _flattenTool->_flatten_mode.lower = newState;
            }
        );
    }

    void FlattenBlurTool::onTick(float deltaTime, TickParameters const& params)
    {
        if (!mapView()->getWorld()->has_selection() || !params.left_mouse)
        {
            return;
        }

        if (params.displayMode == display_mode::in_3D && !params.underMap)
        {
            // Shift + left belongs to the ramp while ramp mode is on -- it is the gesture that
            // sets the start point, handled in onMousePress -- so the flatten stroke stands down.
            // Without this the same held button would set a point AND flatten the ground under
            // the cursor for as long as it was held, which is the opposite of aiming.
            //
            // Ctrl + left below is deliberately NOT gated: blur is what a mapper reaches for
            // immediately after grading a ramp, and the ramp claims neither ctrl nor the falloff
            // type the blur reads.
            if (params.mod_shift_down && !_flattenTool->rampMode())
            {
                NOGGIT_ACTION_MGR->beginAction(mapView(), Noggit::ActionFlags::eCHUNKS_TERRAIN,
                    Noggit::ActionModalityControllers::eSHIFT
                    | Noggit::ActionModalityControllers::eLMB);
                _flattenTool->flatten(mapView()->getWorld(), mapView()->cursorPosition(), deltaTime);
            }
            else if (params.mod_ctrl_down)
            {

                NOGGIT_ACTION_MGR->beginAction(mapView(), Noggit::ActionFlags::eCHUNKS_TERRAIN,
                    Noggit::ActionModalityControllers::eCTRL
                    | Noggit::ActionModalityControllers::eLMB);
                _flattenTool->blur(mapView()->getWorld(), mapView()->cursorPosition(), deltaTime);
            }
        }
    }

    ToolDrawParameters FlattenBlurTool::drawParameters() const
    {
        return
        {
            .radius = _flattenTool->brushRadius(),
            .angle = _flattenTool->angle(),
            .orientation = _flattenTool->orientation(),
            .ref_pos = _flattenTool->ref_pos(),
            .angled_mode = _flattenTool->angled_mode(),
            .use_ref_pos = _flattenTool->use_ref_pos(),
        };
    }

    void FlattenBlurTool::onMousePress(MousePressParameters const& params)
    {
        if (!_flattenTool->rampMode() || !params.mod_shift_down)
        {
            return;
        }

        // Shift + left sets the start, shift + right sets the end and builds, which is the
        // gesture haloreach252's branch uses and the reason the two ends are told apart at all:
        // the grade runs from the first to the second, so swapping them inverts the slope.
        //
        // Shift + right also starts MapView's camera look (MapView.cpp:6539 sets `look` on any
        // right press, before this hook is even reached). A click that does not drag rotates the
        // camera by nothing, so the two coexist; a click that drags will pan the view while
        // setting the point, which is worth knowing but is not worth taking the button away from
        // the camera to avoid.
        auto* const mv (mapView());
        glm::vec3 const cursor (mv->cursorPosition());

        // The ramp is defined by two places ON the terrain, so a click that did not land on any
        // is not a point. isUnderMap is the same question onTick asks before letting a brush run.
        if (mv->getWorld()->isUnderMap(cursor))
        {
            return;
        }

        if (params.button == Qt::MouseButton::LeftButton)
        {
            _flattenTool->setRampStart(cursor);
        }
        else if (params.button == Qt::MouseButton::RightButton)
        {
            _flattenTool->setRampEnd(cursor);
            buildRamp();
        }
    }

    void FlattenBlurTool::buildRamp()
    {
        if (!_flattenTool->hasRampStart() || !_flattenTool->hasRampEnd())
        {
            return;
        }

        glm::vec3 const start (_flattenTool->rampStart());
        glm::vec3 const end (_flattenTool->rampEnd());

        float const dx (end.x - start.x);
        float const dz (end.z - start.z);

        // The same thousandth of a unit World::buildRamp refuses, checked here so a degenerate
        // ramp does not open an action at all. ActionManager::endAction pushes whatever it is given
        // (ActionManager.cpp:126-129), so an action that changes nothing still costs the user a
        // Ctrl+Z that appears to do nothing.
        if (dx * dx + dz * dz < 0.001f * 0.001f)
        {
            return;
        }

        auto* const mv (mapView());

        // ONE ACTION FOR THE WHOLE RAMP, and bracketing it here rather than inside
        // World::buildRamp is what makes that true. A ramp crosses as many chunks as it is long;
        // World::buildRamp calls Action::registerChunkTerrainChange on every one of them, and
        // they all land on whichever action is open, so a single begin/end pair around the lot
        // makes the entire graded run one undo step instead of one per chunk.
        //
        // No modality controllers: this is not a drag. The action opens and closes inside this
        // one call, so it never reaches the modality-mismatch close in MapView::tick.
        NOGGIT_ACTION_MGR->beginAction(mv, Noggit::ActionFlags::eCHUNKS_TERRAIN);

        if (_flattenTool->buildRamp(mv->getWorld()))
        {
            // Live Auto Texture, run explicitly for the same reason the Flatten hotkey does it
            // (RaiseLowerTool.cpp:31-37): this edit is not a drag, so it never passes through the
            // modality mismatch runIfStrokeEnding hooks and that hook would never see it. Called
            // before endAction because endAction takes the redo snapshot -- paint registered
            // after finish() would have a before-image and no after-image. A no-op unless the
            // user has turned the feature on, and a graded road is the single most obvious thing
            // to want auto-textured.
            Noggit::LiveAutoTexture::runNow(mv);
        }

        NOGGIT_ACTION_MGR->endAction();
    }

    std::vector<glm::vec3> FlattenBlurTool::rampOutline(float corner_radius) const
    {
        // The outline of everything within `corner_radius` of the ramp's flat core, which is the
        // isoline of the `outside` distance World::buildRamp computes per vertex. At radius 0
        // that is the core rectangle; at the falloff width it is the outer edge of any effect at
        // all, with ROUNDED corners -- because the distance to a rectangle is rounded at its
        // corners, and drawing a square outline there would claim an influence the maths does not
        // have.
        std::vector<glm::vec3> points;

        glm::vec3 const start (_flattenTool->rampStart());
        glm::vec3 const end (_flattenTool->rampEnd());

        float const dx (end.x - start.x);
        float const dz (end.z - start.z);
        float const run (std::sqrt(dx * dx + dz * dz));

        if (run < 0.001f)
        {
            return points;
        }

        float const ux (dx / run);
        float const uz (dz / run);

        // The perpendicular, in the XZ plane. (-uz, ux) is (ux, uz) turned a quarter turn, so the
        // pair is an orthonormal frame and a point is (t along, s across) with no further work.
        float const px (-uz);
        float const pz (ux);

        float const half_width (_flattenTool->rampHalfWidth());

        // Lifted clear of the surface it describes. WorldRender uses 0.1 for the same job on its
        // angled-mode square (WorldRender.cpp:1538); 0.15 here because this outline sits over
        // ground the ramp is about to move rather than over ground that is standing still, and
        // the extra 0.05 costs nothing at any camera distance a mapper works at.
        float const lift (0.15f);

        // Six segments across each 90 degree corner. Four corners at seven points each is 28,
        // plus one to close the strip: 29 points, re-uploaded per frame by Line::draw. The sagitta
        // of a 6-segment quarter circle is 1 - cos(7.5 degrees) = 0.86% of the radius, so a 20
        // yard falloff band draws its corners 0.17 yards inside the true arc -- about the same as
        // the lift above, and an order of magnitude under anything a mapper is aiming at.
        int const segments_per_corner (6);

        // Each corner is an arc of the offset boundary; the straight edges between them are drawn
        // for free by the line strip joining one arc's last point to the next arc's first. Angles
        // are measured from +along toward +across, so +across is 90 degrees and +along is 0, and
        // the four arcs run clockwise in that frame: 90 to 0 at the far side of `end`, 0 to -90,
        // -90 to -180, and 180 back to 90.
        struct Corner
        {
            float centre_x;
            float centre_z;
            float from_degrees;
            float to_degrees;
        };

        std::array<Corner, 4> const corners
        {{
            { end.x + px * half_width,   end.z + pz * half_width,     90.0f,    0.0f }
          , { end.x - px * half_width,   end.z - pz * half_width,      0.0f,  -90.0f }
          , { start.x - px * half_width, start.z - pz * half_width,  -90.0f, -180.0f }
          , { start.x + px * half_width, start.z + pz * half_width,  180.0f,   90.0f }
        }};

        float const radius (std::max(0.0f, corner_radius));

        points.reserve(static_cast<std::size_t>(corners.size() * (segments_per_corner + 1) + 1));

        for (auto const& corner : corners)
        {
            // A zero radius collapses the arc onto its centre, so emit the corner once rather
            // than seven identical points.
            int const steps (radius > 0.0f ? segments_per_corner : 0);

            for (int step = 0; step <= steps; ++step)
            {
                float const angle_degrees
                    ( corner.from_degrees
                    + (corner.to_degrees - corner.from_degrees)
                      * (steps > 0 ? static_cast<float>(step) / static_cast<float>(steps) : 0.0f)
                    );

                // A direction in the (along, across) frame: cos along, sin across. 0 degrees is
                // +along and 90 is +across, which is what the table above is written against.
                float const angle_radians (angle_degrees * 3.14159265358979f / 180.0f);
                float const cos_a (std::cos(angle_radians));
                float const sin_a (std::sin(angle_radians));

                float const x (corner.centre_x + (ux * cos_a + px * sin_a) * radius);
                float const z (corner.centre_z + (uz * cos_a + pz * sin_a) * radius);

                // The height the ramp is grading toward at this point, so the outline lies in the
                // finished surface rather than on the terrain that is about to be replaced. Past
                // either end the clamp holds it at that end's height, which is what buildRamp
                // does with the same t.
                float const t
                    (std::clamp(((x - start.x) * ux + (z - start.z) * uz) / run, 0.0f, 1.0f));

                points.emplace_back(x, start.y + (end.y - start.y) * t + lift, z);
            }
        }

        if (!points.empty())
        {
            points.push_back(points.front());
        }

        return points;
    }

    void FlattenBlurTool::postRender()
    {
        if (!_flattenTool->rampMode())
        {
            return;
        }

        bool const has_start (_flattenTool->hasRampStart());
        bool const has_end (_flattenTool->hasRampEnd());

        if (!has_start && !has_end)
        {
            return;
        }

        auto* const mv (mapView());
        glm::mat4x4 const mvp (mv->projection() * mv->model_view());
        glm::vec3 const camera (mv->getCamera()->position);

        // Two colours, because the two points are not interchangeable: the grade runs from start
        // to end, and swapping them inverts the slope. Green for the start, orange for the end,
        // which is the order the gesture puts them down in.
        glm::vec4 const start_color (0.25f, 0.95f, 0.4f, 1.0f);
        glm::vec4 const end_color (1.0f, 0.55f, 0.15f, 1.0f);

        // Scaled with distance, the way WorldRender sizes the multi-selection pivot sphere
        // (WorldRender.cpp:383): a fixed world radius is a boulder from the ground and invisible
        // from a working camera height. The band is wider than the pivot's 0.15 to 2 because a
        // ramp is aimed from further away than a selection is inspected from -- these markers
        // have to be findable while the mapper is looking at the whole hillside.
        auto&& marker_radius
        ([&camera](glm::vec3 const& point)
            {
                return std::clamp(glm::distance(camera, point) * 0.02f, 0.5f, 4.0f);
            }
        );

        if (has_start)
        {
            glm::vec3 const point (_flattenTool->rampStart());
            _rampPointRenderer.draw(mvp, point, start_color, marker_radius(point));
        }

        if (has_end)
        {
            glm::vec3 const point (_flattenTool->rampEnd());
            _rampPointRenderer.draw(mvp, point, end_color, marker_radius(point));
        }

        if (!has_start || !has_end)
        {
            return;
        }

        // The footprint: the flat core in the start colour, and the outer edge of the falloff in
        // a dimmed version of the same hue. Two loops rather than one because "where the ground
        // ends up exactly flat" and "where the tool stops touching anything" are different
        // questions and a mapper aiming a road wants both answered.
        //
        // Dimmed by scaling the RGB and NOT by dropping the alpha, because nothing here can
        // promise GL_BLEND is on: WorldRender enables it for its own overlays inside draw(), and
        // this hook runs after that returns (MapView.cpp:5086). A translucent line would be a
        // coin toss; a darker one reads the same either way.
        std::vector<glm::vec3> const core (rampOutline(0.0f));

        if (core.size() >= 2)
        {
            _rampOutlineRenderer.draw(mvp, core, start_color, false);
        }

        float const falloff (_flattenTool->rampFalloffWidth());

        if (falloff > 0.0f)
        {
            std::vector<glm::vec3> const outer (rampOutline(falloff));

            if (outer.size() >= 2)
            {
                _rampOutlineRenderer.draw
                    ( mvp
                    , outer
                    , glm::vec4 (start_color.x * 0.45f, start_color.y * 0.45f, start_color.z * 0.45f, 1.0f)
                    , false
                    );
            }
        }

        // The centre line, at the two picked points themselves, so the grade the read-out quotes
        // has a line in the world to go with it.
        std::vector<glm::vec3> const centre
        {
            glm::vec3(_flattenTool->rampStart().x, _flattenTool->rampStart().y + 0.15f, _flattenTool->rampStart().z)
          , glm::vec3(_flattenTool->rampEnd().x, _flattenTool->rampEnd().y + 0.15f, _flattenTool->rampEnd().z)
        };

        _rampOutlineRenderer.draw(mvp, centre, end_color, false);
    }

    void FlattenBlurTool::onMouseMove(MouseMoveParameters const& params)
    {
        if (params.left_mouse)
        {
            if (params.mod_alt_down && !params.mod_shift_down && !params.mod_ctrl_down)
            {
                _flattenTool->changeRadius(params.relative_movement.dx() / XSENS);
            }

            if (params.mod_space_down)
            {
                _flattenTool->changeSpeed(params.relative_movement.dx() / 30.0f);
            }
        }
    }

    void FlattenBlurTool::onMouseWheel(MouseWheelParameters const& params)
    {
        auto&& delta_for_range
        ([&](float range)
            {
                //! \note / 8.f for degrees, / 40.f for smoothness
                return (params.mod_ctrl_down ? 0.01f : 0.1f)
                    * range
                    // alt = horizontal delta
                    * (params.mod_alt_down ? params.event.angleDelta().x() : params.event.angleDelta().y())
                    / 320.f
                    ;
            }
        );

        if (params.mod_alt_down)
        {
            _flattenTool->changeOrientation(delta_for_range(360.f));
        }
        else if (params.mod_shift_down)
        {
            _flattenTool->changeAngle(delta_for_range(89.f));
        }
        else if (params.mod_space_down)
        {
            //! \note not actual range
            _flattenTool->changeHeight(delta_for_range(40.f));
        }
    }
}
