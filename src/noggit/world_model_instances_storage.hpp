// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once
#include <noggit/ModelInstance.h>
#include <noggit/Selection.h>
#include <noggit/reports/UidCollisionLog.hpp>
#include <noggit/WMOInstance.h>
#include <opengl/scoped.hpp>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <vector>

struct TileIndex;
class World;

using m2_instance_umap = std::unordered_map<std::uint32_t, ModelInstance>;
using wmo_instance_umap = std::unordered_map<std::uint32_t, WMOInstance>;

namespace Noggit
{
  class world_model_instances_storage
  {
  public:
    world_model_instances_storage(World* world);

    world_model_instances_storage() = delete;
    world_model_instances_storage(world_model_instances_storage const&) = delete;
    world_model_instances_storage(world_model_instances_storage&&) = delete;
    world_model_instances_storage& operator= (world_model_instances_storage const&) = delete;
    world_model_instances_storage& operator= (world_model_instances_storage&&) = delete;

    // perform uid duplicate check, return the uid of the stored instance
    std::uint32_t add_model_instance(ModelInstance instance, bool from_reloading, bool action);
    // perform uid duplicate check, return the uid of the stored instance
    std::uint32_t add_wmo_instance(WMOInstance instance, bool from_reloading, bool action);

    std::optional<ModelInstance*> get_model_instance(std::uint32_t uid);
    std::optional<WMOInstance*> get_wmo_instance(std::uint32_t uid);
    std::optional<selection_type> get_instance(std::uint32_t uid, bool lock=true);

    void delete_instances_from_tile(TileIndex const& tile, bool action);
    void delete_instances(std::vector<selected_object_type> const& instances, bool action);
    void delete_instance(std::uint32_t uid, bool action);
    void unload_instance_and_remove_from_selection_if_necessary(std::uint32_t uid);

    void clear();

    void clear_duplicates(bool action);

    bool uid_duplicates_found() const;

    // What uid_duplicates_found() flattens into a bool. Every renumber performed by
    // unsafe_add_*_no_world_upd is recorded here with the uid it replaced, the model it belonged
    // to and the tile it sat in.
    //
    // Deliberately NOT emptied by clear(). clear() has exactly one caller,
    // World::unload_every_model_and_wmo_instance, which itself has exactly one:
    // MapIndex::fixUIDs at map_index.cpp:1104. That is the operation a user runs *because of*
    // these collisions, so wiping the evidence there would destroy it at the moment they go
    // looking for it. This matches _uid_duplicates_found, which is likewise never reset. Call
    // clear() on the log itself once the report has been shown.
    UidCollisionLog const& uid_collision_log() const;
    UidCollisionLog& uid_collision_log();

    void upload();
    void unload();

    unsigned int getTotalModelsCount() const;;

  private: // private functions aren't thread safe
    bool unsafe_uid_is_used(std::uint32_t uid) const;

    // Shared by the M2 and WMO repair sites: the filename and the position both come from
    // SceneObject, so neither concrete type is needed here.
    void unsafe_record_uid_collision( std::uint32_t previous_uid
                                    , std::uint32_t assigned_uid
                                    , UidCollisionKind kind
                                    , SceneObject const& instance
                                    );

    std::uint32_t unsafe_add_model_instance_no_world_upd(ModelInstance instance, bool action);
    std::uint32_t unsafe_add_wmo_instance_no_world_upd(WMOInstance instance, bool action);
    std::optional<ModelInstance*> unsafe_get_model_instance(std::uint32_t uid);
    std::optional<WMOInstance*> unsafe_get_wmo_instance(std::uint32_t uid);

  public:
    template<typename Fun>
      void for_each_wmo_instance(Fun&& function)
    {
      std::unique_lock<std::mutex> const lock (_mutex);

      for (auto& it : _wmos)
      {
        function(it.second);
      }
    }

    template<typename Fun, typename Stop>
      void for_each_wmo_instance(Fun&& function, Stop&& stop_cond)
    {
      std::unique_lock<std::mutex> const lock (_mutex);

      for (auto& it : _wmos)
      {
        function(it.second);

        if (stop_cond())
        {
          break;
        }
      }
    }

    template<typename Fun>
      void for_each_m2_instance(Fun&& function)
    {
      std::unique_lock<std::mutex> const lock (_mutex);

      for (auto& it : _m2s)
      {
        function(it.second);
      }
    }

  private:
    World* _world;
    std::mutex _mutex;
    std::atomic<bool> _uid_duplicates_found = {false};
    // Has its own lock, so readers never contend for _mutex, which instance loading holds for the
    // whole of an add.
    UidCollisionLog _uid_collision_log;

    m2_instance_umap _m2s;
    wmo_instance_umap _wmos;

    OpenGL::Scoped::deferred_upload_buffers<1> _buffers;
    GLuint const& _m2_instances_transform_buf = _buffers[0];
    GLuint _m2_instances_transform_buf_tex;
    std::uint32_t _n_allocated_m2_transforms = 4096;
    std::uint32_t _n_used_m2_transforms = 0;
    bool _transform_storage_uploaded = false;

    std::unordered_map<std::uint32_t, int> _instance_count_per_uid;
  };
}
