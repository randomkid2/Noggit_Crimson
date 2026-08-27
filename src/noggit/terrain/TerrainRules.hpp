// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_TERRAINRULES_HPP
#define NOGGIT_TERRAINRULES_HPP

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Rule-driven automatic texturing by slope and height: "rock above 60 degrees, snow above 400,
// grass elsewhere".
//
// The module is split in two, and the split runs THROUGH this header rather than between two
// headers, the same arrangement as AssetScan.hpp for the same reason:
//
//   - Everything with an out-of-line definition lives in TerrainRules.cpp and depends on nothing
//     but the STL. No Qt, no OpenGL, no glm, no MapChunk. That is what lets
//     tests/TerrainRulesTests.cpp link it on a bare machine, and the rule evaluator is the entire
//     point of the module, so it is the half that must be testable.
//   - TerrainRuleCollector, at the bottom, walks a loaded world. It is a set of templates NOT
//     because it needs to be generic -- there is one World, one MapTile, one MapChunk -- but
//     because a template body is not looked up until instantiation. That keeps World.h, MapTile.h
//     and MapChunk.h out of this header, so adding TerrainRules.cpp to the standalone test target
//     needs no defines and no stubs. It also means the collector CAN be tested, against a
//     hand-written stand-in chunk, which is what the tests do.
//
// glm is deliberately absent even though the rest of the tree uses it for vectors: the standalone
// test target adds no glm include directory, and a three-float POD costs nothing here.
namespace Noggit
{
  // Slope is reported as steepness from horizontal, so it is bounded to a quarter turn. See
  // slopeDegreesFromNormal for why the other half of the circle cannot occur.
  inline constexpr float TERRAIN_SLOPE_MIN_DEGREES = 0.0f;
  inline constexpr float TERRAIN_SLOPE_MAX_DEGREES = 90.0f;

  // Alphamap texels are bytes; a rule's blend strength is written straight into one.
  inline constexpr std::uint8_t TERRAIN_ALPHA_MAX = 255;

  // A closed interval [min, max], with infinite endpoints standing for "no constraint".
  //
  // Three edge cases are decided here rather than left to the caller, because every one of them
  // arises from a user typing numbers into two spin boxes:
  //
  //   - min == max is a legal one-value interval and matches exactly that value. It is NOT
  //     treated as empty.
  //   - min > max is INVERTED and matches nothing. The alternative -- silently swapping the
  //     endpoints -- turns a typo into a rule that paints half the continent, which is far more
  //     expensive to notice than a rule that paints nothing.
  //   - A NaN endpoint matches nothing, because the comparisons below are written so that NaN
  //     fails them. Failing closed is the right direction: a rule that cannot be evaluated must
  //     not win.
  struct TerrainRange
  {
    // Infinite by default, so a default-constructed range is an absent constraint and
    // `TerrainRule r; r.texture = "grass";` is already the catch-all rule.
    float min = -std::numeric_limits<float>::infinity();
    float max = std::numeric_limits<float>::infinity();

    // Unconstrained in both directions.
    static TerrainRange any();
    static TerrainRange atLeast(float min_value);
    static TerrainRange atMost(float max_value);
    static TerrainRange between(float min_value, float max_value);

    bool boundedBelow() const;
    bool boundedAbove() const;
    // Either endpoint constrains.
    bool bounded() const;
    // Inverted, i.e. can never match. min == max is NOT empty.
    bool empty() const;
    // 0, 1 or 2. Feeds TerrainRule::specificity, which is a precedence tie-break.
    int boundCount() const;

    // False for NaN unless the corresponding direction is unconstrained -- an absent constraint
    // performs no test, so a sample whose slope could not be computed still matches a catch-all.
    bool contains(float value) const;
  };

  // One point on the terrain, reduced to the only two things a rule looks at.
  struct TerrainSample
  {
    // World Y. Noggit is Y-up (MapChunk.h:101, mVertices).
    float height = 0.0f;
    // Steepness from horizontal in [0, 90]; may be NaN, see slopeDegreesFromNormal.
    float slope_degrees = 0.0f;
  };

  // Either constraint may be left unbounded, which is how "grass elsewhere" is written: a rule
  // with two unbounded ranges matches every sample and loses every tie to a rule that constrains
  // something. See TerrainRuleSet::precedes.
  struct TerrainRule
  {
    // Opaque here on purpose. This module never opens a file, so a texture is whatever string the
    // caller uses to name one -- an ADT-style "tileset\\..." path in the editor, a bare token in
    // a test.
    std::string texture;
    TerrainRange height = TerrainRange::any();
    TerrainRange slope = TerrainRange::any();
    // Higher wins. First and strongest precedence key.
    int priority = 0;
    // Alpha written where this rule wins. 0 is legal and means "claim the point but paint
    // nothing", which is a usable way to mask a region out.
    std::uint8_t strength = TERRAIN_ALPHA_MAX;
    // A disabled rule never matches and never wins. Kept in the list so a user can toggle a rule
    // without losing its numbers.
    bool enabled = true;

    bool matches(TerrainSample const& sample) const;
    // Number of constrained endpoints across both ranges, 0..4.
    int specificity() const;
  };

  // Which rule won a point and at what alpha.
  struct TerrainRuleResult
  {
    bool matched = false;
    // Index into the rule list AS GIVEN, not into precedence order. Meaningless when !matched.
    std::size_t rule_index = 0;
    // Borrowed from the winning rule. Valid until the TerrainRuleSet is modified -- addRule can
    // reallocate the underlying vector. Copy it if it has to outlive that.
    std::string_view texture;
    std::uint8_t alpha = 0;
  };

  // Y-up, matching the rest of the tree.
  struct TerrainNormal
  {
    float x = 0.0f;
    float y = 1.0f;
    float z = 0.0f;
  };

  // A terrain vertex as three plain floats. `height` is world Y.
  struct TerrainVertex
  {
    float x = 0.0f;
    float height = 0.0f;
    float z = 0.0f;
  };

  // Steepness of the surface in degrees, in [0, 90].
  //
  // The normal need not be unit length; MapChunk::recalcNorms normalises and then quantises to
  // 1/127 steps (MapChunk.cpp:840), so the stored normals are near-unit but not exactly unit, and
  // an unnormalised input must not skew the angle.
  //
  // The Y component is used in ABSOLUTE value, which is the one non-obvious decision here. Signed
  // treatment would map a downward-pointing normal to an angle above 90, and a fully inverted one
  // to 180 -- i.e. it would report a flipped flat surface as "flat" only after a 180-degree trip
  // that also reports a flipped cliff as 90. Terrain is a heightfield and cannot overhang, so a
  // negative Y component is always a sign error somewhere upstream, not geometry. Folding it
  // keeps a cliff reading as a cliff either way.
  //
  // Returns NaN for a zero-length or non-finite normal. NaN then fails every bounded slope range
  // (TerrainRange::contains) and passes every unbounded one, so a chunk with unusable normals
  // falls through to the catch-all rule instead of being painted as flat ground.
  float slopeDegreesFromNormal(TerrainNormal const& normal);

  // Same angle from a height gradient: dh/dx and dh/dz. Equivalent to
  // slopeDegreesFromNormal({-dh_dx, 1, -dh_dz}) and cheaper; the tests assert the two agree.
  float slopeDegreesFromGradient(float dh_dx, float dh_dz);

  // Normal of the plane through three points, oriented to +Y and normalised.
  //
  // Orientation to +Y is what makes the result independent of the corner order, so a caller
  // feeding triangles from a mesh with inconsistent winding gets consistent slopes.
  //
  // A degenerate triangle -- collinear or coincident corners, i.e. zero area -- has no plane, and
  // yields the zero normal {0, 0, 0} rather than a fabricated up vector. slopeDegreesFromNormal
  // turns that into NaN, so the degeneracy reaches the caller instead of masquerading as flat
  // ground.
  TerrainNormal normalFromTriangle(TerrainVertex const& a, TerrainVertex const& b, TerrainVertex const& c);

  float slopeDegreesFromTriangle(TerrainVertex const& a, TerrainVertex const& b, TerrainVertex const& c);

  // A rule list plus its precedence order.
  //
  // The order is computed once per mutation, not once per sample: a tile is 65536 alphamap texels
  // and a continent is thousands of tiles, so sorting inside evaluate() would dominate the run.
  // Holding the order also makes evaluate() a scan that stops at the first match, and makes
  // "which rule wins" answerable without evaluating anything.
  class TerrainRuleSet
  {
  public:
    TerrainRuleSet() = default;
    explicit TerrainRuleSet(std::vector<TerrainRule> initial_rules);

    // Returns the index of the added rule, which is what TerrainRuleResult::rule_index reports.
    std::size_t addRule(TerrainRule rule);
    void clear();

    std::size_t size() const;
    bool empty() const;
    // Throws std::out_of_range for an unknown index.
    TerrainRule const& rule(std::size_t index) const;
    std::vector<TerrainRule> const& rules() const;

    // Indices into rules(), highest precedence first.
    //
    // PRECEDENCE, in full. The first key that differs decides:
    //
    //   1. higher `priority`
    //   2. higher specificity() -- a rule that constrains more endpoints beats one that
    //      constrains fewer. This is what makes an unprioritised "snow above 400" beat an
    //      unprioritised catch-all "grass" without the user having to think about priorities,
    //      which is the mistake every first rule set makes.
    //   3. higher `strength`
    //   4. lexicographically smaller `texture`
    //   5. lower list index
    //
    // Keys 1-4 are functions of rule CONTENT only, so the winning texture and alpha do not depend
    // on the order the rules were added in. Key 5 is reachable only between rules that agree on
    // all four content keys, which therefore produce identical texture and alpha anyway; it exists
    // so rule_index is stable rather than arbitrary.
    std::vector<std::size_t> const& precedenceOrder() const;

    // Highest-precedence matching rule, or a result with matched == false.
    TerrainRuleResult evaluate(TerrainSample const& sample) const;

    // Every matching rule in precedence order, up to `max_results`, written to `out`. Returns the
    // number written; a null `out` or a zero `max_results` writes nothing and returns 0. Use
    // matchCount when only the total is wanted.
    //
    // Exists for multi-layer painting: an ADT chunk carries up to four texture layers, so a caller
    // building one usually wants the top few winners rather than only the first.
    std::size_t evaluateRanked(TerrainSample const& sample, TerrainRuleResult* out, std::size_t max_results) const;

    // Enabled rules matching `sample`, regardless of precedence.
    std::size_t matchCount(TerrainSample const& sample) const;

    // Distinct texture identifiers over ENABLED rules, sorted. What a caller pre-adds as chunk
    // layers before painting.
    std::vector<std::string> distinctTextures() const;

    // Problems that make a rule useless, one line each, empty when the set is sound. Wording is
    // stable enough to assert on.
    std::vector<std::string> validate() const;

  private:
    void rebuildOrder();

    std::vector<TerrainRule> _rules;
    std::vector<std::size_t> _order;
  };

  // One texture's share of an evaluated region.
  struct TerrainCoverageEntry
  {
    std::string texture;
    std::size_t sample_count = 0;
    // Sum of alphas, kept as 64-bit so a full continent cannot overflow it. averageAlpha divides.
    std::uint64_t alpha_total = 0;
    std::uint8_t max_alpha = 0;

    float averageAlpha() const;
  };

  // Aggregates results over a region -- a chunk, a tile, a map.
  //
  // The reason to have it: an ADT chunk holds at most four texture layers, and a rule set with
  // six textures will exceed that somewhere. Deciding which four to keep needs the per-region
  // tally, and that decision is pure arithmetic that belongs under test rather than inside a
  // painting loop.
  class TerrainRuleCoverage
  {
  public:
    void addSample(TerrainRuleResult const& result);

    // Every addSample call, matched or not.
    std::size_t sampleCount() const;
    std::size_t matchedCount() const;
    std::size_t unmatchedCount() const;
    std::size_t distinctTextures() const;

    // Ranked by sample_count descending, then texture ascending. Total and deterministic, so two
    // runs over the same terrain produce the same layer assignment.
    std::vector<TerrainCoverageEntry> entries() const;
    std::vector<TerrainCoverageEntry> topEntries(std::size_t max_entries) const;

    // Folds another region in. Counts add; max_alpha takes the larger. Exists so tiles can be
    // evaluated independently, or on separate threads, and combined afterwards.
    void merge(TerrainRuleCoverage const& other);

    // One line, suitable for a status bar.
    std::string summary() const;

  private:
    struct Entry
    {
      std::size_t sample_count = 0;
      std::uint64_t alpha_total = 0;
      std::uint8_t max_alpha = 0;
    };

    // Ordered rather than hashed: the tally is a handful of textures, and ordered iteration makes
    // entries() deterministic before the sort rather than because of it.
    std::vector<std::pair<std::string, Entry>> _entries;
    std::size_t _samples = 0;
    std::size_t _unmatched = 0;
  };

  // Evaluates `rules` over a resolution x resolution grid covering [x_base, x_base + extent] on X
  // and the same span on Z.
  //
  // Samples sit at CELL CENTRES, not on the boundary: an alphamap is 64x64 texels covering one
  // chunk, and a texel is an area, not a point. Centres also mean the function never has to
  // divide by resolution - 1, so resolution == 1 is a legal single sample at the middle rather
  // than a degenerate case.
  //
  // sample_fn(x, z) -> TerrainSample supplies the terrain; sink(row, col, x, z, sample, result)
  // receives every point, matched or not. Both are templates so this stays free of any Noggit
  // type -- the caller's sample_fn is where World or MapChunk enters, if at all.
  template <typename SampleFn, typename SinkFn>
  void evaluateGrid( TerrainRuleSet const& rules
                   , float x_base
                   , float z_base
                   , float extent
                   , int resolution
                   , SampleFn&& sample_fn
                   , SinkFn&& sink
                   )
  {
    if (resolution <= 0)
    {
      return;
    }

    float const step = extent / static_cast<float>(resolution);

    for (int row = 0; row < resolution; ++row)
    {
      float const z = z_base + (static_cast<float>(row) + 0.5f) * step;

      for (int col = 0; col < resolution; ++col)
      {
        float const x = x_base + (static_cast<float>(col) + 0.5f) * step;

        TerrainSample const sample = sample_fn(x, z);
        sink(row, col, x, z, sample, rules.evaluate(sample));
      }
    }
  }

  // The world walk. Templates only, so this header pulls in no Noggit headers; see the note at the
  // top of the file.
  namespace TerrainRuleCollector
  {
    // mapbufsize (MapChunk.h:46). One chunk's vertex array interleaves a 9x9 outer grid with an
    // 8x8 inner grid, row by row: 9 outer, 8 inner, 9 outer, ... , 9 outer.
    inline constexpr int CHUNK_OUTER_SIDE = 9;
    inline constexpr int CHUNK_INNER_SIDE = 8;
    inline constexpr int CHUNK_VERTEX_COUNT = CHUNK_OUTER_SIDE * CHUNK_OUTER_SIDE
                                            + CHUNK_INNER_SIDE * CHUNK_INNER_SIDE;
    // A tile is 16x16 chunks.
    inline constexpr int TILE_CHUNK_SIDE = 16;

    // MapChunk::indexNoLoD (MapChunk.cpp:356). Restated rather than called because MapChunk.h is
    // exactly what this header must not include; the tests assert the two formulas agree.
    inline constexpr int chunkOuterVertexIndex(int row, int col)
    {
      return row * CHUNK_INNER_SIDE + row * CHUNK_OUTER_SIDE + col;
    }

    // MapChunk::indexLoD (MapChunk.cpp:351).
    inline constexpr int chunkInnerVertexIndex(int row, int col)
    {
      return (row + 1) * CHUNK_OUTER_SIDE + row * CHUNK_INNER_SIDE + col;
    }

    // Height and slope at one entry of a chunk's vertex array.
    //
    // ChunkT is MapChunk. It is a template parameter purely to keep MapChunk.h out of here, and
    // the only members touched are the public mVertices and mNormals arrays (MapChunk.h:101-102),
    // so a three-float stand-in satisfies it -- which is how the collector gets tested.
    template <typename ChunkT>
    TerrainSample sampleChunkVertex(ChunkT const* chunk, int vertex_index)
    {
      TerrainSample sample;

      if (!chunk || vertex_index < 0 || vertex_index >= CHUNK_VERTEX_COUNT)
      {
        return sample;
      }

      auto const& vertex = chunk->mVertices[vertex_index];
      auto const& normal = chunk->mNormals[vertex_index];

      sample.height = static_cast<float>(vertex.y);
      sample.slope_degrees = slopeDegreesFromNormal
        ({static_cast<float>(normal.x), static_cast<float>(normal.y), static_cast<float>(normal.z)});

      return sample;
    }

    // One of the 8x8 units, sampled at its inner vertex.
    //
    // The inner vertex is the apex shared by the unit's four triangles, so its stored normal is
    // already the average of them -- exactly the quantity a per-unit decision wants, and free.
    // The 8x8 unit grid is also what the doodad mapping and the chunk's texture weighting use
    // (texture_set.hpp:126), so a per-unit rule decision lines up with them.
    template <typename ChunkT>
    TerrainSample sampleChunkUnit(ChunkT const* chunk, int unit_row, int unit_col)
    {
      if ( unit_row < 0 || unit_row >= CHUNK_INNER_SIDE
        || unit_col < 0 || unit_col >= CHUNK_INNER_SIDE
         )
      {
        return TerrainSample{};
      }

      return sampleChunkVertex(chunk, chunkInnerVertexIndex(unit_row, unit_col));
    }

    // Evaluates every vertex of one chunk.
    //
    // sink(chunk, vertex_index, sample, result). Exposed separately from collectTile so a caller
    // can run one chunk -- a brush stroke, a preview -- without walking anything else.
    template <typename ChunkT, typename SinkFn>
    void collectChunkVertices(ChunkT* chunk, TerrainRuleSet const& rules, SinkFn&& sink)
    {
      if (!chunk)
      {
        return;
      }

      for (int vertex_index = 0; vertex_index < CHUNK_VERTEX_COUNT; ++vertex_index)
      {
        TerrainSample const sample = sampleChunkVertex(chunk, vertex_index);
        sink(chunk, vertex_index, sample, rules.evaluate(sample));
      }
    }

    // Evaluates the 8x8 unit grid of one chunk. sink(chunk, unit_row, unit_col, sample, result).
    template <typename ChunkT, typename SinkFn>
    void collectChunkUnits(ChunkT* chunk, TerrainRuleSet const& rules, SinkFn&& sink)
    {
      if (!chunk)
      {
        return;
      }

      for (int unit_row = 0; unit_row < CHUNK_INNER_SIDE; ++unit_row)
      {
        for (int unit_col = 0; unit_col < CHUNK_INNER_SIDE; ++unit_col)
        {
          TerrainSample const sample = sampleChunkUnit(chunk, unit_row, unit_col);
          sink(chunk, unit_row, unit_col, sample, rules.evaluate(sample));
        }
      }
    }

    // Every unit of every chunk of one tile. sink is collectChunkUnits'.
    //
    // TileT is MapTile. getChunk is used rather than World::for_all_chunks_on_tile because that
    // helper calls mapIndex.setChanged(tile) on entry (World.inl:16) -- using it would mark every
    // visited tile dirty, which for a preview pass puts unsaved-change prompts in front of a user
    // who only asked what the rules would do.
    template <typename TileT, typename SinkFn>
    void collectTileUnits(TileT* tile, TerrainRuleSet const& rules, SinkFn&& sink)
    {
      // finishedLoading() even though MapIndex::loaded_tiles() already filters on it
      // (map_index.cpp:29), because this is callable directly with a tile obtained some other way,
      // and reading a half-parsed tile's chunk array is a data race.
      if (!tile || !tile->finishedLoading())
      {
        return;
      }

      for (int chunk_z = 0; chunk_z < TILE_CHUNK_SIDE; ++chunk_z)
      {
        for (int chunk_x = 0; chunk_x < TILE_CHUNK_SIDE; ++chunk_x)
        {
          collectChunkUnits(tile->getChunk(static_cast<unsigned int>(chunk_x), static_cast<unsigned int>(chunk_z)), rules, sink);
        }
      }
    }

    // Every LOADED tile of `world`.
    //
    // Call as:
    //
    //   Noggit::TerrainRuleCollector::collectLoadedTileUnits<World, MapTile>
    //     (*world, rule_set, [] (MapChunk* chunk, int unit_row, int unit_col,
    //                            Noggit::TerrainSample const&, Noggit::TerrainRuleResult const& r)
    //      { ... });
    //
    // from a TU that has included World.h, MapTile.h and MapChunk.h. Neither type parameter can be
    // deduced from `world` alone, so both are always written out.
    //
    // Loaded tiles only. Pulling in unvisited ones means loading the whole map, which is a
    // different operation with a different cost and belongs behind its own explicit action.
    template <typename WorldT, typename TileT, typename SinkFn>
    void collectLoadedTileUnits(WorldT& world, TerrainRuleSet const& rules, SinkFn&& sink)
    {
      for (TileT* tile : world.mapIndex.loaded_tiles())
      {
        collectTileUnits(tile, rules, sink);
      }
    }
  }
}

#endif // NOGGIT_TERRAINRULES_HPP
