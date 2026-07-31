// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/ErosionKernel.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>

namespace
{
  constexpr double PI = 3.14159265358979323846;

  // One direction a cell can shed material in.
  //
  // `distance` is in CELLS; the world distance is distance * cell_size. Keeping it that way is
  // what makes the talus threshold an angle: the excess is compared against
  // tan(repose) * cell_size * distance, so the same settings on a lattice sampled twice as finely
  // produce the same shape rather than twice the erosion.
  struct Edge
  {
    int dx;
    int dz;
    float distance;
    float weight;
  };

  // Only the FORWARD half of the neighbourhood.
  //
  // Every undirected edge appears exactly once, which is the whole conservation argument: the
  // loop subtracts one flow from one endpoint and adds the same double to the other. Visiting
  // each edge from both ends would need the two halves to agree bit for bit to conserve, and
  // they would not, because the two subtractions round differently.
  struct EdgeSet
  {
    std::array<Edge, 4> edges{};
    std::array<double, 4> talus{};
    int count = 0;
  };

  // Fraction of a cell's excess that one direction may carry.
  //
  // Inverse distance, normalised over the FULL neighbourhood -- the same weighting
  // MapChunk::blurTerrain uses for its samples (MapChunk.cpp:1152). Two consequences worth being
  // explicit about:
  //
  //   - The weights sum to at most 1 over any cell's real edges, so a cell can never shed more
  //     than `strength` of its excess in one iteration. That, with strength <= 0.5, is what keeps
  //     the update monotone in every input height and therefore incapable of manufacturing a new
  //     peak: the post-update height of a cell cannot exceed the pre-update maximum of its
  //     neighbourhood.
  //   - Normalisation is over the full set even for border and corner cells, which have fewer
  //     edges. Normalising per cell instead would give a corner cell heavier weights than its
  //     interior neighbour, so one edge would have two different weights depending on which end
  //     was higher -- the flow would depend on the sign of the slope, and the border would erode
  //     visibly faster than the middle.
  EdgeSet forwardEdges(Noggit::ErosionNeighbourhood neighbourhood, float repose_tangent, float cell_size)
  {
    constexpr float SQRT2 = 1.41421356237309504880f;

    EdgeSet set;

    if (neighbourhood == Noggit::ErosionNeighbourhood::VonNeumann4)
    {
      // 4 edges of length 1: total inverse distance 4, so 0.25 each.
      set.edges[0] = {1, 0, 1.0f, 0.25f};
      set.edges[1] = {0, 1, 1.0f, 0.25f};
      set.count = 2;
    }
    else
    {
      // 4 orthogonal at 1 plus 4 diagonal at sqrt(2): total inverse distance 4 + 4/sqrt(2)
      // = 6.8284271, giving 0.1464466 orthogonal and 0.1035534 diagonal. They sum to 1.
      constexpr float TOTAL_INVERSE_DISTANCE = 4.0f + 4.0f / SQRT2;
      constexpr float ORTHOGONAL_WEIGHT = 1.0f / TOTAL_INVERSE_DISTANCE;
      constexpr float DIAGONAL_WEIGHT = (1.0f / SQRT2) / TOTAL_INVERSE_DISTANCE;

      set.edges[0] = {1, 0, 1.0f, ORTHOGONAL_WEIGHT};
      set.edges[1] = {1, 1, SQRT2, DIAGONAL_WEIGHT};
      set.edges[2] = {0, 1, 1.0f, ORTHOGONAL_WEIGHT};
      set.edges[3] = {-1, 1, SQRT2, DIAGONAL_WEIGHT};
      set.count = 4;
    }

    for (int e = 0; e < set.count; ++e)
    {
      set.talus[e] = static_cast<double>(repose_tangent)
                   * static_cast<double>(cell_size)
                   * static_cast<double>(set.edges[e].distance);
    }

    return set;
  }

  Noggit::ErosionStatus validateInput( std::size_t cell_count
                                     , int width
                                     , Noggit::ErosionSettings const& settings
                                     , std::size_t influence_size
                                     )
  {
    if (cell_count == 0)
    {
      return Noggit::ErosionStatus::EmptyGrid;
    }

    if (width <= 0)
    {
      return Noggit::ErosionStatus::BadWidth;
    }

    if (cell_count > static_cast<std::size_t>(std::numeric_limits<int>::max()))
    {
      return Noggit::ErosionStatus::GridTooLarge;
    }

    if (cell_count % static_cast<std::size_t>(width) != 0)
    {
      return Noggit::ErosionStatus::RaggedGrid;
    }

    if (influence_size != 0 && influence_size != cell_count)
    {
      return Noggit::ErosionStatus::InfluenceMismatch;
    }

    if (!std::isfinite(settings.cell_size) || settings.cell_size <= 0.0f)
    {
      return Noggit::ErosionStatus::BadCellSize;
    }

    if ( settings.iterations < 0
      || !std::isfinite(settings.strength)
      || !std::isfinite(settings.repose_angle_degrees)
      || !std::isfinite(settings.convergence_epsilon)
       )
    {
      return Noggit::ErosionStatus::BadSettings;
    }

    return Noggit::ErosionStatus::Ok;
  }
}

namespace Noggit
{
  std::string_view erosionStatusText(ErosionStatus status)
  {
    switch (status)
    {
      case ErosionStatus::Ok:                return "ok";
      case ErosionStatus::EmptyGrid:         return "empty grid";
      case ErosionStatus::BadWidth:          return "width must be positive";
      case ErosionStatus::RaggedGrid:        return "cell count is not a multiple of the width";
      case ErosionStatus::GridTooLarge:      return "grid is too large to index";
      case ErosionStatus::InfluenceMismatch: return "influence length does not match the grid";
      case ErosionStatus::BadCellSize:       return "cell size must be positive and finite";
      case ErosionStatus::BadSettings:       return "settings contain a negative or non-finite value";
    }

    return "unknown";
  }

  bool ErosionLattice::valid() const
  {
    return width > 0 && height > 0 && std::isfinite(cell_size) && cell_size > 0.0f;
  }

  std::size_t ErosionLattice::cellCount() const
  {
    if (width <= 0 || height <= 0)
    {
      return 0;
    }

    return static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
  }

  std::size_t ErosionLattice::index(int x, int z) const
  {
    return static_cast<std::size_t>(z) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x);
  }

  float ErosionLattice::worldX(int x) const
  {
    return origin_x + static_cast<float>(x) * cell_size;
  }

  float ErosionLattice::worldZ(int z) const
  {
    return origin_z + static_cast<float>(z) * cell_size;
  }

  float ErosionLattice::gridX(float world_x) const
  {
    return (world_x - origin_x) / cell_size;
  }

  float ErosionLattice::gridZ(float world_z) const
  {
    return (world_z - origin_z) / cell_size;
  }

  bool ErosionLattice::contains(float world_x, float world_z) const
  {
    if (!valid())
    {
      return false;
    }

    float const x = gridX(world_x);
    float const z = gridZ(world_z);

    return x >= 0.0f && z >= 0.0f
        && x <= static_cast<float>(width - 1)
        && z <= static_cast<float>(height - 1);
  }

  ErosionLattice latticeCovering(float center_x, float center_z, float radius, float cell_size)
  {
    ErosionLattice lattice;

    if ( !std::isfinite(center_x) || !std::isfinite(center_z)
      || !std::isfinite(radius) || radius <= 0.0f
      || !std::isfinite(cell_size) || cell_size <= 0.0f
       )
    {
      return lattice;
    }

    double const cells = std::ceil(static_cast<double>(radius) / static_cast<double>(cell_size));

    // +1 for the inert ring described in the header. Bounds-checked before the cast because
    // radius/cell_size is caller data: a 4000-yard radius at a 0.01-yard cell size asks for
    // 6.4e11 cells, and the honest answer to that is an invalid lattice the caller can report,
    // not a silently smaller edit or a bad_alloc out of a paint handler.
    if (cells + 1.0 > static_cast<double>(MAX_LATTICE_HALF_EXTENT))
    {
      return lattice;
    }

    int const half_extent = static_cast<int>(cells) + 1;

    lattice.cell_size = cell_size;
    lattice.width = 2 * half_extent + 1;
    lattice.height = lattice.width;
    // The centre cell lands exactly on (center_x, center_z), so a symmetric brush produces a
    // symmetric lattice and the erosion of a symmetric shape stays symmetric.
    lattice.origin_x = center_x - static_cast<float>(half_extent) * cell_size;
    lattice.origin_z = center_z - static_cast<float>(half_extent) * cell_size;

    return lattice;
  }

  bool ErosionGrid::valid() const
  {
    return lattice.valid()
        && heights.size() == lattice.cellCount()
        && (influence.empty() || influence.size() == heights.size());
  }

  std::size_t ErosionGrid::cellCount() const
  {
    return heights.size();
  }

  float ErosionGrid::at(int x, int z) const
  {
    return heights[lattice.index(x, z)];
  }

  float radialFalloff(float distance, float radius, float inner_radius)
  {
    if (!std::isfinite(distance) || !std::isfinite(radius) || radius <= 0.0f || distance >= radius)
    {
      return 0.0f;
    }

    float const inner = std::clamp(inner_radius, 0.0f, radius);

    if (distance <= inner)
    {
      return 1.0f;
    }

    // inner == radius is the hard-edged brush and was handled by the distance >= radius test
    // above; reaching here with inner == radius is impossible, so the divisor is positive.
    float const t = (radius - distance) / (radius - inner);

    // Smoothstep rather than a linear ramp: a linear falloff leaves a first-derivative
    // discontinuity at both ends of the ramp, and on terrain that reads as a visible circular
    // crease at the brush edge -- the same artefact a hard-edged flatten brush leaves.
    return t * t * (3.0f - 2.0f * t);
  }

  void applyRadialInfluence( ErosionGrid& grid
                           , float center_x
                           , float center_z
                           , float radius
                           , float inner_radius
                           )
  {
    if (!grid.lattice.valid() || grid.heights.size() != grid.lattice.cellCount())
    {
      return;
    }

    if (grid.influence.empty())
    {
      grid.influence.assign(grid.heights.size(), 1.0f);
    }

    for (int z = 0; z < grid.lattice.height; ++z)
    {
      for (int x = 0; x < grid.lattice.width; ++x)
      {
        float const dx = grid.lattice.worldX(x) - center_x;
        float const dz = grid.lattice.worldZ(z) - center_z;
        float const distance = std::sqrt(dx * dx + dz * dz);

        grid.influence[grid.lattice.index(x, z)] *= radialFalloff(distance, radius, inner_radius);
      }
    }
  }

  bool ErosionStats::ok() const
  {
    return status == ErosionStatus::Ok;
  }

  double ErosionStats::conservationError() const
  {
    return height_sum_after - height_sum_before;
  }

  float reposeTangent(float repose_angle_degrees)
  {
    if (!std::isfinite(repose_angle_degrees))
    {
      return 0.0f;
    }

    float const angle = std::clamp(repose_angle_degrees, 0.0f, MAX_REPOSE_ANGLE_DEGREES);

    return static_cast<float>(std::tan(static_cast<double>(angle) * PI / 180.0));
  }

  double heightSum(std::span<float const> heights)
  {
    double sum = 0.0;
    double compensation = 0.0;

    for (float height : heights)
    {
      double const value = static_cast<double>(height);
      double const next = sum + value;

      // Neumaier's variant of Kahan: it also recovers the low bits when the incoming value is the
      // larger of the two, which plain Kahan drops. Terrain hits that case constantly -- a run of
      // small negative heights followed by a mountain.
      if (std::abs(sum) >= std::abs(value))
      {
        compensation += (sum - next) + value;
      }
      else
      {
        compensation += (value - next) + sum;
      }

      sum = next;
    }

    return sum + compensation;
  }

  float maxSlopeTangent( std::span<float const> heights
                       , int width
                       , float cell_size
                       , ErosionNeighbourhood neighbourhood
                       )
  {
    if (heights.empty() || width <= 0 || !std::isfinite(cell_size) || cell_size <= 0.0f)
    {
      return 0.0f;
    }

    if (heights.size() % static_cast<std::size_t>(width) != 0)
    {
      return 0.0f;
    }

    int const grid_height = static_cast<int>(heights.size() / static_cast<std::size_t>(width));
    EdgeSet const set = forwardEdges(neighbourhood, 0.0f, cell_size);

    float steepest = 0.0f;

    for (int z = 0; z < grid_height; ++z)
    {
      for (int x = 0; x < width; ++x)
      {
        std::size_t const i = static_cast<std::size_t>(z) * static_cast<std::size_t>(width)
                            + static_cast<std::size_t>(x);

        for (int e = 0; e < set.count; ++e)
        {
          int const nx = x + set.edges[e].dx;
          int const nz = z + set.edges[e].dz;

          if (nx < 0 || nx >= width || nz < 0 || nz >= grid_height)
          {
            continue;
          }

          std::size_t const j = static_cast<std::size_t>(nz) * static_cast<std::size_t>(width)
                              + static_cast<std::size_t>(nx);

          float const drop = std::abs(heights[i] - heights[j]);
          float const slope = drop / (set.edges[e].distance * cell_size);

          steepest = std::max(steepest, slope);
        }
      }
    }

    return steepest;
  }

  std::size_t countUnstableEdges( std::span<float const> heights
                                , int width
                                , float cell_size
                                , ErosionNeighbourhood neighbourhood
                                , float repose_angle_degrees
                                )
  {
    if (heights.empty() || width <= 0 || !std::isfinite(cell_size) || cell_size <= 0.0f)
    {
      return 0;
    }

    if (heights.size() % static_cast<std::size_t>(width) != 0)
    {
      return 0;
    }

    int const grid_height = static_cast<int>(heights.size() / static_cast<std::size_t>(width));
    EdgeSet const set = forwardEdges(neighbourhood, reposeTangent(repose_angle_degrees), cell_size);

    std::size_t unstable = 0;

    for (int z = 0; z < grid_height; ++z)
    {
      for (int x = 0; x < width; ++x)
      {
        std::size_t const i = static_cast<std::size_t>(z) * static_cast<std::size_t>(width)
                            + static_cast<std::size_t>(x);

        for (int e = 0; e < set.count; ++e)
        {
          int const nx = x + set.edges[e].dx;
          int const nz = z + set.edges[e].dz;

          if (nx < 0 || nx >= width || nz < 0 || nz >= grid_height)
          {
            continue;
          }

          std::size_t const j = static_cast<std::size_t>(nz) * static_cast<std::size_t>(width)
                              + static_cast<std::size_t>(nx);

          if (std::abs(static_cast<double>(heights[i]) - heights[j]) > set.talus[e])
          {
            ++unstable;
          }
        }
      }
    }

    return unstable;
  }

  ErosionStats thermalErode( std::span<float> heights
                           , int width
                           , ErosionSettings const& settings
                           , std::span<float const> influence
                           )
  {
    ErosionStats stats;
    stats.status = validateInput(heights.size(), width, settings, influence.size());

    if (!stats.ok())
    {
      return stats;
    }

    std::span<float const> const readable(heights);

    stats.height_sum_before = heightSum(readable);
    stats.max_slope_before = maxSlopeTangent(readable, width, settings.cell_size, settings.neighbourhood);
    stats.unstable_edges_before = countUnstableEdges( readable
                                                   , width
                                                   , settings.cell_size
                                                   , settings.neighbourhood
                                                   , settings.repose_angle_degrees
                                                   );

    int const grid_height = static_cast<int>(heights.size() / static_cast<std::size_t>(width));
    EdgeSet const set = forwardEdges( settings.neighbourhood
                                    , reposeTangent(settings.repose_angle_degrees)
                                    , settings.cell_size
                                    );

    // Clamped, not rejected: a strength above the stability bound is a slider at its end, and the
    // useful response is the fastest stable run rather than an error the user cannot interpret.
    double const strength = static_cast<double>(std::clamp(settings.strength, 0.0f, MAX_STABLE_STRENGTH));
    double const epsilon = static_cast<double>(std::max(0.0f, settings.convergence_epsilon));
    bool const gated = !influence.empty();

    // Double, and separate from the heights, for the two reasons that matter: the update is
    // simultaneous, so no flow may see a height another flow has already changed; and a cell
    // accumulates up to eight transfers per iteration, each several orders of magnitude smaller
    // than the height it lands on, which in float would round most of them away.
    std::vector<double> delta(heights.size(), 0.0);

    for (int iteration = 0; iteration < settings.iterations; ++iteration)
    {
      std::fill(delta.begin(), delta.end(), 0.0);

      double largest_flow = 0.0;
      double moved_this_iteration = 0.0;

      for (int z = 0; z < grid_height; ++z)
      {
        for (int x = 0; x < width; ++x)
        {
          std::size_t const i = static_cast<std::size_t>(z) * static_cast<std::size_t>(width)
                              + static_cast<std::size_t>(x);

          double const gate_i = gated ? static_cast<double>(influence[i]) : 1.0;

          if (gate_i <= 0.0)
          {
            continue;
          }

          for (int e = 0; e < set.count; ++e)
          {
            int const nx = x + set.edges[e].dx;
            int const nz = z + set.edges[e].dz;

            if (nx < 0 || nx >= width || nz < 0 || nz >= grid_height)
            {
              continue;
            }

            std::size_t const j = static_cast<std::size_t>(nz) * static_cast<std::size_t>(width)
                                + static_cast<std::size_t>(nx);

            double const gate = gated ? gate_i * static_cast<double>(influence[j]) : 1.0;

            if (gate <= 0.0)
            {
              continue;
            }

            double const difference = static_cast<double>(heights[i]) - static_cast<double>(heights[j]);
            double const excess = std::abs(difference) - set.talus[e];

            if (excess <= 0.0)
            {
              continue;
            }

            double const flow = strength * static_cast<double>(set.edges[e].weight) * excess * gate;

            // The one write pair in the module. Same double added and subtracted, so the
            // iteration's deltas sum to exactly zero before rounding.
            if (difference > 0.0)
            {
              delta[i] -= flow;
              delta[j] += flow;
            }
            else
            {
              delta[i] += flow;
              delta[j] -= flow;
            }

            moved_this_iteration += flow;
            largest_flow = std::max(largest_flow, flow);
          }
        }
      }

      if (largest_flow <= epsilon)
      {
        // Deliberately discarded rather than applied. What is left is below the epsilon the
        // caller declared meaningless, and applying it would make this iteration count as work
        // and break the idempotence a converged grid is supposed to have.
        stats.converged = true;
        break;
      }

      for (std::size_t i = 0; i < heights.size(); ++i)
      {
        heights[i] = static_cast<float>(static_cast<double>(heights[i]) + delta[i]);
      }

      stats.material_moved += moved_this_iteration;
      ++stats.iterations_run;
    }

    stats.height_sum_after = heightSum(readable);
    stats.max_slope_after = maxSlopeTangent(readable, width, settings.cell_size, settings.neighbourhood);
    stats.unstable_edges_after = countUnstableEdges( readable
                                                   , width
                                                   , settings.cell_size
                                                   , settings.neighbourhood
                                                   , settings.repose_angle_degrees
                                                   );

    return stats;
  }

  ErosionStats thermalErode(ErosionGrid& grid, ErosionSettings const& settings)
  {
    ErosionStats stats;

    if (!grid.lattice.valid())
    {
      // Which of the three ways a lattice can be invalid, so the caller's message names the field
      // it has to fix rather than "bad grid".
      stats.status = grid.lattice.width <= 0    ? ErosionStatus::BadWidth
                   : grid.lattice.height <= 0   ? ErosionStatus::EmptyGrid
                   : ErosionStatus::BadCellSize;
      return stats;
    }

    if (grid.heights.size() != grid.lattice.cellCount())
    {
      stats.status = ErosionStatus::RaggedGrid;
      return stats;
    }

    ErosionSettings local = settings;
    local.cell_size = grid.lattice.cell_size;

    return thermalErode( std::span<float>(grid.heights)
                       , grid.lattice.width
                       , local
                       , std::span<float const>(grid.influence)
                       );
  }

  std::vector<float> heightDelta(std::span<float const> before, std::span<float const> after)
  {
    if (before.empty() || before.size() != after.size())
    {
      return {};
    }

    std::vector<float> delta(before.size(), 0.0f);

    for (std::size_t i = 0; i < before.size(); ++i)
    {
      delta[i] = after[i] - before[i];
    }

    return delta;
  }

  std::optional<float> sampleBilinear( std::span<float const> values
                                     , int width
                                     , int height
                                     , float grid_x
                                     , float grid_z
                                     )
  {
    if (width <= 0 || height <= 0)
    {
      return std::nullopt;
    }

    if (values.size() != static_cast<std::size_t>(width) * static_cast<std::size_t>(height))
    {
      return std::nullopt;
    }

    // NaN fails every one of these comparisons, which is the intended answer.
    if (!(grid_x >= 0.0f) || !(grid_z >= 0.0f))
    {
      return std::nullopt;
    }

    if (!(grid_x <= static_cast<float>(width - 1)) || !(grid_z <= static_cast<float>(height - 1)))
    {
      return std::nullopt;
    }

    int const x0 = std::min(static_cast<int>(std::floor(grid_x)), width - 1);
    int const z0 = std::min(static_cast<int>(std::floor(grid_z)), height - 1);
    int const x1 = std::min(x0 + 1, width - 1);
    int const z1 = std::min(z0 + 1, height - 1);

    // Zero on the far edge, where x0 was clamped back and x1 == x0; the interpolation then
    // returns the edge value exactly instead of reading past the row.
    float const fx = grid_x - static_cast<float>(x0);
    float const fz = grid_z - static_cast<float>(z0);

    auto const value = [&] (int x, int z) -> float
    {
      return values[static_cast<std::size_t>(z) * static_cast<std::size_t>(width)
                  + static_cast<std::size_t>(x)];
    };

    float const top = value(x0, z0) + fx * (value(x1, z0) - value(x0, z0));
    float const bottom = value(x0, z1) + fx * (value(x1, z1) - value(x0, z1));

    return top + fz * (bottom - top);
  }

  ErosionDeltaField::ErosionDeltaField(ErosionLattice lattice, std::vector<float> delta)
    : _lattice(lattice)
    , _delta(std::move(delta))
  {
    if (_delta.size() != _lattice.cellCount())
    {
      // A field whose delta does not match its lattice would sample garbage or read out of
      // bounds. Emptied instead, so correctionAt is a constant zero and the stroke is a no-op.
      _delta.clear();
    }
  }

  float ErosionDeltaField::correctionAt(float world_x, float world_z) const
  {
    if (_delta.empty() || !_lattice.valid())
    {
      return 0.0f;
    }

    std::optional<float> const correction = sampleBilinear( std::span<float const>(_delta)
                                                          , _lattice.width
                                                          , _lattice.height
                                                          , _lattice.gridX(world_x)
                                                          , _lattice.gridZ(world_z)
                                                          );

    return correction.value_or(0.0f);
  }

  bool ErosionDeltaField::empty() const
  {
    return _delta.empty();
  }

  ErosionLattice const& ErosionDeltaField::lattice() const
  {
    return _lattice;
  }

  std::vector<float> const& ErosionDeltaField::delta() const
  {
    return _delta;
  }

  void ErosionDeltaField::fade(float center_x, float center_z, float radius, float inner_radius)
  {
    if (_delta.empty() || !_lattice.valid())
    {
      return;
    }

    for (int z = 0; z < _lattice.height; ++z)
    {
      for (int x = 0; x < _lattice.width; ++x)
      {
        float const dx = _lattice.worldX(x) - center_x;
        float const dz = _lattice.worldZ(z) - center_z;
        float const distance = std::sqrt(dx * dx + dz * dz);

        _delta[_lattice.index(x, z)] *= radialFalloff(distance, radius, inner_radius);
      }
    }
  }

  float ErosionDeltaField::maxAbsCorrection() const
  {
    float largest = 0.0f;

    for (float value : _delta)
    {
      largest = std::max(largest, std::abs(value));
    }

    return largest;
  }

  double ErosionDeltaField::correctionSum() const
  {
    return heightSum(std::span<float const>(_delta));
  }

  ErosionRun erodeGrid(ErosionGrid grid, ErosionSettings const& settings)
  {
    ErosionRun run;

    std::vector<float> const original = grid.heights;

    run.stats = thermalErode(grid, settings);

    if (!run.stats.ok())
    {
      return run;
    }

    run.field = ErosionDeltaField(grid.lattice, heightDelta(original, grid.heights));

    return run;
  }
}
