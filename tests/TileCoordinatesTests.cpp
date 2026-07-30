// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <noggit/database/TileCoordinates.hpp>

#include <cmath>
#include <limits>

using namespace Noggit::Database;
using namespace Noggit::Database::TileCoordinates;
using Catch::Approx;

namespace
{
  constexpr double PI = 3.14159265358979323846;
  constexpr double TWO_PI = 6.283185307179586476925286766559;

  // Angles are compared to 1e-9 radians: about 6e-8 of a degree, far tighter than anything that
  // could be seen in game and far looser than the 1e-15 of accumulated error a sin/atan2 round
  // trip actually carries. Exact equality would be wrong here and would fail on a different
  // libm.
  constexpr double ANGLE_TOLERANCE = 1e-9;

  // Quaternion components live in [-1, 1], so an absolute tolerance is the right shape.
  constexpr double COMPONENT_TOLERANCE = 1e-12;

  // Tile edges are whole multiples of TILE_SIZE computed in double, so they agree to the last
  // bit; 1e-9 yards against a 533 yard tile leaves the assertion meaningful anyway.
  constexpr double EDGE_TOLERANCE = 1e-9;

  // One epsilon inside a tile edge. Double resolution at world magnitudes is around 4e-12
  // yards, so 1e-6 is unambiguously inside; a tile is 533 yards across, so it cannot reach the
  // far edge.
  constexpr double INSIDE_EPSILON = 1e-6;

  TileIndex makeTile(int x, int y)
  {
    TileIndex tile;
    tile.x = x;
    tile.y = y;

    return tile;
  }

  WorldPosition makePosition(double x, double y, double z)
  {
    WorldPosition position;
    position.x = x;
    position.y = y;
    position.z = z;

    return position;
  }

  // Every corner of the tile that the bounds claim, plus the centre, must resolve back to the
  // tile the bounds came from. max is the owned edge and min belongs to the neighbour, so min is
  // probed one epsilon inside and max is probed exactly.
  bool tileOwnsItsBounds(TileIndex const& tile)
  {
    TileBounds const bounds (boundsForTile(tile));

    double const inner_x (bounds.min_x + INSIDE_EPSILON);
    double const inner_y (bounds.min_y + INSIDE_EPSILON);
    double const centre_x ((bounds.min_x + bounds.max_x) * 0.5);
    double const centre_y ((bounds.min_y + bounds.max_y) * 0.5);

    return tileForPosition(inner_x, inner_y) == tile
        && tileForPosition(bounds.max_x, bounds.max_y) == tile
        && tileForPosition(bounds.max_x - INSIDE_EPSILON, bounds.max_y - INSIDE_EPSILON) == tile
        && tileForPosition(inner_x, bounds.max_y) == tile
        && tileForPosition(bounds.max_x, inner_y) == tile
        && tileForPosition(centre_x, centre_y) == tile;
  }

  double quaternionNorm(Quaternion const& rotation)
  {
    return std::sqrt
      ( rotation.r0 * rotation.r0 + rotation.r1 * rotation.r1
      + rotation.r2 * rotation.r2 + rotation.r3 * rotation.r3
      );
  }
}

TEST_CASE("verified real spawns land in Elwynn Forest tile (49, 31)", "[tile][real]")
{
  // The anchor case: creatures around x = -9500, y = 70 on map 0 are in Elwynn Forest, which is
  // tile (49, 31). Every other assertion in this file is arithmetic; this one is ground truth,
  // and it is what proves the formula is oriented the right way round.
  TileIndex const elwynn (makeTile(49, 31));

  CHECK(tileForPosition(-9500.0, 70.0) == elwynn);
  CHECK(tileForPosition(-9528.7, 92.5) == elwynn);
  CHECK(tileForPosition(-9530.2, 96.3) == elwynn);
  CHECK(tileForPosition(-9429.9, 56.1) == elwynn);

  CHECK(isValidTile(elwynn));
  CHECK(isValidPosition(-9500.0, 70.0));

  // The tile really does contain all four, per its own bounds.
  TileBounds const bounds (boundsForTile(elwynn));

  CHECK(bounds.min_x < -9530.2);
  CHECK(bounds.max_x > -9429.9);
  CHECK(bounds.min_y < 56.1);
  CHECK(bounds.max_y > 96.3);
}

TEST_CASE("the WorldPosition overload agrees and ignores z", "[tile][real]")
{
  // Tiles are columns: a spawn in a cave under Goldshire is in the same tile as one on the road
  // above it.
  CHECK(tileForPosition(makePosition(-9500.0, 70.0, 58.0)) == makeTile(49, 31));
  CHECK(tileForPosition(makePosition(-9500.0, 70.0, -4000.0)) == makeTile(49, 31));
  CHECK(tileForPosition(makePosition(-9500.0, 70.0, 58.0))
        == tileForPosition(-9500.0, 70.0));
}

TEST_CASE("a larger world coordinate gives a smaller tile index", "[tile][inversion]")
{
  // This is the trap the whole module exists to avoid. floor(32 + v / TILE_SIZE) -- the sign
  // flipped -- also yields indices inside 0..63 for every legal coordinate, so the mistake
  // produces plausible tiles rather than an obvious failure. For x = -9500 the flipped formula
  // gives 14 instead of 49; for y = 70 it gives 32 instead of 31. Both are asserted against
  // directly.
  TileIndex const elwynn (tileForPosition(-9500.0, 70.0));

  CHECK(elwynn.x == 49);
  CHECK(elwynn.x != 14);
  CHECK(elwynn.y == 31);
  CHECK(elwynn.y != 32);

  CHECK(tileForPosition(-9500.0, 0.0).x > tileForPosition(-8000.0, 0.0).x);
  CHECK(tileForPosition(-8000.0, 0.0).x > tileForPosition(0.0, 0.0).x);
  CHECK(tileForPosition(0.0, 0.0).x > tileForPosition(9000.0, 0.0).x);

  CHECK(tileForPosition(0.0, -9500.0).y > tileForPosition(0.0, -8000.0).y);
  CHECK(tileForPosition(0.0, -8000.0).y > tileForPosition(0.0, 0.0).y);
  CHECK(tileForPosition(0.0, 0.0).y > tileForPosition(0.0, 9000.0).y);

  // Monotone across the whole axis, not just at the three sampled points.
  bool monotone = true;
  int decreases = 0;
  int previous = tileForPosition(-17000.0, 0.0).x;

  for (int step = 1; step <= 340; ++step)
  {
    double const x (-17000.0 + static_cast<double>(step) * 100.0);
    int const current (tileForPosition(x, 0.0).x);

    if (current > previous)
    {
      monotone = false;
    }

    if (current < previous)
    {
      ++decreases;
    }

    previous = current;
  }

  CHECK(monotone);
  CHECK(previous < tileForPosition(-17000.0, 0.0).x);
  CHECK(decreases >= 60);
}

TEST_CASE("the world origin sits at tile (32, 32)", "[tile][origin]")
{
  CHECK(tileForPosition(0.0, 0.0) == makeTile(TILE_ORIGIN, TILE_ORIGIN));
  CHECK(isValidPosition(0.0, 0.0));

  // The origin is the UPPER corner of tile (32, 32), not its centre: floor(32 - 0) == 32, and
  // any positive coordinate has already crossed into the next tile down.
  TileBounds const bounds (boundsForTile(makeTile(TILE_ORIGIN, TILE_ORIGIN)));

  CHECK(bounds.max_x == Approx(0.0).margin(EDGE_TOLERANCE));
  CHECK(bounds.max_y == Approx(0.0).margin(EDGE_TOLERANCE));
  CHECK(bounds.min_x == Approx(-TILE_SIZE).margin(EDGE_TOLERANCE));
  CHECK(bounds.min_y == Approx(-TILE_SIZE).margin(EDGE_TOLERANCE));

  CHECK(tileForPosition(0.001, 0.001) == makeTile(31, 31));
  CHECK(tileForPosition(-0.001, -0.001) == makeTile(32, 32));
}

TEST_CASE("tile bounds round-trip over the whole 64x64 grid", "[tile][bounds]")
{
  int failures = 0;
  TileIndex first_failure (makeTile(-1, -1));

  for (int y = 0; y < TILES_PER_SIDE; ++y)
  {
    for (int x = 0; x < TILES_PER_SIDE; ++x)
    {
      TileIndex const tile (makeTile(x, y));

      if (!tileOwnsItsBounds(tile))
      {
        if (failures == 0)
        {
          first_failure = tile;
        }

        ++failures;
      }
    }
  }

  CAPTURE(failures, first_failure.x, first_failure.y);
  CHECK(failures == 0);

  // Spelled out for one tile as well, so a reader can see what the sweep is checking.
  TileIndex const elwynn (makeTile(49, 31));
  TileBounds const bounds (boundsForTile(elwynn));

  CHECK(tileForPosition(bounds.min_x + INSIDE_EPSILON, bounds.min_y + INSIDE_EPSILON) == elwynn);
  CHECK(tileForPosition(bounds.max_x, bounds.max_y) == elwynn);
  CHECK(bounds.max_x - bounds.min_x == Approx(TILE_SIZE).margin(EDGE_TOLERANCE));
  CHECK(bounds.max_y - bounds.min_y == Approx(TILE_SIZE).margin(EDGE_TOLERANCE));
}

TEST_CASE("a position exactly on a tile boundary belongs to exactly one tile", "[tile][bounds]")
{
  TileIndex const tile (makeTile(49, 31));
  TileBounds const bounds (boundsForTile(tile));

  double const centre_x ((bounds.min_x + bounds.max_x) * 0.5);
  double const centre_y ((bounds.min_y + bounds.max_y) * 0.5);

  // The upper edge belongs to this tile and the lower edge to the neighbour. Because the axis
  // runs backwards, that neighbour is the one with the HIGHER index -- which is the detail that
  // makes "max is exclusive" the wrong way to describe these bounds.
  CHECK(tileForPosition(bounds.max_x, centre_y) == tile);
  CHECK(tileForPosition(bounds.min_x, centre_y) == makeTile(50, 31));
  CHECK(tileForPosition(centre_x, bounds.max_y) == tile);
  CHECK(tileForPosition(centre_x, bounds.min_y) == makeTile(49, 32));

  // boundsForTile and tileForPosition therefore agree about the owner: the shared edge value is
  // the neighbour's max, and the neighbour is who tileForPosition names.
  CHECK(boundsForTile(makeTile(50, 31)).max_x == Approx(bounds.min_x).margin(EDGE_TOLERANCE));
  CHECK(boundsForTile(makeTile(49, 32)).max_y == Approx(bounds.min_y).margin(EDGE_TOLERANCE));

  // No gaps and no overlaps anywhere on the axis: tile i's lower edge is tile i+1's upper edge,
  // so every world coordinate falls in exactly one interval.
  int abutment_failures = 0;

  for (int index = 0; index < TILES_PER_SIDE - 1; ++index)
  {
    double const lower (boundsForTile(makeTile(index, 0)).min_x);
    double const neighbour_upper (boundsForTile(makeTile(index + 1, 0)).max_x);

    if (std::fabs(lower - neighbour_upper) > EDGE_TOLERANCE)
    {
      ++abutment_failures;
    }

    if (tileForPosition(lower, 0.0).x != index + 1)
    {
      ++abutment_failures;
    }
  }

  CHECK(abutment_failures == 0);
}

TEST_CASE("tile validity covers the 64x64 grid and nothing else", "[tile][validity]")
{
  CHECK(isValidTile(makeTile(0, 0)));
  CHECK(isValidTile(makeTile(63, 63)));
  CHECK(isValidTile(makeTile(49, 31)));
  CHECK(isValidTile(makeTile(32, 32)));

  CHECK_FALSE(isValidTile(makeTile(-1, 0)));
  CHECK_FALSE(isValidTile(makeTile(0, -1)));
  CHECK_FALSE(isValidTile(makeTile(TILES_PER_SIDE, 0)));
  CHECK_FALSE(isValidTile(makeTile(0, TILES_PER_SIDE)));
  CHECK_FALSE(isValidTile(makeTile(64, 64)));
}

TEST_CASE("position validity follows the grid at and beyond the extent", "[tile][validity]")
{
  CHECK(isValidPosition(0.0, 0.0));
  CHECK(isValidPosition(-9500.0, 70.0));

  // The positive extent is the upper edge of tile 0 and is owned by it.
  CHECK(tileForPosition(MAP_HALF_EXTENT, MAP_HALF_EXTENT) == makeTile(0, 0));
  CHECK(isValidPosition(MAP_HALF_EXTENT, MAP_HALF_EXTENT));

  // The negative extent is NOT symmetric. The grid covers (-MAP_HALF_EXTENT, +MAP_HALF_EXTENT]
  // on each axis, so exactly -MAP_HALF_EXTENT floors to index 64, one past the last tile. A
  // position whose tile cannot be loaded is not a valid position.
  CHECK(tileForPosition(-MAP_HALF_EXTENT, 0.0) == makeTile(TILES_PER_SIDE, 32));
  CHECK_FALSE(isValidPosition(-MAP_HALF_EXTENT, 0.0));
  CHECK_FALSE(isValidPosition(0.0, -MAP_HALF_EXTENT));

  // One yard inside it is the last tile and is fine.
  CHECK(tileForPosition(-MAP_HALF_EXTENT + 1.0, -MAP_HALF_EXTENT + 1.0) == makeTile(63, 63));
  CHECK(isValidPosition(-MAP_HALF_EXTENT + 1.0, -MAP_HALF_EXTENT + 1.0));

  // Beyond the extent on either side.
  CHECK_FALSE(isValidPosition(MAP_HALF_EXTENT + 1.0, 0.0));
  CHECK_FALSE(isValidPosition(0.0, MAP_HALF_EXTENT + 1.0));
  CHECK_FALSE(isValidPosition(-MAP_HALF_EXTENT - 1.0, 0.0));
  CHECK_FALSE(isValidPosition(0.0, -MAP_HALF_EXTENT - 1.0));

  // Garbage must answer false rather than convert out of range, which would be undefined.
  double const infinity (std::numeric_limits<double>::infinity());
  double const not_a_number (std::numeric_limits<double>::quiet_NaN());

  CHECK_FALSE(isValidPosition(1e300, 0.0));
  CHECK_FALSE(isValidPosition(-1e300, 0.0));
  CHECK_FALSE(isValidPosition(infinity, 0.0));
  CHECK_FALSE(isValidPosition(-infinity, 0.0));
  CHECK_FALSE(isValidPosition(not_a_number, 0.0));
  CHECK_FALSE(isValidPosition(0.0, not_a_number));

  // Stated independently of the implementation: the grid covers the half-open interval
  // (-MAP_HALF_EXTENT, +MAP_HALF_EXTENT] on each axis, and isValidPosition is exactly that.
  int disagreements = 0;
  double first_disagreement = 0.0;

  for (int step = -180; step <= 180; ++step)
  {
    double const v (static_cast<double>(step) * 100.0);
    bool const expected (v > -MAP_HALF_EXTENT && v <= MAP_HALF_EXTENT);

    if (isValidPosition(v, v) != expected)
    {
      ++disagreements;
      first_disagreement = v;
    }
  }

  CAPTURE(disagreements, first_disagreement);
  CHECK(disagreements == 0);
}

TEST_CASE("quaternionForOrientation is sin and cos of the half angle", "[tile][quaternion]")
{
  double const root_half (std::sqrt(0.5));

  Quaternion const facing_north (quaternionForOrientation(0.0));
  CHECK(facing_north.r0 == Approx(0.0).margin(COMPONENT_TOLERANCE));
  CHECK(facing_north.r1 == Approx(0.0).margin(COMPONENT_TOLERANCE));
  CHECK(facing_north.r2 == Approx(0.0).margin(COMPONENT_TOLERANCE));
  CHECK(facing_north.r3 == Approx(1.0).margin(COMPONENT_TOLERANCE));

  Quaternion const quarter (quaternionForOrientation(PI / 2.0));
  CHECK(quarter.r2 == Approx(root_half).margin(COMPONENT_TOLERANCE));
  CHECK(quarter.r3 == Approx(root_half).margin(COMPONENT_TOLERANCE));

  Quaternion const half (quaternionForOrientation(PI));
  CHECK(half.r2 == Approx(1.0).margin(COMPONENT_TOLERANCE));
  CHECK(half.r3 == Approx(0.0).margin(COMPONENT_TOLERANCE));

  // Three quarters: r2 stays positive and r3 goes negative. An implementation that skipped the
  // normalisation and took the half angle of a raw negative input would produce the negated
  // pair here, which is the same rotation but different stored numbers.
  Quaternion const three_quarters (quaternionForOrientation(3.0 * PI / 2.0));
  CHECK(three_quarters.r2 == Approx(root_half).margin(COMPONENT_TOLERANCE));
  CHECK(three_quarters.r3 == Approx(-root_half).margin(COMPONENT_TOLERANCE));

  Quaternion const from_negative (quaternionForOrientation(-PI / 2.0));
  CHECK(from_negative.r2 == Approx(three_quarters.r2).margin(COMPONENT_TOLERANCE));
  CHECK(from_negative.r3 == Approx(three_quarters.r3).margin(COMPONENT_TOLERANCE));

  Quaternion const wrapped (quaternionForOrientation(3.0 * PI / 2.0 + TWO_PI));
  CHECK(wrapped.r2 == Approx(three_quarters.r2).margin(COMPONENT_TOLERANCE));
  CHECK(wrapped.r3 == Approx(three_quarters.r3).margin(COMPONENT_TOLERANCE));

  // rotation0 and rotation1 are always exactly zero for a yaw, and the result is always a unit
  // quaternion -- the core rejects rotations that are not.
  double const angles[] = {0.0, 0.4, PI / 2.0, PI, 3.0 * PI / 2.0, 5.9, TWO_PI - 1e-6};

  for (double const angle : angles)
  {
    Quaternion const rotation (quaternionForOrientation(angle));

    CAPTURE(angle, rotation.r2, rotation.r3);
    CHECK(rotation.r0 == 0.0);
    CHECK(rotation.r1 == 0.0);
    CHECK(quaternionNorm(rotation) == Approx(1.0).margin(COMPONENT_TOLERANCE));
  }
}

TEST_CASE("orientation survives a quaternion round trip", "[tile][quaternion]")
{
  double const angles[] =
    { 0.0
    , PI / 2.0
    , PI
    , 3.0 * PI / 2.0
    , 1.234
    , 5.9
    , TWO_PI - 1e-6
    , TWO_PI - 1e-9
    , TWO_PI - 1e-12
    };

  for (double const angle : angles)
  {
    double const recovered (orientationForQuaternion(quaternionForOrientation(angle)));

    CAPTURE(angle, recovered);
    CHECK(recovered == Approx(angle).margin(ANGLE_TOLERANCE));
    CHECK(recovered >= 0.0);
    CHECK(recovered < TWO_PI);
  }

  // A full turn is the same facing as none, so it round-trips to 0 rather than to itself. That
  // is the contract -- the result is in [0, 2*pi) -- not a rounding artefact.
  CHECK(orientationForQuaternion(quaternionForOrientation(TWO_PI))
        == Approx(0.0).margin(ANGLE_TOLERANCE));

  // Negative inputs come back as their positive equivalent.
  CHECK(orientationForQuaternion(quaternionForOrientation(-PI / 2.0))
        == Approx(3.0 * PI / 2.0).margin(ANGLE_TOLERANCE));
  CHECK(orientationForQuaternion(quaternionForOrientation(-0.25))
        == Approx(TWO_PI - 0.25).margin(ANGLE_TOLERANCE));
}

TEST_CASE("a negated quaternion decodes to the same orientation", "[tile][quaternion]")
{
  // q and -q are the same rotation. TrinityCore rows written by different tools carry both
  // signs, so decoding must not care.
  Quaternion const rotation (quaternionForOrientation(2.0));

  Quaternion negated;
  negated.r0 = -rotation.r0;
  negated.r1 = -rotation.r1;
  negated.r2 = -rotation.r2;
  negated.r3 = -rotation.r3;

  CHECK(orientationForQuaternion(negated) == Approx(2.0).margin(ANGLE_TOLERANCE));
  CHECK(orientationForQuaternion(rotation) == Approx(2.0).margin(ANGLE_TOLERANCE));

  // The default-constructed quaternion is the identity and means "facing 0".
  Quaternion const identity;
  CHECK(orientationForQuaternion(identity) == Approx(0.0).margin(ANGLE_TOLERANCE));
}

TEST_CASE("normaliseOrientation folds any angle into [0, 2pi)", "[tile][orientation]")
{
  CHECK(normaliseOrientation(0.0) == Approx(0.0).margin(ANGLE_TOLERANCE));
  CHECK(normaliseOrientation(1.0) == Approx(1.0).margin(ANGLE_TOLERANCE));

  // Negative values.
  CHECK(normaliseOrientation(-1.0) == Approx(TWO_PI - 1.0).margin(ANGLE_TOLERANCE));
  CHECK(normaliseOrientation(-PI / 2.0) == Approx(3.0 * PI / 2.0).margin(ANGLE_TOLERANCE));
  CHECK(normaliseOrientation(-3.0 * TWO_PI - 1.0) == Approx(TWO_PI - 1.0).margin(ANGLE_TOLERANCE));

  // Values past a full turn.
  CHECK(normaliseOrientation(TWO_PI + 1.0) == Approx(1.0).margin(ANGLE_TOLERANCE));
  CHECK(normaliseOrientation(10.0 * TWO_PI + PI) == Approx(PI).margin(ANGLE_TOLERANCE));
  CHECK(normaliseOrientation(1000.0 * TWO_PI + 0.5) == Approx(0.5).margin(1e-9));

  // The range is half-open at both ends for every input, not just the sampled ones.
  bool in_range = true;
  bool idempotent = true;

  for (int step = -400; step <= 400; ++step)
  {
    double const angle (static_cast<double>(step) * 0.37);
    double const normalised (normaliseOrientation(angle));

    if (!(normalised >= 0.0 && normalised < TWO_PI))
    {
      in_range = false;
    }

    if (normaliseOrientation(normalised) != normalised)
    {
      idempotent = false;
    }
  }

  CHECK(in_range);
  CHECK(idempotent);
}

TEST_CASE("normaliseOrientation survives the rounding traps at both ends", "[tile][orientation]")
{
  // Exactly a full turn must give 0, not 2*pi.
  CHECK(normaliseOrientation(TWO_PI) == Approx(0.0).margin(ANGLE_TOLERANCE));
  CHECK(normaliseOrientation(TWO_PI) < TWO_PI);
  CHECK(normaliseOrientation(-TWO_PI) == Approx(0.0).margin(ANGLE_TOLERANCE));
  CHECK(normaliseOrientation(-TWO_PI) < TWO_PI);

  // fmod keeps the sign of its argument, so a negative whole turn arrives as -0.0. It compares
  // equal to zero but renders as "-0" in emitted SQL, which reads as a change against a stored
  // 0 when nothing moved.
  CHECK_FALSE(std::signbit(normaliseOrientation(-TWO_PI)));
  CHECK_FALSE(std::signbit(normaliseOrientation(-4.0 * TWO_PI)));

  // The real trap: a tiny negative angle is below half an ulp of 2*pi, so the naive
  // "fmod then add 2*pi" returns exactly 2*pi -- out of range, and a full turn out on the next
  // round trip.
  double const tiny_negatives[] = {-1e-18, -1e-16, -1e-20, -5e-16};

  for (double const angle : tiny_negatives)
  {
    double const normalised (normaliseOrientation(angle));

    CAPTURE(angle, normalised);
    CHECK(normalised >= 0.0);
    CHECK(normalised < TWO_PI);
  }

  CHECK(normaliseOrientation(-1e-18) == Approx(0.0).margin(ANGLE_TOLERANCE));

  // Just below a full turn must stay just below it, not collapse to zero.
  double const almost_full (std::nextafter(TWO_PI, 0.0));

  CHECK(normaliseOrientation(almost_full) < TWO_PI);
  CHECK(normaliseOrientation(almost_full) == Approx(TWO_PI).margin(1e-12));
  CHECK(normaliseOrientation(TWO_PI - 1e-9) == Approx(TWO_PI - 1e-9).margin(ANGLE_TOLERANCE));

  // A NaN is not an angle. It is folded to zero so the postcondition holds unconditionally and
  // so it cannot travel on into a quaternion and out into an INSERT.
  double const not_a_number (std::numeric_limits<double>::quiet_NaN());

  CHECK_FALSE(std::isnan(normaliseOrientation(not_a_number)));
  CHECK(normaliseOrientation(not_a_number) == Approx(0.0).margin(ANGLE_TOLERANCE));
  CHECK_FALSE(std::isnan(quaternionForOrientation(not_a_number).r2));
  CHECK_FALSE(std::isnan(quaternionForOrientation(not_a_number).r3));
}
