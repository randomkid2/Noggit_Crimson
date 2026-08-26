// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

#include <math/trig.hpp>
#include <noggit/MapHeaders.h>
#include <noggit/tool_enums.hpp>
#include <array>
#include <functional>
#include <glm/vec2.hpp>

class MapChunk;
class ChunkWater;

namespace util
{
  class sExtendableArray;
}

enum LiquidLayerUpdateFlags
{
  ll_HEIGHT = 0x1,
  ll_DEPTH = 0x2,
  ll_UV = 0x4,
  ll_TYPE = 0x8,
  ll_FLAGS = 0x10
};

enum LiquidVertexFormats
{
    LVF_HEIGHT_DEPTH = 0,
    LVF_HEIGHT_UV = 1,
    LVF_DEPTH = 2,
    LVF_HEIGHT_DEPTH_UV = 3
};

namespace BlizzardArchive
{
  class ClientFile;
}

// handle liquids like oceans, lakes, rivers, slime, magma
class liquid_layer
{
struct liquid_vertex
{
    glm::vec3 position;
    glm::vec2 uv;
    float depth;

    liquid_vertex() = default;
    liquid_vertex(glm::vec3 const& pos, glm::vec2 const& uv, float depth);
};

public:
  liquid_layer() = delete;
  liquid_layer(ChunkWater* chunk, glm::vec3 const& base, float height, int liquid_id);
  liquid_layer(ChunkWater* chunk, glm::vec3 const& base, mclq& liquid, int liquid_id);
  liquid_layer(ChunkWater* chunk, BlizzardArchive::ClientFile& f, std::size_t base_pos, glm::vec3 const& base, MH2O_Information const& info, std::uint64_t infomask);

  liquid_layer(liquid_layer const& other);
  liquid_layer(liquid_layer&&) noexcept;

  liquid_layer& operator=(liquid_layer&&) noexcept;
  liquid_layer& operator=(liquid_layer const& other);

  void save(util::sExtendableArray& adt, int base_pos, int& info_pos, int& current_pos) const;
  mclq to_mclq(MH2O_Attributes& attributes) const;

  void update_attributes(MH2O_Attributes& attributes);
  void changeLiquidID(int id);

  void crop(MapChunk* chunk);
  void update_opacity(MapChunk* chunk, float factor);
  void update_underground_vertices_depth(MapChunk* chunk);

  std::array<liquid_vertex, 9 * 9>& getVertices();
  // std::array<float, 9 * 9>& getDepth() { return _depth; };
  // std::array<glm::vec2, 9 * 9>& getTexCoords() { return _tex_coords; };

  float min() const;
  float max() const;
  int liquidID() const;
  int mclq_liquid_type() const;
  // order of the flag corresponding to the liquid type in the mcnk header
  int mclq_flag_ordering() const;

  // used for fatigue calculation
  bool subchunk_at_max_depth(int x, int z) const;

  bool hasSubchunk(int x, int z, int size = 1) const;
  void setSubchunk(int x, int z, bool water);

  std::uint64_t getSubchunks();

  bool empty() const;
  bool full() const;
  void clear();

  void paintLiquid(glm::vec3 const& pos
                  , float radius
                  , bool add
                  , math::radians const& angle
                  , math::radians const& orientation
                  , bool lock
                  , glm::vec3 const& origin
                  , bool override_height
                  , MapChunk* chunk
                  , float opacity_factor
                  );

  // ---- per-vertex height brushes ----------------------------------------------------------
  //
  // These edit the same std::array<liquid_vertex, 9 * 9> that the MH2O and MCLQ paths already
  // round-trip (heights live in liquid_vertex::position.y), so none of them reaches
  // serialization: only the float values that were already being written change.
  //
  // Every one of them measures distance from a CANONICAL grid position rather than from the
  // stored liquid_vertex::position. The seam argument for that is at the head of the brush
  // block in liquid_layer.cpp and it is the reason these brushes do not tear at chunk edges.

  bool changeHeight ( glm::vec3 const& pos
                    , float change
                    , float radius
                    , float inner_radius
                    , int brush_type
                    , MapChunk* terrain_chunk
                    , float opacity_factor
                    );

  bool flattenHeight ( glm::vec3 const& pos
                     , float remain
                     , float radius
                     , int brush_type
                     , flatten_mode const& mode
                     , glm::vec3 const& origin
                     , math::radians const& angle
                     , math::radians const& orientation
                     , MapChunk* terrain_chunk
                     , float opacity_factor
                     );

  // Smoothing is split in two because the kernel reaches across chunk borders: if a stroke
  // wrote chunk N before sampling for chunk N+1, the second chunk would average against
  // already-smoothed values and the two owners of a shared border vertex would disagree.
  // gather reads only pre-stroke heights, apply writes; World runs all the gathers first.
  bool gatherSmoothedHeights ( glm::vec3 const& pos
                             , float remain
                             , float radius
                             , int brush_type
                             , flatten_mode const& mode
                             , std::function<bool (float, float, int, float&)> const& sampler
                             , std::array<float, 9 * 9>& target
                             , std::array<bool, 9 * 9>& mask
                             ) const;

  void applyHeights ( std::array<float, 9 * 9> const& target
                    , std::array<bool, 9 * 9> const& mask
                    , MapChunk* terrain_chunk
                    , float opacity_factor
                    );

  // A vertex carries data when at least one of the up-to-four subchunks touching it exists.
  // The four corners of the 9x9 grid touch one subchunk, the edges two, the interior four.
  bool hasVertexData (int x, int z) const;
  bool vertexHeight (int x, int z, float& height) const;

  // Deliberately does NOT refresh the layer's derived min/max: the seam weld writes one vertex
  // at a time and would otherwise pay an 81-vertex rescan plus a fatigue check per write.
  // Call updateMinMax() once when a batch of these is finished.
  void setVertexHeight (int x, int z, float height, MapChunk* terrain_chunk, float opacity_factor);

  void updateMinMax();

  // Index of this layer's (0, 0) vertex on the map-wide liquid grid, counted in UNITSIZE
  // steps from the world origin. Two chunks sharing a border vertex produce the same pair.
  int gridOriginX() const;
  int gridOriginZ() const;

  // The map-wide liquid vertex grid. Every caller that needs a vertex world position or a
  // vertex index from one must go through these two, so that the whole feature agrees on one
  // definition of where a vertex is; the seam argument in liquid_layer.cpp depends on it.
  static int gridIndex (float world_coord);
  static float gridCoord (int index);

  void copy_subchunk_height(int x, int z, liquid_layer const& from);

  ChunkWater* getChunk();

  bool has_fatigue() const;

private:
  void create_vertices(float height);

  void update_min_max();
  void update_vertex_opacity(int x, int z, MapChunk* chunk, float factor);
  int get_lod_level(glm::vec3 const& camera_pos) const;
  // void set_lod_level(int lod_level);

  bool check_fatigue() const;
  // gets enabled when all subchunks are at max depth and type is ocean : check_fatigue()
  bool _fatigue_enabled = false;

  int _liquid_id;
  int _liquid_type;
  int _liquid_vertex_format;
  int _mclq_liquid_type;
  float _minimum;
  float _maximum;

  std::uint64_t _subchunks;
  // std::array<glm::vec3, 9 * 9> _vertices;
  // std::array<float, 9 * 9> _depth;
  // std::array<glm::vec2, 9 * 9> _tex_coords;

  // std::vector<liquid_vertex> _vertices;
  std::array<liquid_vertex, 9 * 9> _vertices;

  // std::map<int, std::vector<liquid_indice>> _indices_by_lod;


private:
  glm::vec3 pos;
  ChunkWater* _chunk;

  friend class MapView;
};
