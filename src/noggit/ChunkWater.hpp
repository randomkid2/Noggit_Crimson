// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#pragma once
#include <math/frustum.hpp>
#include <noggit/liquid_layer.hpp>
#include <noggit/MapHeaders.h>
#include <noggit/tool_enums.hpp>

#include <array>
#include <cstddef>
#include <functional>
#include <vector>

class MapChunk;
class TileWater;

// One chunk-layer's worth of pending liquid vertex heights. The smooth brush computes these
// for every chunk in range before any of them is written, so that a kernel reaching over a
// chunk border always reads pre-stroke values; see the seam argument in liquid_layer.cpp.
struct LiquidHeightPatch
{
  std::size_t layer;
  std::array<float, 9 * 9> height;
  std::array<bool, 9 * 9> mask;
};

namespace BlizzardArchive
{
  class ClientFile;
}

namespace util
{
  class sExtendableArray;
}

class ChunkWater
{
public:
  ChunkWater() = delete;
  explicit ChunkWater(MapChunk* chunk, TileWater* water_tile, float x, float z, bool use_mclq_green_lava);

  ChunkWater (ChunkWater const&) = delete;
  ChunkWater (ChunkWater&&) = delete;
  ChunkWater& operator= (ChunkWater const&) = delete;
  ChunkWater& operator= (ChunkWater&&) = delete;

  void from_mclq(std::vector<mclq>& layers);
  void fromFile(BlizzardArchive::ClientFile& f, size_t basePos);
  void save(util::sExtendableArray& adt, int base_pos, int& header_pos, int& current_pos);
  void save_mclq(util::sExtendableArray& adt, int mcnk_pos, int& current_pos);

  bool is_visible ( const float& cull_distance
                  , const math::frustum& frustum
                  , const glm::vec3& camera
                  , display_mode display
                  ) const;

  void autoGen(MapChunk* chunk, float factor);
  void update_underground_vertices_depth(MapChunk* chunk);
  void CropWater(MapChunk* chunkTerrain);

  void setType(int type, size_t layer);
  int getType(size_t layer) const;
  bool hasData(size_t layer) const;
  void tagUpdate();

  std::vector<liquid_layer>* getLayers();

  // update every layer's render
  void update_layers();
  float getMinHeight() const;
  float getMaxHeight() const;

  void paintLiquid( glm::vec3 const& pos
                  , float radius
                  , int liquid_id
                  , bool add
                  , math::radians const& angle
                  , math::radians const& orientation
                  , bool lock
                  , glm::vec3 const& origin
                  , bool override_height
                  , bool override_liquid_id
                  , MapChunk* chunk
                  , float opacity_factor
                  );

  // ---- per-vertex height brushes ---------------------------------------------------------
  //
  // Each of these fans the brush out over EVERY layer of the chunk rather than over one layer
  // index. Layer order is per chunk and nothing in the format ties chunk N's layer 0 to chunk
  // N+1's layer 0, so selecting by index would let a stroke move one side of a chunk border
  // and not the other - the exact tear these brushes exist to avoid. Fanning over all layers
  // and matching by liquid id where a lookup is needed keeps both owners of a shared vertex
  // in step. Chunks with two stacked layers are rare and both move together.
  //
  // `chunk` is the MapChunk the caller iterated rather than ChunkWater::_chunk, matching
  // paintLiquid. World already holds it, and the depth/opacity update reads that chunk's
  // terrain heights, so taking it from the caller keeps this off TileWater's chunk-index
  // plumbing entirely - two of the four places in that file pair a ChunkWater with the
  // transposed MapChunk (see the report), so it is not somewhere to acquire a dependency.

  bool changeLiquidHeight ( glm::vec3 const& pos
                          , float change
                          , float radius
                          , float inner_radius
                          , int brush_type
                          , MapChunk* chunk
                          , float opacity_factor
                          );

  bool flattenLiquidHeight ( glm::vec3 const& pos
                           , float remain
                           , float radius
                           , int brush_type
                           , flatten_mode const& mode
                           , glm::vec3 const& origin
                           , math::radians const& angle
                           , math::radians const& orientation
                           , MapChunk* chunk
                           , float opacity_factor
                           );

  bool gatherSmoothedLiquidHeights ( glm::vec3 const& pos
                                   , float remain
                                   , float radius
                                   , int brush_type
                                   , flatten_mode const& mode
                                   , std::function<bool (float, float, int, float&)> const& sampler
                                   , std::vector<LiquidHeightPatch>& patches
                                   ) const;

  bool applyLiquidHeightPatches ( std::vector<LiquidHeightPatch> const& patches
                                , MapChunk* chunk
                                , float opacity_factor
                                );

  // Reads the height of the liquid grid vertex nearest (x, z) on the layer whose liquid id is
  // `liquid_id`; pass a negative id to accept the first layer that has data there. Used as the
  // smooth kernel's sampler and by the seam weld.
  bool liquidHeightAt (float x, float z, int liquid_id, float& height) const;

  // The write half of the same lookup. Returns true when a copy of that vertex was found and
  // written. The caller is responsible for registering the undo snapshot first, and for
  // calling finishLiquidHeightEdit once the batch of writes is done - neither this nor
  // liquid_layer::setVertexHeight refreshes the derived min/max or the render tag, because the
  // seam weld writes single vertices in bulk and would pay for both on every one of them.
  bool setLiquidHeightAt (float x, float z, int liquid_id, float height, MapChunk* chunk, float opacity_factor);

  void finishLiquidHeightEdit();

  MapChunk* getChunk();
  TileWater* getWaterTile();

  MH2O_Attributes const& getAttributes() const;
  MH2O_Attributes& getAttributes();

  float xbase, zbase;

  int layer_count() const;

private:
  MH2O_Attributes attributes;

  glm::vec3 vmin, vmax, vcenter;
  bool _use_mclq_green_lava;

  // remove empty layers
  void cleanup();

  void copy_height_to_layer(liquid_layer& target, glm::vec3 const& pos, float radius);

  bool _auto_update_attributes = true;
  // updates attributes for all layers
  void update_attributes();

  std::vector<liquid_layer> _layers;
  int _layer_count = 0;

  MapChunk* _chunk;
  TileWater* _water_tile;

  friend class MapView;
};
