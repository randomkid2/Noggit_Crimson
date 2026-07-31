// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <catch2/catch_test_macros.hpp>

#include <noggit/database/SpawnTypes.hpp>
#include <noggit/database/TileCoordinates.hpp>

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

using namespace Noggit::Database;

namespace
{
  // The dev-DB fixture spawn from tools/dev-db/03_example_changeset.sql: Elwynn Forest, tile
  // 49_31. Using the seeded coordinates rather than round numbers keeps the tests honest about
  // float-hostile values.
  constexpr double FIXTURE_X = -9512.345;
  constexpr double FIXTURE_Y = 83.117;
  constexpr double FIXTURE_Z = 58.271;
  constexpr double FIXTURE_ORIENTATION = 4.7124;   // 3*pi/2

  constexpr double NOT_A_NUMBER = std::numeric_limits<double>::quiet_NaN();

  std::size_t countErrors(std::vector<ValidationIssue> const& issues)
  {
    std::size_t count = 0;

    for (ValidationIssue const& issue : issues)
    {
      if (issue.severity == ValidationIssue::Severity::BLOCKING)
      {
        ++count;
      }
    }

    return count;
  }

  std::size_t countWarnings(std::vector<ValidationIssue> const& issues)
  {
    return issues.size() - countErrors(issues);
  }

  bool mentions(std::vector<ValidationIssue> const& issues, std::string const& fragment)
  {
    for (ValidationIssue const& issue : issues)
    {
      if (issue.message.find(fragment) != std::string::npos)
      {
        return true;
      }
    }

    return false;
  }

  CreatureSpawn validCreature()
  {
    CreatureSpawn spawn;
    spawn.guid = 9000004;
    spawn.id = 990001;
    spawn.map = 0;
    spawn.position = WorldPosition {FIXTURE_X, FIXTURE_Y, FIXTURE_Z};
    spawn.orientation = FIXTURE_ORIENTATION;
    spawn.movement_type = MovementType::IDLE;
    spawn.wander_distance = 0.0;
    return spawn;
  }

  // Built by hand rather than through TileCoordinates::quaternionForOrientation so that a
  // defect in that function shows up as a failure in its own tests, not in these.
  TileCoordinates::Quaternion quaternionFor(double orientation)
  {
    TileCoordinates::Quaternion rotation;
    rotation.r0 = 0.0;
    rotation.r1 = 0.0;
    rotation.r2 = std::sin(orientation * 0.5);
    rotation.r3 = std::cos(orientation * 0.5);
    return rotation;
  }

  GameObjectSpawn validGameObject()
  {
    GameObjectSpawn spawn;
    spawn.guid = 9000010;
    spawn.id = 990002;
    spawn.map = 0;
    spawn.position = WorldPosition {FIXTURE_X, FIXTURE_Y, FIXTURE_Z};
    spawn.orientation = FIXTURE_ORIENTATION;
    spawn.rotation = quaternionFor(FIXTURE_ORIENTATION);
    return spawn;
  }

  // Nodes carry the given point values in the given vector order, each at a distinct position
  // so reordering is observable.
  WaypointPath pathWithPoints(std::vector<std::uint32_t> const& points)
  {
    WaypointPath path;
    path.id = 90000040;

    double offset = 0.0;

    for (std::uint32_t point : points)
    {
      WaypointNode node;
      node.point = point;
      node.position = WorldPosition {FIXTURE_X + offset, FIXTURE_Y, FIXTURE_Z};
      path.nodes.push_back(node);
      offset += 8.0;
    }

    return path;
  }
}

// --- CreatureSpawn -----------------------------------------------------------------------

TEST_CASE("a well-formed creature spawn produces no issues", "[spawn][creature]")
{
  // Guards every negative case below: if the baseline itself failed, "adding a defect produces
  // an error" would pass for the wrong reason.
  std::vector<ValidationIssue> const issues (SpawnValidation::validate(validCreature()));

  CHECK(issues.empty());
  CHECK_FALSE(SpawnValidation::hasErrors(issues));
}

TEST_CASE("MovementType IDLE requires wander_distance 0", "[spawn][creature][movement]")
{
  CreatureSpawn spawn (validCreature());
  spawn.movement_type = MovementType::IDLE;

  spawn.wander_distance = 0.0;
  CHECK(SpawnValidation::validate(spawn).empty());

  spawn.wander_distance = 5.0;
  std::vector<ValidationIssue> const issues (SpawnValidation::validate(spawn));

  CHECK(SpawnValidation::hasErrors(issues));
  CHECK(countErrors(issues) == 1);
  CHECK(mentions(issues, "IDLE"));
}

TEST_CASE("MovementType RANDOM requires a positive wander_distance", "[spawn][creature][movement]")
{
  CreatureSpawn spawn (validCreature());
  spawn.movement_type = MovementType::RANDOM;

  SECTION("zero is rejected")
  {
    spawn.wander_distance = 0.0;
    std::vector<ValidationIssue> const issues (SpawnValidation::validate(spawn));

    CHECK(countErrors(issues) == 1);
    CHECK(mentions(issues, "RANDOM"));
  }

  SECTION("any positive distance is accepted")
  {
    spawn.wander_distance = 5.0;
    CHECK(SpawnValidation::validate(spawn).empty());

    spawn.wander_distance = 0.001;
    CHECK(SpawnValidation::validate(spawn).empty());
  }

  SECTION("a negative distance fails twice: it is negative and it is not positive")
  {
    spawn.wander_distance = -5.0;
    std::vector<ValidationIssue> const issues (SpawnValidation::validate(spawn));

    CHECK(countErrors(issues) == 2);
    CHECK(mentions(issues, "negative"));
    CHECK(mentions(issues, "RANDOM"));
  }
}

TEST_CASE("MovementType WAYPOINT requires an addon path", "[spawn][creature][movement]")
{
  CreatureSpawn spawn (validCreature());
  spawn.movement_type = MovementType::WAYPOINT;

  SECTION("no addon at all")
  {
    spawn.has_addon = false;
    spawn.path_id = 0;
    CHECK(countErrors(SpawnValidation::validate(spawn)) == 1);
  }

  SECTION("a path_id with no addon row to carry it")
  {
    // path_id lives on creature_addon. Without the addon row nothing binds the path.
    spawn.has_addon = false;
    spawn.path_id = 90000040;
    CHECK(countErrors(SpawnValidation::validate(spawn)) == 1);
  }

  SECTION("an addon row with no path")
  {
    spawn.has_addon = true;
    spawn.path_id = 0;
    CHECK(countErrors(SpawnValidation::validate(spawn)) == 1);
  }

  SECTION("an addon row carrying a path")
  {
    spawn.has_addon = true;
    spawn.path_id = 90000040;
    CHECK(SpawnValidation::validate(spawn).empty());
  }
}

TEST_CASE("WAYPOINT does not inherit the wander_distance rules", "[spawn][creature][movement]")
{
  // Only IDLE and RANDOM constrain wander_distance. A patrolling creature with a stale
  // non-zero value is legal, so flagging it would be a false positive.
  CreatureSpawn spawn (validCreature());
  spawn.movement_type = MovementType::WAYPOINT;
  spawn.has_addon = true;
  spawn.path_id = 90000040;
  spawn.wander_distance = 5.0;

  CHECK(SpawnValidation::validate(spawn).empty());
}

TEST_CASE("a MovementType outside 0..2 is rejected", "[spawn][creature][movement]")
{
  CreatureSpawn spawn (validCreature());
  spawn.movement_type = static_cast<MovementType>(7);

  std::vector<ValidationIssue> const issues (SpawnValidation::validate(spawn));

  CHECK(countErrors(issues) == 1);
  CHECK(mentions(issues, "MovementType 7"));
}

TEST_CASE("a creature guid or id of 0 is rejected", "[spawn][creature]")
{
  SECTION("guid")
  {
    CreatureSpawn spawn (validCreature());
    spawn.guid = 0;
    CHECK(countErrors(SpawnValidation::validate(spawn)) == 1);
  }

  SECTION("id")
  {
    CreatureSpawn spawn (validCreature());
    spawn.id = 0;
    CHECK(countErrors(SpawnValidation::validate(spawn)) == 1);
  }

  SECTION("both")
  {
    CreatureSpawn spawn (validCreature());
    spawn.guid = 0;
    spawn.id = 0;
    CHECK(countErrors(SpawnValidation::validate(spawn)) == 2);
  }
}

TEST_CASE("a creature outside the world extent is rejected", "[spawn][creature][position]")
{
  CreatureSpawn spawn (validCreature());

  SECTION("well inside the extent on both axes")
  {
    spawn.position = WorldPosition {17000.0, -17000.0, 0.0};
    CHECK(SpawnValidation::validate(spawn).empty());
  }

  SECTION("x past the edge")
  {
    spawn.position = WorldPosition {20000.0, FIXTURE_Y, FIXTURE_Z};
    CHECK(countErrors(SpawnValidation::validate(spawn)) == 1);
  }

  SECTION("y past the edge")
  {
    spawn.position = WorldPosition {FIXTURE_X, -20000.0, FIXTURE_Z};
    CHECK(countErrors(SpawnValidation::validate(spawn)) == 1);
  }

  SECTION("a NaN coordinate, which would be emitted into SQL as a literal the server rejects")
  {
    spawn.position = WorldPosition {FIXTURE_X, FIXTURE_Y, NOT_A_NUMBER};
    CHECK(countErrors(SpawnValidation::validate(spawn)) == 1);

    spawn.position = WorldPosition {NOT_A_NUMBER, FIXTURE_Y, FIXTURE_Z};
    CHECK(countErrors(SpawnValidation::validate(spawn)) == 1);
  }
}

TEST_CASE("equipment_id below -1 is rejected", "[spawn][creature]")
{
  CreatureSpawn spawn (validCreature());

  SECTION("the three meaningful forms are accepted")
  {
    spawn.equipment_id = static_cast<std::int8_t>(-1);   // random
    CHECK(SpawnValidation::validate(spawn).empty());

    spawn.equipment_id = static_cast<std::int8_t>(0);    // none
    CHECK(SpawnValidation::validate(spawn).empty());

    spawn.equipment_id = static_cast<std::int8_t>(5);    // a creature_equip_template ID
    CHECK(SpawnValidation::validate(spawn).empty());
  }

  SECTION("anything below -1 has no meaning")
  {
    spawn.equipment_id = static_cast<std::int8_t>(-2);
    CHECK(countErrors(SpawnValidation::validate(spawn)) == 1);

    spawn.equipment_id = static_cast<std::int8_t>(-128);
    CHECK(countErrors(SpawnValidation::validate(spawn)) == 1);
  }
}

TEST_CASE("spawntimesecs 0 is a warning, not an error", "[spawn][creature][severity]")
{
  CreatureSpawn spawn (validCreature());
  spawn.spawn_time_secs = 0;

  std::vector<ValidationIssue> const issues (SpawnValidation::validate(spawn));

  // Legal SQL and a legal row -- it just means "never respawn". The changeset must still be
  // emitted, with the warning attached.
  CHECK(countErrors(issues) == 0);
  CHECK(countWarnings(issues) == 1);
  CHECK_FALSE(SpawnValidation::hasErrors(issues));

  spawn.spawn_time_secs = 120;
  CHECK(SpawnValidation::validate(spawn).empty());
}

TEST_CASE("a negative wander_distance is rejected regardless of movement type", "[spawn][creature]")
{
  SECTION("IDLE also fails its own rule, because -1 is not 0")
  {
    CreatureSpawn spawn (validCreature());
    spawn.movement_type = MovementType::IDLE;
    spawn.wander_distance = -1.0;

    std::vector<ValidationIssue> const issues (SpawnValidation::validate(spawn));

    CHECK(countErrors(issues) == 2);
    CHECK(mentions(issues, "negative"));
  }

  SECTION("WAYPOINT fails only the negativity rule")
  {
    CreatureSpawn spawn (validCreature());
    spawn.movement_type = MovementType::WAYPOINT;
    spawn.has_addon = true;
    spawn.path_id = 90000040;
    spawn.wander_distance = -1.0;

    std::vector<ValidationIssue> const issues (SpawnValidation::validate(spawn));

    CHECK(countErrors(issues) == 1);
    CHECK(mentions(issues, "negative"));
  }
}

TEST_CASE("non-finite creature values never slip through", "[spawn][creature][edge]")
{
  SECTION("orientation")
  {
    CreatureSpawn spawn (validCreature());
    spawn.orientation = NOT_A_NUMBER;
    CHECK(countErrors(SpawnValidation::validate(spawn)) == 1);
  }

  SECTION("wander_distance under RANDOM, where a bare > 0 test would pass a NaN")
  {
    // Every comparison against NaN is false, so `!(x > 0)` catches it but `x <= 0` would not.
    CreatureSpawn spawn (validCreature());
    spawn.movement_type = MovementType::RANDOM;
    spawn.wander_distance = NOT_A_NUMBER;

    std::vector<ValidationIssue> const issues (SpawnValidation::validate(spawn));

    CHECK(SpawnValidation::hasErrors(issues));
    CHECK(mentions(issues, "NaN"));
  }
}

// --- GameObjectSpawn ---------------------------------------------------------------------

TEST_CASE("a well-formed gameobject spawn produces no issues", "[spawn][gameobject]")
{
  std::vector<ValidationIssue> const issues (SpawnValidation::validate(validGameObject()));

  CHECK(issues.empty());
  CHECK_FALSE(SpawnValidation::hasErrors(issues));
}

TEST_CASE("a gameobject guid or id of 0 is rejected", "[spawn][gameobject]")
{
  SECTION("guid")
  {
    GameObjectSpawn spawn (validGameObject());
    spawn.guid = 0;
    CHECK(countErrors(SpawnValidation::validate(spawn)) == 1);
  }

  SECTION("id")
  {
    GameObjectSpawn spawn (validGameObject());
    spawn.id = 0;
    CHECK(countErrors(SpawnValidation::validate(spawn)) == 1);
  }
}

TEST_CASE("a gameobject outside the world extent is rejected", "[spawn][gameobject][position]")
{
  GameObjectSpawn spawn (validGameObject());

  spawn.position = WorldPosition {-20000.0, FIXTURE_Y, FIXTURE_Z};
  CHECK(countErrors(SpawnValidation::validate(spawn)) == 1);

  spawn.position = WorldPosition {FIXTURE_X, NOT_A_NUMBER, FIXTURE_Z};
  CHECK(countErrors(SpawnValidation::validate(spawn)) == 1);
}

TEST_CASE("the gameobject rotation must be a unit quaternion", "[spawn][gameobject][rotation]")
{
  GameObjectSpawn spawn (validGameObject());

  SECTION("the default identity rotation is unit length")
  {
    spawn.rotation = TileCoordinates::Quaternion {};
    CHECK(SpawnValidation::validate(spawn).empty());
  }

  SECTION("a rotation built from an orientation is unit length at every angle")
  {
    for (double orientation : {0.0, 1.5708, 3.14159, 4.7124, 6.2831})
    {
      spawn.rotation = quaternionFor(orientation);
      CHECK(SpawnValidation::validate(spawn).empty());
    }
  }

  SECTION("float round-trip noise stays inside the tolerance")
  {
    // A quaternion narrowed to FLOAT and read back is no longer exactly unit length. That must
    // not be reported as a defect, or every gameobject read from the database fails validation.
    TileCoordinates::Quaternion rotation (quaternionFor(FIXTURE_ORIENTATION));
    rotation.r2 *= 1.0000005;
    rotation.r3 *= 1.0000005;
    spawn.rotation = rotation;

    CHECK(SpawnValidation::validate(spawn).empty());
  }

  SECTION("an all-zero rotation is rejected")
  {
    spawn.rotation = TileCoordinates::Quaternion {0.0, 0.0, 0.0, 0.0};
    CHECK(countErrors(SpawnValidation::validate(spawn)) == 1);
  }

  SECTION("a half-length rotation is rejected")
  {
    spawn.rotation = TileCoordinates::Quaternion {0.0, 0.0, 0.0, 0.5};
    CHECK(countErrors(SpawnValidation::validate(spawn)) == 1);
  }

  SECTION("Euler angles written into the quaternion columns are rejected")
  {
    // The failure this check exists for: someone writes (0, 0, orientation, 0).
    spawn.rotation = TileCoordinates::Quaternion {0.0, 0.0, FIXTURE_ORIENTATION, 0.0};
    CHECK(countErrors(SpawnValidation::validate(spawn)) == 1);
  }

  SECTION("a rotation 1% too long is rejected")
  {
    TileCoordinates::Quaternion rotation (quaternionFor(FIXTURE_ORIENTATION));
    rotation.r2 *= 1.01;
    rotation.r3 *= 1.01;
    spawn.rotation = rotation;

    CHECK(countErrors(SpawnValidation::validate(spawn)) == 1);
  }

  SECTION("a NaN component is rejected, which a bare tolerance test would miss")
  {
    spawn.rotation = TileCoordinates::Quaternion {0.0, 0.0, NOT_A_NUMBER, 1.0};
    CHECK(countErrors(SpawnValidation::validate(spawn)) == 1);
  }
}

// --- WaypointPath::isContiguous ----------------------------------------------------------

TEST_CASE("isContiguous accepts 1..n and nothing else", "[waypoint][contiguity]")
{
  SECTION("an empty path has no order to walk")
  {
    CHECK_FALSE(pathWithPoints({}).isContiguous());
  }

  SECTION("1..n is contiguous")
  {
    CHECK(pathWithPoints({1}).isContiguous());
    CHECK(pathWithPoints({1, 2}).isContiguous());
    CHECK(pathWithPoints({1, 2, 3, 4, 5}).isContiguous());
  }

  SECTION("a gap is not")
  {
    // The core walks nodes by point and stops at the gap, silently truncating the path.
    CHECK_FALSE(pathWithPoints({1, 2, 4}).isContiguous());
    CHECK_FALSE(pathWithPoints({1, 3}).isContiguous());
  }

  SECTION("a duplicate is not")
  {
    CHECK_FALSE(pathWithPoints({1, 2, 2}).isContiguous());
    CHECK_FALSE(pathWithPoints({1, 1}).isContiguous());
  }

  SECTION("point is 1-based, so a 0-based path is not contiguous")
  {
    CHECK_FALSE(pathWithPoints({0, 1, 2}).isContiguous());
  }

  SECTION("the right numbers in the wrong order are not")
  {
    // Vector order is authoritative everywhere else in this layer, so a path whose points are
    // the correct set in the wrong sequence needs renumbering just as much as a gapped one.
    CHECK_FALSE(pathWithPoints({2, 1, 3}).isContiguous());
    CHECK_FALSE(pathWithPoints({3, 2, 1}).isContiguous());
  }
}

// --- WaypointPath::renumber --------------------------------------------------------------

TEST_CASE("renumber assigns 1..n in vector order", "[waypoint][renumber]")
{
  SECTION("an out-of-order path")
  {
    WaypointPath path (pathWithPoints({3, 1, 2}));

    double const first_x (path.nodes[0].position.x);
    double const last_x (path.nodes[2].position.x);

    path.renumber();

    CHECK(path.nodes[0].point == 1);
    CHECK(path.nodes[1].point == 2);
    CHECK(path.nodes[2].point == 3);
    CHECK(path.isContiguous());

    // renumber() relabels, it does not reorder. The node that was first in the vector is still
    // first and is now point 1.
    CHECK(path.nodes[0].position.x == first_x);
    CHECK(path.nodes[2].position.x == last_x);
    CHECK(path.nodes.size() == 3);
  }

  SECTION("a gapped path")
  {
    WaypointPath path (pathWithPoints({1, 5, 9}));

    path.renumber();

    CHECK(path.nodes[0].point == 1);
    CHECK(path.nodes[1].point == 2);
    CHECK(path.nodes[2].point == 3);
    CHECK(path.isContiguous());
    CHECK(SpawnValidation::validate(path).empty());
  }

  SECTION("a duplicated path")
  {
    WaypointPath path (pathWithPoints({1, 1, 1}));

    path.renumber();

    CHECK(path.isContiguous());
  }

  SECTION("a single node that was numbered from the wrong base")
  {
    WaypointPath path (pathWithPoints({7}));

    path.renumber();

    CHECK(path.nodes[0].point == 1);
    CHECK(path.isContiguous());
  }

  SECTION("an empty path renumbers to nothing rather than misbehaving")
  {
    WaypointPath path (pathWithPoints({}));

    path.renumber();

    CHECK(path.nodes.empty());
    CHECK_FALSE(path.isContiguous());
  }

  SECTION("renumbering an already-correct path changes nothing")
  {
    WaypointPath path (pathWithPoints({1, 2, 3}));

    path.renumber();

    CHECK(path.nodes[0].point == 1);
    CHECK(path.nodes[1].point == 2);
    CHECK(path.nodes[2].point == 3);
  }
}

// --- WaypointPath validation -------------------------------------------------------------

TEST_CASE("a well-formed waypoint path produces no issues", "[waypoint][validation]")
{
  std::vector<ValidationIssue> const issues
    (SpawnValidation::validate(pathWithPoints({1, 2, 3, 4})));

  CHECK(issues.empty());
  CHECK_FALSE(SpawnValidation::hasErrors(issues));
}

TEST_CASE("an empty waypoint path is an error", "[waypoint][validation]")
{
  std::vector<ValidationIssue> const issues (SpawnValidation::validate(pathWithPoints({})));

  // Exactly one issue: emptiness is the whole story, and also reporting "not contiguous" would
  // bury it.
  CHECK(countErrors(issues) == 1);
  CHECK(issues.size() == 1);
  CHECK(SpawnValidation::hasErrors(issues));
}

TEST_CASE("a non-contiguous waypoint path is an error", "[waypoint][validation]")
{
  SECTION("gapped")
  {
    CHECK(countErrors(SpawnValidation::validate(pathWithPoints({1, 2, 4}))) == 1);
  }

  SECTION("duplicated")
  {
    CHECK(countErrors(SpawnValidation::validate(pathWithPoints({1, 2, 2}))) == 1);
  }

  SECTION("not starting at 1")
  {
    CHECK(countErrors(SpawnValidation::validate(pathWithPoints({2, 3, 4}))) == 1);
  }

  SECTION("and renumbering fixes it")
  {
    WaypointPath path (pathWithPoints({1, 2, 4}));
    REQUIRE(SpawnValidation::hasErrors(SpawnValidation::validate(path)));

    path.renumber();

    CHECK(SpawnValidation::validate(path).empty());
  }
}

TEST_CASE("action_chance must be a percentage", "[waypoint][validation]")
{
  SECTION("0 and 100 are the bounds and both are accepted")
  {
    WaypointPath path (pathWithPoints({1, 2}));
    path.nodes[0].action_chance = 0;
    path.nodes[1].action_chance = 100;

    CHECK(SpawnValidation::validate(path).empty());
  }

  SECTION("101 is not")
  {
    WaypointPath path (pathWithPoints({1, 2}));
    path.nodes[1].action_chance = 101;

    std::vector<ValidationIssue> const issues (SpawnValidation::validate(path));

    CHECK(countErrors(issues) == 1);
    CHECK(mentions(issues, "action_chance"));
  }

  SECTION("every offending node is reported, not just the first")
  {
    WaypointPath path (pathWithPoints({1, 2, 3}));
    path.nodes[0].action_chance = 200;
    path.nodes[2].action_chance = 4294967295u;

    CHECK(countErrors(SpawnValidation::validate(path)) == 2);
  }
}

TEST_CASE("a single-node waypoint path is a warning, not an error", "[waypoint][severity]")
{
  std::vector<ValidationIssue> const issues (SpawnValidation::validate(pathWithPoints({1})));

  // It is a legal row and must still be emitted; it just cannot form a route.
  CHECK(countErrors(issues) == 0);
  CHECK(countWarnings(issues) == 1);
  CHECK_FALSE(SpawnValidation::hasErrors(issues));

  // Two nodes is a route, so no warning.
  CHECK(SpawnValidation::validate(pathWithPoints({1, 2})).empty());
}

TEST_CASE("waypoint node positions are validated too", "[waypoint][validation][position]")
{
  WaypointPath path (pathWithPoints({1, 2, 3}));
  path.nodes[1].position = WorldPosition {-30000.0, FIXTURE_Y, FIXTURE_Z};

  CHECK(countErrors(SpawnValidation::validate(path)) == 1);

  path.nodes[2].position = WorldPosition {FIXTURE_X, FIXTURE_Y, NOT_A_NUMBER};

  CHECK(countErrors(SpawnValidation::validate(path)) == 2);
}

TEST_CASE("a NULL waypoint orientation is legal, a NaN one is not", "[waypoint][validation][edge]")
{
  WaypointPath path (pathWithPoints({1, 2}));

  SECTION("has_orientation false means the column is written NULL, whatever the field holds")
  {
    path.nodes[0].has_orientation = false;
    path.nodes[0].orientation = NOT_A_NUMBER;

    CHECK(SpawnValidation::validate(path).empty());
  }

  SECTION("an orientation that is actually written must be a finite angle")
  {
    path.nodes[0].has_orientation = true;
    path.nodes[0].orientation = FIXTURE_ORIENTATION;
    CHECK(SpawnValidation::validate(path).empty());

    path.nodes[0].orientation = NOT_A_NUMBER;
    CHECK(countErrors(SpawnValidation::validate(path)) == 1);
  }
}

// --- hasErrors ---------------------------------------------------------------------------

TEST_CASE("hasErrors separates a blocking issue from an advisory one", "[spawn][severity]")
{
  // This is what ChangesetBuilder::reject_invalid keys off, so a warning misclassified as an
  // error blocks a legitimate changeset and the inverse emits a broken one.
  CHECK_FALSE(SpawnValidation::hasErrors({}));

  std::vector<ValidationIssue> warning_only;
  warning_only.push_back(ValidationIssue {ValidationIssue::Severity::WARNING, "advisory"});
  CHECK_FALSE(SpawnValidation::hasErrors(warning_only));

  std::vector<ValidationIssue> mixed (warning_only);
  mixed.push_back(ValidationIssue {ValidationIssue::Severity::BLOCKING, "blocking"});
  CHECK(SpawnValidation::hasErrors(mixed));

  // The error is last, so a check that only looked at the first issue would miss it.
  CHECK(mixed.front().severity == ValidationIssue::Severity::WARNING);
}

// --- Display id resolution ---------------------------------------------------------------
//
// The rule the whole M1 rendering path rests on. Tested here, on the pure function, rather than
// only through a decoded row, because it has to give the same answer for both schema variants and
// the two reach it through completely different SQL.

TEST_CASE("creature.modelid overrides the template, and 0 defers to it"
         , "[spawn][creature][display]")
{
  // 0 is not "no model": it is the sentinel meaning "use the template's", and it is what almost
  // every real spawn holds. Reading it as an absent model is why a gameobject and a modelid-0
  // creature could not be rendered at all before this existed.
  CreatureTemplateInfo const deferred
    (SpawnDisplay::resolveCreatureTemplateInfo(0, {17188}, "Hogger"));

  CHECK(deferred.display_id == 17188u);
  CHECK(deferred.display_id_origin == DisplayIdOrigin::TEMPLATE);
  CHECK(deferred.name == "Hogger");

  CreatureTemplateInfo const overridden
    (SpawnDisplay::resolveCreatureTemplateInfo(448, {17188}, "Hogger"));

  CHECK(overridden.display_id == 448u);
  CHECK(overridden.display_id_origin == DisplayIdOrigin::SPAWN);

  // The template's model is still counted. Removing the override means writing
  // creature.modelid = 0, so the editor has to be able to say what that would look like.
  CHECK(overridden.template_model_count == 1u);
}

TEST_CASE("a template with several models resolves to the first non-zero, deterministically"
         , "[spawn][creature][display]")
{
  // TrinityCore rolls one of modelid1..4 per spawn at runtime. The editor cannot: the same tile
  // would draw a different creature on each open, and a changeset diff would move with a dice
  // roll. Leading zeroes are skipped rather than treated as a model, because a fixed-width select
  // list hands over four candidates whether or not the template filled them.
  CreatureTemplateInfo const info
    (SpawnDisplay::resolveCreatureTemplateInfo(0, {0, 1400, 1401, 1402}, "Kobold Vermin"));

  CHECK(info.display_id == 1400u);
  CHECK(info.display_id_origin == DisplayIdOrigin::TEMPLATE);
  CHECK(info.template_model_count == 3u);
  CHECK(info.templateOffersAlternatives());

  // Order is the template's, not sorted: modelid1 wins even when modelid2 is numerically smaller.
  CHECK(SpawnDisplay::resolveCreatureTemplateInfo(0, {900, 100}, "").display_id == 900u);

  // Repeated calls agree. Trivial for a pure function, asserted because determinism is the
  // requirement -- a later reimplementation using RAND() in SQL, as the core effectively does,
  // would fail here rather than in a viewport months later.
  for (int attempt = 0; attempt < 4; ++attempt)
  {
    CHECK(SpawnDisplay::resolveCreatureTemplateInfo(0, {0, 1400, 1401, 1402}, "").display_id
          == 1400u);
  }
}

TEST_CASE("a model listed twice is one model, not an ambiguity", "[spawn][creature][display]")
{
  // modelid1 == modelid2 means the core would roll the same display id either way, so there is
  // nothing to warn about. Reporting it would train the user to ignore the flag that matters.
  CreatureTemplateInfo const duplicated
    (SpawnDisplay::resolveCreatureTemplateInfo(0, {1400, 1400, 1400, 0}, ""));

  CHECK(duplicated.display_id == 1400u);
  CHECK(duplicated.template_model_count == 1u);
  CHECK_FALSE(duplicated.templateOffersAlternatives());

  // Two distinct models with one of them repeated is still an ambiguity.
  CreatureTemplateInfo const mixed
    (SpawnDisplay::resolveCreatureTemplateInfo(0, {1400, 1401, 1400, 0}, ""));

  CHECK(mixed.template_model_count == 2u);
  CHECK(mixed.templateOffersAlternatives());
}

TEST_CASE("an unresolvable creature is reported unresolved, never guessed"
         , "[spawn][creature][display]")
{
  // Neither side names a model. The renderer has to draw a marker for this, not skip it: a spawn
  // silently omitted looks like an empty tile, which is the same failure the query bounds and the
  // zoneId filter are so careful to avoid.
  for (std::vector<std::uint32_t> const& candidates
      : {std::vector<std::uint32_t>{}, std::vector<std::uint32_t>{0, 0, 0, 0}})
  {
    CreatureTemplateInfo const info
      (SpawnDisplay::resolveCreatureTemplateInfo(0, candidates, ""));

    CHECK(info.display_id == 0u);
    CHECK(info.display_id_origin == DisplayIdOrigin::UNRESOLVED);
    CHECK(info.template_model_count == 0u);
  }

  CreatureSpawn spawn (validCreature());
  CHECK_FALSE(SpawnDisplay::isRenderable(spawn));

  spawn.template_info = SpawnDisplay::resolveCreatureTemplateInfo(0, {17188}, "Hogger");
  CHECK(SpawnDisplay::isRenderable(spawn));
}

TEST_CASE("the template name is carried through untouched", "[spawn][creature][display]")
{
  // A label, not data: it reaches no SQL and is never parsed, so whatever the column holds is what
  // the UI shows. Names with punctuation are ordinary in this table.
  CHECK(SpawnDisplay::resolveCreatureTemplateInfo(0, {1}, "Hogger").name == "Hogger");
  CHECK(SpawnDisplay::resolveCreatureTemplateInfo(0, {1}, "Cook \"Torvald\"").name
        == "Cook \"Torvald\"");
  CHECK(SpawnDisplay::resolveCreatureTemplateInfo(0, {1}, "").name.empty());

  // An empty name is the LEFT JOIN having matched nothing, which is worth showing on its own: the
  // spawn names a creature_template row that does not exist.
  CHECK(SpawnDisplay::resolveCreatureTemplateInfo(0, {0}, "").name.empty());
}

TEST_CASE("only the six unrenderable gameobject types are rejected"
         , "[spawn][gameobject][display]")
{
  // From docs/schema-335.md, measured rather than copied from a published table: 6 TRAP and
  // 18 RITUAL are invisible in the client, 11 TRANSPORT and 15 MO_TRANSPORT move, and
  // 12 AREADAMAGE and 13 CAMERA have no model at all.
  for (std::uint32_t const type : {6u, 11u, 12u, 13u, 15u, 18u})
  {
    CAPTURE(type);
    CHECK_FALSE(SpawnDisplay::typeHasRenderableModel(type));
  }

  // Everything else in the verified 0..26 range is renderable. Asserted as a sweep rather than a
  // handful of samples so that widening the reject list by accident fails here.
  for (std::uint32_t type = 0; type <= 26; ++type)
  {
    bool const rejected
      (type == 6u || type == 11u || type == 12u || type == 13u || type == 15u || type == 18u);

    CAPTURE(type);
    CHECK(SpawnDisplay::typeHasRenderableModel(type) == !rejected);
  }

  // A type past the verified range is assumed renderable. docs/schema-335.md records that the
  // numbering above 26 is unverified, so guessing that an unknown type is one of the six would
  // hide real objects on a schema generation this build has not seen.
  CHECK(SpawnDisplay::typeHasRenderableModel(27));
  CHECK(SpawnDisplay::typeHasRenderableModel(1000));
}

TEST_CASE("a gameobject needs both a display id and a drawable type"
         , "[spawn][gameobject][display]")
{
  GameObjectSpawn spawn (validGameObject());

  // displayId 0 is legal, common, and means there is nothing to draw.
  CHECK(spawn.template_info.display_id == 0u);
  CHECK_FALSE(SpawnDisplay::isRenderable(spawn));

  spawn.template_info.display_id = 259;
  spawn.template_info.type = 3;                   // CHEST
  spawn.template_info.name = "Battered Chest";
  CHECK(SpawnDisplay::isRenderable(spawn));

  // A valid displayId on an invisible type still must not be drawn. Traps in particular carry
  // perfectly good display ids and are invisible in the client, so drawing them would put objects
  // in the viewport that no player ever sees.
  spawn.template_info.type = 6;                   // TRAP
  CHECK_FALSE(SpawnDisplay::isRenderable(spawn));

  // Neither condition alone is enough.
  spawn.template_info.display_id = 0;
  spawn.template_info.type = 3;
  CHECK_FALSE(SpawnDisplay::isRenderable(spawn));
}
