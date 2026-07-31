// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#include <noggit/scripting/script_model.hpp>
#include <noggit/scripting/scripting_tool.hpp>
#include <noggit/scripting/script_context.hpp>
#include <noggit/scripting/script_exception.hpp>
#include <noggit/World.h>
#include <noggit/ModelInstance.h>
#include <noggit/object_paste_params.hpp>
#include <noggit/WMOInstance.h>
#include <noggit/ui/ObjectEditor.h>

#include <sol/sol.hpp>

namespace Noggit
{
  namespace Scripting
  {
    model::model (script_context* ctx, SceneObject* object)
      : script_object(ctx)
      , _object (object)
    {}
    

    glm::vec3 model::get_pos()
    {
      return _object->pos;
    }

    void model::set_pos(glm::vec3& pos)
    {
      world()->updateTilesEntry(_object, model_update::remove);
      _object->pos = pos;
      _object->recalcExtents();
      world()->updateTilesEntry(_object, model_update::add);
    }

    glm::vec3 model::get_rot()
    {
      return _object->dir;
    }

    void model::set_rot(glm::vec3& rot)
    {
      math::degrees::vec3 dir = math::degrees::vec3{ rot };
      world()->updateTilesEntry(_object, model_update::remove);
      _object->dir = dir;
      _object->recalcExtents();
      world()->updateTilesEntry(_object, model_update::add);
    }

    float model::get_scale()
    {
      return _object->scale;
    }

    void model::set_scale(float scale)
    {
        world()->updateTilesEntry(_object, model_update::remove);
        _object->scale = scale;
        _object->recalcExtents();
        world()->updateTilesEntry(_object, model_update::add);
    }

    unsigned model::get_uid()
    {
      return _object->uid;
    }

    std::string model::get_filename()
    {
      return _object->instance_model()->file_key().filepath();
    }

    bool model::has_filename(std::string const& name)
    {
      std::string copy = std::string(name);

      std::transform(copy.begin(), copy.end(), copy.begin(),
          [](unsigned char c) { return std::tolower(c); });

      std::replace(copy.begin(),copy.end(),'\\','/');
      return copy == get_filename();
    }

    void model::remove()
    {
      std::vector<SceneObject*> type{_object};

      // action=true, matching add_m2/add_wmo. Scatter scripts delete what they placed on a
      // re-stroke (scripts/prop_placer.lua:46-57), so without this half of a scatter stroke was
      // undoable and half was not -- which is worse than neither, because Ctrl+Z would restore
      // the additions and leave the deletions permanent.
      world()->deleteObjects(type, true);
    }

    void model::replace(std::string const& filename)
    {
      if (get_filename() == filename)
      {
        return;
      }

      // Read the transform BEFORE remove(). remove() ends in
      // world_model_instances_storage::delete_instance, which erases the instance from _m2s/_wmos
      // (world_model_instances_storage.cpp:241-242) and so destroys the object _object points at.
      // These three values used to be fetched through _object in the argument lists below, i.e.
      // after the free.
      glm::vec3 const pos = get_pos();
      glm::vec3 const rot = get_rot();
      float const scale = get_scale();

      remove();

      // action=true, matching remove() above. With false the removal was on the undo stack and
      // the replacement was not, so Ctrl+Z restored the original model and left the replacement
      // in place -- every undo of a replace() left the scene one object heavier.
      if (filename.ends_with(".wmo"))
      {
        _object =
          world()->addWMOAndGetInstance(filename, pos, math::degrees::vec3 {rot}, scale, true);
      }
      else
      {
        auto params = object_paste_params();
        _object =
          world()->addM2AndGetInstance(filename, pos, scale, math::degrees::vec3 {rot}, &params, false, true);
      }
    }

    void collect_models(
        script_context * ctx
      , World * world
      , glm::vec3 const& min
      , glm::vec3 const& max
      , std::vector<model> & vec
    )
    {
      world->getModelInstanceStorage().for_each_m2_instance([&](ModelInstance& mod)
      {
        if (mod.pos.x >= min.x && mod.pos.x <= max.x
          && mod.pos.z >= min.z && mod.pos.z <= max.z)
        {
          vec.push_back(model(ctx, &mod));
        }
      });
      world->getModelInstanceStorage().for_each_wmo_instance([&](WMOInstance& mod)
      {
        if (mod.pos.x >= min.x && mod.pos.x <= max.x
          && mod.pos.z >= min.z && mod.pos.z <= max.z)
        {
          vec.push_back(model(ctx, &mod));
        }
      });
    }

    void register_model(script_context * state)
    {
      state->new_usertype<model>("model"
        , "get_pos", &model::get_pos
        , "set_pos", &model::set_pos
        , "get_rot", &model::get_rot
        , "set_rot", &model::set_rot
        , "get_scale", &model::get_scale
        , "set_scale", &model::set_scale
        , "get_uid", &model::get_uid
        , "remove", &model::remove
        , "get_filename", &model::get_filename
        , "has_filename", &model::has_filename
        , "replace", &model::replace
      );
    }
  } // namespace Scripting
} // namespace Noggit
