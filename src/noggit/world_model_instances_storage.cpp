// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/world_model_instances_storage.hpp>
#include <noggit/World.h>
#include <noggit/ActionManager.hpp>
#include <noggit/Action.hpp>
#include <noggit/AsyncObject.h>
#include <noggit/SceneObject.hpp>
#include <noggit/TileIndex.hpp>

#include <string>
#include <string_view>

namespace
{
  // Model identity on its own, with the transform deliberately left out.
  //
  // SceneObject::isDuplicateOf answers "same model AND same placement", which is the right test
  // for the ordinary multi-tile case where every tile's row agrees. This is its first half, and it
  // is needed for the case where the rows do NOT agree, where the question stops being "is this
  // the same placement" and becomes "is this the same object, moved".
  //
  // Two instances that both have no AsyncObject bound count as a match, for the same reason
  // isDuplicateOf treats them as one: neither carries any evidence about which model it is, so the
  // file key cannot decide anything and the caller's other test has to.
  bool same_model_as(SceneObject const& lhs, SceneObject const& rhs)
  {
    AsyncObject const* lhs_model = lhs.instance_model();
    AsyncObject const* rhs_model = rhs.instance_model();

    if (static_cast<bool>(lhs_model) != static_cast<bool>(rhs_model))
    {
      return false;
    }

    return !lhs_model || lhs_model->file_key() == rhs_model->file_key();
  }
}

namespace Noggit
{
  world_model_instances_storage::world_model_instances_storage(World* world)
    : _world(world)
  {

  }

  std::uint32_t world_model_instances_storage::add_model_instance(ModelInstance instance, bool from_reloading, bool action)
  {
    std::uint32_t uid = instance.uid;
    std::uint32_t uid_after;

    {
      std::lock_guard<std::mutex> const lock (_mutex);
      uid_after = unsafe_add_model_instance_no_world_upd(std::move(instance), action);
    }

    if (from_reloading || uid_after != uid)
    {
      _world->updateTilesModel(&_m2s.at(uid_after), model_update::add);
    }

    return uid_after;
  }
  std::uint32_t world_model_instances_storage::unsafe_add_model_instance_no_world_upd(ModelInstance instance, bool action)
  {
    std::uint32_t uid = instance.uid;
    auto existing_instance = unsafe_get_model_instance(uid);

    if (existing_instance)
    {
      // instance already loaded
      if (existing_instance.value()->isDuplicateOf(instance))
      {
        _instance_count_per_uid[uid]++;
        return uid;
      }

      // The stale row of an object that straddles a tile border and has since been moved.
      //
      // Such an object is written into the MDDF of every tile its bounding box touches, under one
      // uid. Move it, then let one of those neighbouring tiles load for the first time, and the
      // row that tile still holds carries the OLD transform. On the transform alone that is
      // indistinguishable from two different objects sharing a uid, and the renumber below -- the
      // correct answer for a real collision -- re-creates the object at the position the user
      // dragged it away from, then saves it there. The user gets a duplicate of something they
      // moved, sitting where it used to be, with no undo step to blame.
      //
      // Three things must all hold before the row is treated as a leftover, and the third is the
      // one that keeps this from being a cure worse than the disease. "Same model and I moved
      // this session" ALONE would swallow a genuine collision between two instances of the same
      // model as soon as the user had nudged either of them -- and a swallowed instance is not
      // merely unrendered, it is erased: MapTile::saveTile resolves the tile's uid list through
      // World::get_model (MapTile.cpp:582-584) and writes one MDDF row per uid, so the object
      // that lost the race is gone from disk on the next save, with no undo step and no warning.
      // Requiring the incoming row to carry the transform this instance was LOADED at makes the
      // test positive evidence rather than an absence of it: a leftover row is by definition the
      // row we were built from, so it matches exactly, whereas a different object would have to
      // sit at another object's former placement to within the isDuplicateOf epsilons to be
      // mistaken for one. Anything else -- different model, untouched object, or a transform that
      // is neither the current one nor the loaded one -- still falls through to the renumber.
      //
      // The count MUST be incremented on this path, exactly as on the isDuplicateOf path above.
      // MapTile::load registers this uid against the loading tile via MapTile::add_model
      // (MapTile.cpp:361), and World::remove_models_if_needed decrements once per registered uid
      // when that tile unloads. Returning without incrementing would let the neighbour's unload
      // take the count to zero and destroy the object the user had just moved -- the same data
      // loss by a different route.
      if (existing_instance.value()->wasTransformedThisSession()
       && same_model_as(*existing_instance.value(), instance)
       && existing_instance.value()->matchesTransformAtLoad(instance))
      {
        _instance_count_per_uid[uid]++;
        return uid;
      }
    }
    else if(!unsafe_uid_is_used(uid))
    {
      // This causes a crash when undoing while loading a tile, those objects get registered to the action stack
      if (action && NOGGIT_CUR_ACTION)
        NOGGIT_CUR_ACTION->registerObjectAdded(&instance);
      _m2s.emplace(uid, instance);
      _instance_count_per_uid[uid] = 1;
      return uid;
    }

    // the uid is already used for another model/wmo, use a new one
    _uid_duplicates_found = true;

    std::uint32_t const assigned_uid = _world->mapIndex.newGUID();
    // Recorded before the uid is overwritten and before the move, both of which destroy what the
    // record is about. The renumber itself is unchanged: newGUID() is still called exactly once
    // per collision and its result is still what the instance is stored under.
    unsafe_record_uid_collision(uid, assigned_uid, UidCollisionKind::M2, instance);
    instance.uid = assigned_uid;

    return unsafe_add_model_instance_no_world_upd(std::move(instance), action);
  }

  
  std::uint32_t world_model_instances_storage::add_wmo_instance(WMOInstance instance, bool from_reloading, bool action)
  {
    std::uint32_t uid = instance.uid;
    std::uint32_t uid_after;

    {
      std::lock_guard<std::mutex> const lock(_mutex);
      uid_after = unsafe_add_wmo_instance_no_world_upd(std::move(instance), action);
    }

    if (from_reloading || uid_after != uid)
    {
      _world->updateTilesWMO(&_wmos.at(uid_after), model_update::add);
    }

    return uid_after;
  }
  std::uint32_t world_model_instances_storage::unsafe_add_wmo_instance_no_world_upd(WMOInstance instance, bool action)
  {
    std::uint32_t uid = instance.uid;
    auto existing_instance = unsafe_get_wmo_instance(uid);

    if (existing_instance)
    {
      // instance already loaded
      if (existing_instance.value()->isDuplicateOf(instance))
      {
        _instance_count_per_uid[uid]++;

        return uid;
      }

      // Same reasoning as the M2 path above, and it bites harder here: a WMO is large enough that
      // straddling a tile border is the normal case rather than the exception, so nearly every
      // moved WMO had a stale row waiting in a neighbouring tile. See the comment there for why
      // all three tests are required and what the transform test in particular prevents, and
      // SceneObject.hpp for why the flag is never cleared.
      if (existing_instance.value()->wasTransformedThisSession()
       && same_model_as(*existing_instance.value(), instance)
       && existing_instance.value()->matchesTransformAtLoad(instance))
      {
        _instance_count_per_uid[uid]++;

        return uid;
      }
    }
    else if (!unsafe_uid_is_used(uid))
    {
      if (action && NOGGIT_CUR_ACTION)
        NOGGIT_CUR_ACTION->registerObjectAdded(&instance);
      _wmos.emplace(uid, instance);
      _instance_count_per_uid[uid] = 1;
      return uid;
    }

    // the uid is already used for another model/wmo, use a new one
    _uid_duplicates_found = true;

    std::uint32_t const assigned_uid = _world->mapIndex.newGUID();
    unsafe_record_uid_collision(uid, assigned_uid, UidCollisionKind::WMO, instance);
    instance.uid = assigned_uid;

    return unsafe_add_wmo_instance_no_world_upd(std::move(instance), action);
  }

  void world_model_instances_storage::unsafe_record_uid_collision
    ( std::uint32_t previous_uid
    , std::uint32_t assigned_uid
    , UidCollisionKind kind
    , SceneObject const& instance
    )
  {
    // Read out of the FileKey directly rather than through FileKey::stringRepr(), which builds a
    // std::string by value (Listfile.cpp:203-206) purely to have it copied again. The common case
    // -- a key that carries a path -- then costs no allocation at all before the log's own copy.
    // Only the fileDataID-only case has to render digits, and 3.3.5 keys always carry paths.
    std::string_view model_name;
    std::string file_data_id_text;

    if (AsyncObject const* async_object = instance.instance_model())
    {
      auto const& file_key = async_object->file_key();

      if (file_key.hasFilepath())
      {
        model_name = file_key.filepath();
      }
      else
      {
        file_data_id_text = std::to_string(file_key.fileDataID());
        model_name = file_data_id_text;
      }
    }

    // The instance is not attached to any tile yet at this point -- updateTilesModel/updateTilesWMO
    // run after add_*_instance returns -- so getTiles() is empty and the position is the only
    // source for the tile. Same derivation delete_instances_from_tile uses to match a tile.
    TileIndex const tile (instance.pos);
    UidCollisionTile const recorded_tile
      { static_cast<std::uint32_t>(tile.x)
      , static_cast<std::uint32_t>(tile.z)
      , tile.is_valid()
      };

    _uid_collision_log.record(previous_uid, assigned_uid, kind, model_name, recorded_tile);
  }

  void world_model_instances_storage::delete_instances_from_tile(TileIndex const& tile, bool action)
  {
    // std::unique_lock<std::mutex> const lock (_mutex);
    std::vector<selected_object_type> instances_to_remove;

    for (auto it = _m2s.begin(); it != _m2s.end(); ++it)
    {
      if (TileIndex(it->second.pos) == tile)
      {
        instances_to_remove.push_back(&it->second);
      }
    }
    for (auto it = _wmos.begin(); it != _wmos.end();++it)
    {
      if (TileIndex(it->second.pos) == tile)
      {
        instances_to_remove.push_back(&it->second);
      }
    }
    delete_instances(instances_to_remove, action);
  }

  void world_model_instances_storage::delete_instances(std::vector<selected_object_type> const& instances, bool action)
  {
    for (auto& obj : instances)
    {
      // done in delete_instance
      /*
      if (action && NOGGIT_CUR_ACTION)
        NOGGIT_CUR_ACTION->registerObjectRemoved(obj);*/

      if (obj->which() == eMODEL)
      {
        auto instance = static_cast<ModelInstance*>(obj);
        
        _world->updateTilesModel(instance, model_update::remove);
        delete_instance(instance->uid, action);
      }
      else if (obj->which() == eWMO)
      {
        auto instance = static_cast<WMOInstance*>(obj);

        _world->updateTilesWMO(instance, model_update::remove);
        delete_instance(instance->uid, action);
      }
    }
  }

  void world_model_instances_storage::delete_instance(std::uint32_t uid, bool action)
  {
    std::unique_lock<std::mutex> const lock (_mutex);

    if (auto instance = get_instance(uid, false))
    {
      _world->updateTilesEntry(instance.value(), model_update::remove);
      auto obj = std::get<selected_object_type>(instance.value());

      for (auto& selection_group : _world->_selection_groups)
      {
          if (selection_group.contains_object(obj))
          {
              selection_group.remove_member(obj->uid);
          }
      }
      if (action && NOGGIT_CUR_ACTION)
      {
        NOGGIT_CUR_ACTION->registerObjectRemoved(obj);
      }
    }

    _instance_count_per_uid.erase(uid);
    _m2s.erase(uid);
    _wmos.erase(uid);
  }

  void world_model_instances_storage::unload_instance_and_remove_from_selection_if_necessary(std::uint32_t uid)
  {
    std::unique_lock<std::mutex> const lock (_mutex);

    if (!unsafe_uid_is_used(uid))
    {
      LogError << "Trying to unload an instance that wasn't stored" << std::endl;
      return;
    }

    if (--_instance_count_per_uid.at(uid) == 0)
    {
      _world->remove_from_selection(uid, false, false);

      _instance_count_per_uid.erase(uid);
      _m2s.erase(uid);
      _wmos.erase(uid);
    }
  }

  void world_model_instances_storage::clear()
  {
    std::unique_lock<std::mutex> const lock (_mutex);

    _instance_count_per_uid.clear();
    _m2s.clear();
    _wmos.clear();
  }

  std::optional<ModelInstance*> world_model_instances_storage::get_model_instance(std::uint32_t uid)
  {
    std::unique_lock<std::mutex> const lock (_mutex);
    return unsafe_get_model_instance(uid);
  }
  std::optional<ModelInstance*> world_model_instances_storage::unsafe_get_model_instance(std::uint32_t uid)
  {
    auto it = _m2s.find(uid);

    if (it != _m2s.end())
    {
      return &it->second;
    }
    else
    {
      return std::nullopt;
    }
  }

  std::optional<WMOInstance*> world_model_instances_storage::get_wmo_instance(std::uint32_t uid)
  {
    std::unique_lock<std::mutex> const lock (_mutex);
    return unsafe_get_wmo_instance(uid);
  }
  std::optional<WMOInstance*> world_model_instances_storage::unsafe_get_wmo_instance(std::uint32_t uid)
  {
    auto it = _wmos.find(uid);

    if (it != _wmos.end())
    {
      return &it->second;
    }
    else
    {
      return std::nullopt;
    }
  }

  std::optional<selection_type> world_model_instances_storage::get_instance(std::uint32_t uid, bool lock)
  {
    if (lock)
    {
      std::unique_lock<std::mutex> const lock(_mutex);

      auto wmo_it = _wmos.find(uid);

      if (wmo_it != _wmos.end())
      {
        return selection_type{ &wmo_it->second };
      }
      else
      {
        auto m2_it = _m2s.find(uid);

        if (m2_it != _m2s.end())
        {
          return selection_type{ &m2_it->second };
        }
        else
        {
          return std::nullopt;
        }
      }
    }
    else
    {
      auto wmo_it = _wmos.find(uid);

      if (wmo_it != _wmos.end())
      {
        return selection_type{ &wmo_it->second };
      }
      else
      {
        auto m2_it = _m2s.find(uid);

        if (m2_it != _m2s.end())
        {
          return selection_type{ &m2_it->second };
        }
        else
        {
            return std::nullopt;
        }
      }
    }
  }

  bool world_model_instances_storage::unsafe_uid_is_used(std::uint32_t uid) const
  {
    return _instance_count_per_uid.find(uid) != _instance_count_per_uid.end();
  }

  void world_model_instances_storage::clear_duplicates(bool action)
  {
    std::unique_lock<std::mutex> const lock (_mutex);

    int deleted_uids = 0;

    for (auto lhs(_wmos.begin()); lhs != _wmos.end(); ++lhs)
    {
      for (auto rhs(std::next(lhs)); rhs != _wmos.end();)
      {
        assert(lhs->first != rhs->first);

        if (lhs->second.isDuplicateOf(rhs->second))
        {
          _world->updateTilesWMO(&rhs->second, model_update::remove);

          _instance_count_per_uid.erase(rhs->second.uid);
          if (action && NOGGIT_CUR_ACTION)
            NOGGIT_CUR_ACTION->registerObjectRemoved(&rhs->second);
          rhs = _wmos.erase(rhs);
          deleted_uids++;
        }
        else
        {
          rhs++;
        }
      }
    }

    for (auto lhs(_m2s.begin()); lhs != _m2s.end(); ++lhs)
    {
      for (auto rhs(std::next(lhs)); rhs != _m2s.end();)
      {
        assert(lhs->first != rhs->first);

        if (lhs->second.isDuplicateOf(rhs->second))
        {
          _world->updateTilesModel(&rhs->second, model_update::remove);

          _instance_count_per_uid.erase(rhs->second.uid);

          if (action && NOGGIT_CUR_ACTION)
            NOGGIT_CUR_ACTION->registerObjectRemoved(&rhs->second);
          rhs = _m2s.erase(rhs);
          deleted_uids++;
        }
        else
        {
          rhs++;
        }
      }
    }

    Log << "Deleted " << deleted_uids << " duplicate Model/WMO" << std::endl;
  }

  bool world_model_instances_storage::uid_duplicates_found() const
  {
    return _uid_duplicates_found.load();
  }

  UidCollisionLog const& world_model_instances_storage::uid_collision_log() const
  {
    return _uid_collision_log;
  }

  UidCollisionLog& world_model_instances_storage::uid_collision_log()
  {
    return _uid_collision_log;
  }

  void world_model_instances_storage::upload()
  {
    if (_transform_storage_uploaded)
      return;

    _buffers.upload();

    gl.genTextures(1, &_m2_instances_transform_buf_tex);
    gl.activeTexture(GL_TEXTURE0);
    gl.bindTexture(GL_TEXTURE_BUFFER, _m2_instances_transform_buf_tex);
    gl.bindBuffer(GL_TEXTURE_BUFFER, _m2_instances_transform_buf);
    gl.bufferData(GL_TEXTURE_BUFFER, _n_allocated_m2_transforms * sizeof(glm::mat4x4), nullptr, GL_DYNAMIC_DRAW);
    gl.texBuffer(GL_TEXTURE_BUFFER, GL_RGBA32F, _m2_instances_transform_buf);

    _transform_storage_uploaded = true;
  }

  void world_model_instances_storage::unload()
  {
    if (!_transform_storage_uploaded)
      return;

    gl.deleteTextures(1, &_m2_instances_transform_buf_tex);
    _buffers.unload();

    _transform_storage_uploaded = false;
  }

  unsigned int world_model_instances_storage::getTotalModelsCount() const
  {
    return _m2s.size() + _wmos.size();
  }
}
