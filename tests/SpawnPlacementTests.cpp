// This file is part of Noggit3, licensed under GNU General Public License (version 3).

// Tests for the frame conversion between TrinityCore world coordinates and Noggit's internal
// frame. This is the one place in the project where being wrong produces no error -- just spawns
// in the wrong place -- so the numbers are pinned against values derived independently from
// src/math/coordinates.hpp rather than against the implementation.

#include <catch2/catch_test_macros.hpp>

#include <noggit/database/SpawnPlacement.hpp>
#include <noggit/database/SpawnTypes.hpp>
#include <noggit/database/TileCoordinates.hpp>

#include <cmath>
#include <limits>
#include <string>

using namespace Noggit::Database;

namespace
{
  // The verified real spawn: Elwynn Forest, map 0, Database tile (49, 31), which is Noggit
  // TileIndex{31, 49} and the file Azeroth_31_49.adt.
  constexpr double ELWYNN_X = -9500.0;
  constexpr double ELWYNN_Y = 70.0;
  constexpr double ELWYNN_Z = 58.0;

  bool near(double a, double b, double tolerance = 1e-6)
  {
    return std::fabs(a - b) < tolerance;
  }

  CreatureSpawn creatureAt(double x, double y, double z, double orientation)
  {
    CreatureSpawn spawn;
    spawn.guid = 9000001;
    spawn.id = 990001;
    spawn.position = WorldPosition {x, y, z};
    spawn.orientation = orientation;
    return spawn;
  }
}

TEST_CASE("ZEROPOINT is computed exactly as Noggit computes it", "[placement][frame]")
{
  // Noggit does 32.0f * 533.33333f in FLOAT. Doing it in double gives 17066.66656, which differs
  // by 5.4e-4. Matching the float arithmetic is what lets the glue seam static_assert equality.
  CHECK(near(static_cast<double>(SpawnPlacement::ZEROPOINT_F), 17066.666015625, 1e-9));

  // And it is NOT the double-computed value, which is the mistake this guards against.
  CHECK_FALSE(near(static_cast<double>(SpawnPlacement::ZEROPOINT_F), 32.0 * 533.33333, 1e-9));
}

TEST_CASE("a world position converts to Noggit's frame the way math::to_client does"
         , "[placement][frame]")
{
  // Computed by hand from src/math/coordinates.hpp:25-28, not from the implementation:
  //   noggit.x = ZEROPOINT - world_y = 17066.666015625 - 70    = 16996.666015625
  //   noggit.y =             world_z =                           58
  //   noggit.z = ZEROPOINT - world_x = 17066.666015625 + 9500  = 26566.666015625
  NoggitPlacement const p
    (SpawnPlacement::positionFor(WorldPosition {ELWYNN_X, ELWYNN_Y, ELWYNN_Z}));

  CHECK(near(p.x, 16996.666015625, 1e-6));
  CHECK(near(p.y, 58.0, 1e-9));
  CHECK(near(p.z, 26566.666015625, 1e-6));

  // The axis feeds matter more than the magnitudes: x must come from world Y and z from world X.
  // Swapping them is the transposition bug, and both results stay plausibly in range.
  CHECK_FALSE(near(p.x, p.z, 1.0));
}

TEST_CASE("the frame conversion round-trips", "[placement][frame][roundtrip]")
{
  for (double const x : {-9500.0, 0.0, 9500.0, -17000.0, 17000.0})
  {
    for (double const y : {-9500.0, 0.0, 70.0, 16000.0})
    {
      WorldPosition const original {x, y, 58.25};
      WorldPosition const back
        (SpawnPlacement::serverPositionFor(SpawnPlacement::positionFor(original)));

      CAPTURE(x, y);
      CHECK(near(back.x, original.x, 1e-6));
      CHECK(near(back.y, original.y, 1e-6));
      CHECK(near(back.z, original.z, 1e-9));
    }
  }
}

TEST_CASE("the server origin lands on ZEROPOINT in both horizontal axes", "[placement][frame]")
{
  NoggitPlacement const p (SpawnPlacement::positionFor(WorldPosition {0.0, 0.0, 0.0}));

  CHECK(near(p.x, static_cast<double>(SpawnPlacement::ZEROPOINT_F), 1e-9));
  CHECK(near(p.z, static_cast<double>(SpawnPlacement::ZEROPOINT_F), 1e-9));
  CHECK(near(p.y, 0.0, 1e-9));
}

TEST_CASE("height passes through untouched in both directions", "[placement][frame]")
{
  // Noggit's y IS the server's z, with no offset. A ZEROPOINT accidentally applied to height
  // would put every spawn 17 km in the air, which is at least obvious -- but an offset of zero
  // is worth pinning so a future refactor cannot introduce one quietly.
  for (double const height : {-500.0, 0.0, 58.271, 1000.0})
  {
    CAPTURE(height);
    NoggitPlacement const p (SpawnPlacement::positionFor(WorldPosition {0.0, 0.0, height}));
    CHECK(near(p.y, height, 1e-9));
    CHECK(near(SpawnPlacement::serverPositionFor(p).z, height, 1e-9));
  }
}

TEST_CASE("the ADT index for a world position is in filename order", "[placement][frame][tile]")
{
  // Database tile (49, 31) is Noggit TileIndex{31, 49} and file Azeroth_31_49.adt. Reporting
  // {49, 31} here would name a tile 9.6 km away on both axes while still being a legal index.
  AdtFileIndex const adt
    (SpawnPlacement::adtIndexFor(WorldPosition {ELWYNN_X, ELWYNN_Y, ELWYNN_Z}));

  CHECK(adt.x == 31);
  CHECK(adt.z == 49);

  // And it agrees with the database-side tile, transposed.
  TileIndex const db (TileCoordinates::tileForPosition(ELWYNN_X, ELWYNN_Y));
  CHECK(db.x == 49);
  CHECK(db.y == 31);
  CHECK(adt.x == db.y);
  CHECK(adt.z == db.x);
}

TEST_CASE("the Noggit position of a spawn falls inside its own ADT tile", "[placement][frame][tile]")
{
  // The strongest available check that the two frames agree: convert to Noggit coordinates, then
  // derive the tile the way Noggit does -- floor(pos / TILESIZE) on x and z, per
  // src/noggit/TileIndex.cpp:11-12 -- and require it to match the ADT index.
  //
  // The arithmetic below is deliberately in FLOAT, because Noggit's is. Doing it in double is not
  // a harmless widening: Noggit's ZEROPOINT is the float 17066.666015625 while this layer's
  // 32 * TILE_SIZE in double is 17066.66656, and the float value divided by a double TILE_SIZE
  // gives 31.999998 rather than 32. That produces a genuine off-by-one tile at exactly
  // world_y == 0 or world_x == 0 -- values that are common in real data. Mixing the two
  // precisions anywhere near a tile boundary is the hazard; matching Noggit's is the answer.
  constexpr float TILESIZE_F = 533.33333f;

  for (double const x : {-9500.0, -9528.7, -9429.9, 0.0, 5000.0})
  {
    for (double const y : {70.0, 92.5, 0.0, -3000.0})
    {
      WorldPosition const world {x, y, 58.0};
      NoggitPlacement const p (SpawnPlacement::positionFor(world));
      AdtFileIndex const adt (SpawnPlacement::adtIndexFor(world));

      CAPTURE(x, y);
      CHECK(static_cast<int>(std::floor(static_cast<float>(p.x) / TILESIZE_F)) == adt.x);
      CHECK(static_cast<int>(std::floor(static_cast<float>(p.z) / TILESIZE_F)) == adt.z);
    }
  }
}

TEST_CASE("yaw is derived from the orientation and normalised", "[placement][frame][orientation]")
{
  // The applied yaw is dir.y - 90 (SceneObject.cpp:74-78), and the derivation in the header puts
  // dir.y at degrees(o) + 180. What is asserted here is the arithmetic, not the visual result:
  // the YAW_OFFSET_DEGREES constant encodes an unverified assumption about M2 local forward, and
  // the header says so.
  // 0 radians -> 0 + 180 = 180, which normalises to -180: the range is [-180, 180), so the upper
  // bound folds to the lower one. Same angle, and it is the form SceneObject::normalizeDirection
  // produces too.
  CHECK(near(SpawnPlacement::placementFor(creatureAt(0, 0, 0, 0.0)).dir_y_degrees, -180.0, 1e-6));

  // pi radians -> 180 + 180 = 360 -> normalised to 0.
  CHECK(near(SpawnPlacement::placementFor(creatureAt(0, 0, 0, 3.14159265358979)).dir_y_degrees
            , 0.0, 1e-6));

  // 3*pi/2 (the reference changeset's 4.7124) -> 270 + 180 = 450 -> 90.
  CHECK(near(SpawnPlacement::placementFor(creatureAt(0, 0, 0, 4.71238898038469)).dir_y_degrees
            , 90.0, 1e-6));

  // Every orientation in range must produce a normalised angle, and never "-0".
  for (int step = 0; step <= 24; ++step)
  {
    double const o (step * (6.283185307179586 / 24.0));
    double const yaw (SpawnPlacement::placementFor(creatureAt(0, 0, 0, o)).dir_y_degrees);

    CAPTURE(o, yaw);
    CHECK(yaw >= -180.0);
    CHECK(yaw < 180.0);
    // Parenthesised: Catch2's expression decomposer rejects a bare && inside an assertion, the
    // same way it rejects ||.
    CHECK_FALSE((std::signbit(yaw) && yaw == 0.0));
  }

  // Pitch and roll are untouched: a spawn stands upright.
  CHECK(near(SpawnPlacement::placementFor(creatureAt(0, 0, 0, 1.0)).dir_x_degrees, 0.0, 1e-12));
  CHECK(near(SpawnPlacement::placementFor(creatureAt(0, 0, 0, 1.0)).dir_z_degrees, 0.0, 1e-12));
}

TEST_CASE("normaliseDegrees folds any angle into [-180, 180)", "[placement][orientation]")
{
  CHECK(near(SpawnPlacement::normaliseDegrees(0.0), 0.0, 1e-12));
  CHECK(near(SpawnPlacement::normaliseDegrees(90.0), 90.0, 1e-12));
  CHECK(near(SpawnPlacement::normaliseDegrees(180.0), -180.0, 1e-12));
  CHECK(near(SpawnPlacement::normaliseDegrees(360.0), 0.0, 1e-12));
  CHECK(near(SpawnPlacement::normaliseDegrees(450.0), 90.0, 1e-12));
  CHECK(near(SpawnPlacement::normaliseDegrees(-90.0), -90.0, 1e-12));
  CHECK(near(SpawnPlacement::normaliseDegrees(-360.0), 0.0, 1e-12));

  // -0.0 must not survive: it compares equal to zero but renders as "-0".
  CHECK_FALSE(std::signbit(SpawnPlacement::normaliseDegrees(-360.0)));
}

TEST_CASE("an unusable model scale becomes 1 rather than collapsing the model"
         , "[placement][robustness]")
{
  // A scale of 0 renders as nothing, which is indistinguishable from a failed model load and
  // sends someone hunting the wrong bug entirely.
  CHECK(near(SpawnPlacement::placementFor(creatureAt(0, 0, 0, 0.0), 0.0).scale, 1.0, 1e-12));
  CHECK(near(SpawnPlacement::placementFor(creatureAt(0, 0, 0, 0.0), -2.0).scale, 1.0, 1e-12));
  CHECK(near(SpawnPlacement::placementFor(creatureAt(0, 0, 0, 0.0)
            , std::numeric_limits<double>::quiet_NaN()).scale, 1.0, 1e-12));

  // A legitimate scale is passed through.
  CHECK(near(SpawnPlacement::placementFor(creatureAt(0, 0, 0, 0.0), 1.75).scale, 1.75, 1e-12));
}

TEST_CASE("a gameobject placement uses orientation, not the quaternion"
         , "[placement][frame][gameobject]")
{
  GameObjectSpawn spawn;
  spawn.guid = 9000101;
  spawn.id = 990101;
  spawn.position = WorldPosition {ELWYNN_X, ELWYNN_Y, ELWYNN_Z};
  spawn.orientation = 4.71238898038469;

  // A quaternion that disagrees with the orientation: identity, i.e. facing 0.
  spawn.rotation = TileCoordinates::Quaternion {0.0, 0.0, 0.0, 1.0};

  NoggitPlacement const p (SpawnPlacement::placementFor(spawn));

  // orientation wins: 270 + 180 = 450 -> 90. Following the identity quaternion would give 180.
  CHECK(near(p.dir_y_degrees, 90.0, 1e-6));
  CHECK(near(p.x, 16996.666015625, 1e-6));
  CHECK(near(p.z, 26566.666015625, 1e-6));
}
