// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_AMBIENTOCCLUSION_HPP
#define NOGGIT_AMBIENTOCCLUSION_HPP

#include <cstddef>
#include <limits>
#include <vector>

// Ambient-occlusion baking into terrain vertex colours (MCCV).
//
// The split runs THROUGH this header, the same shape as AssetScan.hpp:
//
//   - Everything with an out-of-line definition lives in AmbientOcclusion.cpp and depends on
//     nothing but the STL -- no Qt, no OpenGL, no glm, no MapChunk. That is what lets
//     tests/AmbientOcclusionTests.cpp link it on a bare machine. In particular MapHeaders.h is
//     NOT included even for TILESIZE/UNITSIZE, because it opens with <glm/vec3.hpp> and the
//     standalone test target has no glm include path; the two constants are restated below with
//     their source cited.
//   - AmbientOcclusionBaker, at the bottom, is the world walk. Templates NOT because anything is
//     generic -- there is one MapChunk and one MapTile -- but because a template body is not
//     looked up until instantiation, which keeps MapTile.h and MapChunk.h out of this header.
//
// Deliberately NOT a Tool subclass. editing_mode::mccv is already owned by VertexPainterTool and
// ShaderTool; this is the estimator they lack, not a fourth way to paint.
//
// ---------------------------------------------------------------------------------------------
// MCCV encoding, as measured in THIS tree rather than assumed
//
// The claim "WoW's neutral vertex colour is 127, not 255" is CORRECT here, and the reason is that
// MCCV is a multiplier, not a colour:
//
//   read   MapChunk.cpp:288   mccv[i] = vec3(t[2] / 127.0f, t[1] / 127.0f, t[0] / 127.0f)
//   write  MapChunk.cpp:1564  lmccv[i] = (unsigned char)(mccv[i].z * 127.0f) | ... << 8 | ... << 16
//   init   MapChunk.cpp:2152  mccv[i] = {1.0f, 1.0f, 1.0f}          -- "set default shaders"
//   shade  terrain_frag.glsl:353  out_color.rgb *= vary_mccv
//   absent MapChunk.cpp:293   a chunk with no MCCV block is filled with 1.0f
//
// So there are three domains and it is worth naming them, because confusing two of them is how
// vertex paint ends up twice as bright as intended:
//
//   disk    unsigned char 0..255, neutral 127
//   memory  float multiplier, neutral 1.0f, clamped to [0, 2] by the editor (MapChunk.cpp:914)
//   UI      float 0..1, neutral 0.5 -- ChangeMCCV divides by 0.5f (MapChunk.cpp:903) to reach
//           the memory domain, and setVertexColorImage multiplies QColor::redF() by 2
//           (MapChunk.cpp:2138)
//
// This module works in the disk (byte) and memory (factor) domains and states which in every
// name. The UI domain is the integrator's problem.
//
// One consequence worth knowing before trusting a round trip. The writer at MapChunk.cpp:1564
// casts float to unsigned char, which TRUNCATES rather than rounds. That sounds like it should
// cost an LSB per save, and it does not -- measured, not reasoned about: for all 256 values,
// (unsigned char)((b / 127.0f) * 127.0f) == b in IEEE single precision, so loading and saving an
// untouched chunk is lossless. The relative error of the two operations is ~1e-7 and never
// crosses an integer boundary at this magnitude.
//
// Where truncation DOES cost an LSB is a factor that is not a b/127 quotient, i.e. any value a
// tool computed -- which is every value this module produces. Over 1001 factors sampled evenly
// across [0, 2], truncating and rounding disagree 500 times, always with truncation the darker.
// mccvFactorToByte below therefore rounds. It cannot fix the writer, but a bake that lands on
// byte 64 stays on byte 64 instead of arriving as 63.
namespace Noggit
{
  // --- MCCV domain constants -------------------------------------------------------------

  // Disk-domain neutral. The whole point of the module's colour half.
  inline constexpr unsigned char MCCV_NEUTRAL_BYTE = 127;
  inline constexpr int MCCV_BYTE_MAX = 255;

  // Largest memory-domain factor the editor itself permits (MapChunk.cpp:914-916). Exceeding it
  // is not merely out of range: MapChunk.cpp:1564 masks with 0xFF after an unsigned char cast, so
  // a factor above 255/127 WRAPS -- a vertex meant to be blown out saves as a black one.
  inline constexpr float MCCV_MAX_FACTOR = 2.0f;

  // MapChunk.h:52. Restated rather than included; see the header note.
  inline constexpr unsigned CHUNK_UPDATE_FLAG_MCCV = 0x4;

  // --- terrain lattice constants ---------------------------------------------------------

  // MapHeaders.h:58-60. Restated, not included; see the header note.
  inline constexpr float TILE_SIZE_YARDS = 533.33333f;
  inline constexpr float CHUNK_SIZE_YARDS = TILE_SIZE_YARDS / 16.0f;
  inline constexpr float UNIT_SIZE_YARDS = CHUNK_SIZE_YARDS / 8.0f;

  // MCVT/MCCV vertex count per chunk: 9*9 outer + 8*8 inner (MapChunk.h:46).
  inline constexpr int CHUNK_VERTEX_COUNT = 145;

  // A chunk's 145 vertices do not sit on one square lattice -- the 8x8 inner vertices are offset
  // by half a unit in both axes (MapChunk.cpp:180-184). Sampling at HALF UNIT_SIZE_YARDS is the
  // coarsest spacing on which all 145 land exactly on grid nodes, which is what lets the baker
  // address them by integer index and never round a world coordinate.
  inline constexpr float DEFAULT_GRID_SPACING = UNIT_SIZE_YARDS * 0.5f;

  // Grid CELLS spanned by one chunk / one tile at DEFAULT_GRID_SPACING. Node counts are these +1.
  inline constexpr int CHUNK_GRID_SPAN = 16;
  inline constexpr int TILE_GRID_SPAN = CHUNK_GRID_SPAN * 16;

  // --- pure: height field ----------------------------------------------------------------

  // A regular square-lattice height field in world units, with an optional world-space origin.
  //
  // Heights are absolute world Y. That is what MapChunk stores: mVertices[i].y is xbase-relative
  // only until MapChunk.cpp:200 zeroes ybase, after which it is absolute.
  //
  // A sample may be NaN, and that is load-bearing rather than an error state: it is how the baker
  // marks "no chunk has been written here yet" so fillGaps can interpolate, and how the occlusion
  // estimator recognises a sample it must ignore rather than propagate.
  class HeightField
  {
  public:
    HeightField();
    // `spacing` is world units between adjacent nodes. Non-positive or non-finite width, height
    // or spacing yields an empty field rather than a throw -- every query on an empty field has a
    // defined answer, so a bad size degrades to "no occlusion" instead of taking the editor down.
    HeightField(int width, int height, float spacing = 1.0f, float fill_value = 0.0f);
    // A trailing partial row is DROPPED, not padded: padding would invent heights.
    HeightField(std::vector<float> heights, int width, float spacing = 1.0f);

    int width() const;
    int height() const;
    float spacing() const;
    bool empty() const;
    std::size_t size() const;

    // World coordinate of node (0,0). Z, not Y: the field is a plan view.
    void setOrigin(float world_x, float world_z);
    float originX() const;
    float originZ() const;

    // True only for indices actually inside the field. at() does NOT use this -- it clamps -- so
    // callers that must distinguish an edge node from beyond-the-edge have to ask separately.
    // fillGaps is exactly such a caller: a clamped read at the border returns the border node
    // itself, which would let a NaN border fill from its own NaN.
    bool contains(int x, int y) const;

    // Clamp-to-edge read. Out-of-range indices return the nearest border node, i.e. the terrain
    // is treated as extending flat outward forever. Chosen over "unoccluded beyond the border"
    // because it keeps the horizon search continuous across the edge of the sampled region; the
    // cost is that a field with no margin under-occludes near its border, which is what the
    // `margin` parameter of makeTileHeightField exists to fix.
    float at(int x, int y) const;

    // Ignores out-of-range indices. That is what makes feeding neighbouring tiles into a field
    // sized for one tile a no-op for the parts that fall outside, with no clipping arithmetic at
    // the call site.
    void set(int x, int y, float value);

    void fill(float value);

    // Bilinear, clamp-to-edge, in GRID coordinates. Returns NaN for a non-finite coordinate and
    // for an empty field. NaN samples propagate; callers must test the result.
    float sampleGrid(float grid_x, float grid_y) const;
    float sampleWorld(float world_x, float world_z) const;

    // World <-> grid, fractional. No rounding and no bounds test.
    float toGridX(float world_x) const;
    float toGridY(float world_z) const;
    float toWorldX(float grid_x) const;
    float toWorldZ(float grid_y) const;

    std::vector<float> const& heights() const;

  private:
    std::vector<float> _heights;
    int _width = 0;
    int _height = 0;
    float _spacing = 1.0f;
    float _origin_x = 0.0f;
    float _origin_z = 0.0f;
  };

  // Replaces every non-finite sample with the mean of its finite 8-neighbours, repeatedly.
  //
  // Two-phase per pass: every replacement in a pass is computed from the state at the START of
  // that pass, so the result does not depend on scan order and two runs agree exactly. Returns
  // the number of samples filled. Samples with no finite neighbour within `max_passes` steps stay
  // NaN, which the occlusion estimator then ignores -- there is no invented height anywhere.
  std::size_t fillGaps(HeightField& field, int max_passes = 4);

  // --- pure: occlusion estimator ---------------------------------------------------------

  struct AmbientOcclusionSettings
  {
    // Azimuths sampled around the vertex. Cost is direction_count * step_count samples per
    // vertex; 16 x 12 over a 257x257 tile field is ~7.9M bilinear reads, which is seconds, not
    // minutes, and is why this is a bake and not a per-frame effect.
    int direction_count = 16;
    // Radial samples per direction. Too few and a narrow ridge is stepped over entirely.
    int step_count = 12;
    // Horizon search radius in world units (yards). 20 is a little over two chunk-units either
    // side -- large enough for a gully, small enough that a distant mountain does not flatten a
    // whole valley into uniform shadow.
    float max_radius = 20.0f;
    // Scales the result. Clamped into [0, 8] by sanitized(); the RESULT is still clamped to
    // [0, 1], so a strength above 1 saturates rather than escaping the range.
    float strength = 1.0f;
    // Horizon elevations below this are treated as no horizon at all. Zero by default so the
    // estimator means exactly what it says. A few degrees is worth setting for real terrain: with
    // no bias every gentle uniform slope occludes itself and the whole map merely gets darker,
    // which is the one result indistinguishable from a bug.
    float bias_degrees = 0.0f;
    // Weight the hemisphere by the cosine of the incident angle (Lambert) rather than by solid
    // angle alone. Per direction the visible fraction is cos^2(horizon) weighted, 1 - sin(horizon)
    // unweighted -- both are 1 at a flat horizon and 0 at a vertical wall, but the weighted form
    // reaches darkness more slowly, which is what matches a rendered reference.
    bool cosine_weighted = true;

    // Clamps every field into a range with defined behaviour. Idempotent.
    AmbientOcclusionSettings sanitized() const;
    // True when a sanitized copy of these settings can produce a non-zero result.
    bool valid() const;
  };

  // Elevation of the horizon in radians along one azimuth, in [0, pi/2), measured from the
  // horizontal at the sample point. Zero when nothing along that azimuth rises above the sample.
  //
  // Exposed rather than hidden in the .cpp because it is the one quantity with an answer that can
  // be worked out by hand -- a 45-degree ramp has a 45-degree horizon uphill and none downhill --
  // which makes it the anchor the aggregate tests hang off.
  //
  // `azimuth_radians` is measured in GRID space: 0 is +x, pi/2 is +y.
  float horizonAngleAtGrid( HeightField const& field
                          , float grid_x
                          , float grid_y
                          , float azimuth_radians
                          , AmbientOcclusionSettings const& settings
                          );

  // Occlusion at one point: 0 is fully open sky, 1 is fully enclosed. Always in [0, 1], for every
  // input including NaN coordinates, NaN heights, infinities and degenerate settings -- those all
  // yield 0, the "no occlusion" answer, because darkening terrain on the strength of a value the
  // estimator could not compute is the worse failure.
  float occlusionAtGrid(HeightField const& field, float grid_x, float grid_y, AmbientOcclusionSettings const& settings);
  float occlusionAtWorld(HeightField const& field, float world_x, float world_z, AmbientOcclusionSettings const& settings);

  // Occlusion at every node, row-major, same size as the field. Empty for an empty field.
  std::vector<float> bakeOcclusionField(HeightField const& field, AmbientOcclusionSettings const& settings);

  // --- pure: occlusion -> MCCV -----------------------------------------------------------

  // Disk-domain colour. Defaults to neutral so a default-constructed one is "no change".
  struct MccvColor
  {
    unsigned char r = MCCV_NEUTRAL_BYTE;
    unsigned char g = MCCV_NEUTRAL_BYTE;
    unsigned char b = MCCV_NEUTRAL_BYTE;
  };

  bool operator==(MccvColor lhs, MccvColor rhs);
  bool operator!=(MccvColor lhs, MccvColor rhs);

  inline constexpr MccvColor MCCV_NEUTRAL{MCCV_NEUTRAL_BYTE, MCCV_NEUTRAL_BYTE, MCCV_NEUTRAL_BYTE};

  struct AmbientOcclusionColorSettings
  {
    // Memory-domain multiplier where nothing occludes. 1.0 is neutral; leave it there unless the
    // intent is to brighten the whole map, which is not what an AO bake is for.
    float open_factor = 1.0f;
    // Multiplier in the darkest crevice. sanitized() clamps this to at most open_factor, which is
    // what makes the mapping monotonically non-increasing in occlusion by construction: AO can
    // only ever darken.
    float occluded_factor = 0.45f;
    // Tint of the SHADOW end only, per channel, in [0, 1]. Applied to occluded_factor and not to
    // open_factor, so an unoccluded vertex stays exactly neutral and no hue is introduced where
    // there is no occlusion. Slightly lifting blue is the usual outdoor sky-bounce cheat.
    float shadow_tint_r = 1.0f;
    float shadow_tint_g = 1.0f;
    float shadow_tint_b = 1.0f;
    // Exponent applied to occlusion before the interpolation. Above 1 confines darkening to the
    // deepest pockets; below 1 spreads it. Monotone for any positive value, so it cannot break
    // the ordering guarantee.
    float contrast = 1.0f;

    AmbientOcclusionColorSettings sanitized() const;
  };

  // Memory-domain multiplier for an occlusion value, ignoring tint. Non-increasing in occlusion.
  float aoToMccvFactor(float occlusion, AmbientOcclusionColorSettings const& settings);

  // Disk-domain colour for an occlusion value. Every channel is non-increasing in occlusion and
  // always inside [0, 255].
  MccvColor aoToMccvColor(float occlusion, AmbientOcclusionColorSettings const& settings);

  // Domain conversions, matching MapChunk.cpp:288 and :1564 -- except that the byte direction
  // ROUNDS where MapChunk truncates. See the note at the top of this header.
  float mccvByteToFactor(unsigned char value);
  unsigned char mccvFactorToByte(float factor);

  // Clamps a memory-domain factor into the range the ADT writer can represent without wrapping.
  float clampMccvFactor(float factor);

  // Layers `overlay` onto `base` multiplicatively in factor space -- the correct way to apply a
  // bake over existing hand-painted vertex colour, because a neutral overlay is exactly the
  // identity and the artist's hue survives.
  MccvColor multiplyMccv(MccvColor base, MccvColor overlay);

  // Linear interpolation in factor space. weight 0 is `base`, weight 1 is `target`. For exposing
  // bake strength as a slider without recomputing occlusion.
  MccvColor blendMccv(MccvColor base, MccvColor target, float weight);

  // --- pure: chunk vertex lattice --------------------------------------------------------

  // Position of one MCVT/MCCV vertex on a half-unit grid local to its chunk.
  struct ChunkVertexGridOffset
  {
    int column = -1;
    int row = -1;
  };

  // Grid offset for vertex `index` in the 145-entry MCVT order, or {-1, -1} out of range.
  //
  // Encodes the loader's own traversal (MapChunk.cpp:176-190): 17 rows, alternating 9 outer and 8
  // inner vertices, inner rows offset half a unit in x, rows half a unit apart in z. On a
  // DEFAULT_GRID_SPACING lattice that is: row = j, column = 2*i for an outer row and 2*i+1 for an
  // inner one -- so every vertex lands on an integer node and the baker never rounds a float
  // coordinate to find one.
  //
  // mccv[] and mVertices[] share this order: both are filled by a single loop over all 145
  // entries (MapChunk.cpp:176-190 and :285-289), so an index into one indexes the other.
  ChunkVertexGridOffset chunkVertexGridOffset(int index);

  // Nearest grid node to a world coordinate. Rounds; the chunk origins the baker feeds it are
  // exact multiples of the spacing, so the rounding is only there to absorb float representation
  // error in xbase/zbase, not to resample.
  int gridColumnForWorldX(HeightField const& field, float world_x);
  int gridRowForWorldZ(HeightField const& field, float world_z);

  // How a bake combines with the vertex colour already on the chunk.
  enum class AoBlendMode
  {
    // Overwrite. Existing vertex paint is lost.
    Replace,
    // Multiply into what is there. Neutral existing colour gives the same result as Replace, so
    // this is the safe default for a map with hand-painted vertex colour.
    //
    // NOT idempotent, and that is the one trap in this module: baking twice multiplies twice and
    // the terrain gets darker each time. The integrator must either snapshot the chunk's mccv
    // into the undo action before baking -- which it has to do anyway to make the bake
    // undoable -- or expose Replace for a re-bake.
    Multiply
  };

  // --- impure: the world walk ------------------------------------------------------------

  // Templates only, so this header pulls in no Noggit headers; see the note at the top.
  //
  // Typical call from a TU that has included MapTile.h and MapChunk.h:
  //
  //   Noggit::AmbientOcclusionSettings ao;
  //   Noggit::AmbientOcclusionColorSettings colors;
  //
  //   Noggit::HeightField field
  //     = Noggit::AmbientOcclusionBaker::makeTileHeightField<MapTile, MapChunk>(tile, ao.max_radius);
  //
  //   for (MapTile* neighbour : surrounding_tiles)                 // optional, removes the seam
  //     Noggit::AmbientOcclusionBaker::addTileToField<MapTile, MapChunk>(neighbour, field);
  //
  //   Noggit::fillGaps(field);
  //   Noggit::AmbientOcclusionBaker::bakeTile<MapTile, MapChunk>(tile, field, ao, colors);
  //
  // The caller is responsible for the undo action and for marking the tile changed; nothing here
  // touches World or MapIndex, so nothing here can mark a tile dirty behind the user's back.
  namespace AmbientOcclusionBaker
  {
    // Writes one chunk's 145 heights into `field`. Nodes outside the field are dropped, which is
    // what makes feeding a neighbouring tile into a tile-sized field free of clipping arithmetic.
    template <typename ChunkT>
    void addChunkToField(ChunkT* chunk, HeightField& field)
    {
      if (!chunk || field.empty())
      {
        return;
      }

      // xbase/zbase, not px/py: they are the chunk's world origin and are what mVertices was
      // built from (MapChunk.cpp:185), so this needs no knowledge of the tile's chunk indexing.
      int const base_column = gridColumnForWorldX(field, chunk->xbase);
      int const base_row = gridRowForWorldZ(field, chunk->zbase);

      for (int index = 0; index < CHUNK_VERTEX_COUNT; ++index)
      {
        ChunkVertexGridOffset const offset = chunkVertexGridOffset(index);
        field.set(base_column + offset.column, base_row + offset.row, chunk->mVertices[index].y);
      }
    }

    // Writes all 256 chunks of a tile.
    template <typename TileT, typename ChunkT>
    void addTileToField(TileT* tile, HeightField& field)
    {
      // finishedLoading() rather than trusting the caller: reading mVertices of a tile still being
      // parsed by the async loader is a data race, and a bake is exactly the kind of long
      // operation a user starts right after clicking a new tile into view.
      if (!tile || !tile->finishedLoading() || field.empty())
      {
        return;
      }

      // 16x16 by hand because MapTile has no chunk iterator (MapTile.h:84 carries the \todo).
      // World::for_all_chunks_on_tile is unusable here: it calls mapIndex.setChanged(tile) on
      // entry (World.inl:16), so using it to READ neighbouring tiles for margin would mark them
      // unsaved and put a save prompt in front of a user who edited one tile.
      for (unsigned int chunk_z = 0; chunk_z < 16; ++chunk_z)
      {
        for (unsigned int chunk_x = 0; chunk_x < 16; ++chunk_x)
        {
          addChunkToField<ChunkT>(tile->getChunk(chunk_x, chunk_z), field);
        }
      }
    }

    // A field covering one tile plus `margin_yards` of context on every side, filled with NaN and
    // then populated from that tile alone.
    //
    // The margin is the difference between a correct bake and a visible seam: without it, a
    // vertex on the tile border searches its horizon over clamp-to-edge terrain and comes back
    // unoccluded, so every tile boundary reads as a bright cross. Pass at least
    // AmbientOcclusionSettings::max_radius and add the neighbouring tiles with addTileToField;
    // anything the caller does not supply stays NaN and is ignored rather than guessed.
    template <typename TileT, typename ChunkT>
    HeightField makeTileHeightField(TileT* tile, float margin_yards, float spacing = DEFAULT_GRID_SPACING)
    {
      if (!tile)
      {
        return HeightField();
      }

      int margin_nodes = 0;

      if (margin_yards > 0.0f && spacing > 0.0f)
      {
        // +1 so the margin is never short by a fraction of a node.
        margin_nodes = static_cast<int>(margin_yards / spacing) + 1;
      }

      int const span = TILE_GRID_SPAN + 1 + 2 * margin_nodes;

      HeightField field(span, span, spacing);
      field.fill(std::numeric_limits<float>::quiet_NaN());
      field.setOrigin( tile->xbase - static_cast<float>(margin_nodes) * spacing
                     , tile->zbase - static_cast<float>(margin_nodes) * spacing
                     );

      addTileToField<TileT, ChunkT>(tile, field);

      return field;
    }

    // Bakes one chunk's vertex colours from an already-built field. Returns false when nothing
    // was written.
    //
    // initMCCV() first (MapChunk.cpp:2146): it is a no-op on a chunk that already has vertex
    // colour, and on one that does not it sets both the 1.0f defaults and header_flags has_mccv,
    // without which MapChunk::save writes ofsMCCV = 0 and the whole bake is silently discarded
    // (MapChunk.cpp:1553).
    template <typename ChunkT>
    bool bakeChunk( ChunkT* chunk
                  , HeightField const& field
                  , AmbientOcclusionSettings const& ao_settings
                  , AmbientOcclusionColorSettings const& color_settings
                  , AoBlendMode blend = AoBlendMode::Multiply
                  )
    {
      if (!chunk || field.empty())
      {
        return false;
      }

      int const base_column = gridColumnForWorldX(field, chunk->xbase);
      int const base_row = gridRowForWorldZ(field, chunk->zbase);

      chunk->initMCCV();

      for (int index = 0; index < CHUNK_VERTEX_COUNT; ++index)
      {
        ChunkVertexGridOffset const offset = chunkVertexGridOffset(index);

        float const occlusion = occlusionAtGrid( field
                                               , static_cast<float>(base_column + offset.column)
                                               , static_cast<float>(base_row + offset.row)
                                               , ao_settings
                                               );

        MccvColor const baked = aoToMccvColor(occlusion, color_settings);

        // auto& rather than glm::vec3&, so this header needs no glm. x/y/z are r/g/b; the
        // channel order flip lives in the file reader and writer, not here (MapChunk.cpp:288).
        auto& color = chunk->mccv[index];

        if (blend == AoBlendMode::Replace)
        {
          color.x = mccvByteToFactor(baked.r);
          color.y = mccvByteToFactor(baked.g);
          color.z = mccvByteToFactor(baked.b);
        }
        else
        {
          color.x = clampMccvFactor(color.x * mccvByteToFactor(baked.r));
          color.y = clampMccvFactor(color.y * mccvByteToFactor(baked.g));
          color.z = clampMccvFactor(color.z * mccvByteToFactor(baked.b));
        }
      }

      chunk->registerChunkUpdate(CHUNK_UPDATE_FLAG_MCCV);

      return true;
    }

    // Bakes all 256 chunks of a tile. Returns the number of chunks written.
    template <typename TileT, typename ChunkT>
    std::size_t bakeTile( TileT* tile
                        , HeightField const& field
                        , AmbientOcclusionSettings const& ao_settings
                        , AmbientOcclusionColorSettings const& color_settings
                        , AoBlendMode blend = AoBlendMode::Multiply
                        )
    {
      if (!tile || !tile->finishedLoading() || field.empty())
      {
        return 0;
      }

      std::size_t baked = 0;

      for (unsigned int chunk_z = 0; chunk_z < 16; ++chunk_z)
      {
        for (unsigned int chunk_x = 0; chunk_x < 16; ++chunk_x)
        {
          if (bakeChunk<ChunkT>(tile->getChunk(chunk_x, chunk_z), field, ao_settings, color_settings, blend))
          {
            ++baked;
          }
        }
      }

      return baked;
    }
  }
}

#endif // NOGGIT_AMBIENTOCCLUSION_HPP
