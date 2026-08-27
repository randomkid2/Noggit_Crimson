// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/ContextObject.hpp>
#include <noggit/MapHeaders.h> // ENTRY_MDDF
#include <noggit/Misc.h> // checkinside
#include <noggit/reports/MissingPlacementLog.hpp>
#include <noggit/Model.h> // Model, etc.
#include <noggit/ModelInstance.h>
#include <noggit/rendering/Primitives.hpp>
#include <noggit/TextureManager.h>
#include <noggit/WMOInstance.h>

#include <ClientFile.hpp>
#include <glm/glm.hpp>
#include <glm/gtx/euler_angles.hpp>
#include <glm/gtx/quaternion.hpp>
#include <glm/vec3.hpp>
#include <math/bounding_box.hpp>
#include <math/frustum.hpp>
#include <math/ray.hpp>

#include <sstream>

ModelInstance::ModelInstance(BlizzardArchive::Listfile::FileKey const& file_key
                             , Noggit::NoggitRenderContext context)
  : SceneObject(SceneObjectTypes::eMODEL, context)
  , model(file_key, context)
{
}

ModelInstance::ModelInstance(BlizzardArchive::Listfile::FileKey const& file_key
                             , ENTRY_MDDF const*d, Noggit::NoggitRenderContext context)
  : SceneObject(SceneObjectTypes::eMODEL, context)
  , model(file_key, context)
{
	uid = d->uniqueID;
	pos = glm::vec3(d->pos[0], d->pos[1], d->pos[2]);
  dir = math::degrees::vec3( math::degrees(d->rot[0])._, math::degrees(d->rot[1])._, math::degrees(d->rot[2])._);
	// scale factor - divide by 1024. blizzard devs must be on crack, why not just use a float?
	scale = d->scale / 1024.0f;
  _need_recalc_extents = true;
}

ModelInstance::ModelInstance(ModelInstance&& other) noexcept
  : SceneObject(other._type, other._context)
  , model(std::move(other.model))
  , light_color(other.light_color)
  , size_cat(other.size_cat)
  , _need_recalc_extents(other._need_recalc_extents)
{
  pos = other.pos;
  dir = other.dir;
  scale = other.scale;
  extents[0] = other.extents[0];
  extents[1] = other.extents[1];
  _transform_mat_inverted = other._transform_mat_inverted;
  _context = other._context;
  uid = other.uid;
}

ModelInstance& ModelInstance::operator= (ModelInstance&& other) noexcept
{
  std::swap(model, other.model);
  std::swap(pos, other.pos);
  std::swap(dir, other.dir);
  std::swap(light_color, other.light_color);
  std::swap(uid, other.uid);
  std::swap(scale, other.scale);
  std::swap(size_cat, other.size_cat);
  std::swap(_need_recalc_extents, other._need_recalc_extents);
  std::swap(extents, other.extents);
  std::swap(_transform_mat_inverted, other._transform_mat_inverted);
  std::swap(_context, other._context);
  return *this;
}


void ModelInstance::draw_box (glm::mat4x4 const& model_view
                             , glm::mat4x4 const& projection
                             , bool is_current_selection
                             , bool draw_collision_box
                             , bool draw_aabb
                             , bool draw_anim_bb
                             )
{
  gl.enable(GL_BLEND);
  gl.blendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

  if (is_current_selection)
  {
    // draw bounding box
    Noggit::Rendering::Primitives::WireBox::getInstance(_context).draw ( model_view
      , projection
      , transformMatrix()
      , {1.0f, 1.0f, 1.0f, 1.0f} // white
      , misc::transform_model_box_coords(model->bounding_box_min)
      , misc::transform_model_box_coords(model->bounding_box_max)
      );

    if (draw_collision_box)
    {
      Noggit::Rendering::Primitives::WireBox::getInstance(_context).draw ( model_view
        , projection
        , transformMatrix()
        , { 1.0f, 1.0f, 0.0f, 1.0f } // yellow
        , misc::transform_model_box_coords(model->collision_box_min)
        , misc::transform_model_box_coords(model->collision_box_max)
        );
    }

    // draw extents
    if (draw_aabb)
    {
      Noggit::Rendering::Primitives::WireBox::getInstance(_context).draw ( model_view
        , projection
        , glm::mat4x4(1)
        , {0.0f, 1.0f, 0.0f, 1.0f} // green
        , extents[0]
        , extents[1]
        );
    }

    // animated bounding box
    // worldRnder already draws it for any model with mesh_bounds_ratio < 0.3
    if (draw_anim_bb && model->mesh_bounds_ratio < 0.5f)
    {
      auto animated_bb = model->getAnimatedBoundingBox();
      Noggit::Rendering::Primitives::WireBox::getInstance(_context).draw(model_view
        , projection
        , transformMatrix()
        , { 0.6f, 0.6f, 0.6f, 1.0f } // gray
        , animated_bb[0]
        , animated_bb[1]
      );

      // those are for debugging
      
      // base vertex box
      // Noggit::Rendering::Primitives::WireBox::getInstance(_context).draw(model_view
      //   , projection
      //   , transformMatrix()
      //   , { 1.0f, 0.0f, 0.0f, 1.0f } // red
      //   , misc::transform_model_box_coords(model->vertices_bounds[0])
      //   , misc::transform_model_box_coords(model->vertices_bounds[1])
      // );

      // current animation bounds
      // if (model->getCurrentSequenceBounds())
      // {
      //   auto curr_anim_bb = model->getCurrentSequenceBounds().value();
      //   Noggit::Rendering::Primitives::WireBox::getInstance(_context).draw(model_view
      //     , projection
      //     , transformMatrix()
      //     , { 0.0f, 0.0f, 0.0f, 1.0f } // black
      //     , misc::transform_model_box_coords(curr_anim_bb[0])
      //     , misc::transform_model_box_coords(curr_anim_bb[1])
      //   );
      // }
    }



  }
  else
  {
    const glm::vec4 color = _grouped ? glm::vec4(0.5f, 0.5f, 1.0f, 0.5f) : glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);
    Noggit::Rendering::Primitives::WireBox::getInstance(_context).draw ( model_view
      , projection
      , transformMatrix()
      , color
      , misc::transform_model_box_coords(model->bounding_box_min)
      , misc::transform_model_box_coords(model->bounding_box_max)
      );
  }
}

void ModelInstance::intersect (glm::mat4x4 const& model_view
                              , math::ray const& ray
                              , selection_result* results
                              , int animtime
                              , bool animate
                              )
{
  if (!finishedLoading())
    return;

  // A placement whose model failed to load used to return here, and that is the defect this
  // whole change exists to fix: with no geometry there was nothing to ray-test, so the object
  // could not be clicked, which meant it could not be selected, which meant it could not be
  // deleted or repointed. The map was unfixable from inside the editor.
  //
  // The placeholder cube is picked instead. recalcExtents below gives the instance a real,
  // fixed-size axis-aligned box for exactly this reason, so the same box the viewport draws is
  // the box the ray hits -- there is no second definition of where the placeholder is.
  //
  // Note the deliberate absence of _transform_mat here. The rotation of a model that does not
  // exist means nothing, and ModelInstance::recalcExtents returns before updateTransformMatrix()
  // in the failed case, so _transform_mat is the default-constructed glm::mat4x4 from
  // SceneObject.hpp:94, which glm leaves UNINITIALISED unless GLM_FORCE_CTOR_INIT is defined --
  // and it is not defined anywhere in this tree. A world-space test cannot read it and so cannot
  // be poisoned by it.
  if (model->loading_failed())
  {
    ensureExtents();

    if (auto const distance = ray.intersect_bounds(extents[0], extents[1]))
    {
      results->emplace_back(*distance, this);
    }

    return;
  }

  ensureExtents();

  std::vector<std::tuple<int, int, int>> triangle_indices;
  math::ray subray (_transform_mat_inverted, ray);

  if ( !subray.intersect_bounds ( fixCoordSystem (model->bounding_box_min)
                                , fixCoordSystem (model->bounding_box_max)
                                )
     )
  {
    return;
  }

  for (auto&& result : model->intersect (model_view, subray, animtime, animate))
  {
    //! \todo why is only sc important? these are relative to subray,
    //! so should be inverted by model_matrix?
    results->emplace_back (result.first * scale, this);
    triangle_indices.emplace_back(result.second);
  }
  return;
}


bool ModelInstance::isInFrustum(const math::frustum& frustum)
{
  if (_need_recalc_extents)
  {
    recalcExtents();
  }

  if (!frustum.intersects(extents[1], extents[0]))
    return false;

  return true;
}

bool ModelInstance::isInRenderDist(const float cull_distance, const glm::vec3& camera, display_mode display)
{
  float dist;

  // model->bounding_box_radius is INDETERMINATE for a model that failed to load: Model's
  // constructor leaves it unset (the memset at Model.cpp:19-24 is commented out) and glm does
  // not zero-initialise without GLM_FORCE_CTOR_INIT, which this tree never defines. This was
  // already a latent garbage read before placeholders existed -- WorldRender.cpp reaches here
  // for failed instances -- and the placeholder path makes it reachable on every frame, so the
  // radius is taken from the placeholder cube instead.
  float const radius
    ( model->loading_failed()
      ? Noggit::MissingPlacementGeometry::halfExtentFor(Noggit::MissingPlacementKind::Model)
          * 1.7320508f
      : model->bounding_box_radius * scale
    );

  if (display == display_mode::in_3D)
  {
    dist = glm::distance(camera, pos) - radius;
  }
  else
  {
    dist = std::abs(pos.y - camera.y) - radius;
  }

  if (dist >= cull_distance)
  {
    return false;
  }

  ensureExtents();

  if (size_cat < 1.f && dist > 300.f)
  {
    return false;
  }
  else if (size_cat < 4.f && dist > 500.f)
  {
    return false;
  }
  else if (size_cat < 25.f && dist > 1000.f)
  {
    return false;
  }

  return true;
}

bool ModelInstance::extentsDirty() const
{
  return _need_recalc_extents || !model->finishedLoading();
}

[[nodiscard]]
glm::vec3 const& ModelInstance::get_pos() const
{
  return pos;
}

void ModelInstance::recalcExtents()
{
  if (!model->finishedLoading())
  {
    _need_recalc_extents = true;
    return;
  }

  if (model->loading_failed())
  {
    // Was `extents[0] = extents[1] = pos;`, a zero-volume box. Every frustum test then reported
    // "not visible" and every ray test missed, which is the other half of why a broken placement
    // could not be selected -- World::select_objects_in_area projects this box to screen space
    // and a degenerate box projects to a single point.
    //
    // A FIXED cube, never model->bounding_box_*: those three floats are uninitialised for a
    // model that failed to load (Model.cpp:19-24 has the memset commented out), so the previous
    // code was right to avoid them and wrong about what to do instead.
    float const half
      (Noggit::MissingPlacementGeometry::halfExtentFor
        ( isWMODoodad() ? Noggit::MissingPlacementKind::WorldModelDoodad
                        : Noggit::MissingPlacementKind::Model
        ));

    // get_pos(), not pos. wmo_doodad_instance overrides it to return world_pos, because a WMO
    // doodad's `pos` is in the owning WMO's local space; centring the box on `pos` would drop it
    // wherever the map origin happens to be. update_transform_matrix_wmo gates only on
    // finishedLoading(), which is true after a failure, so world_pos is valid here.
    glm::vec3 const centre (get_pos());

    extents[0] = centre - glm::vec3(half, half, half);
    extents[1] = centre + glm::vec3(half, half, half);

    // The circumscribed sphere of that cube: half * sqrt(3). World.cpp:3952 subtracts this from
    // a screen-space w to decide whether an object clips the near plane, so leaving it at
    // whatever it happened to be would make a placeholder near the camera flicker in and out of
    // marquee selection.
    bounding_radius = half * 1.7320508f;

    // size_cat drives the distance ladder in isInRenderDist. A placeholder is a diagnostic and
    // must not be culled by size before the thing it stands for would have been, so it is given
    // the cube's diagonal -- the same quantity the loaded path computes from the real box.
    size_cat = half * 2.0f * 1.7320508f;

    _need_recalc_extents = false;
    return;
  }

  updateTransformMatrix();

  // math::aabb const relative_to_model
  //   ( glm::min ( model->collision_box_min, model->bounding_box_min)
  //   , glm::max ( model->collision_box_max, model->bounding_box_max)
  //   );
  math::aabb const relative_to_model(model->bounding_box_min, model->bounding_box_max);

  //! \todo If both boxes are {inf, -inf}, or well, if any min.c > max.c,
  //! the model is bad itself. We *could* detect that case and explicitly
  //! assume {-1, 1} then, to be nice to fuckported models.

  std::array<glm::vec3, 8> const rotated_corners_in_world = relative_to_model.rotated_corners(_transform_mat, true);

  math::aabb const bounding_of_rotated_points (std::vector<glm::vec3>(rotated_corners_in_world.begin()
                                              , rotated_corners_in_world.end()));

  extents[0] = bounding_of_rotated_points.min;
  extents[1] = bounding_of_rotated_points.max;

  _need_recalc_extents = false;

  // TODO We only need to recalculate size_cat if size changed

  if (model->mesh_bounds_ratio < 0.80f)
  {
    // size cat for animated models with smaller mesh than the BB

    math::aabb const vert_relative_to_model(model->vertices_bounds[0], model->vertices_bounds[1]);

    std::array<glm::vec3, 8> const vert_rotated_corners_in_world = vert_relative_to_model.rotated_corners(_transform_mat, true);

    math::aabb const vert_bounding_of_rotated_points(std::vector<glm::vec3>(vert_rotated_corners_in_world.begin()
      , vert_rotated_corners_in_world.end()));

    size_cat = glm::distance(vert_bounding_of_rotated_points.max, vert_bounding_of_rotated_points.min);
  }
  else
  {
    // this is basically AABB sphere diameter
    size_cat = glm::distance(bounding_of_rotated_points.max, bounding_of_rotated_points.min);
  }

  // Reference :  Using original blizzard model BB radius
  // TODO : No approach seems to generate the same value
  // float reference_bounding_radius = model->bounding_box_radius * scale;

  bounding_radius = glm::distance(model->bounding_box_max, model->bounding_box_min) * scale / 2.0f;
 
  // glm::vec3 halfExtents = (extents[1] - extents[0]) / 2.0f;
  // bounding_radius = glm::length(halfExtents);// Note : this is the same as (size_cat / 2.0f)
  // float aabb_bounding_radius = size_cat / 2.0f; // using AABB means it can be bigger than the object

  // how to calc it from OBB, gives a very similar result to glm::distance(model->bounding_box_max, model->bounding_box_min)
  // float test_recalc_bounding_radius = math::calculateOBBRadius(rotated_corners_in_world);


}

void ModelInstance::ensureExtents()
{
  if (_need_recalc_extents && model->finishedLoading())
  {
    recalcExtents();
  }
}

bool ModelInstance::finishedLoading()
{
  return model->finishedLoading();
}

std::array<glm::vec3, 2> const& ModelInstance::getExtents()
{
  ensureExtents();

  return extents;
}

// Returns by value on purpose. The braced-init-list materialises a temporary std::array, so a
// reference return handed the caller a reference to storage destroyed at the end of this return
// statement (MSVC C4172). Its one caller (World.cpp:3959) copies into a by-value local anyway.
std::array<glm::vec3, 2> ModelInstance::getLocalExtents() const
{
  return { model->bounding_box_min , model->bounding_box_max };
}

std::array<glm::vec3, 8> ModelInstance::getBoundingBox()
{
  // auto extents = getExtents();
  if (_need_recalc_extents)
    updateTransformMatrix();

  // math::aabb const relative_to_model
  // (glm::min(model->collision_box_min, model->bounding_box_min)
  //   , glm::max(model->collision_box_max, model->bounding_box_max)
  // );
  math::aabb const relative_to_model( model->bounding_box_min, model->bounding_box_max);

  return relative_to_model.rotated_corners(_transform_mat, true);
}

[[nodiscard]]
bool ModelInstance::isWMODoodad() const
{
  return false;
}

[[nodiscard]]
AsyncObject* ModelInstance::instance_model() const
{
  return model.get();
}

void ModelInstance::updateDetails(Noggit::Ui::detail_infos* detail_widget)
{
  std::stringstream select_info;

  select_info << "<b>filename:</b> " << model->file_key().filepath()
    // << "<br><b>FileDataID:</b> " << model->file_key().fileDataID() // not in WOTLK
    << "<br><b>unique ID:</b> " << uid
    << "<br><b>position X/Y/Z:</b> {" << pos.x << " , " << pos.y << " , " << pos.z << "}"
    << "<br><b>rotation X/Y/Z:</b> {" << dir.x << " , " << dir.y << " , " << dir.z << "}"
    << "<br><b>scale:</b> " << scale

    << "<br><b>server position X/Y/Z: </b>{" << (ZEROPOINT - pos.z) << ", " << (ZEROPOINT - pos.x) << ", " << pos.y << "}"
    << "<br><b>server orientation:  </b>" << fabs(2 * glm::pi<float>() - glm::pi<float>() / 180.0 * (float(dir.y) < 0 ? fabs(float(dir.y)) + 180.0 : fabs(float(dir.y) - 180.0)))

    << "<br><b>textures Used:</b> " << model->_textures.size()
    << "<br><b>size category:</b><span> " << size_cat;

  for (unsigned j  = 0; j < model->_textures.size(); j++)
  {
    bool stuck = !model->_textures[j]->finishedLoading();
    bool error = model->_textures[j]->finishedLoading() && !model->_textures[j]->is_uploaded();

    select_info << "<br> ";

    if (stuck)
      select_info << "<font color=\"Orange\">";

    if (error)
      select_info << "<font color=\"Red\">";

    select_info << "<b>" << (j + 1) << ":</b> " << model->_textures[j]->file_key().stringRepr();

    if (stuck || error)
      select_info << "</font>";
  }

  select_info << "</span>";

  detail_widget->setText(select_info.str());
}

[[nodiscard]]
std::uint32_t ModelInstance::gpuTransformUid() const
{
  return _gpu_transform_uid;
}

wmo_doodad_instance::wmo_doodad_instance(BlizzardArchive::Listfile::FileKey const& file_key
                                         , BlizzardArchive::ClientFile* f
                                         , Noggit::NoggitRenderContext context)
  : ModelInstance(file_key, context)
  , world_pos(glm::vec3(0.0f))
{
  float ff[4];

  f->read(ff, 12);
  pos = glm::vec3(ff[0], ff[2], -ff[1]);

  f->read(ff, 16);
  // doodad_orientation = glm::quat (-ff[0], -ff[2], ff[1], ff[3]);
  doodad_orientation = glm::quat(ff[3], -ff[0], -ff[2], ff[1]); // in GLM w is the first argument
  doodad_orientation = glm::normalize(doodad_orientation);
  dir = glm::degrees(glm::eulerAngles(doodad_orientation));

  f->read(&scale, 4);

  union
  {
    uint32_t packed;
    struct
    {
      uint8_t b, g, r, a;
    }bgra;
  } color;

  f->read(&color.packed, 4);

  light_color = glm::vec3(color.bgra.r / 255.f, color.bgra.g / 255.f, color.bgra.b / 255.f);
}

// titi : issue with those constructors: ModelInstance data is lost (scale, pos...)
/**/
wmo_doodad_instance::wmo_doodad_instance(wmo_doodad_instance const& other)
// : ModelInstance(other.model->file_key(), other._context)
  : ModelInstance(other)  // titi : Use the copy constructor of ModelInstance instead
  , doodad_orientation(other.doodad_orientation)
  , world_pos(other.world_pos)
  , _need_matrix_update(other._need_matrix_update)
{
  // titi: added those.
  // pos = other.pos;
  // scale = other.scale;
  // frame = other.frame;
}

wmo_doodad_instance::wmo_doodad_instance(wmo_doodad_instance&& other) noexcept
  : ModelInstance(reinterpret_cast<ModelInstance&&>(other))
  , doodad_orientation(other.doodad_orientation)
  , world_pos(other.world_pos)
  , _need_matrix_update(other._need_matrix_update)
{
}

wmo_doodad_instance& wmo_doodad_instance::operator= (wmo_doodad_instance&& other) noexcept
{
  ModelInstance::operator= (reinterpret_cast<ModelInstance&&>(other));
  std::swap(doodad_orientation, other.doodad_orientation);
  std::swap(world_pos, other.world_pos);
  std::swap(_need_matrix_update, other._need_matrix_update);
  return *this;
}

[[nodiscard]]
bool wmo_doodad_instance::need_matrix_update() const
{
  return _need_matrix_update;
}

void wmo_doodad_instance::update_transform_matrix_wmo(WMOInstance* wmo)
{
  if (!model->finishedLoading() || !wmo->finishedLoading())
  {
    return;
  }  

  // world_pos = wmo->transformMatrix() * glm::vec4(pos,0);
  world_pos = wmo->transformMatrix() * glm::vec4(pos, 1.0f);

  auto m2_mat = glm::mat4x4(1.0f);

  // m2_mat = glm::translate(m2_mat, pos);
  m2_mat = m2_mat * glm::translate(glm::mat4(1.0f), pos);
  m2_mat = m2_mat * glm::toMat4(glm::inverse(doodad_orientation));
  m2_mat = glm::scale(m2_mat, glm::vec3(scale));

  _transform_mat = wmo->transformMatrix() * m2_mat;
  _transform_mat_inverted = glm::inverse(_transform_mat);

  // to compute the size category (used in culling)
  // updateTransformMatrix() is overriden to do nothing
  recalcExtents();

  _need_matrix_update = false;
}

bool wmo_doodad_instance::isInRenderDist(const float cull_distance, const glm::vec3& camera, display_mode display)
{
  float dist;

  // Same indeterminate read as ModelInstance::isInRenderDist above, and reached from the WMO
  // doodad loop in WorldRender.cpp, which does not filter failed models before calling this.
  float const radius
    ( model->loading_failed()
      ? Noggit::MissingPlacementGeometry::halfExtentFor
          (Noggit::MissingPlacementKind::WorldModelDoodad) * 1.7320508f
      : model->bounding_box_radius * scale
    );

  if (display == display_mode::in_3D)
  {
    dist = glm::distance(camera, world_pos) - radius;
  }
  else
  {
    dist = std::abs(world_pos.y - camera.y) - radius;
  }

  if (dist >= cull_distance)
  {
    return false;
  }

  ensureExtents();

  if (size_cat < 1.f && dist > 300.f)
  {
    return false;
  }
  else if (size_cat < 4.f && dist > 500.f)
  {
    return false;
  }
  else if (size_cat < 25.f && dist > 1000.f)
  {
    return false;
  }

  return true;
}

[[nodiscard]]
glm::vec3 const& wmo_doodad_instance::get_pos() const
{
  return world_pos;
}

[[nodiscard]]
bool wmo_doodad_instance::isWMODoodad() const
{
  return true;
}

// to avoid redefining recalcExtents
void wmo_doodad_instance::updateTransformMatrix()
{
}
