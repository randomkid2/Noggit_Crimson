// This file is part of Noggit3, licensed under GNU General Public License (version 3).

// Tests for the thermal erosion kernel.
//
// Everything here runs on a bare machine: the kernel is plain heights, plain floats and no Qt,
// OpenGL, MapChunk or World, which is the entire reason it was written as a separate module
// rather than as another MapChunk method next to blurTerrain.
//
// The tests are invariants, not samples of the output. Erosion has no "expected height" a test
// can be written against without reimplementing the solver in the test, and a test that pins
// specific numbers would fail on every legitimate tuning change while catching none of the bugs
// that matter. What matters is:
//
//   - CONSERVATION. Material moves, it is not created or destroyed. Every plausible bug in the
//     flow bookkeeping -- an edge counted twice, a sign flipped, a border edge leaking, an
//     in-place update reading a height another flow already changed -- breaks this and nothing
//     else catches all of them. The error observed on a 65x65 grid over 120 iterations is 1.8e-5
//     against 5.9e5 of material moved (3e-11 relative), so the tolerances below are 100x headroom
//     over float store rounding and would still catch a single misplaced flow.
//   - MONOTONICITY. The steepest slope never increases, iteration by iteration.
//   - CONVERGENCE. Work per iteration decays, and a settled grid re-eroded is bit-identical.
//   - SYMMETRY. The kernel commutes with the symmetries of the square, exactly.
//
// The glue templates at the bottom of the header are covered too: they take callbacks, so a
// lambda and a three-float struct stand in for World and glm::vec3.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <noggit/ErosionKernel.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

using namespace Noggit;

namespace
{
  // Deterministic, so a failure is reproducible and a tolerance measured once stays measured.
  // std::mt19937 would do as well but its output is not fixed across standard libraries for the
  // distributions, and the point is that this exact terrain is tested everywhere.
  class Lcg
  {
  public:
    explicit Lcg(std::uint32_t seed) : _state(seed) {}

    float next(float low, float high)
    {
      _state = _state * 1664525u + 1013904223u;
      float const unit = static_cast<float>((_state >> 8) & 0xFFFFFFu) / static_cast<float>(0xFFFFFFu);
      return low + unit * (high - low);
    }

  private:
    std::uint32_t _state;
  };

  std::vector<float> randomGrid(int width, int height, float amplitude, std::uint32_t seed)
  {
    Lcg random(seed);
    std::vector<float> grid(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));

    for (float& cell : grid)
    {
      cell = random.next(-amplitude, amplitude);
    }

    return grid;
  }

  std::vector<float> ramp(int width, int height, float rise_per_cell)
  {
    std::vector<float> grid(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));

    for (int z = 0; z < height; ++z)
    {
      for (int x = 0; x < width; ++x)
      {
        grid[static_cast<std::size_t>(z) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)]
          = rise_per_cell * static_cast<float>(x);
      }
    }

    return grid;
  }

  std::vector<float> spike(int size, float peak)
  {
    std::vector<float> grid(static_cast<std::size_t>(size) * static_cast<std::size_t>(size), 0.0f);
    grid[static_cast<std::size_t>(size / 2) * static_cast<std::size_t>(size) + static_cast<std::size_t>(size / 2)] = peak;

    return grid;
  }

  ErosionSettings defaultSettings()
  {
    ErosionSettings settings;
    settings.repose_angle_degrees = 30.0f;
    settings.cell_size = 1.0f;
    settings.iterations = 20;

    return settings;
  }

  float at(std::vector<float> const& grid, int width, int x, int z)
  {
    return grid[static_cast<std::size_t>(z) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)];
  }

  // Runs the settings' iterations ONE AT A TIME, checking the invariant after each. Erosion is a
  // relaxation, so the interesting claim is about every intermediate state, not the endpoint --
  // a solver that overshoots and comes back would pass an end-to-end check and be visibly wrong
  // under an interactive brush, where the user sees every iteration.
  struct StepwiseReport
  {
    int steps_taken = 0;
    int slope_increases = 0;
    int work_increases = 0;
    int peak_violations = 0;
    int pit_violations = 0;
    float final_slope = 0.0f;
  };

  StepwiseReport erodeStepwise(std::vector<float>& heights, int width, ErosionSettings const& settings)
  {
    ErosionSettings step = settings;
    step.iterations = 1;

    float const initial_peak = *std::max_element(heights.begin(), heights.end());
    float const initial_pit = *std::min_element(heights.begin(), heights.end());

    float previous_slope = maxSlopeTangent(heights, width, settings.cell_size, settings.neighbourhood);
    double previous_work = 0.0;
    bool have_work = false;

    StepwiseReport report;

    for (int iteration = 0; iteration < settings.iterations; ++iteration)
    {
      ErosionStats const stats = thermalErode(std::span<float>(heights), width, step);

      if (stats.converged)
      {
        break;
      }

      ++report.steps_taken;

      float const slope = maxSlopeTangent(heights, width, settings.cell_size, settings.neighbourhood);

      if (slope > previous_slope)
      {
        ++report.slope_increases;
      }

      if (have_work && stats.material_moved > previous_work)
      {
        ++report.work_increases;
      }

      if (*std::max_element(heights.begin(), heights.end()) > initial_peak)
      {
        ++report.peak_violations;
      }

      if (*std::min_element(heights.begin(), heights.end()) < initial_pit)
      {
        ++report.pit_violations;
      }

      previous_slope = slope;
      previous_work = stats.material_moved;
      have_work = true;
    }

    report.final_slope = previous_slope;

    return report;
  }
}

TEST_CASE("reposeTangent clamps rather than running away", "[erosion]")
{
  CHECK(reposeTangent(0.0f) == Catch::Approx(0.0f).margin(1e-7));
  CHECK(reposeTangent(45.0f) == Catch::Approx(1.0f).epsilon(1e-6));
  CHECK(reposeTangent(30.0f) == Catch::Approx(0.5773503f).epsilon(1e-6));

  // A slider can reach both ends. Neither may produce an infinity that turns the whole grid into
  // NaN on the first multiply.
  CHECK(std::isfinite(reposeTangent(90.0f)));
  CHECK(std::isfinite(reposeTangent(1000.0f)));
  CHECK(reposeTangent(90.0f) == reposeTangent(MAX_REPOSE_ANGLE_DEGREES));

  // A negative angle means "nothing is stable"; it must not come back as a negative threshold,
  // which would make every edge unstable by the width of its own slope and erode uphill.
  CHECK(reposeTangent(-10.0f) == 0.0f);
  CHECK(reposeTangent(std::nanf("")) == 0.0f);

  // Monotone over the usable range, so a bigger angle never erodes harder.
  float previous = -1.0f;

  for (float angle = 0.0f; angle < 89.0f; angle += 1.0f)
  {
    float const tangent = reposeTangent(angle);
    CHECK(tangent > previous);
    previous = tangent;
  }
}

TEST_CASE("heightSum recovers what a naive accumulator throws away", "[erosion]")
{
  // The conservation check is only as trustworthy as the sum it is built on. At 1e16 a double's
  // ulp is 2, so a naive accumulator drops both 1.0s entirely and reports that a grid which
  // gained 2 units of material gained none -- the measurement would hide exactly the class of bug
  // it exists to find.
  std::vector<float> const heights = {1e16f, 1.0f, 1.0f, -1e16f};

  double naive = 0.0;

  for (float height : heights)
  {
    naive += static_cast<double>(height);
  }

  CHECK(naive == 0.0);
  CHECK(heightSum(heights) == 2.0);

  CHECK(heightSum({}) == 0.0);
  CHECK(heightSum(std::vector<float>{3.5f, -1.25f}) == Catch::Approx(2.25));
}

TEST_CASE("maxSlopeTangent measures the steepest edge the neighbourhood has", "[erosion]")
{
  std::vector<float> const flat(25, 7.0f);
  CHECK(maxSlopeTangent(flat, 5, 1.0f, ErosionNeighbourhood::Moore8) == 0.0f);

  // Slope, not height difference: the same drop over a doubled cell size is half the tangent.
  std::vector<float> const step = {0.0f, 3.0f};
  CHECK(maxSlopeTangent(step, 2, 1.0f, ErosionNeighbourhood::VonNeumann4) == Catch::Approx(3.0f));
  CHECK(maxSlopeTangent(step, 2, 2.0f, ErosionNeighbourhood::VonNeumann4) == Catch::Approx(1.5f));

  // A ramp rising along the diagonal: the axis-aligned neighbours see 1 per cell, the diagonal
  // sees 2 over sqrt(2). This is why Moore8 is the default -- a 4-neighbour talus rule is blind
  // to the steeper direction and leaves diagonal ridges standing.
  std::vector<float> diagonal(25, 0.0f);

  for (int z = 0; z < 5; ++z)
  {
    for (int x = 0; x < 5; ++x)
    {
      diagonal[static_cast<std::size_t>(z) * 5u + static_cast<std::size_t>(x)] = static_cast<float>(x + z);
    }
  }

  CHECK(maxSlopeTangent(diagonal, 5, 1.0f, ErosionNeighbourhood::VonNeumann4) == Catch::Approx(1.0f));
  CHECK(maxSlopeTangent(diagonal, 5, 1.0f, ErosionNeighbourhood::Moore8) == Catch::Approx(1.4142136f).epsilon(1e-6));

  // Refuses rather than guesses.
  CHECK(maxSlopeTangent({}, 5, 1.0f, ErosionNeighbourhood::Moore8) == 0.0f);
  CHECK(maxSlopeTangent(flat, 0, 1.0f, ErosionNeighbourhood::Moore8) == 0.0f);
  CHECK(maxSlopeTangent(flat, 4, 1.0f, ErosionNeighbourhood::Moore8) == 0.0f);
  CHECK(maxSlopeTangent(flat, 5, 0.0f, ErosionNeighbourhood::Moore8) == 0.0f);
}

TEST_CASE("countUnstableEdges is exactly zero at the repose angle", "[erosion]")
{
  float const tangent = reposeTangent(30.0f);

  // A ramp rising by exactly tan(repose) per cell is at rest: the axis-aligned edges sit on the
  // threshold, and the diagonals rise by the same amount over sqrt(2) times the distance, so they
  // are below it. Both neighbourhoods must agree that there is nothing to do.
  std::vector<float> const at_repose = ramp(5, 5, tangent);
  CHECK(countUnstableEdges(at_repose, 5, 1.0f, ErosionNeighbourhood::Moore8, 30.0f) == 0);
  CHECK(countUnstableEdges(at_repose, 5, 1.0f, ErosionNeighbourhood::VonNeumann4, 30.0f) == 0);

  // Half again as steep. Every one of the 20 axis-aligned x-edges is over, and so are all 32
  // diagonals (1.5 > sqrt(2)); the 20 z-edges are level and stay stable.
  std::vector<float> const steeper = ramp(5, 5, tangent * 1.5f);
  CHECK(countUnstableEdges(steeper, 5, 1.0f, ErosionNeighbourhood::VonNeumann4, 30.0f) == 20);
  CHECK(countUnstableEdges(steeper, 5, 1.0f, ErosionNeighbourhood::Moore8, 30.0f) == 52);
}

TEST_CASE("a flat grid is a fixed point, bit for bit", "[erosion]")
{
  std::vector<float> heights(64, 12.5f);
  std::vector<float> const before = heights;

  ErosionStats const stats = thermalErode(std::span<float>(heights), 8, defaultSettings());

  REQUIRE(stats.ok());
  // Not "close to unchanged". A brush that rewrites every vertex it touches with an arithmetically
  // equal value still marks the tile dirty, still costs an undo step, and still shows up in a diff
  // of the saved ADT.
  CHECK(heights == before);
  CHECK(stats.converged);
  CHECK(stats.iterations_run == 0);
  CHECK(stats.material_moved == 0.0);
  CHECK(stats.conservationError() == 0.0);
}

TEST_CASE("a slope at the repose angle is a fixed point", "[erosion]")
{
  ErosionSettings const settings = defaultSettings();
  float const tangent = reposeTangent(settings.repose_angle_degrees);

  SECTION("just under the angle nothing is even unstable")
  {
    std::vector<float> heights = ramp(9, 9, tangent * 0.999f);
    std::vector<float> const before = heights;

    ErosionStats const stats = thermalErode(std::span<float>(heights), 9, settings);

    REQUIRE(stats.ok());
    CHECK(heights == before);
    CHECK(stats.converged);
    CHECK(stats.iterations_run == 0);
    CHECK(stats.unstable_edges_before == 0);
  }

  SECTION("exactly at the angle the grid still does not move")
  {
    std::vector<float> heights = ramp(9, 9, tangent);
    std::vector<float> const before = heights;

    ErosionStats const stats = thermalErode(std::span<float>(heights), 9, settings);

    REQUIRE(stats.ok());
    CHECK(heights == before);
    CHECK(stats.converged);
    CHECK(stats.iterations_run == 0);

    // But `unstable_edges_before` is NOT zero here, and that is the float arithmetic rather than
    // the kernel: tangent*x for x = 0..8 does not have exactly `tangent` between consecutive
    // entries, so 18 of the 288 edges exceed the threshold by up to 1.2e-7. The resulting flows
    // are around 1e-8, below the caller's convergence epsilon, so the run correctly declines to
    // apply them. Asserting zero here would be asserting that float multiplication is exact.
    CHECK(stats.unstable_edges_before > 0);
    CHECK(stats.max_slope_before <= tangent * (1.0f + 1e-5f));
  }

  SECTION("one degree steeper is not a fixed point")
  {
    // The fixed point is the angle itself, not a plateau around it.
    std::vector<float> steeper = ramp(9, 9, reposeTangent(settings.repose_angle_degrees + 1.0f));
    ErosionStats const moved = thermalErode(std::span<float>(steeper), 9, settings);

    CHECK(moved.iterations_run > 0);
    CHECK(moved.material_moved > 0.0);
    CHECK(moved.max_slope_after < moved.max_slope_before);
  }
}

TEST_CASE("thermal erosion conserves material", "[erosion]")
{
  struct Case
  {
    char const* name;
    int width;
    float amplitude;
    int iterations;
    ErosionNeighbourhood neighbourhood;
  };

  Case const cases[] =
    { {"small", 17, 40.0f, 25, ErosionNeighbourhood::Moore8}
    , {"medium", 33, 100.0f, 50, ErosionNeighbourhood::Moore8}
    , {"large", 65, 250.0f, 120, ErosionNeighbourhood::Moore8}
    , {"four-neighbour", 33, 100.0f, 50, ErosionNeighbourhood::VonNeumann4}
    };

  for (Case const& test_case : cases)
  {
    INFO(test_case.name);

    std::vector<float> heights = randomGrid(test_case.width, test_case.width, test_case.amplitude, 12345u);

    ErosionSettings settings = defaultSettings();
    settings.iterations = test_case.iterations;
    settings.neighbourhood = test_case.neighbourhood;

    double const before = heightSum(heights);

    ErosionStats const stats = thermalErode(std::span<float>(heights), test_case.width, settings);

    REQUIRE(stats.ok());
    REQUIRE(stats.material_moved > 0.0);

    // Measured independently of the stats, so a bug in the kernel's own bookkeeping cannot make
    // the test agree with it.
    CHECK(heightSum(heights) == Catch::Approx(before).margin(1e-3));
    CHECK(std::abs(stats.conservationError()) < 1e-3);

    // The real statement: the residue is float store rounding, orders of magnitude below the
    // material that actually moved. A dropped or doubled flow would land near material_moved.
    CHECK(std::abs(stats.conservationError()) < stats.material_moved * 1e-6);
  }
}

TEST_CASE("the steepest slope never increases and no new peak appears", "[erosion]")
{
  ErosionSettings settings = defaultSettings();
  settings.iterations = 120;

  struct Case
  {
    char const* name;
    std::vector<float> heights;
    int width;
  };

  std::vector<Case> cases;
  cases.push_back({"random", randomGrid(21, 21, 50.0f, 999u), 21});
  cases.push_back({"spike", spike(21, 100.0f), 21});
  cases.push_back({"steep ramp", ramp(21, 21, 5.0f), 21});

  // A cliff, which is where an order-dependent solver shows itself: half the grid at one height,
  // half at another.
  {
    std::vector<float> cliff(441, 0.0f);

    for (int z = 0; z < 21; ++z)
    {
      for (int x = 10; x < 21; ++x)
      {
        cliff[static_cast<std::size_t>(z) * 21u + static_cast<std::size_t>(x)] = 60.0f;
      }
    }

    cases.push_back({"cliff", cliff, 21});
  }

  // A checkerboard is the shape an unstable relaxation blows up on first: every cell is a peak or
  // a pit, and an overshooting update inverts the whole grid every iteration.
  {
    std::vector<float> checker(441, 0.0f);

    for (int z = 0; z < 21; ++z)
    {
      for (int x = 0; x < 21; ++x)
      {
        checker[static_cast<std::size_t>(z) * 21u + static_cast<std::size_t>(x)]
          = ((x + z) % 2) ? 20.0f : -20.0f;
      }
    }

    cases.push_back({"checkerboard", checker, 21});
  }

  for (Case& test_case : cases)
  {
    for (ErosionNeighbourhood neighbourhood : {ErosionNeighbourhood::Moore8, ErosionNeighbourhood::VonNeumann4})
    {
      char const* const neighbourhood_name
        = neighbourhood == ErosionNeighbourhood::Moore8 ? "Moore8" : "VonNeumann4";

      INFO(test_case.name);
      INFO(neighbourhood_name);

      settings.neighbourhood = neighbourhood;

      std::vector<float> heights = test_case.heights;
      float const initial_slope = maxSlopeTangent(heights, test_case.width, settings.cell_size, neighbourhood);

      StepwiseReport const report = erodeStepwise(heights, test_case.width, settings);

      CHECK(report.slope_increases == 0);
      // Material moved per iteration decays as well: the run is settling, not sloshing.
      CHECK(report.work_increases == 0);
      // The discrete maximum principle. Every cell's new height is a weighted mean of its
      // neighbourhood, so erosion cannot invent a summit or dig below the deepest pit.
      CHECK(report.peak_violations == 0);
      CHECK(report.pit_violations == 0);
      CHECK(report.final_slope < initial_slope);
    }
  }
}

TEST_CASE("a spike spreads symmetrically", "[erosion]")
{
  int const size = 15;

  std::vector<float> heights = spike(size, 80.0f);

  ErosionSettings settings = defaultSettings();
  settings.iterations = 40;

  REQUIRE(thermalErode(std::span<float>(heights), size, settings).ok());

  // Exact equality, not a tolerance. The kernel commutes with all eight symmetries of the square
  // because every iteration is simultaneous and the weights depend only on edge length -- if it
  // were sequential and in place, the east side would relax further than the west by exactly the
  // amount one traversal gets ahead, and this is the assertion that would fail.
  for (int z = 0; z < size; ++z)
  {
    for (int x = 0; x < size; ++x)
    {
      float const value = at(heights, size, x, z);

      CHECK(value == at(heights, size, size - 1 - x, z));           // mirror in x
      CHECK(value == at(heights, size, x, size - 1 - z));           // mirror in z
      CHECK(value == at(heights, size, z, x));                      // transpose
      CHECK(value == at(heights, size, size - 1 - z, size - 1 - x));// anti-transpose
    }
  }

  // It really did spread: the peak came down and its neighbours came up.
  CHECK(at(heights, size, 7, 7) < 80.0f);
  CHECK(at(heights, size, 7, 6) > 0.0f);
  CHECK(at(heights, size, 8, 7) > 0.0f);
}

TEST_CASE("erosion converges and a settled grid is idempotent", "[erosion]")
{
  int const size = 9;

  std::vector<float> heights = spike(size, 30.0f);

  ErosionSettings settings = defaultSettings();
  settings.iterations = 100000;

  ErosionStats const first = thermalErode(std::span<float>(heights), size, settings);

  REQUIRE(first.ok());
  CHECK(first.converged);
  CHECK(first.iterations_run > 0);
  CHECK(first.iterations_run < settings.iterations);

  // At rest means at the talus angle, up to the convergence epsilon the caller asked for. The
  // residual is a fraction of a percent of the threshold, not a different answer.
  CHECK(first.max_slope_after == Catch::Approx(reposeTangent(settings.repose_angle_degrees)).epsilon(1e-3));

  std::vector<float> const settled = heights;

  ErosionStats const second = thermalErode(std::span<float>(heights), size, settings);

  // Idempotence is the property thermal erosion was chosen for: re-running a converged stroke is
  // free and changes nothing, so a user who holds the brush down does not slowly melt the terrain.
  CHECK(heights == settled);
  CHECK(second.converged);
  CHECK(second.iterations_run == 0);
  CHECK(second.material_moved == 0.0);
}

TEST_CASE("splitting a run in two changes nothing", "[erosion]")
{
  int const width = 17;

  std::vector<float> whole = randomGrid(width, width, 30.0f, 4242u);
  std::vector<float> halves = whole;

  ErosionSettings ten = defaultSettings();
  ten.iterations = 10;

  ErosionSettings five = ten;
  five.iterations = 5;

  thermalErode(std::span<float>(whole), width, ten);
  thermalErode(std::span<float>(halves), width, five);
  thermalErode(std::span<float>(halves), width, five);

  // Each iteration is a pure function of the grid it starts from. If this ever fails the kernel
  // has grown hidden state, and an interactive brush -- which necessarily arrives in dribs -- will
  // not produce the same terrain as one long stroke.
  CHECK(whole == halves);
}

TEST_CASE("strength above the stability bound is clamped, not honoured", "[erosion]")
{
  int const width = 11;

  std::vector<float> absurd = randomGrid(width, width, 30.0f, 77u);
  std::vector<float> bounded = absurd;

  ErosionSettings wild = defaultSettings();
  wild.strength = 25.0f;

  ErosionSettings sane = defaultSettings();
  sane.strength = MAX_STABLE_STRENGTH;

  thermalErode(std::span<float>(absurd), width, wild);
  thermalErode(std::span<float>(bounded), width, sane);

  CHECK(absurd == bounded);

  // A strength of zero is a legal request to do nothing, and must not be mistaken for one.
  std::vector<float> idle = randomGrid(width, width, 30.0f, 77u);
  std::vector<float> const before = idle;

  ErosionSettings still = defaultSettings();
  still.strength = 0.0f;

  ErosionStats const stats = thermalErode(std::span<float>(idle), width, still);

  CHECK(idle == before);
  CHECK(stats.ok());
  CHECK(stats.material_moved == 0.0);
}

TEST_CASE("erosion is invariant under translation and reflection", "[erosion]")
{
  int const width = 13;

  ErosionSettings settings = defaultSettings();
  settings.iterations = 12;

  std::vector<float> base = randomGrid(width, width, 20.0f, 31337u);
  std::vector<float> raised = base;

  for (float& height : raised)
  {
    height += 1000.0f;
  }

  std::vector<float> mirrored(base.size());
  std::vector<float> transposed(base.size());

  for (int z = 0; z < width; ++z)
  {
    for (int x = 0; x < width; ++x)
    {
      mirrored[static_cast<std::size_t>(z) * static_cast<std::size_t>(width)
             + static_cast<std::size_t>(width - 1 - x)] = at(base, width, x, z);
      transposed[static_cast<std::size_t>(x) * static_cast<std::size_t>(width)
               + static_cast<std::size_t>(z)] = at(base, width, x, z);
    }
  }

  thermalErode(std::span<float>(base), width, settings);
  thermalErode(std::span<float>(raised), width, settings);
  thermalErode(std::span<float>(mirrored), width, settings);
  thermalErode(std::span<float>(transposed), width, settings);

  for (int z = 0; z < width; ++z)
  {
    for (int x = 0; x < width; ++x)
    {
      float const expected = at(base, width, x, z);

      // The talus threshold is an absolute height difference, so only differences matter and the
      // terrain's altitude must not change the answer. Not exact: at +1000 a float's ulp is 6e-5,
      // and the sum of a height and its correction rounds differently there.
      CHECK(at(raised, width, x, z) - 1000.0f == Catch::Approx(expected).margin(2e-4));

      // These two ARE exact -- see the spike symmetry case.
      CHECK(at(mirrored, width, width - 1 - x, z) == expected);
      CHECK(at(transposed, width, z, x) == expected);
    }
  }
}

TEST_CASE("influence gates flow without breaking conservation", "[erosion]")
{
  int const width = 15;

  ErosionSettings settings = defaultSettings();
  settings.iterations = 60;

  SECTION("a locked region is untouched and material stops at its edge")
  {
    std::vector<float> heights = spike(width, 60.0f);
    std::vector<float> influence(heights.size(), 1.0f);

    for (int z = 0; z < width; ++z)
    {
      for (int x = 10; x < width; ++x)
      {
        influence[static_cast<std::size_t>(z) * static_cast<std::size_t>(width)
                + static_cast<std::size_t>(x)] = 0.0f;
      }
    }

    double const before = heightSum(heights);

    ErosionStats const stats = thermalErode( std::span<float>(heights)
                                           , width
                                           , settings
                                           , std::span<float const>(influence)
                                           );

    REQUIRE(stats.ok());

    for (int z = 0; z < width; ++z)
    {
      for (int x = 10; x < width; ++x)
      {
        CHECK(at(heights, width, x, z) == 0.0f);
      }
    }

    // The material the locked cells refused to accept is still on the grid, piled against the
    // boundary. Gating must not become a leak.
    CHECK(heightSum(heights) == Catch::Approx(before).margin(1e-3));
  }

  SECTION("zero influence everywhere is a no-op")
  {
    std::vector<float> heights = randomGrid(width, width, 30.0f, 8u);
    std::vector<float> const before = heights;
    std::vector<float> const influence(heights.size(), 0.0f);

    ErosionStats const stats = thermalErode( std::span<float>(heights)
                                           , width
                                           , settings
                                           , std::span<float const>(influence)
                                           );

    CHECK(heights == before);
    CHECK(stats.converged);
    CHECK(stats.iterations_run == 0);
  }

  SECTION("a soft radial brush leaves everything beyond its radius alone")
  {
    std::vector<float> heights = spike(width, 60.0f);
    std::vector<float> influence(heights.size(), 0.0f);

    float const centre = static_cast<float>(width / 2);

    for (int z = 0; z < width; ++z)
    {
      for (int x = 0; x < width; ++x)
      {
        float const dx = static_cast<float>(x) - centre;
        float const dz = static_cast<float>(z) - centre;

        influence[static_cast<std::size_t>(z) * static_cast<std::size_t>(width)
                + static_cast<std::size_t>(x)] = radialFalloff(std::sqrt(dx * dx + dz * dz), 5.0f, 2.0f);
      }
    }

    double const before = heightSum(heights);

    REQUIRE(thermalErode(std::span<float>(heights), width, settings, std::span<float const>(influence)).ok());

    for (int z = 0; z < width; ++z)
    {
      for (int x = 0; x < width; ++x)
      {
        float const dx = static_cast<float>(x) - centre;
        float const dz = static_cast<float>(z) - centre;

        if (std::sqrt(dx * dx + dz * dz) >= 5.0f)
        {
          CHECK(at(heights, width, x, z) == 0.0f);
        }
      }
    }

    CHECK(heightSum(heights) == Catch::Approx(before).margin(1e-3));
    CHECK(at(heights, width, width / 2, width / 2) < 60.0f);
  }

  SECTION("an influence array of the wrong length is refused")
  {
    std::vector<float> heights = randomGrid(width, width, 30.0f, 8u);
    std::vector<float> const before = heights;
    std::vector<float> const influence(heights.size() - 1, 1.0f);

    ErosionStats const stats = thermalErode( std::span<float>(heights)
                                           , width
                                           , settings
                                           , std::span<float const>(influence)
                                           );

    CHECK(stats.status == ErosionStatus::InfluenceMismatch);
    CHECK(heights == before);
  }
}

TEST_CASE("bad input is reported, never eroded", "[erosion]")
{
  std::vector<float> heights(12, 1.0f);
  ErosionSettings settings = defaultSettings();

  CHECK(thermalErode({}, 4, settings).status == ErosionStatus::EmptyGrid);
  CHECK(thermalErode(std::span<float>(heights), 0, settings).status == ErosionStatus::BadWidth);
  CHECK(thermalErode(std::span<float>(heights), -3, settings).status == ErosionStatus::BadWidth);
  CHECK(thermalErode(std::span<float>(heights), 5, settings).status == ErosionStatus::RaggedGrid);

  ErosionSettings bad_cell = settings;
  bad_cell.cell_size = 0.0f;
  CHECK(thermalErode(std::span<float>(heights), 4, bad_cell).status == ErosionStatus::BadCellSize);

  bad_cell.cell_size = std::nanf("");
  CHECK(thermalErode(std::span<float>(heights), 4, bad_cell).status == ErosionStatus::BadCellSize);

  ErosionSettings bad_iterations = settings;
  bad_iterations.iterations = -1;
  CHECK(thermalErode(std::span<float>(heights), 4, bad_iterations).status == ErosionStatus::BadSettings);

  ErosionSettings bad_strength = settings;
  bad_strength.strength = std::nanf("");
  CHECK(thermalErode(std::span<float>(heights), 4, bad_strength).status == ErosionStatus::BadSettings);

  // Zero iterations is a legal request, not an error.
  ErosionSettings none = settings;
  none.iterations = 0;
  CHECK(thermalErode(std::span<float>(heights), 4, none).status == ErosionStatus::Ok);

  // Every rejection above left the heights alone.
  CHECK(heights == std::vector<float>(12, 1.0f));

  CHECK(erosionStatusText(ErosionStatus::Ok) == "ok");
  CHECK(!erosionStatusText(ErosionStatus::RaggedGrid).empty());
}

TEST_CASE("sampleBilinear interpolates and refuses to extrapolate", "[erosion]")
{
  //  0 10
  // 20 30
  std::vector<float> const values = {0.0f, 10.0f, 20.0f, 30.0f};

  // Exact at the nodes, which is what makes the delta field reproduce the kernel's own output
  // where a vertex happens to sit on a lattice point.
  CHECK(sampleBilinear(values, 2, 2, 0.0f, 0.0f) == 0.0f);
  CHECK(sampleBilinear(values, 2, 2, 1.0f, 0.0f) == 10.0f);
  CHECK(sampleBilinear(values, 2, 2, 0.0f, 1.0f) == 20.0f);
  CHECK(sampleBilinear(values, 2, 2, 1.0f, 1.0f) == 30.0f);

  CHECK(*sampleBilinear(values, 2, 2, 0.5f, 0.5f) == Catch::Approx(15.0f));
  CHECK(*sampleBilinear(values, 2, 2, 0.5f, 0.0f) == Catch::Approx(5.0f));
  CHECK(*sampleBilinear(values, 2, 2, 1.0f, 0.5f) == Catch::Approx(20.0f));

  // Outside is a miss, not a clamp: clamping would smear the border value across the whole map.
  CHECK(!sampleBilinear(values, 2, 2, 1.001f, 0.0f).has_value());
  CHECK(!sampleBilinear(values, 2, 2, -0.001f, 0.0f).has_value());
  CHECK(!sampleBilinear(values, 2, 2, 0.0f, 1.5f).has_value());
  CHECK(!sampleBilinear(values, 2, 2, std::nanf(""), 0.0f).has_value());
  CHECK(!sampleBilinear(values, 2, 2, 0.0f, std::nanf("")).has_value());

  // Degenerate shapes answer rather than read out of bounds.
  CHECK(*sampleBilinear(std::vector<float>{7.0f}, 1, 1, 0.0f, 0.0f) == 7.0f);
  CHECK(!sampleBilinear(std::vector<float>{7.0f}, 2, 2, 0.0f, 0.0f).has_value());
  CHECK(!sampleBilinear(values, 0, 2, 0.0f, 0.0f).has_value());

  // A constant field samples to that constant everywhere -- the weights are a partition of unity,
  // so an unmodified region cannot pick up a ripple from the interpolation itself.
  std::vector<float> const constant(16, -4.25f);

  for (float z = 0.0f; z <= 3.0f; z += 0.25f)
  {
    for (float x = 0.0f; x <= 3.0f; x += 0.25f)
    {
      CHECK(*sampleBilinear(constant, 4, 4, x, z) == Catch::Approx(-4.25f));
    }
  }
}

TEST_CASE("radialFalloff is a bounded, monotone brush profile", "[erosion]")
{
  CHECK(radialFalloff(0.0f, 10.0f, 4.0f) == 1.0f);
  CHECK(radialFalloff(4.0f, 10.0f, 4.0f) == 1.0f);
  CHECK(radialFalloff(10.0f, 10.0f, 4.0f) == 0.0f);
  CHECK(radialFalloff(1000.0f, 10.0f, 4.0f) == 0.0f);
  CHECK(radialFalloff(7.0f, 10.0f, 4.0f) == Catch::Approx(0.5f));

  // Never leaves [0,1] and never rises with distance: an influence outside that range would make
  // the flow gate amplify rather than attenuate, and material would be created at the rim.
  float previous = 2.0f;

  for (float distance = 0.0f; distance <= 12.0f; distance += 0.1f)
  {
    float const weight = radialFalloff(distance, 10.0f, 4.0f);

    CHECK(weight >= 0.0f);
    CHECK(weight <= 1.0f);
    CHECK(weight <= previous);

    previous = weight;
  }

  // A hard-edged brush: inner == radius.
  CHECK(radialFalloff(9.99f, 10.0f, 10.0f) == 1.0f);
  CHECK(radialFalloff(10.0f, 10.0f, 10.0f) == 0.0f);

  // Nonsense in, zero out.
  CHECK(radialFalloff(1.0f, 0.0f, 0.0f) == 0.0f);
  CHECK(radialFalloff(1.0f, -5.0f, 0.0f) == 0.0f);
  CHECK(radialFalloff(std::nanf(""), 10.0f, 4.0f) == 0.0f);
  // An inner radius past the outer one clamps instead of dividing by a negative.
  CHECK(radialFalloff(5.0f, 10.0f, 50.0f) == 1.0f);
}

TEST_CASE("latticeCovering pads the brush with an inert ring", "[erosion]")
{
  for (float radius : {1.0f, 5.0f, 8.3f, 40.0f})
  {
    for (float cell_size : {1.0f, 2.0f, 4.1667f})
    {
      INFO(radius);
      INFO(cell_size);

      ErosionLattice const lattice = latticeCovering(100.0f, -200.0f, radius, cell_size);

      REQUIRE(lattice.valid());

      // Odd, so a cell sits exactly on the brush centre and a symmetric brush stays symmetric.
      CHECK(lattice.width % 2 == 1);
      CHECK(lattice.width == lattice.height);
      CHECK(lattice.worldX(lattice.width / 2) == Catch::Approx(100.0f));
      CHECK(lattice.worldZ(lattice.height / 2) == Catch::Approx(-200.0f));

      // The border is at least one whole cell beyond the brush, so the wall material piles
      // against is outside the region the user is editing.
      CHECK(std::abs(lattice.worldX(0) - 100.0f) >= radius + cell_size * 0.999f);
      CHECK(lattice.contains(100.0f + radius, -200.0f));
      CHECK(lattice.contains(100.0f, -200.0f - radius));
      CHECK(!lattice.contains(100.0f + radius + 3.0f * cell_size, -200.0f));
    }
  }

  // Refusals, all reported as an invalid lattice rather than a smaller edit or a bad_alloc.
  CHECK(!latticeCovering(0.0f, 0.0f, -1.0f, 1.0f).valid());
  CHECK(!latticeCovering(0.0f, 0.0f, 0.0f, 1.0f).valid());
  CHECK(!latticeCovering(0.0f, 0.0f, 5.0f, 0.0f).valid());
  CHECK(!latticeCovering(0.0f, 0.0f, std::nanf(""), 1.0f).valid());
  // 4000 yards at a hundredth of a yard is 6.4e11 cells.
  CHECK(!latticeCovering(0.0f, 0.0f, 4000.0f, 0.01f).valid());
}

TEST_CASE("applyRadialInfluence multiplies into what is already there", "[erosion]")
{
  ErosionLattice const lattice = latticeCovering(0.0f, 0.0f, 4.0f, 1.0f);

  ErosionGrid grid;
  grid.lattice = lattice;
  grid.heights.assign(lattice.cellCount(), 0.0f);

  // A cell the sampler could not answer for.
  grid.influence.assign(lattice.cellCount(), 1.0f);
  grid.influence[lattice.index(lattice.width / 2, lattice.height / 2)] = 0.0f;

  applyRadialInfluence(grid, 0.0f, 0.0f, 4.0f, 2.0f);

  REQUIRE(grid.influence.size() == lattice.cellCount());

  // Overwriting instead of multiplying would re-enable erosion on the hole, which is the whole
  // reason this is a multiply.
  CHECK(grid.influence[lattice.index(lattice.width / 2, lattice.height / 2)] == 0.0f);

  for (int z = 0; z < lattice.height; ++z)
  {
    for (int x = 0; x < lattice.width; ++x)
    {
      float const dx = lattice.worldX(x);
      float const dz = lattice.worldZ(z);
      float const distance = std::sqrt(dx * dx + dz * dz);
      float const weight = grid.influence[lattice.index(x, z)];

      CHECK(weight >= 0.0f);
      CHECK(weight <= 1.0f);

      if (distance >= 4.0f)
      {
        CHECK(weight == 0.0f);
      }
    }
  }

  // The border ring is entirely outside the brush, so nothing can flow into the wall.
  for (int x = 0; x < lattice.width; ++x)
  {
    CHECK(grid.influence[lattice.index(x, 0)] == 0.0f);
    CHECK(grid.influence[lattice.index(x, lattice.height - 1)] == 0.0f);
  }
}

TEST_CASE("heightDelta and ErosionDeltaField apply the difference, not the result", "[erosion]")
{
  CHECK(heightDelta({}, {}).empty());
  CHECK(heightDelta(std::vector<float>{1.0f}, std::vector<float>{1.0f, 2.0f}).empty());

  std::vector<float> const before = {1.0f, 2.0f, 3.0f};
  std::vector<float> const after = {1.0f, 2.5f, 0.0f};
  std::vector<float> const delta = heightDelta(before, after);

  REQUIRE(delta.size() == 3);
  CHECK(delta[0] == 0.0f);
  CHECK(delta[1] == Catch::Approx(0.5f));
  CHECK(delta[2] == Catch::Approx(-3.0f));

  ErosionLattice lattice;
  lattice.origin_x = 10.0f;
  lattice.origin_z = 20.0f;
  lattice.width = 3;
  lattice.height = 3;
  lattice.cell_size = 2.0f;

  std::vector<float> field_delta(9, 0.0f);
  field_delta[lattice.index(1, 1)] = -8.0f;

  ErosionDeltaField const field(lattice, field_delta);

  REQUIRE(!field.empty());
  CHECK(field.maxAbsCorrection() == 8.0f);
  CHECK(field.correctionAt(12.0f, 22.0f) == -8.0f);
  // Halfway to the neighbouring node, which is zero.
  CHECK(field.correctionAt(13.0f, 22.0f) == Catch::Approx(-4.0f));

  // Outside the lattice the correction is exactly zero, so a vertex beyond the brush is left
  // bit-identical rather than nudged by an extrapolated value.
  CHECK(field.correctionAt(1000.0f, 1000.0f) == 0.0f);
  CHECK(field.correctionAt(9.9f, 20.0f) == 0.0f);

  // A delta that does not match its lattice is dropped rather than sampled out of bounds.
  ErosionDeltaField const mismatched(lattice, std::vector<float>(4, 5.0f));
  CHECK(mismatched.empty());
  CHECK(mismatched.correctionAt(12.0f, 22.0f) == 0.0f);

  ErosionDeltaField const nothing;
  CHECK(nothing.empty());
  CHECK(nothing.correctionAt(0.0f, 0.0f) == 0.0f);
  CHECK(nothing.maxAbsCorrection() == 0.0f);
}

TEST_CASE("erodeGrid leaves its argument alone and returns the node deltas", "[erosion]")
{
  ErosionLattice const lattice = latticeCovering(0.0f, 0.0f, 6.0f, 1.0f);

  ErosionGrid grid;
  grid.lattice = lattice;
  grid.heights = randomGrid(lattice.width, lattice.height, 20.0f, 606u);

  std::vector<float> const original = grid.heights;

  ErosionSettings settings = defaultSettings();
  settings.iterations = 20;

  ErosionRun const run = erodeGrid(grid, settings);

  REQUIRE(run.stats.ok());
  REQUIRE(run.field.delta().size() == lattice.cellCount());

  // By value: the caller's grid is untouched, so a rejected preview costs nothing to discard.
  CHECK(grid.heights == original);

  // At a lattice node the correction is the kernel's own delta -- no interpolation error is
  // introduced where the answer is already known.
  for (int z = 0; z < lattice.height; ++z)
  {
    for (int x = 0; x < lattice.width; ++x)
    {
      CHECK(run.field.correctionAt(lattice.worldX(x), lattice.worldZ(z))
            == run.field.delta()[lattice.index(x, z)]);
    }
  }

  // The deltas sum to zero for the same reason the heights do.
  CHECK(heightSum(run.field.delta()) == Catch::Approx(0.0).margin(1e-3));

  // An invalid grid produces an empty field rather than a partially valid one.
  ErosionGrid broken;
  broken.lattice = lattice;
  broken.heights.assign(3, 0.0f);

  ErosionRun const refused = erodeGrid(broken, settings);
  CHECK(refused.stats.status == ErosionStatus::RaggedGrid);
  CHECK(refused.field.empty());

  // Each way a lattice can be malformed names its own field, so the caller's message can too.
  ErosionGrid unlatticed;
  CHECK(erodeGrid(unlatticed, settings).stats.status == ErosionStatus::BadWidth);

  ErosionGrid flat_lattice;
  flat_lattice.lattice.width = 4;
  flat_lattice.lattice.height = 0;
  CHECK(erodeGrid(flat_lattice, settings).stats.status == ErosionStatus::EmptyGrid);

  ErosionGrid zero_cell;
  zero_cell.lattice.width = 4;
  zero_cell.lattice.height = 4;
  zero_cell.lattice.cell_size = 0.0f;
  zero_cell.heights.assign(16, 0.0f);
  CHECK(erodeGrid(zero_cell, settings).stats.status == ErosionStatus::BadCellSize);
}

TEST_CASE("sampleLattice marks the cells it could not answer for as inert", "[erosion]")
{
  ErosionLattice const lattice = latticeCovering(0.0f, 0.0f, 4.0f, 1.0f);

  int calls = 0;

  // Stands in for World::GetVertex returning false over a hole or an unloaded tile.
  ErosionGrid const grid = ErosionCollector::sampleLattice
    ( lattice
    , [&calls] (float world_x, float /*world_z*/, float& out_height) -> bool
      {
        ++calls;

        if (world_x > 2.0f)
        {
          return false;
        }

        out_height = 5.0f;
        return true;
      }
    );

  CHECK(static_cast<std::size_t>(calls) == lattice.cellCount());
  REQUIRE(grid.heights.size() == lattice.cellCount());
  REQUIRE(grid.influence.size() == lattice.cellCount());
  CHECK(grid.valid());

  for (int z = 0; z < lattice.height; ++z)
  {
    for (int x = 0; x < lattice.width; ++x)
    {
      bool const answered = lattice.worldX(x) <= 2.0f;

      CHECK(grid.influence[lattice.index(x, z)] == (answered ? 1.0f : 0.0f));
      CHECK(grid.heights[lattice.index(x, z)] == (answered ? 5.0f : 0.0f));
    }
  }

  // The unanswered cells hold 0.0f, which is NOT a height -- if the gate did not hold them
  // inert, the kernel would see a five-unit cliff along x = 2 and erode it. It does not.
  ErosionSettings settings = defaultSettings();
  settings.iterations = 30;

  ErosionRun const run = erodeGrid(grid, settings);

  REQUIRE(run.stats.ok());
  CHECK(run.stats.iterations_run == 0);
  CHECK(run.field.maxAbsCorrection() == 0.0f);

  // A sampler that answers everything leaves the influence array empty, which is the solver's
  // ungated fast path and means the same thing as all ones.
  ErosionGrid const complete = ErosionCollector::sampleLattice
    (lattice, [] (float, float, float& out_height) -> bool { out_height = 3.0f; return true; });

  CHECK(complete.influence.empty());
  CHECK(complete.valid());

  // An invalid lattice produces an empty grid and never calls the sampler.
  int uncalled = 0;
  ErosionGrid const nothing = ErosionCollector::sampleLattice
    (ErosionLattice{}, [&uncalled] (float, float, float&) -> bool { ++uncalled; return true; });

  CHECK(uncalled == 0);
  CHECK(nothing.heights.empty());
}

TEST_CASE("applyToVertices adds the interpolated correction to whatever it is given", "[erosion]")
{
  // Stands in for glm::vec3 and MapChunk::mVertices, which this module deliberately cannot name.
  struct Vertex
  {
    float x;
    float y;
    float z;
  };

  ErosionLattice const lattice = latticeCovering(0.0f, 0.0f, 4.0f, 1.0f);

  std::vector<float> delta(lattice.cellCount(), 0.0f);
  delta[lattice.index(lattice.width / 2, lattice.height / 2)] = -6.0f;

  ErosionDeltaField const field(lattice, delta);

  Vertex vertices[4] =
    { {0.0f, 100.0f, 0.0f}      // on the moved node
    , {1000.0f, 50.0f, 1000.0f} // far outside the lattice
    , {0.5f, 0.0f, 0.0f}        // halfway to an unmoved node
    , {3.0f, 12.0f, 3.0f}       // inside the lattice, but where nothing moved
    };

  std::size_t const moved = ErosionCollector::applyToVertices(field, vertices, 4, 0.5f);

  CHECK(moved == 2);
  CHECK(vertices[0].y == Catch::Approx(97.0f));
  CHECK(vertices[1].y == 50.0f);
  CHECK(vertices[2].y == Catch::Approx(-1.5f));
  // Exactly unchanged, not approximately: a zero correction must not cost an undo step.
  CHECK(vertices[3].y == 12.0f);

  // The weight scales the whole stroke, which is where brush pressure enters.
  Vertex full[1] = {{0.0f, 100.0f, 0.0f}};
  CHECK(ErosionCollector::applyToVertices(field, full, 1) == 1);
  CHECK(full[0].y == Catch::Approx(94.0f));

  Vertex none[1] = {{0.0f, 100.0f, 0.0f}};
  CHECK(ErosionCollector::applyToVertices(field, none, 1, 0.0f) == 0);
  CHECK(none[0].y == 100.0f);
}

TEST_CASE("fade scales the corrections instead of the flow", "[erosion]")
{
  ErosionLattice const lattice = latticeCovering(0.0f, 0.0f, 6.0f, 1.0f);

  std::vector<float> delta(lattice.cellCount(), -4.0f);
  ErosionDeltaField field(lattice, delta);

  double const before_sum = field.correctionSum();

  field.fade(0.0f, 0.0f, 6.0f, 3.0f);

  CHECK(field.maxAbsCorrection() == 4.0f);              // the centre is untouched
  CHECK(field.correctionAt(0.0f, 0.0f) == -4.0f);
  CHECK(field.correctionAt(3.0f, 0.0f) == -4.0f);       // still inside the plateau
  CHECK(field.correctionAt(6.0f, 0.0f) == 0.0f);        // at the radius
  CHECK(field.correctionAt(20.0f, 0.0f) == 0.0f);       // beyond the lattice

  // Material is discarded, which is the whole difference from gating, and correctionSum is how a
  // caller finds out how much.
  CHECK(field.correctionSum() > before_sum);
  CHECK(field.correctionSum() < 0.0);

  for (float value : field.delta())
  {
    // Every correction shrank toward zero; none changed sign or grew.
    CHECK(value <= 0.0f);
    CHECK(value >= -4.0f);
  }

  // Fading an empty field is a no-op rather than a crash.
  ErosionDeltaField empty_field;
  empty_field.fade(0.0f, 0.0f, 6.0f, 3.0f);
  CHECK(empty_field.empty());
}

TEST_CASE("erodeRegion drives the whole module from a sampler", "[erosion]")
{
  // The integrator's call, with World replaced by a lambda: a cone at roughly 76 degrees, far
  // steeper than the 30-degree repose angle, eroded through a brush smaller than the cone.
  float const cell_size = 1.0f;
  float const radius = 8.0f;
  float const centre_x = 500.0f;
  float const centre_z = -300.0f;

  ErosionLattice const lattice = latticeCovering(centre_x, centre_z, radius, cell_size);

  auto const sample = [] (float world_x, float world_z, float& out_height) -> bool
  {
    float const dx = world_x - 500.0f;
    float const dz = world_z + 300.0f;

    out_height = std::max(0.0f, 40.0f - 4.0f * std::sqrt(dx * dx + dz * dz));
    return true;
  };

  ErosionSettings settings = defaultSettings();
  settings.cell_size = cell_size;
  settings.iterations = 60;

  // What the terrain looks like once the caller has added the corrections, which is the only
  // surface the user ever sees.
  auto applied = [&] (ErosionRun const& run)
  {
    ErosionGrid const grid = ErosionCollector::sampleLattice(lattice, sample);
    std::vector<float> result(grid.heights.size());

    for (std::size_t i = 0; i < result.size(); ++i)
    {
      result[i] = grid.heights[i] + run.field.delta()[i];
    }

    return result;
  };

  ErosionRun const gated = ErosionCollector::erodeRegion
    (lattice, settings, sample, centre_x, centre_z, radius, radius * 0.6f, ErosionEdgeMode::GateFlow);

  ErosionRun const faded = ErosionCollector::erodeRegion
    (lattice, settings, sample, centre_x, centre_z, radius, radius * 0.6f, ErosionEdgeMode::FadeResult);

  REQUIRE(gated.stats.ok());
  REQUIRE(faded.stats.ok());
  CHECK(gated.stats.iterations_run > 0);
  CHECK(faded.stats.iterations_run > 0);

  // Both bring the peak down, and neither touches anything past the brush.
  for (ErosionRun const* run : {&gated, &faded})
  {
    CHECK(run->field.correctionAt(centre_x, centre_z) < 0.0f);
    CHECK(run->field.correctionAt(centre_x + radius, centre_z) == 0.0f);
    CHECK(run->field.correctionAt(centre_x + radius + 5.0f, centre_z) == 0.0f);
  }

  float const original_slope = maxSlopeTangent( ErosionCollector::sampleLattice(lattice, sample).heights
                                              , lattice.width
                                              , cell_size
                                              , settings.neighbourhood
                                              );

  std::vector<float> const gated_result = applied(gated);
  std::vector<float> const faded_result = applied(faded);

  float const gated_slope = maxSlopeTangent(gated_result, lattice.width, cell_size, settings.neighbourhood);
  float const faded_slope = maxSlopeTangent(faded_result, lattice.width, cell_size, settings.neighbourhood);

  // THE TRADE, asserted rather than described. Gating conserves to float rounding and leaves the
  // brush rim STEEPER than the terrain started -- material arrived there and could not leave.
  // Fading throws material away and leaves the surface almost as smooth as an ungated run.
  //
  // Both directions are checked, because a change that quietly turned one mode into the other
  // would otherwise pass: an integrator choosing GateFlow for a volume-preserving edit would
  // silently stop preserving volume.
  CHECK(gated.field.correctionSum() == Catch::Approx(0.0).margin(1e-3));
  CHECK(gated_slope > original_slope);

  CHECK(faded.field.correctionSum() < -1.0);
  CHECK(faded_slope < gated_slope);
  CHECK(faded_slope < original_slope * 1.1f);

  // The core of the brush is properly relaxed under both modes; the argument above is only about
  // what happens at the rim.
  auto core_slope = [&] (std::vector<float> const& heights)
  {
    float steepest = 0.0f;

    for (int z = 0; z + 1 < lattice.height; ++z)
    {
      for (int x = 0; x + 1 < lattice.width; ++x)
      {
        float const dx = lattice.worldX(x) - centre_x;
        float const dz = lattice.worldZ(z) - centre_z;

        if (std::sqrt(dx * dx + dz * dz) > radius * 0.5f)
        {
          continue;
        }

        steepest = std::max(steepest, std::abs(at(heights, lattice.width, x, z)
                                             - at(heights, lattice.width, x + 1, z)));
      }
    }

    return steepest;
  };

  CHECK(core_slope(gated_result) < original_slope);
  CHECK(core_slope(faded_result) < original_slope);
}
