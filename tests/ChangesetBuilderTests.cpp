// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <catch2/catch_test_macros.hpp>

#include <FixtureLoader.hpp>
#include <noggit/database/ChangesetBuilder.hpp>
#include <noggit/database/SchemaModel.hpp>
#include <noggit/database/SpawnTypes.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

using namespace Noggit::Database;
using Noggit::Tests::fixturePath;
using Noggit::Tests::loadSchemaFixture;

namespace
{
  constexpr char const* REAL_FIXTURE = "schema-tdb335-25101.tsv";
  constexpr char const* DRIFTED_FIXTURE = "schema-alt-drifted.tsv";

  SchemaModel modelFrom(char const* fixture)
  {
    return SchemaModel(loadSchemaFixture(fixturePath(fixture)));
  }

  bool contains(std::string const& haystack, std::string const& needle)
  {
    return haystack.find(needle) != std::string::npos;
  }

  // Collapses every run of whitespace to one space so a value tuple can be asserted as a
  // single string regardless of where the emitter chose to wrap the line. The wrapping is
  // cosmetic; the order and content of the values is not.
  std::string flattened(std::string const& text)
  {
    std::string out;
    bool in_space = false;

    for (char c : text)
    {
      if (c == ' ' || c == '\n' || c == '\r' || c == '\t')
      {
        if (!in_space)
        {
          out.push_back(' ');
          in_space = true;
        }
      }
      else
      {
        out.push_back(c);
        in_space = false;
      }
    }

    return out;
  }

  // Every occurrence of `keyword` paired with the backtick-quoted table name that follows it.
  std::vector<std::pair<std::size_t, std::string>> statementsOf
    (std::string const& sql, std::string const& keyword)
  {
    std::vector<std::pair<std::size_t, std::string>> out;

    for (std::size_t at = sql.find(keyword); at != std::string::npos;
         at = sql.find(keyword, at + 1))
    {
      std::size_t const open (sql.find('`', at));
      REQUIRE(open != std::string::npos);

      std::size_t const close (sql.find('`', open + 1));
      REQUIRE(close != std::string::npos);

      out.emplace_back(at, sql.substr(open + 1, close - open - 1));
    }

    return out;
  }

  // The spawn from tools/dev-db/03_example_changeset.sql, so the emitted output can be checked
  // against a changeset that is known to apply cleanly to a real world database.
  CreatureSpawn referenceCreature()
  {
    CreatureSpawn spawn;
    spawn.guid = 9000004;
    spawn.id = 990001;
    spawn.map = 0;
    spawn.position = WorldPosition {-9512.345, 83.117, 58.271};
    spawn.orientation = 4.7124;
    spawn.spawn_time_secs = 120;
    spawn.wander_distance = 0.0;
    spawn.cur_health = 1;
    spawn.movement_type = MovementType::WAYPOINT;
    spawn.has_addon = true;
    spawn.path_id = 90000040;
    return spawn;
  }

  GameObjectSpawn referenceGameObject()
  {
    GameObjectSpawn spawn;
    spawn.guid = 8000001;
    spawn.id = 180000;
    spawn.map = 0;
    spawn.position = WorldPosition {-9512.345, 83.117, 58.271};
    spawn.orientation = 3.14159265358979;   // pi: rotation2 = sin(o/2) = 1
    spawn.spawn_time_secs = 300;
    spawn.anim_progress = 100;
    spawn.state = 1;
    return spawn;
  }

  WaypointPath referencePath()
  {
    WaypointPath path;
    path.id = 90000040;

    for (int i = 0; i < 4; ++i)
    {
      WaypointNode node;
      node.point = static_cast<std::uint32_t>(i) + 1;
      node.position = WorldPosition {-9512.345 + static_cast<double>(i), 83.117, 58.271};
      node.delay_ms = 0;
      node.move_type = WaypointMoveType::WALK;
      node.action = 0;
      node.action_chance = 100;
      path.nodes.push_back(node);
    }

    return path;
  }

  // The whole point of the module: identical input, two schemas, two different column names.
  std::string buildReferenceSpawn(char const* fixture)
  {
    ChangesetBuilder builder (modelFrom(fixture));
    builder.addCreature(referenceCreature());
    return builder.build();
  }
}

// --- the single most important test in the module ------------------------------------------

TEST_CASE("the wander column name comes from the schema, not from a literal", "[changeset][drift]")
{
  std::string const real (buildReferenceSpawn(REAL_FIXTURE));
  std::string const drifted (buildReferenceSpawn(DRIFTED_FIXTURE));

  // TrinityCore 3.3.5.
  CHECK(contains(real, "`wander_distance`"));
  CHECK_FALSE(contains(real, "spawndist"));

  // AzerothCore and older cores. Same spawn, same builder, different schema.
  CHECK(contains(drifted, "`spawndist`"));
  CHECK_FALSE(contains(drifted, "wander_distance"));

  // A builder that produced identical text for both schemas would mean the branch is never
  // taken and the capability model is a hardcode with extra steps.
  CHECK(real != drifted);
}

TEST_CASE("the addon pose columns come from the schema, not from a literal", "[changeset][drift]")
{
  std::string const real (buildReferenceSpawn(REAL_FIXTURE));
  std::string const drifted (buildReferenceSpawn(DRIFTED_FIXTURE));

  CHECK(contains(real, "`StandState`"));
  CHECK(contains(real, "`MountCreatureID`"));
  CHECK(contains(real, "`AnimTier`"));
  CHECK(contains(real, "`VisFlags`"));
  CHECK(contains(real, "`SheathState`"));
  CHECK(contains(real, "`PvPFlags`"));
  CHECK_FALSE(contains(real, "bytes1"));
  CHECK_FALSE(contains(real, "bytes2"));

  CHECK(contains(drifted, "`bytes1`"));
  CHECK(contains(drifted, "`bytes2`"));
  CHECK_FALSE(contains(drifted, "StandState"));
  CHECK_FALSE(contains(drifted, "MountCreatureID"));
  CHECK_FALSE(contains(drifted, "SheathState"));
}

TEST_CASE("addon columns absent from the target schema are not named", "[changeset][drift]")
{
  // visibilityDistanceType exists on TDB 335.25101 and not on the drifted schema. Naming a
  // column that does not exist fails the whole statement, so presence has to be checked
  // rather than assumed from the pose encoding alone.
  CHECK(contains(buildReferenceSpawn(REAL_FIXTURE), "`visibilityDistanceType`"));
  CHECK_FALSE(contains(buildReferenceSpawn(DRIFTED_FIXTURE), "visibilityDistanceType"));
}

// --- shape --------------------------------------------------------------------------------

TEST_CASE("emitted SQL never names a derived or core-managed column", "[changeset][safety]")
{
  for (char const* fixture : {REAL_FIXTURE, DRIFTED_FIXTURE})
  {
    ChangesetBuilder builder (modelFrom(fixture));
    builder.addCreature(referenceCreature());
    builder.addGameObject(referenceGameObject());
    builder.addWaypointPath(referencePath());
    builder.removeCreature(9000099);
    builder.removeGameObject(8000099);
    builder.removeWaypointPath(90000990);

    std::string const sql (builder.build());

    // ObjectMgr::LoadCreatures does not read these and the core's own insert omits them.
    CHECK_FALSE(contains(sql, "zoneId"));
    CHECK_FALSE(contains(sql, "areaId"));

    // Core-managed. Authoring it corrupts the path.
    CHECK_FALSE(contains(sql, "wpguid"));
  }
}

TEST_CASE("every INSERT is preceded by a DELETE for the same table", "[changeset][idempotency]")
{
  ChangesetBuilder builder (modelFrom(REAL_FIXTURE));
  builder.addCreature(referenceCreature());
  builder.addGameObject(referenceGameObject());
  builder.addWaypointPath(referencePath());

  std::string const sql (builder.build());

  auto const inserts (statementsOf(sql, "INSERT INTO "));
  auto const deletes (statementsOf(sql, "DELETE FROM "));

  REQUIRE(inserts.size() >= 4);   // creature, creature_addon, gameobject, waypoint_data

  for (auto const& insert : inserts)
  {
    bool preceded = false;

    for (auto const& remove : deletes)
    {
      if (remove.second == insert.second && remove.first < insert.first)
      {
        preceded = true;
        break;
      }
    }

    INFO("INSERT INTO `" << insert.second << "` has no preceding DELETE");
    CHECK(preceded);
  }
}

TEST_CASE("the creature row matches the reference changeset", "[changeset][shape]")
{
  std::string const sql (flattened(buildReferenceSpawn(REAL_FIXTURE)));

  // The core's own WORLD_INS_CREATURE column order, and the reference changeset's values.
  CHECK(contains(sql, "(`guid`, `id`, `map`, `spawnMask`, `phaseMask`, `modelid`,"
                      " `equipment_id`, `position_x`, `position_y`, `position_z`,"
                      " `orientation`, `spawntimesecs`, `wander_distance`, `currentwaypoint`,"
                      " `curhealth`, `curmana`, `MovementType`, `npcflag`, `unit_flags`,"
                      " `dynamicflags`) VALUES"));

  CHECK(contains(sql, "(@CGUID, 990001, 0, 1, 1, 0, 0, -9512.345000, 83.117000, 58.271000,"
                      " 4.712400, 120, 0.000000, 0, 1, 0, 2, 0, 0, 0);"));
}

TEST_CASE("the creature_addon row matches the reference changeset", "[changeset][shape]")
{
  // Byte for byte the values on line 51 of tools/dev-db/03_example_changeset.sql.
  CHECK(contains(flattened(buildReferenceSpawn(REAL_FIXTURE))
                , "(@CGUID, @PATH, 0, 0, 0, 0, 0, 1, 0, 0, 0, NULL);"));

  // The same pose in the packed layout: bytes1 holds the stand state, bytes2 the sheath
  // state, and the drifted schema has no visibilityDistanceType column.
  CHECK(contains(flattened(buildReferenceSpawn(DRIFTED_FIXTURE))
                , "(@CGUID, @PATH, 0, 0, 1, 0, NULL);"));
}

TEST_CASE("the changeset is variable-driven", "[changeset][shape]")
{
  ChangesetBuilder builder (modelFrom(REAL_FIXTURE));

  CreatureSpawn second (referenceCreature());
  second.guid = 9000006;
  second.path_id = 0;                          // derived from the multiplier
  second.movement_type = MovementType::IDLE;
  second.has_addon = false;

  builder.addCreature(referenceCreature());
  builder.addCreature(second);

  std::string const sql (builder.build());

  // Declared once at the top so a reviewer can retarget the file by editing the header.
  CHECK(contains(sql, "SET @CGUID  := 9000004;"));
  CHECK(contains(sql, "SET @PATH   := 90000040;"));

  // Offsets from the base, not repeated literals.
  CHECK(contains(sql, "@CGUID+2"));
  CHECK_FALSE(contains(sql, "(9000004,"));
  CHECK_FALSE(contains(sql, "(9000006,"));
}

TEST_CASE("waypoint rows are 1-based, contiguous and carry no core-managed guid"
         , "[changeset][waypoint]")
{
  ChangesetBuilder builder (modelFrom(REAL_FIXTURE));
  builder.addWaypointPath(referencePath());

  std::string const sql (builder.build());
  std::string const flat (flattened(sql));

  CHECK(contains(flat, "(`id`, `point`, `position_x`, `position_y`, `position_z`,"
                       " `orientation`, `delay`, `move_type`, `action`, `action_chance`)"
                       " VALUES"));

  CHECK(contains(flat, "(@PATH, 1, -9512.345000, 83.117000, 58.271000, NULL, 0, 0, 0, 100),"));
  CHECK(contains(flat, "(@PATH, 4, -9509.345000, 83.117000, 58.271000, NULL, 0, 0, 0, 100);"));

  CHECK_FALSE(contains(sql, "wpguid"));
}

TEST_CASE("a waypoint node with an orientation emits a value instead of NULL"
         , "[changeset][waypoint]")
{
  WaypointPath path (referencePath());
  path.nodes[1].has_orientation = true;
  path.nodes[1].orientation = 4.7124;

  ChangesetBuilder builder (modelFrom(REAL_FIXTURE));
  builder.addWaypointPath(path);

  std::string const flat (flattened(builder.build()));

  CHECK(contains(flat, "(@PATH, 2, -9511.345000, 83.117000, 58.271000, 4.712400,"));
}

TEST_CASE("gameobject rotation is emitted as a quaternion", "[changeset][gameobject]")
{
  ChangesetBuilder builder (modelFrom(REAL_FIXTURE));
  builder.addGameObject(referenceGameObject());

  std::string const sql (builder.build());
  std::string const flat (flattened(sql));

  CHECK(contains(flat, "`rotation0`, `rotation1`, `rotation2`, `rotation3`"));

  // orientation pi: rotation2 = sin(pi/2) = 1, rotation3 = cos(pi/2) = 0. cos(pi/2) in double
  // is ~6.1e-17, so this also proves nothing leaks out in scientific notation.
  CHECK(contains(flat, " 0.000000, 0.000000, 1.000000, 0.000000, 300, 100, 1);"));
}

TEST_CASE("an explicitly supplied rotation is preserved rather than recomputed"
         , "[changeset][gameobject]")
{
  GameObjectSpawn spawn (referenceGameObject());
  spawn.orientation = 0.0;
  spawn.rotation.r0 = 0.25;
  spawn.rotation.r1 = 0.0;
  spawn.rotation.r2 = 0.0;
  spawn.rotation.r3 = 0.968246;

  ChangesetBuilder builder (modelFrom(REAL_FIXTURE));
  builder.addGameObject(spawn);

  std::string const flat (flattened(builder.build()));

  // A tilted object would lose its tilt if the emitter always derived the quaternion from
  // the orientation column.
  CHECK(contains(flat, "0.250000, 0.000000, 0.000000, 0.968246,"));
}

TEST_CASE("removals emit a DELETE and no INSERT", "[changeset][removal]")
{
  ChangesetBuilder builder (modelFrom(REAL_FIXTURE));
  builder.removeCreature(9000004);
  builder.removeGameObject(8000001);
  builder.removeWaypointPath(90000040);

  std::string const sql (builder.build());

  CHECK_FALSE(builder.empty());
  CHECK(contains(sql, "DELETE FROM `creature` WHERE `guid` = @CGUID;"));
  CHECK(contains(sql, "DELETE FROM `gameobject` WHERE `guid` = @OGUID;"));
  CHECK(contains(sql, "DELETE FROM `waypoint_data` WHERE `id` = @PATH;"));

  // Removing a creature must take its addon row with it, or the spawn is gone and a dangling
  // addon row is left behind.
  CHECK(contains(sql, "DELETE FROM `creature_addon` WHERE `guid` = @CGUID;"));

  CHECK_FALSE(contains(sql, "INSERT INTO"));
}

TEST_CASE("a replaced creature has its old addon row cleared even without a new one"
         , "[changeset][idempotency]")
{
  CreatureSpawn spawn (referenceCreature());
  spawn.movement_type = MovementType::IDLE;
  spawn.wander_distance = 0.0;
  spawn.has_addon = false;
  spawn.path_id = 0;

  ChangesetBuilder builder (modelFrom(REAL_FIXTURE));
  builder.addCreature(spawn);

  std::string const sql (builder.build());

  // Without this the spawn keeps following the path a previous changeset gave it.
  CHECK(contains(sql, "DELETE FROM `creature_addon` WHERE `guid` = @CGUID;"));
  CHECK_FALSE(contains(sql, "INSERT INTO `creature_addon`"));
}

// --- validation ---------------------------------------------------------------------------

TEST_CASE("an invalid spawn refuses to build", "[changeset][validation]")
{
  CreatureSpawn spawn (referenceCreature());
  spawn.movement_type = MovementType::RANDOM;   // requires wander_distance > 0
  spawn.wander_distance = 0.0;
  spawn.has_addon = false;
  spawn.path_id = 0;

  ChangesetBuilder builder (modelFrom(REAL_FIXTURE));
  builder.addCreature(spawn);

  CHECK_FALSE(builder.issues().empty());
  CHECK(SpawnValidation::hasErrors(builder.issues()));
  CHECK_THROWS_AS(builder.build(), ChangesetError);
}

TEST_CASE("an idle spawn with a wander distance refuses to build", "[changeset][validation]")
{
  CreatureSpawn spawn (referenceCreature());
  spawn.movement_type = MovementType::IDLE;     // requires wander_distance == 0
  spawn.wander_distance = 12.5;
  spawn.has_addon = false;
  spawn.path_id = 0;

  ChangesetBuilder builder (modelFrom(REAL_FIXTURE));
  builder.addCreature(spawn);

  CHECK_THROWS_AS(builder.build(), ChangesetError);
}

TEST_CASE("reject_invalid off downgrades a refusal to a comment", "[changeset][validation]")
{
  CreatureSpawn spawn (referenceCreature());
  spawn.movement_type = MovementType::RANDOM;
  spawn.wander_distance = 0.0;
  spawn.has_addon = false;
  spawn.path_id = 0;

  ChangesetBuilder::Options options;
  options.reject_invalid = false;

  ChangesetBuilder builder (modelFrom(REAL_FIXTURE), options);
  builder.addCreature(spawn);

  std::string sql;
  REQUIRE_NOTHROW(sql = builder.build());

  CHECK(contains(sql, "ERROR"));
  CHECK(contains(sql, "INSERT INTO `creature`"));
}

TEST_CASE("the same guid added twice is refused", "[changeset][validation]")
{
  ChangesetBuilder builder (modelFrom(REAL_FIXTURE));
  builder.addCreature(referenceCreature());
  builder.addCreature(referenceCreature());

  // Two rows with one guid collide on the primary key and destroy idempotency.
  CHECK(SpawnValidation::hasErrors(builder.issues()));
  CHECK_THROWS_AS(builder.build(), ChangesetError);
}

TEST_CASE("a waypoint creature without a path id gets the conventional one"
         , "[changeset][validation]")
{
  CreatureSpawn spawn (referenceCreature());
  spawn.path_id = 0;

  ChangesetBuilder builder (modelFrom(REAL_FIXTURE));
  builder.addCreature(spawn);

  std::string sql;
  REQUIRE_NOTHROW(sql = builder.build());

  // path_id = guid * 10 is a data convention only, but a waypoint creature with path_id 0
  // silently stands still, so something has to fill it in.
  CHECK(contains(sql, "SET @PATH   := 90000040;"));
  CHECK(contains(sql, "INSERT INTO `creature_addon`"));
}

TEST_CASE("a multiplier that derives no path is refused", "[changeset][validation]")
{
  CreatureSpawn spawn (referenceCreature());
  spawn.path_id = 0;

  ChangesetBuilder::Options options;
  options.path_id_multiplier = 0;

  ChangesetBuilder builder (modelFrom(REAL_FIXTURE), options);
  builder.addCreature(spawn);

  CHECK(SpawnValidation::hasErrors(builder.issues()));
  CHECK_THROWS_AS(builder.build(), ChangesetError);
}

// --- empty --------------------------------------------------------------------------------

TEST_CASE("an empty builder is harmless", "[changeset][empty]")
{
  ChangesetBuilder builder (modelFrom(REAL_FIXTURE));

  CHECK(builder.empty());
  CHECK(builder.issues().empty());

  std::string const sql (builder.build());

  CHECK_FALSE(sql.empty());
  CHECK_FALSE(contains(sql, "INSERT INTO"));
  CHECK_FALSE(contains(sql, "DELETE FROM"));
  CHECK_FALSE(contains(sql, "SET @"));
}

TEST_CASE("an empty builder does not consult the schema", "[changeset][empty][safety]")
{
  // A default SchemaModel answers every capability question by throwing. Building nothing
  // must not need an answer, or the editor cannot produce an empty changeset before it has
  // introspected a database.
  ChangesetBuilder builder {SchemaModel()};

  CHECK(builder.empty());
  CHECK_NOTHROW(builder.build());
}

TEST_CASE("empty stops reporting empty as soon as anything is added", "[changeset][empty]")
{
  ChangesetBuilder builder (modelFrom(REAL_FIXTURE));
  REQUIRE(builder.empty());

  SECTION("added creature")
  {
    builder.addCreature(referenceCreature());
    CHECK_FALSE(builder.empty());
  }

  SECTION("added gameobject")
  {
    builder.addGameObject(referenceGameObject());
    CHECK_FALSE(builder.empty());
  }

  SECTION("added path")
  {
    builder.addWaypointPath(referencePath());
    CHECK_FALSE(builder.empty());
  }

  SECTION("removal only")
  {
    builder.removeCreature(1);
    CHECK_FALSE(builder.empty());
  }
}

TEST_CASE("a description is fully commented out", "[changeset][safety]")
{
  ChangesetBuilder::Options options;
  options.description = "Moved Northshire guards.\nDROP TABLE creature;";

  ChangesetBuilder builder (modelFrom(REAL_FIXTURE), options);
  builder.addCreature(referenceCreature());

  std::string const sql (builder.build());

  // A newline in free text would otherwise end the comment and let the rest of the line run.
  CHECK(contains(sql, "-- Moved Northshire guards."));
  CHECK(contains(sql, "-- DROP TABLE creature;"));
  CHECK_FALSE(contains(sql, "\nDROP TABLE"));
}

// --- SqlFormat ----------------------------------------------------------------------------

TEST_CASE("coordinate formats at six decimals with no scientific notation"
         , "[changeset][sqlformat]")
{
  CHECK(SqlFormat::coordinate(-9512.345) == "-9512.345000");
  CHECK(SqlFormat::coordinate(0.0) == "0.000000");
  CHECK(SqlFormat::coordinate(17066.66656) == "17066.666560");

  // -0.0000001 rounds to zero at six decimals. "-0.000000" is legal SQL and reads as a bug.
  CHECK(SqlFormat::coordinate(-0.0000001) == "0.000000");
  CHECK(SqlFormat::coordinate(-0.0) == "0.000000");

  // Negatives that survive rounding keep their sign.
  CHECK(SqlFormat::coordinate(-0.0000042) == "-0.000004");
  CHECK(SqlFormat::coordinate(-17066.66656) == "-17066.666560");

  // std::fixed everywhere: the default float format would emit 1e-09 and 1e+20.
  CHECK(SqlFormat::coordinate(1.0e-9).find('e') == std::string::npos);
  CHECK(SqlFormat::coordinate(1.0e-9).find('E') == std::string::npos);
  CHECK(SqlFormat::coordinate(1.0e20).find('e') == std::string::npos);

  // Exactly six decimals, no more: the column is a FLOAT and cannot hold more anyway.
  CHECK(SqlFormat::coordinate(1.0) == "1.000000");
  CHECK(SqlFormat::coordinate(1.0 / 3.0) == "0.333333");
}

TEST_CASE("coordinate refuses a value that is not a number", "[changeset][sqlformat][safety]")
{
  // "nan" and "inf" are not SQL numbers, and substituting zero would move the spawn to the
  // map origin without saying so.
  CHECK_THROWS_AS
    (SqlFormat::coordinate(std::numeric_limits<double>::quiet_NaN()), ChangesetError);
  CHECK_THROWS_AS
    (SqlFormat::coordinate(std::numeric_limits<double>::infinity()), ChangesetError);
}

TEST_CASE("quote escapes what would break out of a literal", "[changeset][sqlformat][safety]")
{
  // A quote is escaped by DOUBLING it, which is correct whether or not the target server runs
  // with NO_BACKSLASH_ESCAPES. Backslash escaping would be a syntax error under that sql_mode,
  // and sql_mode belongs to whichever server the changeset is applied against -- not to any
  // connection this tool controls.
  CHECK(SqlFormat::quote("O'Brien") == "O''Brien");
  CHECK(SqlFormat::quote("it's a mess") == "it''s a mess");

  // Backslash is left ALONE. It cannot close a literal, and doubling it would insert a spurious
  // second backslash on a NO_BACKSLASH_ESCAPES server.
  CHECK(SqlFormat::quote("C:\\path") == "C:\\path");
  CHECK(SqlFormat::quote("it's a \\ mess") == "it''s a \\ mess");

  CHECK(SqlFormat::quote("") == "");
  CHECK(SqlFormat::quote("plain name 42") == "plain name 42");

  // A double quote needs no escaping inside a single-quoted literal.
  CHECK(SqlFormat::quote("say \"hi\"") == "say \"hi\"");

  // Control characters are refused rather than escaped: every escape sequence for them is
  // backslash-based, so none of them is portable across sql_modes. Refusing is honest; emitting
  // something that works on one server and corrupts on another is not.
  CHECK_THROWS_AS(SqlFormat::quote("a\nb"), ChangesetError);
  CHECK_THROWS_AS(SqlFormat::quote("a\rb"), ChangesetError);
  CHECK_THROWS_AS(SqlFormat::quote(std::string("a\0b", 3)), ChangesetError);
}

TEST_CASE("a quoted value is safe to embed in a single-quoted literal"
         , "[changeset][sqlformat][safety]")
{
  // The classic escape: a trailing backslash that would otherwise escape the closing quote
  // and swallow whatever follows.
  for (std::string const& hostile :
        { std::string("'; DROP TABLE creature; -- ")
        , std::string("ends with a backslash \\")
        , std::string("\\' already looks escaped")
        , std::string("mixed \" and ' quotes")
        })
  {
    std::string const literal ("'" + SqlFormat::quote(hostile) + "'");

    // Walk the literal the way a parser would under NO_BACKSLASH_ESCAPES, which is the mode
    // that matters: a backslash is an ORDINARY character there, and the only way to represent a
    // quote inside a literal is to double it. Escaping with a backslash instead would terminate
    // the literal early on such a server -- a syntax error part-way through a file whose
    // earlier DELETEs had already committed.
    //
    // So: no special handling of '\\' here, deliberately. A doubled '' is consumed as one
    // datum character, and a single ' closes the literal.
    std::size_t at = 1;
    bool closed = false;

    while (at < literal.size())
    {
      if (literal[at] == '\'')
      {
        if (at + 1 < literal.size() && literal[at + 1] == '\'')
        {
          at += 2;           // an escaped quote, still inside the literal
          continue;
        }

        closed = true;
        break;
      }

      ++at;
    }

    INFO("literal: " << literal);
    CHECK(closed);
    CHECK(at == literal.size() - 1);
  }
}
