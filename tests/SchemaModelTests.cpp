// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <catch2/catch_test_macros.hpp>

#include <FixtureLoader.hpp>
#include <noggit/database/SchemaModel.hpp>

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
}

TEST_CASE("fixtures load and are non-trivial", "[schema][fixture]")
{
  SchemaModel const real (modelFrom(REAL_FIXTURE));
  SchemaModel const drifted (modelFrom(DRIFTED_FIXTURE));

  // A fixture that silently loaded empty would make every assertion below vacuously pass.
  CHECK(real.tableCount() >= 40);
  CHECK(real.columnCount() >= 400);
  CHECK(drifted.tableCount() >= 10);
  CHECK(drifted.columnCount() >= 100);
}

TEST_CASE("real TDB 335.25101 schema resolves to 3.3.5 shapes", "[schema][real]")
{
  SchemaModel const m (modelFrom(REAL_FIXTURE));

  CHECK(m.wanderDistanceColumn() == "wander_distance");
  CHECK(m.versionTable() == "version");
  CHECK(m.poolMembersTable() == "pool_members");
  CHECK(m.waypointPathTable() == "waypoint_data");
  CHECK(m.addonPoseEncoding() == AddonPoseEncoding::DISCRETE_COLUMNS);
  CHECK(m.creatureModelSource() == CreatureModelSource::TEMPLATE_MODELID_COLUMNS);
  CHECK(m.smartScriptsEventParamCount() == 5);
  CHECK(m.smartScriptsTargetParamCount() == 4);

  // Facts the source research brief got wrong, asserted so a regression is loud.
  CHECK_FALSE(m.hasColumn("creature", "spawndist"));
  CHECK_FALSE(m.hasColumn("creature", "Comment"));
  CHECK_FALSE(m.hasColumn("creature_addon", "bytes1"));
  CHECK_FALSE(m.hasColumn("creature_addon", "bytes2"));
  CHECK_FALSE(m.hasTable("waypoints"));
  CHECK_FALSE(m.hasTable("script_waypoint"));
  CHECK_FALSE(m.hasTable("creature_template_model"));
  CHECK_FALSE(m.hasColumn("gameobject_template_addon", "artkit4"));
  CHECK(m.hasColumn("creature_model_info", "Gender"));
  CHECK_FALSE(m.hasColumn("creature_model_info", "VerifiedBuild"));
}

TEST_CASE("drifted schema resolves to the other variant everywhere", "[schema][drift]")
{
  SchemaModel const m (modelFrom(DRIFTED_FIXTURE));

  CHECK(m.wanderDistanceColumn() == "spawndist");
  CHECK(m.versionTable() == "version_db_world");
  CHECK(m.poolMembersTable() == "pool_creature");
  CHECK(m.waypointPathTable() == "waypoint_data");
  CHECK(m.addonPoseEncoding() == AddonPoseEncoding::PACKED_BYTES);
  CHECK(m.creatureModelSource() == CreatureModelSource::TEMPLATE_MODEL_TABLE);
  CHECK(m.smartScriptsEventParamCount() == 4);
  CHECK(m.smartScriptsTargetParamCount() == 3);

  CHECK(m.hasTable("waypoints"));
  CHECK(m.hasTable("script_waypoint"));
  CHECK(m.hasColumn("gameobject_template_addon", "artkit4"));
  CHECK(m.hasColumn("gameobject_template_addon", "WorldEffectID"));
}

TEST_CASE("the two fixtures disagree on every decision point", "[schema][drift]")
{
  // The point of the whole layer. A model that returns identical answers for both schemas is
  // broken even when one of those answers happens to be correct -- it means the branch is
  // never taken and the "capability model" is a hardcode with extra steps.
  SchemaModel const real (modelFrom(REAL_FIXTURE));
  SchemaModel const drifted (modelFrom(DRIFTED_FIXTURE));

  CHECK(real.wanderDistanceColumn() != drifted.wanderDistanceColumn());
  CHECK(real.versionTable() != drifted.versionTable());
  CHECK(real.poolMembersTable() != drifted.poolMembersTable());
  CHECK(real.addonPoseEncoding() != drifted.addonPoseEncoding());
  CHECK(real.creatureModelSource() != drifted.creatureModelSource());
  CHECK(real.smartScriptsEventParamCount() != drifted.smartScriptsEventParamCount());
  CHECK(real.smartScriptsTargetParamCount() != drifted.smartScriptsTargetParamCount());
}

TEST_CASE("an unrecognised schema throws rather than guessing", "[schema][safety]")
{
  // creature exists but carries neither wander_distance nor spawndist, and none of the other
  // discriminating tables are present.
  std::vector<ColumnInfo> columns;

  ColumnInfo guid;
  guid.table_name = "creature";
  guid.ordinal_position = 1;
  guid.column_name = "guid";
  columns.push_back(guid);

  SchemaModel const m (columns);

  CHECK_THROWS_AS(m.wanderDistanceColumn(), SchemaCapabilityError);
  CHECK_THROWS_AS(m.versionTable(), SchemaCapabilityError);
  CHECK_THROWS_AS(m.poolMembersTable(), SchemaCapabilityError);
  CHECK_THROWS_AS(m.waypointPathTable(), SchemaCapabilityError);
  CHECK_THROWS_AS(m.addonPoseEncoding(), SchemaCapabilityError);
  CHECK_THROWS_AS(m.creatureModelSource(), SchemaCapabilityError);
  CHECK_THROWS_AS(m.smartScriptsEventParamCount(), SchemaCapabilityError);
  CHECK_THROWS_AS(m.smartScriptsTargetParamCount(), SchemaCapabilityError);
}

TEST_CASE("an empty schema throws rather than reporting a default shape", "[schema][safety]")
{
  SchemaModel const m;

  CHECK(m.tableCount() == 0);
  CHECK_THROWS_AS(m.wanderDistanceColumn(), SchemaCapabilityError);
  CHECK_THROWS_AS(m.versionTable(), SchemaCapabilityError);
}

TEST_CASE("lookups ignore identifier case", "[schema][robustness]")
{
  // MySQL identifier case behaviour depends on platform and lower_case_table_names, so a
  // case-sensitive comparison passes on one machine and fails on the next.
  std::vector<ColumnInfo> columns;

  ColumnInfo c;
  c.table_name = "CREATURE";
  c.ordinal_position = 15;
  c.column_name = "Wander_Distance";
  columns.push_back(c);

  SchemaModel const m (columns);

  CHECK(m.hasTable("creature"));
  CHECK(m.hasTable("Creature"));
  CHECK(m.hasColumn("creature", "wander_distance"));
  CHECK(m.hasColumn("CrEaTuRe", "WANDER_DISTANCE"));
  CHECK(m.columnPosition("creature", "wander_distance") == 15);
  CHECK(m.wanderDistanceColumn() == "wander_distance");
}

TEST_CASE("column positions and absence report correctly", "[schema][basics]")
{
  SchemaModel const m (modelFrom(REAL_FIXTURE));

  // Ordinal positions matter: positional INSERT against the wrong order corrupts silently.
  CHECK(m.columnPosition("creature", "guid") == 1);
  CHECK(m.columnPosition("creature", "wander_distance") == 15);
  CHECK(m.columnPosition("creature", "zoneId") == 4);
  CHECK(m.columnPosition("creature", "areaId") == 5);

  CHECK(m.columnPosition("creature", "no_such_column") == 0);
  CHECK(m.columnPosition("no_such_table", "guid") == 0);
  CHECK_FALSE(m.hasTable("no_such_table"));
}

TEST_CASE("prefix counting does not over-match neighbouring columns", "[schema][basics]")
{
  SchemaModel const m (modelFrom(REAL_FIXTURE));

  // event_phase_mask, event_chance and event_flags all begin with "event_" and must not be
  // counted as event parameters.
  CHECK(m.countColumnsWithPrefix("smart_scripts", "event_param") == 5);
  CHECK(m.countColumnsWithPrefix("smart_scripts", "action_param") == 6);
  CHECK(m.countColumnsWithPrefix("smart_scripts", "target_param") == 4);
  CHECK(m.countColumnsWithPrefix("creature_template", "modelid") == 4);
  CHECK(m.countColumnsWithPrefix("smart_scripts", "no_such_prefix") == 0);
}
