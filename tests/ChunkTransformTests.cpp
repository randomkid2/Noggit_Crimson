// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <FixtureLoader.hpp>
#include <noggit/database/ChangesetBuilder.hpp>
#include <noggit/database/ChunkTransform.hpp>
#include <noggit/database/SchemaModel.hpp>
#include <noggit/database/SpawnTypes.hpp>
#include <noggit/database/TileCoordinates.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

using namespace Noggit::Database;
using Catch::Approx;
using Noggit::Tests::fixturePath;
using Noggit::Tests::loadSchemaFixture;

namespace
{
  constexpr char const* REAL_FIXTURE = "schema-tdb335-25101.tsv";

  constexpr double PI = 3.14159265358979323846;
  constexpr double TWO_PI = 6.283185307179586476925286766559;
  constexpr double HALF_PI = 1.57079632679489661923;

  // A nanometre. Double resolution at world magnitudes is around 2e-12 yards, so this is loose
  // enough to absorb a few thousand roundings and still four orders of magnitude tighter than the
  // 2.7e-4 yard step of the FLOAT column these values are eventually narrowed into. A comparison
  // that this tolerance passes and the column cannot even represent is not measuring anything the
  // world can see.
  constexpr double POSITION_TOLERANCE = 1e-9;

  // Angles carry a sin/cos/atan2 round trip, which costs about 1e-15 radians.
  constexpr double ANGLE_TOLERANCE = 1e-9;

  // Quaternion components live in [-1, 1], so an absolute tolerance is the right shape.
  constexpr double COMPONENT_TOLERANCE = 1e-12;

  // The fixture tile: Elwynn Forest, (49, 31) in world-axis order, which really does contain
  // these coordinates. x runs (-9599.99994, -9066.66661] and y runs (0, 533.33333].
  constexpr int FIXTURE_TILE_X = 49;
  constexpr int FIXTURE_TILE_Y = 31;

  TileIndex tile(int x, int y)
  {
    TileIndex out;
    out.x = x;
    out.y = y;

    return out;
  }

  WorldPosition at(double x, double y, double z)
  {
    WorldPosition out;
    out.x = x;
    out.y = y;
    out.z = z;

    return out;
  }

  WorldDelta delta(double dx, double dy, double dz)
  {
    WorldDelta out;
    out.dx = dx;
    out.dy = dy;
    out.dz = dz;

    return out;
  }

  CreatureSpawn creature(std::uint32_t guid, WorldPosition const& position, double orientation)
  {
    CreatureSpawn spawn;
    spawn.guid = guid;
    spawn.id = 990001;
    spawn.map = 0;
    spawn.position = position;
    spawn.orientation = orientation;
    spawn.cur_health = 1;
    spawn.movement_type = MovementType::IDLE;

    return spawn;
  }

  GameObjectSpawn gameObject
    (std::uint32_t guid, WorldPosition const& position, double orientation)
  {
    GameObjectSpawn spawn;
    spawn.guid = guid;
    spawn.id = 180000;
    spawn.map = 0;
    spawn.position = position;
    spawn.orientation = orientation;

    return spawn;
  }

  // The M4 fixture set from docs/milestones.md: three creatures and one gameobject in tile
  // 49_31, plus the five-node path bound to the third creature. Using the shape the milestone
  // names keeps the numeric claim here and the claim against the seeded database the same claim.
  TileSpawns fixtureTile()
  {
    TileSpawns source;
    source.tile = tile(FIXTURE_TILE_X, FIXTURE_TILE_Y);
    source.map = 0;

    source.creatures.push_back(creature(9000001, at(-9512.345, 83.117, 58.271), 4.7124));

    CreatureSpawn wanderer (creature(9000002, at(-9530.2, 96.3, 60.0), 0.0));
    wanderer.movement_type = MovementType::RANDOM;
    wanderer.wander_distance = 5.0;
    source.creatures.push_back(wanderer);

    CreatureSpawn patroller (creature(9000003, at(-9429.9, 56.1, 55.5), 3.0));
    patroller.movement_type = MovementType::WAYPOINT;
    patroller.has_addon = true;
    patroller.path_id = 90000030;
    source.creatures.push_back(patroller);

    source.gameobjects.push_back(gameObject(8000001, at(-9500.0, 70.0, 58.0), PI));

    WaypointPath path;
    path.id = 90000030;

    for (int i = 0; i < 5; ++i)
    {
      WaypointNode node;
      node.point = static_cast<std::uint32_t>(i) + 1;
      node.position = at(-9512.345 + static_cast<double>(i), 83.117, 58.271);
      path.nodes.push_back(node);
    }

    source.paths.push_back(path);

    return source;
  }

  // Re-forms a plan as a source, which the module deliberately does NOT provide: chaining plans
  // is the accumulation it exists to prevent. It lives here so the tests can demonstrate the
  // accumulation the module avoids by re-planning from the original instead.
  TileSpawns accumulate(TransformPlan const& plan)
  {
    TileSpawns out;
    out.tile = plan.source_tile;
    out.map = plan.map;
    out.creatures = plan.creatures;
    out.gameobjects = plan.gameobjects;
    out.paths = plan.paths;

    return out;
  }

  void checkPosition
    ( std::string const& what
    , WorldPosition const& actual
    , WorldPosition const& expected
    , double margin
    )
  {
    INFO("position of " << what);
    CHECK(actual.x == Approx(expected.x).margin(margin));
    CHECK(actual.y == Approx(expected.y).margin(margin));
    CHECK(actual.z == Approx(expected.z).margin(margin));
  }

  double quaternionNorm(TileCoordinates::Quaternion const& rotation)
  {
    return std::sqrt
      ( rotation.r0 * rotation.r0 + rotation.r1 * rotation.r1
      + rotation.r2 * rotation.r2 + rotation.r3 * rotation.r3
      );
  }

  std::size_t countOf(std::vector<ValidationIssue> const& issues, ValidationIssue::Severity s)
  {
    std::size_t count = 0;

    for (ValidationIssue const& issue : issues)
    {
      if (issue.severity == s)
      {
        ++count;
      }
    }

    return count;
  }

  std::size_t errorCount(std::vector<ValidationIssue> const& issues)
  {
    return countOf(issues, ValidationIssue::Severity::BLOCKING);
  }

  std::size_t warningCount(std::vector<ValidationIssue> const& issues)
  {
    return countOf(issues, ValidationIssue::Severity::WARNING);
  }

  bool anyIssueMentions(std::vector<ValidationIssue> const& issues, std::string const& needle)
  {
    for (ValidationIssue const& issue : issues)
    {
      if (issue.message.find(needle) != std::string::npos)
      {
        return true;
      }
    }

    return false;
  }

  bool contains(std::vector<TileIndex> const& tiles, TileIndex const& wanted)
  {
    for (TileIndex const& candidate : tiles)
    {
      if (candidate == wanted)
      {
        return true;
      }
    }

    return false;
  }
}

// --- translation ---------------------------------------------------------------------------

TEST_CASE("a translation moves every dependent by the same delta", "[chunk][tile]")
{
  TileSpawns const source (fixtureTile());
  WorldDelta const d (delta(120.5, -37.25, 8.0));

  TransformPlan const plan (planTranslation(source, d));

  REQUIRE(plan.creatures.size() == source.creatures.size());
  REQUIRE(plan.gameobjects.size() == source.gameobjects.size());
  REQUIRE(plan.paths.size() == source.paths.size());
  REQUIRE(plan.paths[0].nodes.size() == 5);

  // A moved chunk with one unmoved spawn is the failure this module exists to prevent, so every
  // kind of dependent is checked, not a representative one.
  for (std::size_t i = 0; i < source.creatures.size(); ++i)
  {
    CreatureSpawn const& before (source.creatures[i]);
    CreatureSpawn const& after (plan.creatures[i]);

    checkPosition
      ( "creature " + std::to_string(before.guid)
      , after.position
      , at(before.position.x + d.dx, before.position.y + d.dy, before.position.z + d.dz)
      , POSITION_TOLERANCE
      );

    // A translation does not turn anything.
    CHECK(after.orientation == before.orientation);

    // Identity is preserved: a moved spawn is the same row, not a new one.
    CHECK(after.guid == before.guid);
    CHECK(after.id == before.id);
    CHECK(after.movement_type == before.movement_type);
    CHECK(after.path_id == before.path_id);
  }

  checkPosition
    ( "gameobject 8000001"
    , plan.gameobjects[0].position
    , at(-9500.0 + d.dx, 70.0 + d.dy, 58.0 + d.dz)
    , POSITION_TOLERANCE
    );

  CHECK(plan.gameobjects[0].orientation == source.gameobjects[0].orientation);

  CHECK(plan.paths[0].id == source.paths[0].id);

  for (std::size_t i = 0; i < source.paths[0].nodes.size(); ++i)
  {
    WaypointNode const& before (source.paths[0].nodes[i]);
    WaypointNode const& after (plan.paths[0].nodes[i]);

    checkPosition
      ( "waypoint point " + std::to_string(before.point)
      , after.position
      , at(before.position.x + d.dx, before.position.y + d.dy, before.position.z + d.dz)
      , POSITION_TOLERANCE
      );

    // The core walks nodes by point and a renumbered path is a rerouted path.
    CHECK(after.point == before.point);
    CHECK(after.has_orientation == before.has_orientation);
  }

  CHECK(plan.source_tile == tile(FIXTURE_TILE_X, FIXTURE_TILE_Y));
  CHECK(plan.map == 0);
  CHECK_FALSE(plan.empty());
}

TEST_CASE("a translation leaves a gameobject tilt untouched", "[chunk][validation]")
{
  // ChangesetBuilder emits an authored quaternion verbatim, so a translation that recomputed it
  // from the orientation column would flatten a tilted object without saying anything.
  TileSpawns source;
  source.tile = tile(FIXTURE_TILE_X, FIXTURE_TILE_Y);

  GameObjectSpawn tilted (gameObject(8000002, at(-9500.0, 70.0, 58.0), 0.0));
  tilted.rotation.r0 = 0.25;
  tilted.rotation.r1 = 0.0;
  tilted.rotation.r2 = 0.0;
  tilted.rotation.r3 = 0.968246;
  source.gameobjects.push_back(tilted);

  TransformPlan const plan (planTranslation(source, delta(10.0, 0.0, 0.0)));

  CHECK(plan.gameobjects[0].rotation.r0 == 0.25);
  CHECK(plan.gameobjects[0].rotation.r1 == 0.0);
  CHECK(plan.gameobjects[0].rotation.r2 == 0.0);
  CHECK(plan.gameobjects[0].rotation.r3 == 0.968246);
  CHECK(plan.issues.empty());
}

TEST_CASE("re-planning from the original does not accumulate drift", "[chunk][safety]")
{
  // The premise, from docs/schema-335.md: double addition is not associative, so a transform fed
  // its own previous output lands somewhere a single transform does not. Asserted rather than
  // assumed, so a platform where it stopped being true says so instead of quietly making the rest
  // of this case vacuous. One tile width is the delta a chunk move actually uses.
  double const step (TILE_SIZE);
  double const start (-9512.345);

  INFO("chained " << ((start + step) + step) << " vs single " << (start + 2.0 * step));
  REQUIRE(((start + step) + step) != (start + 2.0 * step));

  TileSpawns const source (fixtureTile());

  TransformPlan const single (planTranslation(source, delta(2.0 * step, 0.0, 0.0)));

  TransformPlan const first (planTranslation(source, delta(step, 0.0, 0.0)));
  TransformPlan const chained (planTranslation(accumulate(first), delta(step, 0.0, 0.0)));

  // Bit-exact, because the module adds the delta to the value the caller supplied and to nothing
  // else. This is the whole guarantee.
  CHECK(single.creatures[0].position.x == start + 2.0 * step);

  // Accumulating gives a different number. One step of drift is far below anything visible; a
  // session of them is not, which is why the module never takes this path itself.
  CHECK(chained.creatures[0].position.x != single.creatures[0].position.x);
  CHECK(chained.creatures[0].position.x == Approx(single.creatures[0].position.x).margin(1e-9));
}

TEST_CASE("a translation and its inverse return the content to where it started"
         , "[chunk][safety]")
{
  TileSpawns const source (fixtureTile());
  WorldDelta const out (delta(TILE_SIZE, -TILE_SIZE, 17.5));
  WorldDelta const back (delta(-TILE_SIZE, TILE_SIZE, -17.5));

  TransformPlan const there (planTranslation(source, out));
  TransformPlan const round_trip (planTranslation(accumulate(there), back));

  for (std::size_t i = 0; i < source.creatures.size(); ++i)
  {
    checkPosition
      ( "round-tripped creature " + std::to_string(source.creatures[i].guid)
      , round_trip.creatures[i].position
      , source.creatures[i].position
      , POSITION_TOLERANCE
      );
  }

  for (std::size_t i = 0; i < source.paths[0].nodes.size(); ++i)
  {
    checkPosition
      ( "round-tripped waypoint point " + std::to_string(i + 1)
      , round_trip.paths[0].nodes[i].position
      , source.paths[0].nodes[i].position
      , POSITION_TOLERANCE
      );
  }

  // The supported way to undo a move is to re-plan from the same source, and that is exact
  // rather than merely close.
  TransformPlan const undone (planTranslation(source, delta(0.0, 0.0, 0.0)));

  CHECK(undone.creatures[0].position.x == source.creatures[0].position.x);
  CHECK(undone.creatures[0].position.y == source.creatures[0].position.y);
  CHECK(undone.creatures[0].position.z == source.creatures[0].position.z);
}

TEST_CASE("a delta of exactly zero warns that the plan is a no-op", "[chunk][validation]")
{
  TileSpawns const source (fixtureTile());
  TransformPlan const plan (planTranslation(source, delta(0.0, 0.0, 0.0)));

  CHECK(errorCount(plan.issues) == 0);
  CHECK(warningCount(plan.issues) == 1);
  CHECK(anyIssueMentions(plan.issues, "exactly zero"));

  // Content is still carried, so a caller that ignores the warning writes the rows it already
  // has rather than losing them.
  CHECK(plan.creatures.size() == source.creatures.size());
  CHECK(plan.occupied_tiles == std::vector<TileIndex>{tile(FIXTURE_TILE_X, FIXTURE_TILE_Y)});
}

// --- rotation ------------------------------------------------------------------------------

TEST_CASE("a quarter turn about a pivot lands where hand arithmetic says", "[chunk][tile]")
{
  // Pivot (-9500, 70). The creature sits 10 yards along +x from it; a quarter turn towards +y
  // must put it 10 yards along +y, at the same height.
  TileSpawns source;
  source.tile = tile(FIXTURE_TILE_X, FIXTURE_TILE_Y);
  source.creatures.push_back(creature(9000001, at(-9490.0, 70.0, 58.271), 0.0));
  source.gameobjects.push_back(gameObject(8000001, at(-9500.0, 80.0, 61.5), 0.0));

  WorldPosition const pivot (at(-9500.0, 70.0, 0.0));

  TransformPlan const plan (planRotation(source, pivot, HALF_PI));

  checkPosition
    ("creature a quarter turn on", plan.creatures[0].position, at(-9500.0, 80.0, 58.271)
    , POSITION_TOLERANCE);

  // 10 yards along +y goes to 10 yards along -x, which on this axis is a LARGER tile index.
  checkPosition
    ("gameobject a quarter turn on", plan.gameobjects[0].position, at(-9510.0, 70.0, 61.5)
    , POSITION_TOLERANCE);

  // Height is not part of a rotation about the vertical axis, and is bit-exact rather than close.
  CHECK(plan.creatures[0].position.z == 58.271);
  CHECK(plan.gameobjects[0].position.z == 61.5);

  // The facing turns with the content, or a rotated guard ends up looking at a wall.
  CHECK(plan.creatures[0].orientation == Approx(HALF_PI).margin(ANGLE_TOLERANCE));
  CHECK(plan.gameobjects[0].orientation == Approx(HALF_PI).margin(ANGLE_TOLERANCE));

  // The pivot itself does not move.
  TileSpawns pinned;
  pinned.creatures.push_back(creature(9000009, pivot, 0.0));

  checkPosition
    ( "content standing on the pivot"
    , planRotation(pinned, pivot, HALF_PI).creatures[0].position
    , pivot
    , POSITION_TOLERANCE
    );
}

TEST_CASE("an orientation past a full turn is folded back into [0, 2pi)", "[chunk][validation]")
{
  // 4.7124 + pi/2 overshoots 2pi by 1.1e-5. The core stores orientations in [0, 2pi) and a
  // wrapped value round-trips as a different facing, so the fold has to happen here.
  TileSpawns source;
  source.tile = tile(FIXTURE_TILE_X, FIXTURE_TILE_Y);
  source.creatures.push_back(creature(9000001, at(-9512.345, 83.117, 58.271), 4.7124));

  TransformPlan const plan (planRotation(source, at(-9500.0, 70.0, 0.0), HALF_PI));

  double const expected (4.7124 + HALF_PI - TWO_PI);

  REQUIRE(expected > 0.0);
  REQUIRE(expected < 1.0);
  CHECK(plan.creatures[0].orientation == Approx(expected).margin(ANGLE_TOLERANCE));
  CHECK(plan.creatures[0].orientation >= 0.0);
  CHECK(plan.creatures[0].orientation < TWO_PI);
}

TEST_CASE("a rotation of a whole turn is an exact no-op and says so", "[chunk][validation]")
{
  TileSpawns const source (fixtureTile());

  for (double turns : {TWO_PI, 2.0 * TWO_PI, -TWO_PI})
  {
    INFO("rotating by " << turns << " radians");

    TransformPlan const plan (planRotation(source, at(-9500.0, 70.0, 0.0), turns));

    // Bit-exact, not merely close. pivot + (p - pivot) is not guaranteed to reproduce p to the
    // last bit, so an identity that went through the arithmetic would rewrite every coordinate
    // in the tile and read in review as a change nobody made.
    CHECK(plan.creatures[0].position.x == source.creatures[0].position.x);
    CHECK(plan.creatures[0].position.y == source.creatures[0].position.y);
    CHECK(plan.creatures[0].position.z == source.creatures[0].position.z);
    CHECK(plan.creatures[0].orientation == source.creatures[0].orientation);
    CHECK(plan.paths[0].nodes[4].position.x == source.paths[0].nodes[4].position.x);

    // And the gameobject rotation is not recomputed either, so a tilt survives an identity.
    CHECK(plan.gameobjects[0].rotation.r3 == source.gameobjects[0].rotation.r3);

    CHECK(errorCount(plan.issues) == 0);
    CHECK(warningCount(plan.issues) == 1);
    CHECK(anyIssueMentions(plan.issues, "whole number of turns"));
  }
}

TEST_CASE("four quarter turns come back to the start", "[chunk][safety]")
{
  // The numeric counterpart to the exact-identity case above: this one really does go through
  // sin and cos four times, from the previous result each time, and has to survive it.
  TileSpawns const source (fixtureTile());
  WorldPosition const pivot (at(-9500.0, 70.0, 0.0));

  TileSpawns current (source);

  for (int i = 0; i < 4; ++i)
  {
    current = accumulate(planRotation(current, pivot, HALF_PI));
    current.tile = source.tile;
  }

  for (std::size_t i = 0; i < source.creatures.size(); ++i)
  {
    checkPosition
      ( "creature " + std::to_string(source.creatures[i].guid) + " after four quarter turns"
      , current.creatures[i].position
      , source.creatures[i].position
      , POSITION_TOLERANCE
      );

    CHECK(current.creatures[i].orientation
          == Approx(source.creatures[i].orientation).margin(ANGLE_TOLERANCE));
  }

  for (std::size_t i = 0; i < source.paths[0].nodes.size(); ++i)
  {
    checkPosition
      ( "waypoint point " + std::to_string(i + 1) + " after four quarter turns"
      , current.paths[0].nodes[i].position
      , source.paths[0].nodes[i].position
      , POSITION_TOLERANCE
      );
  }
}

TEST_CASE("the rotated quaternion is unit length and agrees with the orientation"
         , "[chunk][validation]")
{
  for (double angle : {0.3, HALF_PI, PI, 4.7124, 6.0, -0.75})
  {
    INFO("rotating by " << angle << " radians");

    TileSpawns source;
    source.tile = tile(FIXTURE_TILE_X, FIXTURE_TILE_Y);
    source.gameobjects.push_back(gameObject(8000001, at(-9500.0, 70.0, 58.0), 1.0));

    TransformPlan const plan (planRotation(source, at(-9500.0, 70.0, 0.0), angle));
    GameObjectSpawn const& moved (plan.gameobjects[0]);

    CHECK(moved.orientation
          == Approx(TileCoordinates::normaliseOrientation(1.0 + angle)).margin(ANGLE_TOLERANCE));

    // A non-unit quaternion renders as a sheared or invisible object rather than erroring.
    CHECK(quaternionNorm(moved.rotation) == Approx(1.0).margin(COMPONENT_TOLERANCE));

    // Yaw only: r0 and r1 stay at zero, exactly.
    CHECK(moved.rotation.r0 == 0.0);
    CHECK(moved.rotation.r1 == 0.0);

    // The core reads the quaternion and the editor reads the column, so a disagreement means the
    // object faces one way in game and another on screen. They are written from one angle here,
    // so they cannot disagree.
    CHECK(TileCoordinates::orientationForQuaternion(moved.rotation)
          == Approx(moved.orientation).margin(ANGLE_TOLERANCE));

    CHECK(errorCount(plan.issues) == 0);
  }
}

TEST_CASE("a rotation takes the quaternion as the authority over the orientation column"
         , "[chunk][validation]")
{
  // The row says orientation 0 but stores a quaternion for pi. The core uses the quaternion, so
  // the object is really facing pi and a rotation that started from the column would swing it
  // half a turn away from where it visibly points.
  TileSpawns source;
  source.tile = tile(FIXTURE_TILE_X, FIXTURE_TILE_Y);

  GameObjectSpawn spawn (gameObject(8000001, at(-9500.0, 70.0, 58.0), 0.0));
  spawn.rotation = TileCoordinates::quaternionForOrientation(PI);
  source.gameobjects.push_back(spawn);

  TransformPlan const plan (planRotation(source, at(-9500.0, 70.0, 0.0), HALF_PI));

  CHECK(plan.gameobjects[0].orientation == Approx(PI + HALF_PI).margin(ANGLE_TOLERANCE));

  // Silently picking one of two disagreeing fields is how a spawn ends up rotated by half a turn
  // with nothing in the changeset to explain it.
  CHECK(errorCount(plan.issues) == 0);
  CHECK(warningCount(plan.issues) == 1);
  CHECK(anyIssueMentions(plan.issues, "orientation column"));
}

TEST_CASE("a rotation reports the tilt it cannot carry", "[chunk][validation]")
{
  // A yaw-only recomputation cannot preserve rotation0/rotation1. Losing a tilt is acceptable;
  // losing it silently is not, because nothing downstream can tell it was ever there.
  TileSpawns source;
  source.tile = tile(FIXTURE_TILE_X, FIXTURE_TILE_Y);

  GameObjectSpawn tilted (gameObject(8000002, at(-9500.0, 70.0, 58.0), 0.0));
  tilted.rotation.r0 = 0.25;
  tilted.rotation.r1 = 0.0;
  tilted.rotation.r2 = 0.0;
  tilted.rotation.r3 = 0.968246;
  source.gameobjects.push_back(tilted);

  TransformPlan const plan (planRotation(source, at(-9500.0, 70.0, 0.0), HALF_PI));

  CHECK(plan.gameobjects[0].rotation.r0 == 0.0);
  CHECK(plan.gameobjects[0].rotation.r1 == 0.0);
  CHECK(quaternionNorm(plan.gameobjects[0].rotation) == Approx(1.0).margin(COMPONENT_TOLERANCE));

  CHECK(errorCount(plan.issues) == 0);
  CHECK(warningCount(plan.issues) == 1);
  CHECK(anyIssueMentions(plan.issues, "tilt"));
  CHECK(anyIssueMentions(plan.issues, "8000002"));
}

TEST_CASE("a rotation turns a waypoint node orientation only when it has one"
         , "[chunk][waypoint]")
{
  // The column is nullable and usually NULL. Inventing a facing for a node that had none changes
  // what the creature does when it arrives, which a move was not asked to do.
  TileSpawns source;
  source.tile = tile(FIXTURE_TILE_X, FIXTURE_TILE_Y);

  WaypointPath path;
  path.id = 90000030;

  WaypointNode facing;
  facing.point = 1;
  facing.position = at(-9500.0, 70.0, 58.0);
  facing.has_orientation = true;
  facing.orientation = 1.0;
  path.nodes.push_back(facing);

  WaypointNode silent;
  silent.point = 2;
  silent.position = at(-9490.0, 70.0, 58.0);
  path.nodes.push_back(silent);

  source.paths.push_back(path);

  TransformPlan const plan (planRotation(source, at(-9500.0, 70.0, 0.0), HALF_PI));

  CHECK(plan.paths[0].nodes[0].has_orientation);
  CHECK(plan.paths[0].nodes[0].orientation == Approx(1.0 + HALF_PI).margin(ANGLE_TOLERANCE));

  CHECK_FALSE(plan.paths[0].nodes[1].has_orientation);
  CHECK(plan.paths[0].nodes[1].orientation == 0.0);
}

// --- tiles ---------------------------------------------------------------------------------

TEST_CASE("crossing a tile boundary warns and shows up in occupiedTiles", "[chunk][tile]")
{
  // Increasing world x DECREASES the tile index, so a move towards +x crosses into a lower one.
  // Tile 49 owns x in (-9599.99994, -9066.66661]; +20 yards from -9080 leaves it.
  TileSpawns source;
  source.tile = tile(FIXTURE_TILE_X, FIXTURE_TILE_Y);

  source.creatures.push_back(creature(9000010, at(-9080.0, 100.0, 50.0), 0.0));  // leaves on x
  source.creatures.push_back(creature(9000011, at(-9500.0, 100.0, 50.0), 0.0));  // stays
  source.creatures.push_back(creature(9000012, at(-9500.0, 520.0, 50.0), 0.0));  // leaves on y

  // A gameobject in the corner leaves on both axes at once, and is the only content that lands
  // in its destination tile -- so a tile list built from creatures and nodes alone is short by
  // one, and the terrain half of the edit never loads that ADT.
  source.gameobjects.push_back(gameObject(8000010, at(-9080.0, 520.0, 50.0), 0.0));

  TransformPlan const plan (planTranslation(source, delta(20.0, 20.0, 0.0)));

  CHECK(errorCount(plan.issues) == 0);
  CHECK(warningCount(plan.issues) == 3);

  // The direction is reported, not just the fact. A from/to swap would still produce a warning
  // and would be exactly the kind of mistake the inverted axis invites.
  CHECK(anyIssueMentions(plan.issues, "creature guid 9000010"));
  CHECK(anyIssueMentions(plan.issues, "(49, 31) -> (48, 31)"));
  CHECK(anyIssueMentions(plan.issues, "creature guid 9000012"));
  CHECK(anyIssueMentions(plan.issues, "(49, 31) -> (49, 30)"));
  CHECK(anyIssueMentions(plan.issues, "gameobject guid 8000010"));
  CHECK(anyIssueMentions(plan.issues, "(49, 31) -> (48, 30)"));
  CHECK_FALSE(anyIssueMentions(plan.issues, "creature guid 9000011"));

  // The terrain half of the edit has to load and save these, so they cannot be left implicit.
  CHECK(plan.occupied_tiles == occupiedTiles(plan));
  CHECK(plan.occupied_tiles.size() == 4);
  CHECK(contains(plan.occupied_tiles, tile(48, 30)));
  CHECK(contains(plan.occupied_tiles, tile(48, 31)));
  CHECK(contains(plan.occupied_tiles, tile(49, 30)));
  CHECK(contains(plan.occupied_tiles, tile(49, 31)));

  // Ordered by (x, y) so a diff of two plans is readable.
  CHECK(plan.occupied_tiles[0] == tile(48, 30));
  CHECK(plan.occupied_tiles[1] == tile(48, 31));
  CHECK(plan.occupied_tiles[2] == tile(49, 30));
  CHECK(plan.occupied_tiles[3] == tile(49, 31));
}

TEST_CASE("content brought back onto the map is not reported as having crossed", "[chunk][tile]")
{
  // A position already off the grid has no tile to have crossed FROM -- tileForPosition saturates
  // to an index outside 0..63 rather than answering -- and "crossed from tile (-1, 31)" would be
  // a fiction printed next to a move that actually repaired the row.
  TileSpawns source;
  source.tile = tile(0, 31);
  source.creatures.push_back(creature(9000040, at(17200.0, 100.0, 50.0), 0.0));

  REQUIRE_FALSE(TileCoordinates::isValidPosition(17200.0, 100.0));

  TransformPlan const plan (planTranslation(source, delta(-400.0, 0.0, 0.0)));

  CHECK(plan.issues.empty());
  CHECK(plan.occupied_tiles == std::vector<TileIndex>{tile(0, 31)});
}

TEST_CASE("the plan carries the tile and map it was built from", "[chunk]")
{
  // The tile grid is identical on every map, so the map is carried rather than used. A plan that
  // dropped it could not be reviewed -- or committed to the right map -- without the source it
  // came from, which is the thing the caller is about to stop holding.
  TileSpawns source;
  source.tile = tile(39, 28);
  source.map = 571;

  CreatureSpawn spawn (creature(9000030, at(-4000.0, 2000.0, 50.0), 0.0));
  spawn.map = 571;
  source.creatures.push_back(spawn);

  REQUIRE(TileCoordinates::tileForPosition(-4000.0, 2000.0) == tile(39, 28));

  TransformPlan const plan (planTranslation(source, delta(5.0, 0.0, 0.0)));

  CHECK(plan.map == 571);
  CHECK(plan.source_tile == tile(39, 28));

  // A move does not change which map a row is on.
  CHECK(plan.creatures[0].map == 571);

  // Including on the path that refuses to move anything, where the plan is all the caller gets.
  TransformPlan const refused
    (planTranslation(source, delta(std::numeric_limits<double>::infinity(), 0.0, 0.0)));

  CHECK(refused.map == 571);
  CHECK(refused.source_tile == tile(39, 28));
}

TEST_CASE("occupiedTiles deduplicates and covers waypoint nodes too", "[chunk][tile]")
{
  TileSpawns const source (fixtureTile());

  // Nine entities, one tile. A list with an entry per entity would have the caller loading the
  // same ADT nine times.
  TransformPlan const still (planTranslation(source, delta(1.0, 1.0, 0.0)));

  CHECK(still.occupied_tiles == std::vector<TileIndex>{tile(FIXTURE_TILE_X, FIXTURE_TILE_Y)});

  // A path node crossing on its own still has to be reported: the nodes are the patrol route and
  // the creature walks off the loaded terrain without them.
  TileSpawns nodes_only;
  nodes_only.tile = tile(FIXTURE_TILE_X, FIXTURE_TILE_Y);

  WaypointPath path;
  path.id = 90000030;

  WaypointNode inside;
  inside.point = 1;
  inside.position = at(-9500.0, 100.0, 50.0);
  path.nodes.push_back(inside);

  WaypointNode edge;
  edge.point = 2;
  edge.position = at(-9080.0, 100.0, 50.0);
  path.nodes.push_back(edge);

  nodes_only.paths.push_back(path);

  TransformPlan const moved (planTranslation(nodes_only, delta(20.0, 0.0, 0.0)));

  CHECK(moved.occupied_tiles.size() == 2);
  CHECK(contains(moved.occupied_tiles, tile(48, 31)));
  CHECK(anyIssueMentions(moved.issues, "waypoint path 90000030 point 2"));
}

// --- refusals ------------------------------------------------------------------------------

TEST_CASE("content moved off the map edge is an ERROR", "[chunk][safety]")
{
  // Valid x runs (-17066.66656, +17066.66656]: the negative extent floors to index 64, one past
  // the grid. -9530.2 - 7550 leaves it; the other two do not, so this also proves the check is
  // per entity rather than per plan.
  TileSpawns const source (fixtureTile());

  TransformPlan const plan (planTranslation(source, delta(-7550.0, 0.0, 0.0)));

  CHECK(errorCount(plan.issues) == 1);
  CHECK(SpawnValidation::hasErrors(plan.issues));
  CHECK(anyIssueMentions(plan.issues, "creature guid 9000002"));
  CHECK(anyIssueMentions(plan.issues, "outside the world extent"));

  // Nothing is clamped: the returned position is where the caller asked for, so the caller can
  // see how far out it went instead of finding a spawn parked on the map edge.
  CHECK(plan.creatures[1].position.x == Approx(-9530.2 - 7550.0).margin(POSITION_TOLERANCE));
  CHECK_FALSE(TileCoordinates::isValidPosition
                (plan.creatures[1].position.x, plan.creatures[1].position.y));

  // There is no ADT at index 64 for the caller to load, so it contributes no tile.
  for (TileIndex const& occupied : plan.occupied_tiles)
  {
    CHECK(TileCoordinates::isValidTile(occupied));
  }

  SECTION("the positive edge is checked too")
  {
    // +17066.66656 is the upper edge of tile 0 and is INSIDE; one tile further is not.
    TileSpawns east;
    east.creatures.push_back(creature(9000020, at(17000.0, 0.5, 50.0), 0.0));

    CHECK(errorCount(planTranslation(east, delta(60.0, 0.0, 0.0)).issues) == 0);
    CHECK(errorCount(planTranslation(east, delta(200.0, 0.0, 0.0)).issues) == 1);
  }

  SECTION("the y axis is checked too")
  {
    TileSpawns north;
    north.creatures.push_back(creature(9000021, at(-9500.0, 17000.0, 50.0), 0.0));

    CHECK(errorCount(planTranslation(north, delta(0.0, 200.0, 0.0)).issues) == 1);
  }
}

TEST_CASE("a transform that cannot be computed moves nothing", "[chunk][safety]")
{
  TileSpawns const source (fixtureTile());

  double const nan (std::numeric_limits<double>::quiet_NaN());
  double const inf (std::numeric_limits<double>::infinity());

  SECTION("a non-finite delta")
  {
    for (WorldDelta const& bad : {delta(nan, 0.0, 0.0), delta(0.0, inf, 0.0), delta(0.0, 0.0, nan)})
    {
      TransformPlan const plan (planTranslation(source, bad));

      // Writing NaN into a FLOAT column and leaving the spawns behind while the terrain moves are
      // both worse than producing nothing and saying why.
      CHECK(plan.empty());
      CHECK(plan.occupied_tiles.empty());
      CHECK(errorCount(plan.issues) == 1);
      CHECK(plan.source_tile == source.tile);
    }
  }

  SECTION("a non-finite angle")
  {
    for (double bad : {nan, inf, -inf})
    {
      TransformPlan const plan (planRotation(source, at(-9500.0, 70.0, 0.0), bad));

      CHECK(plan.empty());
      CHECK(errorCount(plan.issues) == 1);
    }
  }

  SECTION("a non-finite pivot")
  {
    // Worth its own check: with a NaN pivot even an identity rotation produces NaN everywhere,
    // because the pivot is subtracted and added back.
    TransformPlan const plan (planRotation(source, at(nan, 70.0, 0.0), HALF_PI));

    CHECK(plan.empty());
    CHECK(errorCount(plan.issues) == 1);
    CHECK(anyIssueMentions(plan.issues, "pivot"));
  }
}

// --- changeset -----------------------------------------------------------------------------

TEST_CASE("appendTo carries the whole dependent set into one changeset", "[chunk][changeset]")
{
  // The point of the milestone: terrain and database move together, so the moved creatures,
  // the moved gameobject and the moved patrol route are all in the same reviewable file.
  TileSpawns const source (fixtureTile());
  TransformPlan const plan (planTranslation(source, delta(10.0, -5.0, 2.0)));

  REQUIRE(errorCount(plan.issues) == 0);

  ChangesetBuilder builder (SchemaModel(loadSchemaFixture(fixturePath(REAL_FIXTURE))));
  appendTo(builder, plan);

  REQUIRE_FALSE(builder.empty());
  REQUIRE_FALSE(SpawnValidation::hasErrors(builder.issues()));

  std::string const sql (builder.build());

  auto const has ([&sql](std::string const& needle) { return sql.find(needle) != std::string::npos; });

  // The moved coordinates, and NOT the ones they came from. A changeset holding the old numbers
  // is the broken world this module exists to prevent.
  CHECK(has("-9502.345000"));
  CHECK(has("78.117000"));
  CHECK(has("60.271000"));
  CHECK_FALSE(has("-9512.345000"));
  CHECK_FALSE(has("83.117000"));

  // All three creatures, the gameobject, the addon row binding the patrol path, and the path.
  CHECK(has("INSERT INTO `creature`"));
  CHECK(has("INSERT INTO `creature_addon`"));
  CHECK(has("INSERT INTO `gameobject`"));
  CHECK(has("INSERT INTO `waypoint_data`"));

  // The last node of the moved path: -9508.345 + 10.
  CHECK(has("-9498.345000"));

  // Still the schema-resolved column name, still no core-derived columns.
  CHECK(has("`wander_distance`"));
  CHECK_FALSE(has("zoneId"));
  CHECK_FALSE(has("wpguid"));
}

TEST_CASE("appendTo hands an off-map move to the builder to refuse", "[chunk][changeset][safety]")
{
  // Not gated twice. The builder already refuses a position outside the extent, and duplicating
  // the gate here would take away the reject_invalid escape hatch for no extra safety.
  TileSpawns const source (fixtureTile());
  TransformPlan const plan (planTranslation(source, delta(-7550.0, 0.0, 0.0)));

  REQUIRE(SpawnValidation::hasErrors(plan.issues));

  ChangesetBuilder builder (SchemaModel(loadSchemaFixture(fixturePath(REAL_FIXTURE))));

  CHECK_NOTHROW(appendTo(builder, plan));
  CHECK(SpawnValidation::hasErrors(builder.issues()));
  CHECK_THROWS_AS(builder.build(), ChangesetError);
}

TEST_CASE("appendTo of an empty plan leaves the changeset empty", "[chunk][changeset]")
{
  // A refused transform must not turn into a changeset that quietly rewrites rows with the values
  // they already hold.
  TileSpawns source;
  source.tile = tile(FIXTURE_TILE_X, FIXTURE_TILE_Y);

  TransformPlan const plan
    (planTranslation(source, delta(std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0)));

  ChangesetBuilder builder (SchemaModel(loadSchemaFixture(fixturePath(REAL_FIXTURE))));
  appendTo(builder, plan);

  CHECK(builder.empty());
}
