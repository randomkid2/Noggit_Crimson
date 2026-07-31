#ifndef NOGGIT_VIEWPORTGIZMO_HPP
#define NOGGIT_VIEWPORTGIZMO_HPP

#include <external/qtimgui/imgui/imgui.h>
#include <external/imguizmo/ImGuizmo.h>
#include <noggit/Selection.h>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <vector>

class MapView;
class World;

namespace Noggit
{
    namespace Ui::Tools::ViewportGizmo
    {
        enum GizmoContext
        {
          MAP_VIEW,
          PRESET_EDITOR
        };

        enum GizmoInternalMode
        {
            MODEL,
            WMO,
            MULTISELECTION
        };

        // This class is intended to be used by QOpenGLWidget presenting an ImGui drawing context.
        // To use pass the context via setImGuiContext() in initializeGL() of a QOpenGLWidget and call the required
        // gizmo method in paintGL()

        class ViewportGizmo
        {
        public:
            // What one frame of dragging produced, for a caller whose target is not a SceneObject.
            //
            // Deltas, not absolutes. ImGuizmo reports the change since the previous frame, and the
            // caller keeps whatever the authoritative value is -- which for a database spawn is a
            // row in `creature`/`gameobject`, not the ModelInstance the gizmo is drawn on.
            struct GizmoDelta
            {
                // The gizmo is being dragged right now. False means nothing below is meaningful.
                bool active = false;

                // World-space translation delta, Noggit frame.
                glm::vec3 translation = glm::vec3(0.f, 0.f, 0.f);

                // Yaw delta about Noggit +Y in degrees, on the same sign convention
                // SceneObject::dir.y uses -- it is extracted from the delta exactly as the
                // single-object path at handleTransformGizmo does, so a spawn and an ADT object
                // dragged by the same ring turn the same way.
                float yaw_degrees = 0.f;
            };

            ViewportGizmo(GizmoContext gizmo_context, World* world = nullptr);

            // Would handleTransformGizmo draw handles for this selection?
            //
            // The condition handleTransformGizmo early-returns on, exposed so a caller can ask it
            // before committing a frame to the object gizmo. handleTransformGizmo itself is the
            // only other user, so the answer cannot drift from the behaviour it describes.
            //
            // Note what it is NOT: "is anything selected". doSelection auto-selects the MapChunk
            // under the cursor on every frame the selection is empty, so World::has_selection() is
            // true after one mouse movement over terrain and stays true. A lone chunk is not a
            // gizmo target -- this returns false for it -- which is exactly the distinction a
            // second gizmo needs in order to know the frame is free.
            [[nodiscard]]
            static bool drawsForSelection(std::vector<selection_type> const& selection);

            void handleTransformGizmo(MapView* map_view
                , std::vector<selection_type> const& selection
                , glm::mat4x4 const& model_view
                , glm::mat4x4 const& projection);

            // Draws a gizmo at an arbitrary world position and reports the drag, touching nothing.
            //
            // The seam for consumers that own their target rather than having it in World's
            // selection. handleTransformGizmo cannot serve them: it is typed on selection_type,
            // and everything it does after the Manipulate call -- registerObjectTransformed,
            // updateTilesEntry, writing SceneObject::pos/dir/scale -- is either meaningless or
            // actively wrong for an object that is not in the ADT scene graph. So this shares the
            // preamble, the Manipulate call and the delta decomposition, and stops there.
            //
            // The matrix is translation only: the caller's target may have no meaningful
            // orientation for the gizmo to align to, and a yaw-only editable is better served by
            // world-aligned rings anyway.
            //
            // Must be called inside a begun ImGui frame. SCALE and BOUNDS are drawn as TRANSLATE,
            // because a caller with no scale to write would otherwise be given a handle whose
            // result is silently discarded.
            [[nodiscard]]
            GizmoDelta handleDetachedGizmo(glm::vec3 const& position
                , glm::mat4x4 const& model_view
                , glm::mat4x4 const& projection);

            void setCurrentGizmoOperation(ImGuizmo::OPERATION operation);
            void setCurrentGizmoMode(ImGuizmo::MODE mode);
            bool isOver() const;;
            bool isUsing() const;;
            void setUseMultiselectionPivot(bool use_pivot);;
            void setMultiselectionPivot(glm::vec3 const& pivot);;
            void setWorld(World* world);

        private:

            // The ImGuizmo per-frame setup both public entry points need, kept in one place so the
            // two cannot drift into configuring the gizmo differently.
            void beginGizmoFrame();

            World* _world;
            ImGuizmo::OPERATION _gizmo_operation;
            ImGuizmo::MODE _gizmo_mode;
            GizmoContext _gizmo_context;
            bool _use_multiselection_pivot;
            glm::vec3 _multiselection_pivot;
            float _last_pivot_scale;
        };
    }
}

#endif //NOGGIT_VIEWPORTGIZMO_HPP
