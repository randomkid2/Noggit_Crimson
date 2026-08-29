// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

#include <noggit/AsyncObject.h>
#include <noggit/ContextObject.hpp>
#include <noggit/map_enums.hpp>
#include <noggit/MapHeaders.h>
#include <noggit/rendering/FlightBoundsRender.hpp>
#include <noggit/rendering/TileRender.hpp>
#include <noggit/Selection.h>
#include <noggit/TileIndex.hpp>
#include <noggit/TileWater.hpp>

#include <external/tsl/robin_map.h>

#include <array>
#include <atomic>
#include <map>
#include <string>
#include <vector>

namespace math
{
  class frustum;
  struct vector_3d;
  struct ray;
}

class MapChunk;
struct texture_heightmapping_data;
class World;


class MapTile : public AsyncObject
{
  friend class Noggit::Rendering::TileRender;
  friend class Noggit::Rendering::FlightBoundsRender;
  friend class MapChunk;
  friend class TextureSet;

public:
	MapTile( int x0
         , int z0
         , std::string const& pFilename
         , bool pBigAlpha
         , bool pLoadModels
         , bool use_mclq_green_lava
         , bool reloading_tile
         , World*
         , Noggit::NoggitRenderContext context
         , tile_mode mode = tile_mode::edit
         , bool pLoadTextures = true
         );
  ~MapTile();

  void finishLoading() override;
  void waitForChildrenLoaded() override;

  //! \todo on destruction, unload ModelInstances and WMOInstances on this tile:
  // a) either keep up the information what tiles the instances are on at all times
  //    (even while moving), to then check if all tiles it was on were unloaded, or
  // b) do the reference count lazily by iterating over all instances and checking
  //    what MapTiles they span. if any of those tiles is still loaded, keep it,
  //    otherwise remove it.
  //
  // I think b) is easier. It only requires
  // `std::set<C2iVector> XInstance::spanning_tiles() const` followed by
  // `if_none (isTileLoaded (x, y)): unload instance`, which is way easier than
  // constantly updating the reference counters.
  // Note that both approaches do not cover the issue that the instance might not
  // be saved to any tile, thus the movement might have been lost.

	//! \brief Get the maximum height of terrain on this map tile.
	float getMaxHeight();
	float getMinHeight();
  void forceRecalcExtents();

  void convert_alphamap(bool to_big_alpha);

  //! \brief Get chunk for sub offset x,z.

  [[nodiscard]]
  MapChunk* getChunk(unsigned int x, unsigned int z);
  //! \todo map_index style iterators

  [[nodiscard]]
  std::vector<MapChunk*> chunks_in_range (glm::vec3 const& pos, float radius) const;

  [[nodiscard]]
  std::vector<MapChunk*> chunks_in_rect (glm::vec3 const& pos, float radius) const;

  const TileIndex index;
  float xbase, zbase;

  // Three different questions used to be answered by this one flag, and MapIndex::unloadTiles
  // (map_index.cpp) could not tell them apart, so any of the three kept the tile in memory for
  // the rest of the session. `changed` now means exactly one of them: THIS TILE HOLDS EDITS THAT
  // ARE NOT ON DISK. That is what "save changed tiles" writes out (MapIndex::saveChanged) and
  // what the minimap paints in its own colour (minimap_widget.cpp:468).
  //
  // The other two have their own homes now:
  //   - "an operation is holding raw MapTile*/MapChunk* pointers into this tile across something
  //     that pumps the event loop" is a lifetime, not an edit. Use pin()/unpin() below.
  //   - "a model was re-attached to this tile while it streamed in" is neither. See
  //     tile_dirty_intent in map_index.hpp.
  std::atomic<bool> changed;

  // A count and not a bool, because two operations can legitimately hold the same tile at the same
  // time -- the ambient occlusion bake and the ground effect set editor both take whole-tile
  // scopes -- and the second one to finish must not release the first one's tile.
  //
  // A pinned tile is NOT dirty: nothing is written for it and the minimap does not mark it. It is
  // only undeletable, which is the entire requirement. Every pin() must be matched by an unpin()
  // on every exit path, including the ones taken when the user cancels.
  void pin();
  void unpin();

  [[nodiscard]]
  bool pinned() const;

  // A third reason a tile must not be released, and not the same as either of the two above: the
  // last attempt to write this tile's ADT did not reach the disk, so the only surviving copy of
  // those edits is this object. `changed` cannot carry it, because the six export paths in
  // World.cpp call MapIndex::unsetChanged and then MapIndex::unloadTile unconditionally after
  // saveTile (World.cpp:2249-2255, 3310-3317, 4358-4364, 4421-4427, 4479-4485, 4531-4537), which
  // clears the flag and drops the tile whether or not the write worked.
  //
  // False for every tile that has never been saved, so it costs nothing in normal streaming: it
  // can only become true after MapTile::save has actually tried and failed, and it goes back to
  // false the moment a later save succeeds.
  [[nodiscard]]
  bool lastSaveFailed() const;

  bool _was_rendered_last_frame = false;

  bool intersect (math::ray const&, selection_result*);


  bool GetVertex(float x, float z, glm::vec3 *V);
  void getVertexInternal(float x, float z, glm::vec3* v);

	void CropWater();

  // Returns whether the ADT is on disk. False means the write failed and the previous file was
  // left exactly as it was, so `changed` must NOT be cleared for this tile: MapIndex::unloadTiles
  // refuses to release a tile whose `changed` flag is set, and that refusal is the only thing
  // keeping the edits alive in memory once the disk copy could not be replaced.
  //
  // Deliberately not [[nodiscard]]. Six call sites in World.cpp ignore the result today and the
  // user is told about the failure by Noggit::reportSaveFailure regardless of who checks; making
  // those six a warning would be noise, not safety.
  bool saveTile(World* world);

private:
  bool save(World* world, bool save_using_mclq_liquids);

public:

  bool isTile(int pX, int pZ);

  bool hasFlightBounds() const;;

  async_priority loading_priority() const override;

  bool has_model(uint32_t uid) const;

  void remove_model(uint32_t uid);
  void remove_model(SceneObject* instance);
  void add_model(uint32_t uid);
  void add_model(SceneObject* instance);

  TileWater Water;

  bool tile_is_being_reloaded() const;

  std::vector<uint32_t>* get_uids();

  void initEmptyChunks();

  void setFilename(const std::string& new_filename) {_file_key.setFilepath(new_filename);};

  QImage getHeightmapImage(float min_height, float max_height);
  QImage getAlphamapImage(unsigned layer);
  QImage getAlphamapImage(std::string const& filename);
  QImage getVertexColorsImage();
  QImage getNormalmapImage();
  void setHeightmapImage(QImage const& baseimage, float min_height, float max_height, int mode, bool tiledEdges);
  // void setWatermapImage(QImage const& baseimage, float multiplier, int mode, bool tiledEdges);
  void setAlphaImage(QImage const& image, unsigned layer, bool cleanup);
  void setVertexColorImage(QImage const& image, int mode, bool tiledEdges);
  void registerChunkUpdate(unsigned flags);;
  void endChunkUpdates();;
  std::array<float, 145 * 256 * 4>& getChunkHeightmapBuffer();;
  unsigned getChunkUpdateFlags() const;
  void recalcExtents();
  void recalcObjectInstanceExtents();
  void recalcCombinedExtents();
  std::array<glm::vec3, 2>& getExtents();;
  std::array<glm::vec3, 2>& getCombinedExtents();;

  World* getWorld();;

  [[nodiscard]]
  tsl::robin_map<AsyncObject*, std::vector<SceneObject*>> const& getObjectInstances() const;;

  float camDist() const;
  void calcCamDist(glm::vec3 const& camera);
  void markExtentsDirty();
  void tagCombinedExtents(bool state);;

  Noggit::Rendering::TileRender* renderer();;
  Noggit::Rendering::FlightBoundsRender* flightBoundsRenderer();;

  // By value: the underlying lookup returns a prvalue, so a reference return dangled. See MapTile.cpp.
  texture_heightmapping_data GetTextureHeightMappingData(const std::string& name) const;

  void forceAlphaUpdate();
  bool childrenFinishedLoading();
  bool texturesFinishedLoading();
  bool objectsFinishedLoading();

private:

  tile_mode _mode;
  bool _tile_is_being_reloaded;

  // Read by MapIndex::unloadTiles from the main thread while pin()/unpin() can be called from a
  // dialog that is pumping the event loop, so it is atomic for the same reason `changed` is.
  std::atomic<int> _pin_count {0};

  // Written by MapTile::save and read by MapIndex::unloadTile and MapIndex::unsetChanged. Atomic
  // for the same reason as the two above: saving runs from paths that are not all on one thread.
  std::atomic<bool> _last_save_failed {false};

  bool _extents_dirty = true;
  bool _combined_extents_dirty = true;
  bool _requires_object_extents_recalc = true;



  std::array<glm::vec3, 2> _extents;
  std::array<glm::vec3, 2> _object_instance_extents;
  std::array<glm::vec3, 2> _combined_extents;
  glm::vec3 _center;
  float _cam_dist;

  // MFBO: requires mFlags & 1
  glm::vec3 mMinimumValues[3 * 3] = {};
  glm::vec3 mMaximumValues[3 * 3] = {};

  unsigned _chunk_update_flags;

  bool _textures_finished_loading = false;
  bool _objects_finished_loading = false;

  // MHDR:
  int mFlags = 0;
  bool mBigAlpha;

  // Data to be loaded and later unloaded.
  std::vector<std::string> mTextureFilenames;
  // std::vector<std::string> mModelFilenames;
  // std::vector<std::string> mWMOFilenames;
  std::map<std::string, mtxf_entry> _mtxf_entries;
  
  std::vector<uint32_t> uids;
  tsl::robin_map<AsyncObject*, std::vector<SceneObject*>> object_instances; // only includes M2 and WMO. perhaps a medium common ancestor then?

  std::unique_ptr<MapChunk> mChunks[16][16];
  std::array<float, 145 * 256 * 4> _chunk_heightmap_buffer;

  bool _load_models;
  bool _load_textures;
  World* _world;

  Noggit::Rendering::TileRender _renderer;
  Noggit::Rendering::FlightBoundsRender _fl_bounds_render;

  Noggit::NoggitRenderContext _context;

};
