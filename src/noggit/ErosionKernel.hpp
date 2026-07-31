// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_EROSIONKERNEL_HPP
#define NOGGIT_EROSIONKERNEL_HPP

#include <cstddef>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

// Thermal erosion (talus / angle-of-repose relaxation) on a plain height lattice.
//
// The split runs THROUGH this header, the way it does in AssetScan.hpp: everything with an
// out-of-line definition lives in ErosionKernel.cpp and depends on nothing but the STL -- no Qt,
// no OpenGL, no MapChunk, no glm. That is what lets tests/ErosionKernelTests.cpp link it on a
// bare machine, and it is the larger half on purpose. Only ErosionCollector at the bottom touches
// the world, and only through callbacks and templates, so no Noggit header is pulled in here.
//
// WHY THERMAL AND NOT HYDRAULIC. Hydraulic erosion needs water: per-droplet state, a carry
// capacity, a deposition rule, and a time step small enough that none of it explodes. It also has
// no fixed point -- it is a simulation you stop, not one that finishes. Thermal erosion is a
// relaxation: material sitting steeper than the angle of repose slides downhill until it is not,
// and a surface already at or below repose is a fixed point. That gives idempotence at
// convergence, which is what makes the result reproducible, undo-able in one step, and testable
// at all.
//
// THE SHAPE IS DELIBERATELY MapChunk::blurTerrain's (MapChunk.cpp:1110-1194): a neighbourhood
// filter with inverse-distance weights, driven over a brush region. Two differences, both
// intentional:
//
//   - blurTerrain moves each vertex toward a weighted average and conserves nothing; flattening a
//     hill with it removes the hill's material from the map. Every flow here is an edge transfer,
//     so what one cell loses another gains and the total is unchanged. That is the property that
//     makes the result read as erosion rather than as blur.
//   - blurTerrain resamples the world through World::GetVertex per neighbour per vertex
//     (MapChunk.cpp:1148) -- O(radius^2) world queries per vertex. This samples a lattice once,
//     relaxes it in memory, and applies the result as an interpolated correction.
//
// Nothing here writes to a vertex. The kernel returns a delta field; the caller adds it. That is
// what keeps MapChunk, World and the render-update bookkeeping out of this module entirely.
namespace Noggit
{
  // Which neighbours a cell can shed material to.
  //
  // Moore8 is the default because a 4-neighbour rule enforces the talus along the two axes only.
  // A surface whose steepest descent runs diagonally has no edge in that direction to relax, so
  // it settles at up to sqrt(2) times tan(repose) -- 41% steeper than asked for, and visible as an
  // axis-aligned grain in the result. The test on a diagonal ramp measures exactly that: 1.0
  // under VonNeumann4 against 1.414 under Moore8.
  enum class ErosionNeighbourhood
  {
    VonNeumann4,
    Moore8
  };

  // What a brush does at its own edge. There is no third option and neither is right for every
  // edit.
  //
  // Measured on a 76-degree cone (slope 4.0 per cell, the same one ErosionGrid::influence cites)
  // eroded to 30 degrees through a radius-8 brush with the falloff starting at 0.6r:
  //
  //   GateFlow    steepest slope after 8.86, material discarded 0
  //   FadeResult  steepest slope after 4.10, material discarded 820
  //
  // Neither number is a defect. The gate keeps every grain of material on the map and pays for it
  // with a rim; the fade keeps the shape clean and pays for it by not depositing what it moved.
  enum class ErosionEdgeMode
  {
    // Gate the flow. Conserves exactly. Material accumulates just inside the brush rim, which on
    // a slope far steeper than repose is a visible ridge.
    GateFlow,
    // Erode everything the lattice covers, then fade the correction that gets applied. Leaves at
    // most a few percent of rim; the applied stroke loses whatever the fade scales away, which
    // ErosionDeltaField::correctionSum measures.
    FadeResult
  };

  // Why a run did nothing. Reported rather than thrown: the caller is a brush stroke, and a
  // half-configured brush must leave the terrain alone and say so, not unwind through a paint
  // handler.
  enum class ErosionStatus
  {
    Ok,
    EmptyGrid,
    BadWidth,          // width <= 0
    RaggedGrid,        // cell count is not a multiple of width
    GridTooLarge,      // cell count exceeds what the int index arithmetic can address
    InfluenceMismatch, // influence span present but not the same length as the heights
    BadCellSize,       // cell size <= 0 or not finite
    BadSettings        // negative iteration count, or a non-finite tunable
  };

  std::string_view erosionStatusText(ErosionStatus status);

  // Upper bound on the per-iteration relaxation factor.
  //
  // Derived, not tuned. Take two neighbours differing by d with talus threshold T. The flow is
  // s*w*(d - T) out of the high cell and the same into the low one, so the pair's difference
  // becomes d - 2*s*w*(d - T), which stays at or above T -- no inversion -- exactly when
  // 2*s*w <= 1. Edge weights sum to 1 over a full neighbourhood, so w <= 1 is the worst any
  // weighting could ask for and s <= 0.5 satisfies the bound for all of them. Past it the pair
  // overshoots, the next iteration sends the material back, and at s*w = 1 that is a perfect
  // two-cycle that never settles. The real weights are far below the bound (0.25 under
  // VonNeumann4, 0.1464 orthogonal under Moore8), so one iteration moves a fraction of the excess
  // and convergence is asymptotic rather than immediate. Larger values are clamped, not honoured.
  inline constexpr float MAX_STABLE_STRENGTH = 0.5f;

  // tan() runs away at 90 degrees, and a repose angle is a UI slider. Clamped, not rejected.
  inline constexpr float MAX_REPOSE_ANGLE_DEGREES = 89.0f;

  // A regular sampling lattice, in world units. Plain numbers -- no glm, no World.
  //
  // Row 0 is at origin_z, column 0 at origin_x, and the grid is row-major: index = z*width + x.
  // Noggit's terrain vertices are NOT this lattice -- a chunk is a 9x9 outer grid interleaved
  // with an 8x8 inner grid offset by half a unit (MapChunk.cpp's mVertices layout), which is a
  // brick pattern, not a rectangle. The kernel therefore works on a resampled lattice and the
  // result is applied back by interpolation. blurTerrain already does exactly this resampling
  // (MapChunk.cpp:1139-1148); this at least does it once per stroke instead of once per vertex
  // per neighbour.
  struct ErosionLattice
  {
    float origin_x = 0.0f;
    float origin_z = 0.0f;
    int width = 0;
    int height = 0;
    // World distance between adjacent samples. UNITSIZE (MapHeaders.h:60, 4.1667 yards) matches
    // the outer vertex spacing; UNITSIZE/2 also catches the inner vertex rows at 4x the cells.
    float cell_size = 1.0f;

    bool valid() const;
    std::size_t cellCount() const;
    std::size_t index(int x, int z) const;

    float worldX(int x) const;
    float worldZ(int z) const;

    // Fractional lattice coordinates for a world position. Not clamped and not range-checked:
    // callers that care use contains() or sampleBilinear, which report the miss.
    float gridX(float world_x) const;
    float gridZ(float world_z) const;

    bool contains(float world_x, float world_z) const;
  };

  // Largest half-extent latticeCovering will build, i.e. a hard cap of 2049x2049 cells (~17MB of
  // heights and deltas). A brush radius over cell size beyond this is refused rather than
  // truncated; see latticeCovering.
  inline constexpr int MAX_LATTICE_HALF_EXTENT = 1024;

  // Smallest lattice covering a disc, padded by one cell on every side.
  //
  // The padding is not cosmetic. Flow across the lattice border is impossible -- there is no edge
  // to carry it -- so the border row acts as a wall that material piles against. Putting the wall
  // one cell outside the brush, where the influence has already fallen to zero, keeps the pile
  // out of the region the user is editing.
  //
  // Returns an INVALID lattice (width 0) for a nonsensical or oversized request, which downstream
  // reports as ErosionStatus::EmptyGrid.
  ErosionLattice latticeCovering(float center_x, float center_z, float radius, float cell_size);

  // A lattice plus what was sampled onto it.
  //
  // `influence` is per-cell and in [0,1]. It is a GATE, not a weight applied to the result: an
  // edge carries flow scaled by the product of its two endpoints' influence, so a cell at zero
  // neither gives nor receives and its stored height is irrelevant. That single mechanism covers
  // three unrelated needs -- brush falloff, holes and unloaded tiles the sampler could not answer
  // for, and terrain the user has locked -- without any of them being a special case in the
  // solver. Empty means every cell is fully active.
  //
  // GATING PILES MATERIAL AT ITS OWN EDGE, and that is not a bug that can be fixed here. Erosion
  // transports material; a gated region is a region material can enter but not leave, so it
  // accumulates just inside the gate. Measured on a 76-degree cone eroded to 30 degrees through a
  // radius-8 brush: the core relaxes from a slope of 4.0 to 1.03, and the rim rises to 11.1 --
  // steeper than the terrain started. A soft falloff spreads the pile rather than removing it
  // (5.27 at its worst with the softest profile). It is the price of the conservation guarantee,
  // and ErosionEdgeMode::FadeResult is the other side of that trade.
  struct ErosionGrid
  {
    ErosionLattice lattice;
    std::vector<float> heights;
    std::vector<float> influence;

    bool valid() const;
    std::size_t cellCount() const;
    float at(int x, int z) const;
  };

  // Brush weight at a distance from the centre: 1 at or inside `inner_radius`, 0 at or outside
  // `radius`, smoothstep between. A non-positive radius weights everything 0.
  float radialFalloff(float distance, float radius, float inner_radius);

  // Multiplies a smooth radial falloff into grid.influence, allocating it if it was empty.
  //
  // Multiplies rather than assigns because the sampler has already used the same array to mark
  // cells it could not answer for; overwriting would re-enable erosion on a hole.
  //
  // `inner_radius` is where the falloff starts; inside it the weight is exactly 1, at and outside
  // `radius` exactly 0, and between them smoothstep. A hard-edged disc (inner == radius) is
  // legal and produces a visible rim, for the same reason a hard-edged brush does.
  void applyRadialInfluence( ErosionGrid& grid
                           , float center_x
                           , float center_z
                           , float radius
                           , float inner_radius
                           );

  struct ErosionSettings
  {
    // The angle a slope is allowed to keep. Steeper edges shed material until they reach it.
    // 45 degrees is a scree slope; 30-35 is dry sand or gravel; below ~20 the terrain reads as
    // eroded farmland rather than mountain.
    float repose_angle_degrees = 45.0f;

    // Fraction of the excess moved per iteration, clamped to MAX_STABLE_STRENGTH. Lower values
    // are not more accurate, only slower -- the fixed point is the same.
    float strength = MAX_STABLE_STRENGTH;

    int iterations = 16;

    // World distance between adjacent lattice samples. Only the ratio of height to cell_size
    // matters, which is why the talus threshold is an angle and not a height.
    float cell_size = 1.0f;

    ErosionNeighbourhood neighbourhood = ErosionNeighbourhood::Moore8;

    // An iteration whose largest single flow is at or below this stops the run. Below roughly
    // 1e-7 of the terrain's height scale the transfer is smaller than the float rounding of the
    // store that applies it, so continuing burns iterations to move nothing.
    float convergence_epsilon = 1e-6f;
  };

  // What a run did. Every field is evidence for a claim the caller would otherwise have to take
  // on trust -- conservation above all.
  struct ErosionStats
  {
    ErosionStatus status = ErosionStatus::Ok;

    // Iterations that actually moved material. An iteration that finds nothing above the
    // convergence epsilon is not counted, so a converged grid re-eroded reports zero.
    int iterations_run = 0;

    // True when an iteration found nothing left to move. False after a run that used its whole
    // budget -- which does not prove anything remains, only that no pass proved otherwise.
    bool converged = false;

    // Compensated sums (see heightSum). The difference between them is the kernel's conservation
    // error and nothing else, which is the point of summing that way.
    double height_sum_before = 0.0;
    double height_sum_after = 0.0;

    // Total material transferred across all edges and iterations. A measure of how much work the
    // run did, not of how much the surface changed: material moved down a slope and then further
    // down counts twice.
    double material_moved = 0.0;

    // Steepest edge as a TANGENT (height difference over world distance), not an angle. Compare
    // against reposeTangent(settings.repose_angle_degrees).
    float max_slope_before = 0.0f;
    float max_slope_after = 0.0f;

    // Edges still steeper than repose. Zero after means the surface is at rest everywhere.
    std::size_t unstable_edges_before = 0;
    std::size_t unstable_edges_after = 0;

    bool ok() const;

    // Signed: positive means material was created. Should be within a few ulps of the height
    // scale times the cell count; anything larger is a bug in the flow bookkeeping.
    double conservationError() const;
  };

  // tan of the repose angle, clamped to [0, MAX_REPOSE_ANGLE_DEGREES]. A negative angle clamps to
  // zero, which means "no slope is acceptable" and relaxes toward flat.
  float reposeTangent(float repose_angle_degrees);

  // Neumaier-compensated sum.
  //
  // Plain accumulation is not good enough for the one number the caller checks the kernel with. A
  // 129x129 lattice of Azeroth heights sums to ~1e6 with individual terms around 100, where naive
  // float summation loses far more than any error the solver introduces -- the measurement would
  // dominate the thing measured, and a conservation test built on it would pass or fail on
  // summation order.
  double heightSum(std::span<float const> heights);

  // Steepest edge in the grid, as a tangent. Only edges the given neighbourhood actually has are
  // considered, so the same surface reports a different value under VonNeumann4 and Moore8.
  // Returns 0 for a grid with no edges.
  float maxSlopeTangent( std::span<float const> heights
                       , int width
                       , float cell_size
                       , ErosionNeighbourhood neighbourhood
                       );

  // Edges steeper than the repose angle, i.e. how much of the surface is still unstable.
  std::size_t countUnstableEdges( std::span<float const> heights
                                , int width
                                , float cell_size
                                , ErosionNeighbourhood neighbourhood
                                , float repose_angle_degrees
                                );

  // THE KERNEL. Relaxes `heights` in place toward the angle of repose.
  //
  // Every iteration is a simultaneous (Jacobi) update: all flows are computed from the heights at
  // the start of the iteration and applied together. Sequential in-place updating would be
  // cheaper and is what most published implementations do, but it makes the result depend on
  // traversal order -- a spike relaxes further to the east than to the west because the east was
  // visited later -- and that asymmetry is visible in the terrain.
  //
  // Each undirected edge is visited exactly once and its flow subtracted from one endpoint and
  // added to the other, in double. Conservation is therefore structural rather than something the
  // arithmetic has to be careful about; the only residue is the float rounding of the final store.
  //
  // The grid border does not leak: there is no edge leading off the lattice, so material piles at
  // the edge instead of vanishing. See latticeCovering for why the brush should not reach it.
  //
  // `influence` may be empty, or must have one entry per height. NaN in the input propagates and
  // is not checked for -- the check would cost a pass over the grid on every iteration to defend
  // against an input no sampler here produces.
  ErosionStats thermalErode( std::span<float> heights
                           , int width
                           , ErosionSettings const& settings
                           , std::span<float const> influence = {}
                           );

  // Same, taking cell size and influence from the grid. settings.cell_size is ignored.
  ErosionStats thermalErode(ErosionGrid& grid, ErosionSettings const& settings);

  // Element-wise after - before. Empty if the lengths disagree.
  std::vector<float> heightDelta(std::span<float const> before, std::span<float const> after);

  // Bilinear sample in LATTICE coordinates (cells, not world units).
  //
  // nullopt outside [0, width-1] x [0, height-1] rather than a clamp, because a clamped read at
  // the border would smear the border row's correction outward across every vertex beyond the
  // lattice.
  std::optional<float> sampleBilinear( std::span<float const> values
                                     , int width
                                     , int height
                                     , float grid_x
                                     , float grid_z
                                     );

  // The result of a run, in the form the caller applies: a height correction per world position.
  //
  // The correction is the INTERPOLATED DELTA, not the interpolated eroded height. Applying
  // absolute heights would resample the terrain to the lattice, erasing every detail finer than
  // cell_size -- the stroke would flatten the surface between samples even where erosion moved
  // nothing. Applying a delta leaves detail alone: where the kernel moved nothing the correction
  // is exactly 0.0f, and h + 0.0f is bit-identical to h for every finite h, so an unaffected
  // vertex is not merely close to unchanged, it is unchanged.
  class ErosionDeltaField
  {
  public:
    ErosionDeltaField() = default;
    ErosionDeltaField(ErosionLattice lattice, std::vector<float> delta);

    // 0 outside the lattice, so the per-vertex loop needs no bounds test.
    float correctionAt(float world_x, float world_z) const;

    // Multiplies a radial falloff into the corrections themselves.
    //
    // The alternative to gating the flow: erode the whole lattice with nothing held back, then
    // fade what gets applied. No material piles at the brush rim, because inside the lattice
    // nothing stopped it -- but the stroke as APPLIED no longer conserves, since the material the
    // fade scales away is simply not deposited. Which of the two is wrong depends on the edit:
    // shaping one mountain wants the fade, and a stroke that must not change the total volume of
    // a basin wants the gate.
    void fade(float center_x, float center_z, float radius, float inner_radius);

    bool empty() const;
    ErosionLattice const& lattice() const;
    std::vector<float> const& delta() const;

    // Largest absolute correction anywhere on the lattice. What a UI shows as "this stroke moves
    // the ground by up to N yards" before committing it.
    float maxAbsCorrection() const;

    // Sum of the corrections. Zero (to float rounding) for a gated run, negative or positive for
    // a faded one -- which is the measurement that tells the two apart.
    double correctionSum() const;

  private:
    ErosionLattice _lattice;
    std::vector<float> _delta;
  };

  struct ErosionRun
  {
    ErosionStats stats;
    ErosionDeltaField field;
  };

  // Erodes a COPY of `grid` and returns the correction field. Takes the grid by value because the
  // delta needs the original heights, and a caller that has to keep its own copy to get one will
  // eventually forget to.
  ErosionRun erodeGrid(ErosionGrid grid, ErosionSettings const& settings);

  // The world walk. Templates only, so this header pulls in no Noggit header; see AssetScan.hpp
  // for the same argument at more length.
  namespace ErosionCollector
  {
    // Reads a lattice of heights out of whatever the caller has.
    //
    // `sample` has World::GetVertex's shape as used at MapChunk.cpp:1148 -- bool(x, z, float&),
    // false meaning "no terrain here". A rejected cell is marked inert via influence 0 rather
    // than filled with a guess: an unloaded tile is not a height of zero, and treating it as one
    // would carve a cliff along the edge of the loaded region and then erode the cliff.
    //
    // Wrap World like this at the call site (World.h stays out of this header):
    //
    //   auto sample = [world] (float x, float z, float& out) -> bool
    //   {
    //     glm::vec3 vertex;
    //     if (!world->GetVertex(x, z, &vertex)) { return false; }
    //     out = vertex.y;
    //     return true;
    //   };
    template <typename SampleFn>
    ErosionGrid sampleLattice(ErosionLattice const& lattice, SampleFn sample)
    {
      ErosionGrid grid;
      grid.lattice = lattice;

      if (!lattice.valid())
      {
        return grid;
      }

      std::size_t const count = lattice.cellCount();
      grid.heights.assign(count, 0.0f);

      bool any_rejected = false;
      std::vector<float> influence(count, 1.0f);

      for (int z = 0; z < lattice.height; ++z)
      {
        for (int x = 0; x < lattice.width; ++x)
        {
          std::size_t const i = lattice.index(x, z);
          float height = 0.0f;

          if (sample(lattice.worldX(x), lattice.worldZ(z), height))
          {
            grid.heights[i] = height;
          }
          else
          {
            influence[i] = 0.0f;
            any_rejected = true;
          }
        }
      }

      // Left empty when nothing was rejected: an empty influence array is the solver's fast path
      // and means exactly the same thing as an array of ones.
      if (any_rejected)
      {
        grid.influence = std::move(influence);
      }

      return grid;
    }

    // Adds the field's correction to a vertex array, returning how many vertices moved.
    //
    // A template purely to avoid naming glm::vec3 or MapChunk here: VertexArrayT is anything
    // indexable whose elements have .x, .y and .z, which is MapChunk::mVertices. `weight` scales
    // the whole stroke, for the alpha/pressure the brush already has.
    //
    // The caller still owns the consequences of a vertex move -- MapChunk::registerChunkUpdate
    // (ChunkUpdateFlags::VERTEX), the recalculation of normals, and marking the tile changed.
    // None of that can be done from here without the headers this module refuses to include, and
    // guessing at it would be worse than leaving it to the one line that knows.
    template <typename VertexArrayT>
    std::size_t applyToVertices( ErosionDeltaField const& field
                               , VertexArrayT& vertices
                               , std::size_t count
                               , float weight = 1.0f
                               )
    {
      std::size_t moved = 0;

      for (std::size_t i = 0; i < count; ++i)
      {
        float const correction = field.correctionAt(vertices[i].x, vertices[i].z) * weight;

        if (correction != 0.0f)
        {
          vertices[i].y += correction;
          ++moved;
        }
      }

      return moved;
    }

    // Sample, erode, hand back the correction field. The whole module in one call.
    //
    //   Noggit::ErosionSettings settings;
    //   settings.cell_size = UNITSIZE;
    //   auto const lattice = Noggit::latticeCovering(pos.x, pos.z, radius, settings.cell_size);
    //   auto const run = Noggit::ErosionCollector::erodeRegion
    //     (lattice, settings, sample, pos.x, pos.z, radius, radius * 0.6f);
    //
    // then feed run.field to applyToVertices for each chunk in range. Note that erosion is not a
    // per-chunk operation: sampling must cover the whole brush, across chunk and tile boundaries,
    // or material stops at the seam and the seam becomes a ridge. blurTerrain has the same
    // requirement and meets it the same way, by sampling the world rather than the chunk.
    //
    // Cells the sampler rejected stay inert under BOTH edge modes. That gate is not the brush,
    // it is the difference between terrain whose height is known and terrain whose is not, and
    // fading a correction computed against an invented height would still apply the invention.
    template <typename SampleFn>
    ErosionRun erodeRegion( ErosionLattice const& lattice
                          , ErosionSettings const& settings
                          , SampleFn sample
                          , float center_x
                          , float center_z
                          , float radius
                          , float inner_radius
                          , ErosionEdgeMode edge_mode = ErosionEdgeMode::GateFlow
                          )
    {
      ErosionGrid grid = sampleLattice(lattice, sample);

      if (edge_mode == ErosionEdgeMode::GateFlow)
      {
        applyRadialInfluence(grid, center_x, center_z, radius, inner_radius);

        return erodeGrid(std::move(grid), settings);
      }

      ErosionRun run = erodeGrid(std::move(grid), settings);
      run.field.fade(center_x, center_z, radius, inner_radius);

      return run;
    }
  }
}

#endif // NOGGIT_EROSIONKERNEL_HPP
