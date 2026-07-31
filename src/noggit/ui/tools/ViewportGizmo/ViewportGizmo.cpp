#include "ViewportGizmo.hpp"

#include <noggit/Action.hpp>
#include <noggit/ActionManager.hpp>
#include <noggit/application/Configuration/NoggitApplicationConfiguration.hpp>
#include <noggit/application/NoggitApplication.hpp>
#include <noggit/MapView.h>
#include <noggit/ModelInstance.h>
#include <noggit/WMOInstance.h>
#include <noggit/World.h>

#include <external/glm/glm.hpp>
#include <external/glm/gtc/quaternion.hpp>
#include <external/glm/gtc/type_ptr.hpp>
#include <external/glm/gtx/matrix_decompose.hpp>
#include <external/glm/gtx/string_cast.hpp>

using namespace Noggit::Ui::Tools::ViewportGizmo;

namespace
{
  // The delta matrix ImGuizmo::Manipulate fills, decomposed the way every consumer here needs it.
  //
  // Shared rather than copied because the conjugate is not obvious: without it every rotation
  // comes back with the opposite sign, and a second hand-written copy is exactly where that gets
  // lost.
  void decomposeGizmoDelta( glm::mat4x4 const& delta_matrix
                          , glm::vec3& scale
                          , glm::quat& orientation
                          , glm::vec3& translation
                          )
  {
    glm::vec3 skew_;
    glm::vec4 perspective_;

    glm::decompose(delta_matrix, scale, orientation, translation, skew_, perspective_);

    orientation = glm::conjugate(orientation);
  }
}

ViewportGizmo::ViewportGizmo(Noggit::Ui::Tools::ViewportGizmo::GizmoContext gizmo_context, World* world)
: _gizmo_context(gizmo_context)
, _world(world)
{
}

bool ViewportGizmo::drawsForSelection(std::vector<selection_type> const& selection)
{
  std::size_t const n_selected = selection.size();

  // A single non-object entry -- in practice the MapChunk doSelection puts under the cursor every
  // frame -- gives the gizmo nothing to transform. Two or more entries take the multiselection
  // pivot path, which draws regardless of what the entries are.
  return !(!n_selected || (n_selected == 1 && selection[0].index() != eEntry_Object));
}

void ViewportGizmo::handleTransformGizmo(MapView* map_view
                                        , const std::vector<selection_type>& selection
                                        , glm::mat4x4 const& model_view
                                        , glm::mat4x4 const& projection)
{

  if (!isUsing())
  {
    _last_pivot_scale = 1.f;
  }

  GizmoInternalMode gizmo_selection_type;

  auto model_view_trs = model_view;
  auto projection_trs = projection;

  int n_selected = static_cast<int>(selection.size());

  // The one place this question is answered. Callers deciding whether the object gizmo owns the
  // frame ask the same function, so no second gizmo can ever be drawn into a frame this one claims.
  if (!drawsForSelection(selection))
    return;

  if (n_selected == 1)
  {
    gizmo_selection_type = std::get<selected_object_type>(selection[0])->which() == eMODEL ? GizmoInternalMode::MODEL : GizmoInternalMode::WMO;
  }
  else
  {
    gizmo_selection_type = GizmoInternalMode::MULTISELECTION;
  }

  SceneObject* obj_instance;

  beginGizmoFrame();

  glm::mat4x4 delta_matrix = glm::mat4x4(1.0f);
  glm::mat4x4 object_matrix = glm::mat4x4(1.0f);
  glm::mat4x4 pivot_matrix = glm::translate(glm::mat4x4(1.f),
                                                   {_multiselection_pivot.x,
                                                    _multiselection_pivot.y,
                                                    _multiselection_pivot.z });
  float last_pivot_scale = 1.f;

  switch (gizmo_selection_type)
  {
    case MODEL:
    case WMO:
    {
      obj_instance = std::get<selected_object_type>(selection[0]);
      obj_instance->ensureExtents();
      object_matrix = obj_instance->transformMatrix();
      ImGuizmo::Manipulate(glm::value_ptr(model_view_trs), glm::value_ptr(projection_trs), _gizmo_operation, _gizmo_mode, glm::value_ptr(object_matrix), glm::value_ptr(delta_matrix), nullptr);
      break;
    }
    case MULTISELECTION:
    {
      if (isUsing())
        _last_pivot_scale = ImGuizmo::GetOperationScaleLast();

      ImGuizmo::Manipulate(glm::value_ptr(model_view_trs), glm::value_ptr(projection_trs), _gizmo_operation, _gizmo_mode, glm::value_ptr(pivot_matrix), glm::value_ptr(delta_matrix), nullptr);
      break;
    }
  }

  if (!isUsing())
  {
    return;
  }

  glm::vec3 new_scale;
  glm::quat new_orientation;
  glm::vec3 new_translation;

  decomposeGizmoDelta(delta_matrix, new_scale, new_orientation, new_translation);

  // if nothing was changed, just return early
  switch (_gizmo_operation)
  {
    case ImGuizmo::TRANSLATE:
    {
      if (new_translation.x == 0.0f && new_translation.y == 0.0f && new_translation.z == 0.0f)
        return;
      break;
    }
    case ImGuizmo::ROTATE:
    {

      if (new_orientation.x == -0.0f && new_orientation.y == -0.0f && new_orientation.z == -0.0f)
        return;
      break;
    }
    case ImGuizmo::SCALE:
    {
      if (new_scale.x == 1.0f && new_scale.y == 1.0f && new_scale.z == 1.0f)
        return;
      break;
    }
    case ImGuizmo::BOUNDS:
    {
      throw std::logic_error("Bounds are not supported by this gizmo.");
    }
  }

  NOGGIT_ACTION_MGR->beginAction(map_view, Noggit::ActionFlags::eOBJECTS_TRANSFORMED,
                                                 Noggit::ActionModalityControllers::eLMB);

  bool modern_features = Noggit::Application::NoggitApplication::instance()->getConfiguration()->modern_features;

  if (gizmo_selection_type == MULTISELECTION)
  {

    for (auto& selected : selection)
    {

      if (selected.index() != eEntry_Object)
        continue;

      obj_instance = std::get<selected_object_type>(selected);
      NOGGIT_CUR_ACTION->registerObjectTransformed(obj_instance);

      obj_instance->ensureExtents();
      object_matrix = obj_instance->transformMatrix();

      glm::vec3& pos = obj_instance->pos;
      math::degrees::vec3& rotation = obj_instance->dir;
      float& scale = obj_instance->scale;

      // If modern features are disabled, we don't want to scale WMOs
      if (obj_instance->which() == eWMO && !modern_features && _gizmo_operation == ImGuizmo::SCALE)
      {
          scale = 1.0f;
          continue;
      }

      if (_world)
        _world->updateTilesEntry(selected, model_update::remove);

      switch (_gizmo_operation)
      {
        case ImGuizmo::TRANSLATE:
        {
            pos += glm::vec3(new_translation.x, new_translation.y, new_translation.z);
          break;
        }
        case ImGuizmo::ROTATE:
        {
          auto rot_euler = glm::degrees(glm::eulerAngles(new_orientation).operator*=(-1.f));
          auto rot_euler_pivot = glm::eulerAngles(new_orientation);

          if (!_use_multiselection_pivot)
          {
            rotation += glm::vec3(math::degrees(rot_euler.x)._, math::degrees(rot_euler.y)._, math::degrees(rot_euler.z)._);
          }
          else
          {
            //LogDebug << rot_euler.x << " " << rot_euler.y << " " << rot_euler.z << std::endl;

            /*float alpha = math::degrees(rot_euler.x)._;
            float beta = math::degrees(rot_euler.y)._;
            float gamma = math::degrees(rot_euler.z)._;

            glm::mat3 rotation_matrix = glm::mat3(
                        cos(beta)*cos(gamma),                                    -cos(beta)*sin(gamma),                                   sin(beta),
                        cos(alpha)*sin(gamma) + cos(gamma)*sin(alpha)*sin(beta), cos(alpha)*cos(gamma) - sin(alpha)*sin(beta)*sin(gamma), -cos(beta)*sin(alpha),
                        sin(alpha)*sin(gamma) - cos(alpha)*cos(gamma)*sin(beta), cos(gamma)*sin(alpha) + cos(alpha)*sin(beta)*sin(gamma), cos(alpha)*cos(beta)
                        );

            rotation.x += atan(-rotation_matrix[1].z / rotation_matrix[2].z);
            rotation.z += atan(-rotation_matrix[0].y / rotation_matrix[0].x);*/

            float beta = math::degrees(rot_euler.y)._;
            rotation.y += atan(sin(beta) / (sqrt(1 - pow(sin(beta), 2))));

            // building model matrix
            glm::mat4 model_transform = object_matrix;

            // only translation of pivot
            glm::mat4 transformed_pivot = pivot_matrix;

            // model matrix relative to translated pivot
            glm::mat4 model_transform_rel = glm::inverse(transformed_pivot) * model_transform;

            // rotate multiselection in same direction as user want
            glm::mat4 gizmo_rotation = glm::mat4_cast(glm::quat(new_orientation.w, glm::eulerAngles(new_orientation).operator*=(-1.f)));

            glm::mat4 _transformed_pivot_rot = transformed_pivot * gizmo_rotation;

            // apply transform to model matrix
            glm::mat4 result_matrix = _transformed_pivot_rot * model_transform_rel;

            glm::vec3 rot_result_scale;
            glm::quat rot_result_orientation;
            glm::vec3 rot_result_translation;
            glm::vec3 rot_result_skew_;
            glm::vec4 rot_result_perspective_;

            glm::decompose(result_matrix,
                           rot_result_scale,
                           rot_result_orientation,
                           rot_result_translation,
                           rot_result_skew_,
                           rot_result_perspective_
            );

            rot_result_orientation = glm::conjugate(rot_result_orientation);

            auto rot_result_orientation_euler = glm::degrees(glm::eulerAngles(rot_result_orientation));

            pos = {rot_result_translation.x, rot_result_translation.y, rot_result_translation.z};
            //rotation = {rot_result_orientation_euler.x, rot_result_orientation_euler.y, rot_result_orientation_euler.z};

          }

          break;
        }
        case ImGuizmo::SCALE:
        {
          scale = std::max(0.001f, scale * (new_scale.x / _last_pivot_scale));
          break;
        }
        case ImGuizmo::BOUNDS:
        {
          throw std::logic_error("Bounds are not supported by this gizmo.");
          break;
        }
      }
      obj_instance->recalcExtents();

      if (_world)
        _world->updateTilesEntry(selected, model_update::add);
    }
  }
  else
  {
    for (auto& selected : selection)
    {
      if (selected.index() != eEntry_Object)
        continue;

      obj_instance = std::get<selected_object_type>(selected);
      NOGGIT_CUR_ACTION->registerObjectTransformed(obj_instance);

      obj_instance->ensureExtents();
      object_matrix = obj_instance->transformMatrix();

      glm::vec3& pos = obj_instance->pos;
      math::degrees::vec3& rotation = obj_instance->dir;
      float& scale = obj_instance->scale;

      // If modern features are disabled, we don't want to scale WMOs
      if (obj_instance->which() == eWMO && !modern_features && _gizmo_operation == ImGuizmo::SCALE)
      {
          scale = 1.0f;
          continue;
      }

      if (_world)
        _world->updateTilesEntry(selected, model_update::remove);

      switch (_gizmo_operation)
      {

        case ImGuizmo::TRANSLATE:
        {
            pos += glm::vec3(new_translation.x, new_translation.y, new_translation.z);
          break;
        }
        case ImGuizmo::ROTATE:
        {
          auto rot_euler = glm::eulerAngles(new_orientation).operator*=(-1.f) * 57.2957795f;
          rotation += glm::vec3(math::degrees(rot_euler.x)._, math::degrees(rot_euler.y)._, math::degrees(rot_euler.z)._);
          break;
        }
        case ImGuizmo::SCALE:
        {
          scale = std::max(0.001f, new_scale.x);
          break;
        }
        case ImGuizmo::BOUNDS:
        {
          throw std::logic_error("Bounds are not supported by this gizmo.");
        }
      }

      obj_instance->normalizeDirection();

      obj_instance->recalcExtents();


      if (map_view)
      {
          emit map_view->rotationChanged();
      }

      if (_world)
        _world->updateTilesEntry(selected, model_update::add);
    }
  }
  if (_world)
    _world->update_selected_model_groups();

  _world->update_selection_pivot();
}

void ViewportGizmo::ViewportGizmo::beginGizmoFrame()
{
  ImGuizmo::SetID(_gizmo_context);

  ImGuizmo::SetDrawlist();

  ImGuizmo::SetOrthographic(false);
  ImGuizmo::SetScaleGizmoAxisLock(true);
  ImGuizmo::BeginFrame();

  ImGuiIO& io = ImGui::GetIO();
  ImGuizmo::SetRect(0, 0, io.DisplaySize.x, io.DisplaySize.y);
}

Noggit::Ui::Tools::ViewportGizmo::ViewportGizmo::GizmoDelta
  ViewportGizmo::ViewportGizmo::handleDetachedGizmo( glm::vec3 const& position
                                                   , glm::mat4x4 const& model_view
                                                   , glm::mat4x4 const& projection
                                                   )
{
  GizmoDelta result;

  // A caller with nowhere to put a scale must not be handed a scale handle. Substituted rather
  // than refused so the gizmo stays usable when the shared toolbar happens to be on SCALE -- the
  // arrows instead of boxes are the visible answer to "why did nothing scale".
  ImGuizmo::OPERATION const operation
    ( _gizmo_operation == ImGuizmo::TRANSLATE || _gizmo_operation == ImGuizmo::ROTATE
        ? _gizmo_operation
        : ImGuizmo::TRANSLATE
    );

  // Copied because ImGuizmo takes non-const float*, exactly as handleTransformGizmo does.
  auto model_view_trs = model_view;
  auto projection_trs = projection;

  beginGizmoFrame();

  glm::mat4x4 delta_matrix = glm::mat4x4(1.0f);
  glm::mat4x4 object_matrix = glm::translate(glm::mat4x4(1.f), position);

  ImGuizmo::Manipulate( glm::value_ptr(model_view_trs)
                      , glm::value_ptr(projection_trs)
                      , operation
                      , _gizmo_mode
                      , glm::value_ptr(object_matrix)
                      , glm::value_ptr(delta_matrix)
                      , nullptr
                      );

  if (!isUsing())
  {
    return result;
  }

  glm::vec3 new_scale;
  glm::quat new_orientation;
  glm::vec3 new_translation;

  decomposeGizmoDelta(delta_matrix, new_scale, new_orientation, new_translation);

  result.active = true;

  if (operation == ImGuizmo::TRANSLATE)
  {
    result.translation = new_translation;
  }
  else
  {
    // The same extraction the single-object ROTATE branch performs, kept identical on purpose.
    auto const rot_euler = glm::degrees(glm::eulerAngles(new_orientation).operator*=(-1.f));
    result.yaw_degrees = rot_euler.y;
  }

  return result;
}

void ViewportGizmo::ViewportGizmo::setCurrentGizmoOperation(ImGuizmo::OPERATION operation)
{
  _gizmo_operation = operation;
}

void ViewportGizmo::ViewportGizmo::setCurrentGizmoMode(ImGuizmo::MODE mode)
{
  _gizmo_mode = mode;
}

bool ViewportGizmo::ViewportGizmo::isOver() const
{
  ImGuizmo::SetID(_gizmo_context);
  return ImGuizmo::IsOver();
}

bool ViewportGizmo::ViewportGizmo::isUsing() const
{
  ImGuizmo::SetID(_gizmo_context);
  return ImGuizmo::IsUsing();
}

void ViewportGizmo::ViewportGizmo::setUseMultiselectionPivot(bool use_pivot)
{
  _use_multiselection_pivot = use_pivot;
}

void ViewportGizmo::ViewportGizmo::setMultiselectionPivot(glm::vec3 const& pivot)
{
  _multiselection_pivot = pivot;
}

void ViewportGizmo::ViewportGizmo::setWorld(World* world)
{
  _world = world;
}

