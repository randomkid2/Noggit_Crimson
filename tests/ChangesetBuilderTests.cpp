// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <catch2/catch_test_macros.hpp>

#include <FixtureLoader.hpp>
#include <noggit/database/ChangesetBuilder.hpp>
#include <noggit/database/SchemaModel.hpp>
#include <noggit/database/SpawnTypes.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <locale>
#include <sstream>
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

  // The value list of the INSERT into `gameobject`, split on its commas and stripped of the
  // emitter's line wrapping. Reading the emitted text back is the only honest way to test what
  // MySQL will store: the values a reviewer sees are the only ones that reach the column.
  std::vector<std::string> gameObjectValues(std::string const& sql)
  {
    std::string const flat (flattened(sql));
    std::string const marker (") VALUES (");

    std::size_t const insert (flat.find("INSERT INTO `gameobject`"));
    REQUIRE(insert != std::string::npos);

    std::size_t const values (flat.find(marker, insert));
    REQUIRE(values != std::string::npos);

    std::size_t const begin (values + marker.size());
    std::size_t const end (flat.find(')', begin));
    REQUIRE(end != std::string::npos);

    std::vector<std::string> out;
    std::string field;

    for (std::size_t at (begin); at < end; ++at)
    {
      if (flat[at] == ',')
      {
        out.push_back(field);
        field.clear();
      }
      else if (flat[at] != ' ')
      {
        field.push_back(flat[at]);
      }
    }

    out.push_back(field);

    return out;
  }

  // Parses emitted text the way the server does: as a double, then narrowed to the FLOAT the
  // column holds. Locale-classic on purpose -- a host under a comma-decimal locale would read
  // -9512.345 as -9512 and every assertion below would pass for the wrong reason.
  float reparsed(std::string const& text)
  {
    std::istringstream in (text);
    in.imbue(std::locale::classic());

    double value (0.0);
    in >> value;

    return static_cast<float>(value);
  }

  // The stored bytes, which is what a changeset review diffs. Compared as bits rather than with
  // == so "unchanged" means unchanged, and not merely "equal to within a comparison that treats
  // -0.0 and 0.0 as the same value".
  std::uint32_t bitsOf(float value)
  {
    std::uint32_t bits (0);
    std::memcpy(&bits, &value, sizeof bits);
    return bits;
  }

  // A rotation component as it comes back from the database: a FLOAT, widened to the double the
  // spawn structs carry. Every value the editor reads has been through this narrowing, so these
  // are the values a re-emitted changeset has to reproduce exactly.
  double asStoredFloat(double value)
  {
    return static_cast<double>(static_cast<float>(value));
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

TEST_CASE("every INSERT is either preceded by a DELETE or upserts", "[changeset][idempotency]")
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
    // Applying twice must leave identical rows. Clearing the keys first is how three of the
    // four tables get there; creature_addon cannot, because the emitter holds only one of its
    // columns and a DELETE would discard the rest. It reaches the same property with ON
    // DUPLICATE KEY UPDATE instead.
    if (insert.second == "creature_addon")
    {
      std::size_t const terminator (sql.find(";\n", insert.first));
      REQUIRE(terminator != std::string::npos);

      std::size_t const upsert (sql.find("ON DUPLICATE KEY UPDATE", insert.first));

      INFO("INSERT INTO `creature_addon` is neither DELETE-preceded nor an upsert");
      CHECK((upsert != std::string::npos && upsert < terminator));
      continue;
    }

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
  // Byte for byte the values on line 51 of tools/dev-db/03_example_changeset.sql. The statement
  // terminator moved off the value tuple when the row became an upsert; the values did not.
  CHECK(contains(flattened(buildReferenceSpawn(REAL_FIXTURE))
                , "(@CGUID, @PATH, 0, 0, 0, 0, 0, 1, 0, 0, 0, NULL) ON DUPLICATE KEY UPDATE"));

  // The same pose in the packed layout: bytes1 holds the stand state, bytes2 the sheath
  // state, and the drifted schema has no visibilityDistanceType column.
  CHECK(contains(flattened(buildReferenceSpawn(DRIFTED_FIXTURE))
                , "(@CGUID, @PATH, 0, 0, 1, 0, NULL) ON DUPLICATE KEY UPDATE"));
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

  // orientation ~pi: rotation2 = sin(o/2) = 1, rotation3 = cos(o/2). The reference orientation is
  // pi truncated to fifteen digits, so cos(o/2) is 1.6155445744325867e-15 rather than zero, and
  // the quaternion formatter says so in full: nine significant digits of it, in fixed notation,
  // because rounding it away would rewrite a column the editor never touched. Doubles as proof
  // that nothing leaks out in scientific notation even four decades below the coordinate grid.
  CHECK(contains(flat, " 0.000000000, 0.000000000, 1.000000000,"
                       " 0.00000000000000161554457, 300, 100, 1);"));

  // Asserted over the extracted values rather than the whole file: the comment header legitimately
  // contains "e-" in prose such as "core-derived", so scanning the text would pass for the wrong
  // reason today and fail for the wrong reason tomorrow.
  for (std::string const& value : gameObjectValues(sql))
  {
    CAPTURE(value);
    CHECK(value.find('e') == std::string::npos);
    CHECK(value.find('E') == std::string::npos);
  }
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
  CHECK(contains(flat, "0.250000000, 0.000000000, 0.000000000, 0.968246000,"));
}

TEST_CASE("an unedited gameobject's rotation re-emits bit for bit", "[changeset][gameobject]")
{
  // The defect this covers: rotation0..3 were formatted with the coordinate helper, at six
  // DECIMALS. That is eleven significant digits at a world coordinate's magnitude and only six
  // where |v| <= 1, so a gameobject that was read and written back unchanged had its stored
  // rotation bytes rewritten -- about eight float steps, ~1e-6 radians of yaw. Invisible in game
  // and highly visible in a changeset review, which is the whole point of emitting a reviewable
  // file.
  //
  // Every value here is a FLOAT widened to double, exactly as the reader hands it over, so the
  // comparison is against what the database actually holds.
  GameObjectSpawn spawn (referenceGameObject());
  spawn.orientation = 1.0;
  spawn.rotation.r0 = 0.0;
  spawn.rotation.r1 = 0.0;
  spawn.rotation.r2 = asStoredFloat(std::sin(0.5));    // 0.4794255495071411
  spawn.rotation.r3 = asStoredFloat(std::cos(0.5));    // 0.8775825500488281

  ChangesetBuilder builder (modelFrom(REAL_FIXTURE));
  builder.addGameObject(spawn);

  // The quaternion agrees with the orientation column, so nothing here is a warning about
  // disagreement -- the only thing under test is precision.
  CHECK(builder.issues().empty());

  std::vector<std::string> const values (gameObjectValues(builder.build()));

  REQUIRE(values.size() == 16);

  double const stored[4]
    {spawn.rotation.r0, spawn.rotation.r1, spawn.rotation.r2, spawn.rotation.r3};

  for (std::size_t i (0); i < 4u; ++i)
  {
    std::string const& text (values[9 + i]);

    CAPTURE(i, text);

    // Bit-identical after the emitted text has been read back the way the server reads it.
    CHECK(bitsOf(reparsed(text)) == bitsOf(static_cast<float>(stored[i])));

    // And no exponent, which would be legal SQL but unreadable in review and unmatchable here.
    CHECK(text.find('e') == std::string::npos);
    CHECK(text.find('E') == std::string::npos);
  }

  // The two components that carry the rotation are exactly the ones the old formatter lost, so
  // this asserts the defect was real rather than theoretical. If coordinate() ever gains enough
  // precision to pass this, the separate formatter has stopped being necessary.
  for (std::size_t i (2); i < 4u; ++i)
  {
    CAPTURE(i);
    CHECK(bitsOf(reparsed(SqlFormat::coordinate(stored[i])))
            != bitsOf(static_cast<float>(stored[i])));
  }
}

TEST_CASE("applying a changeset and re-emitting the row it wrote is a no-op"
         , "[changeset][gameobject][idempotency]")
{
  // The consequence a reviewer actually sees, stated end to end: emit, apply, read the row back,
  // emit again. The reloaded spawn has to be the spawn that was written, and the second file has
  // to be the first file, or every later review carries a rotation diff nobody made.
  //
  // Both halves are asserted, because only the first one distinguishes the formatters. The old
  // six-decimal path satisfies the file comparison on its own -- once it has rounded a value, it
  // rounds the rounded value to the same text for ever -- so a test that checked only that would
  // pass while the first application of the file quietly rewrote the data. That is the worse
  // failure: the one changeset that did move the rotation looks exactly like the ones that did not.
  double const orientation (2.4980915);

  GameObjectSpawn authored (referenceGameObject());
  authored.orientation = orientation;
  authored.rotation.r0 = 0.0;
  authored.rotation.r1 = 0.0;
  authored.rotation.r2 = asStoredFloat(std::sin(orientation / 2.0));
  authored.rotation.r3 = asStoredFloat(std::cos(orientation / 2.0));

  ChangesetBuilder original (modelFrom(REAL_FIXTURE));
  original.addGameObject(authored);

  std::string const emitted (original.build());
  std::vector<std::string> const values (gameObjectValues(emitted));

  REQUIRE(values.size() == 16);

  // What the reader hands back once the file has been applied: the FLOAT the column now holds,
  // widened to the double a spawn struct carries.
  GameObjectSpawn reloaded (authored);
  reloaded.rotation.r0 = static_cast<double>(reparsed(values[9]));
  reloaded.rotation.r1 = static_cast<double>(reparsed(values[10]));
  reloaded.rotation.r2 = static_cast<double>(reparsed(values[11]));
  reloaded.rotation.r3 = static_cast<double>(reparsed(values[12]));

  CHECK(bitsOf(static_cast<float>(reloaded.rotation.r0))
          == bitsOf(static_cast<float>(authored.rotation.r0)));
  CHECK(bitsOf(static_cast<float>(reloaded.rotation.r1))
          == bitsOf(static_cast<float>(authored.rotation.r1)));
  CHECK(bitsOf(static_cast<float>(reloaded.rotation.r2))
          == bitsOf(static_cast<float>(authored.rotation.r2)));
  CHECK(bitsOf(static_cast<float>(reloaded.rotation.r3))
          == bitsOf(static_cast<float>(authored.rotation.r3)));

  ChangesetBuilder rebuilt (modelFrom(REAL_FIXTURE));
  rebuilt.addGameObject(reloaded);

  CHECK(rebuilt.build() == emitted);
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

// --- creature_addon: the columns the editor never reads -------------------------------------
//
// The defect these cover, stated once: the emitter used to DELETE creature_addon for every
// creature in the changeset and re-INSERT a row only for spawns carrying path data. mount,
// MountCreatureID, StandState, AnimTier, VisFlags, SheathState, PvPFlags, emote,
// visibilityDistanceType and auras are never selected by the read path
// (SpawnQueryDetail.cpp:557-568), so nothing in the editor could rewrite what that DELETE
// removed. Moving a mounted, kneeling or aura-carrying creature one yard destroyed all of it,
// in the user's own database, silently.

TEST_CASE("moving a creature with no addon data touches creature_addon not at all"
         , "[changeset][addon][dataloss]")
{
  CreatureSpawn spawn (referenceCreature());
  spawn.movement_type = MovementType::IDLE;
  spawn.wander_distance = 0.0;
  spawn.has_addon = false;        // the read found no addon row
  spawn.path_id = 0;

  ChangesetBuilder builder (modelFrom(REAL_FIXTURE));
  builder.addCreature(spawn);

  std::string const sql (builder.build());

  CHECK(contains(sql, "INSERT INTO `creature`"));

  // A changeset must not state anything about data it never read. Emitting a DELETE here was
  // the whole defect: has_addon false means the reader saw no row, which is not the same claim
  // as "there is no row", and the difference cost the user their addon.
  //
  // Checked as statements rather than as the word, because the file's own comment header now
  // explains the exception and names the table in prose.
  CHECK_FALSE(contains(sql, "DELETE FROM `creature_addon`"));
  CHECK_FALSE(contains(sql, "INSERT INTO `creature_addon`"));

  // Not even the section rule, so a reviewer is not left wondering what happened under it.
  CHECK_FALSE(contains(sql, "-- creature_addon -"));
}

TEST_CASE("a creature that keeps its addon row keeps the columns nobody read"
         , "[changeset][addon][dataloss]")
{
  ChangesetBuilder builder (modelFrom(REAL_FIXTURE));
  builder.addCreature(referenceCreature());     // has_addon, on a waypoint path

  std::string const sql (builder.build());
  std::string const flat (flattened(sql));

  CHECK_FALSE(contains(sql, "DELETE FROM `creature_addon`"));
  CHECK(contains(flat, "ON DUPLICATE KEY UPDATE `path_id` = VALUES(`path_id`);"));

  // path_id is the only column the editor authored, so it is the only one allowed to overwrite
  // an existing row. The rest of the value tuple exists solely to give a creature with no addon
  // row a complete one; naming any of them in the update clause reinstates the data loss.
  for (char const* column :
        { "mount", "MountCreatureID", "StandState", "AnimTier", "VisFlags", "SheathState"
        , "PvPFlags", "emote", "visibilityDistanceType", "auras", "guid"
        })
  {
    CAPTURE(column);
    CHECK_FALSE(contains(flat, "VALUES(`" + std::string(column) + "`)"));
  }
}

TEST_CASE("a path the editor removed is still cleared", "[changeset][addon]")
{
  // The one behaviour the destructive DELETE did provide: a spawn that lost its path must stop
  // walking it. It is reached without destroying anything, because such a spawn arrives with
  // its addon row still present and path_id 0, and the update writes that 0.
  CreatureSpawn spawn (referenceCreature());
  spawn.movement_type = MovementType::IDLE;
  spawn.wander_distance = 0.0;
  spawn.has_addon = true;
  spawn.path_id = 0;

  ChangesetBuilder builder (modelFrom(REAL_FIXTURE));
  builder.addCreature(spawn);

  std::string const flat (flattened(builder.build()));

  CHECK(contains(flat, "(@CGUID, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, NULL) ON DUPLICATE KEY UPDATE"));
  CHECK(contains(flat, "ON DUPLICATE KEY UPDATE `path_id` = VALUES(`path_id`);"));

  // No path was authored, so nothing declares @PATH and the row must not reference it.
  CHECK_FALSE(contains(flat, "@PATH"));
}

TEST_CASE("deleting a creature still takes its addon row with it", "[changeset][addon][removal]")
{
  // The one case where DELETE is right: the `creature` row is going away, so what is left is an
  // orphan rather than anybody's data. Asserted alongside an edited creature to prove the two
  // are separated -- the edited spawn's guid must not appear in the DELETE.
  ChangesetBuilder builder (modelFrom(REAL_FIXTURE));
  builder.addCreature(referenceCreature());     // guid 9000004, edited
  builder.removeCreature(9000006);              // deleted

  std::string const sql (builder.build());

  CHECK(contains(sql, "DELETE FROM `creature_addon` WHERE `guid` = @CGUID+2;"));
  CHECK_FALSE(contains(sql, "DELETE FROM `creature_addon` WHERE `guid` IN"));
}

// --- creating spawns ------------------------------------------------------------------------
//
// The property under test throughout this block is guid allocation, and it is the part of
// creation that fails silently rather than loudly. creature.guid and gameobject.guid are two
// independent primary-key sequences, both counting up from 1 with no reserved range, so a number
// the editor invents in order to have something to select is a number that very probably names a
// real row in one or both tables. Emitting it would not error -- it would overwrite somebody's
// spawn, in their database, with no diff to show for it.

namespace
{
  // A spawn the database has never seen, carrying the shape SpawnSceneCache hands out: a
  // provisional guid well above anything a real sequence has reached, which must never appear in
  // the emitted file.
  constexpr std::uint32_t PROVISIONAL_GUID = 0xF0000001u;

  CreatureSpawn newCreature()
  {
    CreatureSpawn spawn;
    spawn.guid = PROVISIONAL_GUID;
    spawn.id = 299;
    spawn.map = 0;
    spawn.position = WorldPosition {-8913.230, -132.087, 82.663};
    spawn.orientation = 0.0;
    spawn.spawn_time_secs = 120;
    spawn.cur_health = 1;
    spawn.movement_type = MovementType::IDLE;
    return spawn;
  }

  GameObjectSpawn newGameObject()
  {
    GameObjectSpawn spawn;
    spawn.guid = PROVISIONAL_GUID;
    spawn.id = 180000;
    spawn.map = 0;
    spawn.position = WorldPosition {-8913.230, -132.087, 82.663};
    spawn.orientation = 0.0;
    return spawn;
  }
}

TEST_CASE("a created spawn allocates its guid from MAX at apply time", "[changeset][create]")
{
  ChangesetBuilder builder (modelFrom(REAL_FIXTURE));
  builder.addNewCreature(newCreature());

  std::string const sql (builder.build());

  // The whole allocation, in one line the reviewer can read. IFNULL because MAX() of an empty
  // table is NULL, and NULL+1 is NULL -- which inserts guid 0 under a lax sql_mode.
  CHECK(contains(sql, "SET @CGUID_NEW := (SELECT IFNULL(MAX(`guid`), 0) FROM `creature`);"));

  // Offsets start at 1 because the variable holds the maximum, not the next free value.
  CHECK(contains(flattened(sql), "(@CGUID_NEW+1, 299,"));
}

TEST_CASE("a created spawn's provisional guid never reaches the SQL", "[changeset][create][safety]")
{
  ChangesetBuilder builder (modelFrom(REAL_FIXTURE));
  builder.addNewCreature(newCreature());
  builder.addNewGameObject(newGameObject());

  std::string const sql (builder.build());

  // The number the editor invented so it had something to select, list and drag. It is an
  // editor-side identity and nothing else; a file naming it would be claiming the database had
  // issued it.
  CHECK_FALSE(contains(sql, std::to_string(PROVISIONAL_GUID)));

  // Nor in hex, in case a future change formats it differently.
  CHECK_FALSE(contains(sql, "F0000001"));
}

TEST_CASE("creature and gameobject guids come from independent sequences"
         , "[changeset][create][guid]")
{
  // The audit finding this pins: a guid is unique within its table and nowhere else. One
  // allocation serving both tables would put a creature and a gameobject on the same number,
  // which is legal, and then use one table's maximum to place a row in the other, which is not.
  ChangesetBuilder builder (modelFrom(REAL_FIXTURE));
  builder.addNewCreature(newCreature());
  builder.addNewGameObject(newGameObject());

  std::string const sql (builder.build());

  CHECK(contains(sql, "SET @CGUID_NEW := (SELECT IFNULL(MAX(`guid`), 0) FROM `creature`);"));
  CHECK(contains(sql, "SET @OGUID_NEW := (SELECT IFNULL(MAX(`guid`), 0) FROM `gameobject`);"));

  // Each row keyed off its own table's allocation, never the other's.
  std::string const flat (flattened(sql));
  CHECK(contains(flat, "(@CGUID_NEW+1, 299,"));
  CHECK(contains(flat, "(@OGUID_NEW+1, 180000,"));

  CHECK_FALSE(contains(sql, "MAX(`guid`), 0) FROM `creature`);\nSET @OGUID_NEW := (SELECT "
                            "IFNULL(MAX(`guid`), 0) FROM `creature`)"));
}

TEST_CASE("created spawns are numbered in the order they were added", "[changeset][create]")
{
  CreatureSpawn second (newCreature());
  second.guid = PROVISIONAL_GUID + 1;
  second.id = 300;

  ChangesetBuilder builder (modelFrom(REAL_FIXTURE));
  builder.addNewCreature(newCreature());
  builder.addNewCreature(second);

  std::string const flat (flattened(builder.build()));

  CHECK(contains(flat, "(@CGUID_NEW+1, 299,"));
  CHECK(contains(flat, "(@CGUID_NEW+2, 300,"));

  // Two rows on the same number would collide on the primary key at apply time, after the
  // DELETEs above them had committed.
  CHECK_FALSE(contains(flat, "(@CGUID_NEW+1, 300,"));
}

TEST_CASE("creating and editing in one file do not share a guid base", "[changeset][create][guid]")
{
  ChangesetBuilder builder (modelFrom(REAL_FIXTURE));
  builder.addCreature(referenceCreature());     // guid 9000004, read from the database
  builder.addNewCreature(newCreature());        // no guid yet

  std::string const sql (builder.build());

  // @CGUID is a guid the editor READ; @CGUID_NEW is one the SERVER will choose. Folding the
  // provisional number into @CGUID's base would move every offset in the edited section onto
  // rows that have nothing to do with it.
  CHECK(contains(sql, "SET @CGUID  := 9000004;"));
  CHECK(contains(sql, "SET @CGUID_NEW := (SELECT"));

  // The edited row is cleared before it is rewritten; the created row has nothing to clear.
  CHECK(contains(sql, "DELETE FROM `creature` WHERE `guid` = @CGUID;"));
  CHECK_FALSE(contains(sql, "@CGUID_NEW+1;"));
  CHECK_FALSE(contains(sql, "DELETE FROM `creature` WHERE `guid` = @CGUID_NEW"));
}

TEST_CASE("a created spawn writes exactly the columns an edited one does"
         , "[changeset][create][shape]")
{
  // Two INSERT statements into one table is two places a column list can be wrong. They are
  // built from one list precisely so this assertion can be true by construction; it is here to
  // catch the day somebody makes it not be.
  ChangesetBuilder builder (modelFrom(REAL_FIXTURE));
  builder.addCreature(referenceCreature());
  builder.addNewCreature(newCreature());
  builder.addGameObject(referenceGameObject());
  builder.addNewGameObject(newGameObject());

  std::string const flat (flattened(builder.build()));

  std::string const creature_columns
    ( "INSERT INTO `creature` (`guid`, `id`, `map`, `spawnMask`, `phaseMask`, `modelid`,"
      " `equipment_id`, `position_x`, `position_y`, `position_z`, `orientation`,"
      " `spawntimesecs`, `wander_distance`, `currentwaypoint`, `curhealth`, `curmana`,"
      " `MovementType`, `npcflag`, `unit_flags`, `dynamicflags`) VALUES" );

  std::size_t const first (flat.find(creature_columns));
  REQUIRE(first != std::string::npos);
  CHECK(flat.find(creature_columns, first + 1) != std::string::npos);

  std::string const gameobject_columns
    ( "INSERT INTO `gameobject` (`guid`, `id`, `map`, `spawnMask`, `phaseMask`, `position_x`,"
      " `position_y`, `position_z`, `orientation`, `rotation0`, `rotation1`, `rotation2`,"
      " `rotation3`, `spawntimesecs`, `animprogress`, `state`) VALUES" );

  std::size_t const object_first (flat.find(gameobject_columns));
  REQUIRE(object_first != std::string::npos);
  CHECK(flat.find(gameobject_columns, object_first + 1) != std::string::npos);
}

TEST_CASE("a created spawn's coordinates survive the round trip to FLOAT"
         , "[changeset][create][roundtrip]")
{
  // The requirement that a created spawn, once applied and reloaded, is in the same place. The
  // emitted text is the only thing that reaches the column, so it is what gets parsed back.
  CreatureSpawn spawn (newCreature());
  spawn.position = WorldPosition {-8913.234375, -132.0869140625, 82.66312408447266};

  ChangesetBuilder builder (modelFrom(REAL_FIXTURE));
  builder.addNewCreature(spawn);

  std::string const flat (flattened(builder.build()));

  std::size_t const values (flat.find("(@CGUID_NEW+1,"));
  REQUIRE(values != std::string::npos);

  std::vector<std::string> fields;
  std::string field;

  for (std::size_t at (values + 1); at < flat.size() && flat[at] != ')'; ++at)
  {
    if (flat[at] == ',')
    {
      fields.push_back(field);
      field.clear();
    }
    else if (flat[at] != ' ')
    {
      field.push_back(flat[at]);
    }
  }

  fields.push_back(field);

  // Column 8, 9, 10 of the WORLD_INS_CREATURE shape, zero-based 7, 8, 9.
  REQUIRE(fields.size() == 20);

  CHECK(bitsOf(reparsed(fields[7])) == bitsOf(static_cast<float>(spawn.position.x)));
  CHECK(bitsOf(reparsed(fields[8])) == bitsOf(static_cast<float>(spawn.position.y)));
  CHECK(bitsOf(reparsed(fields[9])) == bitsOf(static_cast<float>(spawn.position.z)));
}

TEST_CASE("creating a spawn invents no creature_addon row", "[changeset][create][addon]")
{
  // A creature the editor placed carries no mount, pose, emote or aura, because the editor has
  // no way to author any of them. Writing a row of defaults would state something about data
  // nobody supplied -- the same class of claim the addon DELETE used to make.
  ChangesetBuilder builder (modelFrom(REAL_FIXTURE));
  builder.addNewCreature(newCreature());

  std::string const sql (builder.build());

  CHECK(contains(sql, "INSERT INTO `creature`"));
  CHECK_FALSE(contains(sql, "INSERT INTO `creature_addon`"));
  CHECK_FALSE(contains(sql, "DELETE FROM `creature_addon`"));
  CHECK_FALSE(contains(sql, "-- creature_addon -"));
}

TEST_CASE("a created creature with an authored path gets an addon row on the new guid"
         , "[changeset][create][addon]")
{
  // The one case that does write an addon row: the caller authored a waypoint binding, so the
  // path_id is data somebody supplied rather than a default this class invented. It has to be
  // keyed off @CGUID_NEW, not off the provisional guid.
  CreatureSpawn spawn (newCreature());
  spawn.movement_type = MovementType::WAYPOINT;
  spawn.path_id = 2990010;

  ChangesetBuilder builder (modelFrom(REAL_FIXTURE));
  builder.addNewCreature(spawn);

  std::string const flat (flattened(builder.build()));

  CHECK(contains(flat, "(@CGUID_NEW+1, @PATH, 0, 0, 0, 0, 0, 1, 0, 0, 0, NULL)"));
  CHECK(contains(flat, "ON DUPLICATE KEY UPDATE `path_id` = VALUES(`path_id`);"));
  CHECK(contains(flat, "SET @PATH := 2990010;"));
}

TEST_CASE("a created waypoint creature with no path id is refused"
         , "[changeset][create][validation]")
{
  // The conventional path_id = guid * 10 cannot be applied to a guid that does not exist yet.
  // Deriving one from the provisional number would bind the creature to a path nobody wrote
  // nodes for, and it would stand still with nothing in the file explaining why.
  CreatureSpawn spawn (newCreature());
  spawn.movement_type = MovementType::WAYPOINT;
  spawn.path_id = 0;

  ChangesetBuilder builder (modelFrom(REAL_FIXTURE));
  builder.addNewCreature(spawn);

  CHECK_THROWS_AS(builder.build(), ChangesetError);

  bool mentioned = false;

  for (auto const& issue : builder.issues())
  {
    if (issue.message.find("does not exist until this file is applied") != std::string::npos)
    {
      mentioned = true;
    }
  }

  CHECK(mentioned);
}

TEST_CASE("a file that creates spawns says it is not idempotent", "[changeset][create][safety]")
{
  // The class header promises idempotency and this is the exception. A reviewer who applies a
  // file twice on the strength of that promise and gets duplicate spawns was misled by the file,
  // so the file has to carry the correction, not just the source.
  ChangesetBuilder builder (modelFrom(REAL_FIXTURE));
  builder.addNewCreature(newCreature());

  CHECK(contains(builder.build(), "NOT IDEMPOTENT"));

  // And a file that creates nothing must not carry the warning, or it stops being read.
  ChangesetBuilder edits_only (modelFrom(REAL_FIXTURE));
  edits_only.addCreature(referenceCreature());

  CHECK_FALSE(contains(edits_only.build(), "NOT IDEMPOTENT"));
}

// --- deleting spawns ------------------------------------------------------------------------

TEST_CASE("deleting a creature emits a DELETE for its addon and no INSERT for either"
         , "[changeset][removal][addon]")
{
  ChangesetBuilder builder (modelFrom(REAL_FIXTURE));
  builder.removeCreature(9000004);

  std::string const sql (builder.build());

  CHECK(contains(sql, "DELETE FROM `creature` WHERE `guid` = @CGUID;"));
  CHECK(contains(sql, "DELETE FROM `creature_addon` WHERE `guid` = @CGUID;"));

  // Nothing is written back for a spawn that is going away. In particular the addon row is not
  // re-created from the defaults, which would leave a row bound to a creature that no longer
  // exists.
  CHECK_FALSE(contains(sql, "INSERT INTO"));
}

TEST_CASE("a guid that is both written and removed is refused", "[changeset][removal][validation]")
{
  // DELETE runs before INSERT, so the row would be removed and put straight back. Either intent
  // is plausible -- "delete it" and "move it" -- and guessing gets it wrong half the time, in a
  // direction the user is told nothing about.
  SECTION("creature")
  {
    ChangesetBuilder builder (modelFrom(REAL_FIXTURE));
    builder.addCreature(referenceCreature());
    builder.removeCreature(referenceCreature().guid);

    CHECK_THROWS_AS(builder.build(), ChangesetError);
  }

  SECTION("gameobject")
  {
    ChangesetBuilder builder (modelFrom(REAL_FIXTURE));
    builder.addGameObject(referenceGameObject());
    builder.removeGameObject(referenceGameObject().guid);

    CHECK_THROWS_AS(builder.build(), ChangesetError);
  }

  SECTION("a created spawn does not conflict with a removal on the same number")
  {
    // The provisional guid is an editor-side identity in a different namespace entirely. A real
    // removal that happens to carry the same number is not the same row.
    ChangesetBuilder builder (modelFrom(REAL_FIXTURE));
    builder.addNewCreature(newCreature());
    builder.removeCreature(PROVISIONAL_GUID);

    CHECK_NOTHROW(builder.build());
  }
}

TEST_CASE("removing guid 0 is refused", "[changeset][removal][validation]")
{
  // 0 is the core's "no spawn" sentinel and no row carries it, so this DELETE names nothing --
  // but it reads as though something was removed, which is the part that matters. It is also
  // what an uninitialised SpawnRef reaching the emitter would look like.
  ChangesetBuilder builder (modelFrom(REAL_FIXTURE));
  builder.removeCreature(0);

  CHECK_THROWS_AS(builder.build(), ChangesetError);
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

TEST_CASE("rotationComponent works to nine significant digits, not six decimals"
         , "[changeset][sqlformat]")
{
  // Nine SIGNIFICANT digits, so the number of decimals follows the magnitude. A quaternion
  // component lives in [-1, 1], where six decimals would be only six significant digits.
  CHECK(SqlFormat::rotationComponent(0.0) == "0.000000000");
  CHECK(SqlFormat::rotationComponent(1.0) == "1.000000000");
  CHECK(SqlFormat::rotationComponent(-1.0) == "-1.000000000");
  CHECK(SqlFormat::rotationComponent(0.25) == "0.250000000");

  // A component one decade smaller gets one more decimal place, because nine significant digits
  // of 0.0479425549 do not fit in nine decimals.
  CHECK(SqlFormat::rotationComponent(0.0479425549) == "0.0479425549");

  // The value from the defect report, as stored in the FLOAT column and as computed in double.
  CHECK(SqlFormat::rotationComponent(asStoredFloat(std::sin(0.5))) == "0.479425550");
  CHECK(SqlFormat::rotationComponent(std::sin(0.5)) == "0.479425539");

  // Fixed notation throughout: the default float format would emit 1.61554457e-15 for a
  // rotation3 derived from an orientation near pi, and the emitter's other guarantee is that no
  // exponent reaches the SQL.
  CHECK(SqlFormat::rotationComponent(1.6155445744325867e-15)
          == "0.00000000000000161554457");
  CHECK(SqlFormat::rotationComponent(1.0e-9).find('e') == std::string::npos);
  CHECK(SqlFormat::rotationComponent(1.0e-30).find('e') == std::string::npos);

  // "-0" is as unwelcome here as it is in a coordinate: a value with no significant digit left
  // must not keep a sign that means nothing.
  CHECK(SqlFormat::rotationComponent(-0.0) == "0.000000000");

  // A negative that does survive keeps its sign, to all nine digits.
  CHECK(SqlFormat::rotationComponent(asStoredFloat(-std::sin(0.5))) == "-0.479425550");

  // A finite value too large for a FLOAT is not a quaternion component at all, and narrowing it to
  // one is undefined behaviour rather than merely lossy. It is still formatted rather than refused:
  // reporting the shape of a quaternion is validation's job, and saying what the value was is
  // this function's.
  CHECK(SqlFormat::rotationComponent(1.0e40).find('e') == std::string::npos);
  CHECK(SqlFormat::rotationComponent(-1.0e40).front() == '-');
}

TEST_CASE("rotationComponent round-trips a float where coordinate cannot"
         , "[changeset][sqlformat]")
{
  // float -> text -> float, unchanged, for values that actually occur in rotation0..3. sin(0.5)
  // and cos(0.5) are the quaternion of a one-radian yaw; the rest are the components of the
  // eighth-turn orientations, plus the awkward cases at the ends of the range.
  std::vector<double> values
    { std::sin(0.5), std::cos(0.5)
    , 1.0, -1.0, 0.0, 0.5, -0.5, 0.25
    , 1.0 / 3.0, 0.1, 0.0123456789, 0.968246, -0.707107
    , 1.6155445744325867e-15
    };

  for (int k (0); k < 16; ++k)
  {
    double const half_orientation (static_cast<double>(k) * 3.14159265358979 / 16.0);
    values.push_back(std::sin(half_orientation));
    values.push_back(std::cos(half_orientation));
  }

  for (double const raw : values)
  {
    // Both the double the maths produced and the float the column would hold, because the
    // editor sees the second and derives the first.
    for (double const value : {raw, asStoredFloat(raw)})
    {
      std::string const text (SqlFormat::rotationComponent(value));

      CAPTURE(value, text);

      CHECK(bitsOf(reparsed(text)) == bitsOf(static_cast<float>(value)));
      CHECK(text.find('e') == std::string::npos);
      CHECK(text.find('E') == std::string::npos);
    }
  }
}

TEST_CASE("rotationComponent tells two adjacent floats apart", "[changeset][sqlformat]")
{
  // What "round-trips a float exactly" means, stated as the property rather than as a list of
  // values: no two distinct floats may format to the same text, or one of them is being written
  // as the other.
  float const stored (static_cast<float>(std::sin(0.5)));
  float const neighbour (std::nextafter(stored, 1.0f));

  REQUIRE(bitsOf(stored) != bitsOf(neighbour));

  CHECK(SqlFormat::rotationComponent(stored) != SqlFormat::rotationComponent(neighbour));

  // And the reason a second formatter had to exist: at six decimals the two are the same text,
  // so whichever one was in the database, the other one is what gets written back.
  CHECK(SqlFormat::coordinate(stored) == SqlFormat::coordinate(neighbour));
}

TEST_CASE("rotationComponent refuses a value that is not a number"
         , "[changeset][sqlformat][safety]")
{
  // A NaN or infinite quaternion component is not a rotation at all. Substituting the identity
  // would turn the object to face a direction nobody chose, and emitting "nan" produces a syntax
  // error part-way through a file whose DELETE statements have already committed.
  CHECK_THROWS_AS
    (SqlFormat::rotationComponent(std::numeric_limits<double>::quiet_NaN()), ChangesetError);
  CHECK_THROWS_AS
    (SqlFormat::rotationComponent(std::numeric_limits<double>::infinity()), ChangesetError);
  CHECK_THROWS_AS
    (SqlFormat::rotationComponent(-std::numeric_limits<double>::infinity()), ChangesetError);
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

