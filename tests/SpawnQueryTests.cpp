// This file is part of Noggit3, licensed under GNU General Public License (version 3).

// Pure tests for the tile spawn reader.
//
// Nothing here touches a database. The SQL builders are pure functions of a SchemaModel and a
// set of bounds, and the row decoders are pure functions of a result row, which is exactly what
// makes the interesting failure modes -- the wrong column name, a filter on a derived column, a
// tile whose bounds do not match its index -- testable on a machine with no MySQL at all.
// Live behaviour is covered separately by the integration suite.

#include <catch2/catch_test_macros.hpp>

#include <FixtureLoader.hpp>
#include <noggit/database/SchemaModel.hpp>
#include <noggit/database/SpawnQuery.hpp>
#include <noggit/database/SpawnTypes.hpp>
#include <noggit/database/TileCoordinates.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

using namespace Noggit::Database;
using Noggit::Tests::fixturePath;
using Noggit::Tests::loadSchemaFixture;

// Implementation entry points defined in SpawnQuery.cpp but deliberately kept out of the
// header: they are not part of the module's contract, only of how it keeps its promises.
// Declaring them here rather than in a header means no test-only surface ships in src/.
namespace Noggit::Database::SpawnQuery
{
  bool isPlainIdentifier(std::string const& name);

  std::string waypointSelectSql
    (SchemaModel const& schema, std::vector<std::uint32_t> const& path_ids);

  CreatureSpawn parseCreatureRow(std::vector<std::string> const& row);
  GameObjectSpawn parseGameObjectRow(std::vector<std::string> const& row);
  WaypointNode parseWaypointRow(std::vector<std::string> const& row);
}

namespace
{
  constexpr char const* REAL_FIXTURE = "schema-tdb335-25101.tsv";
  constexpr char const* DRIFTED_FIXTURE = "schema-alt-drifted.tsv";

  // The tile the coordinate math was verified against: x ~ -9500, y ~ 70 on map 0 is Elwynn
  // Forest, tile (49, 31).
  constexpr int ELWYNN_TILE_X = 49;
  constexpr int ELWYNN_TILE_Y = 31;
  constexpr double ELWYNN_SPAWN_X = -9500.0;
  constexpr double ELWYNN_SPAWN_Y = 70.0;

  SchemaModel modelFrom(char const* fixture)
  {
    return SchemaModel(loadSchemaFixture(fixturePath(fixture)));
  }

  // The inverse of blockX = floor(32 - x / TILE_SIZE), written out here rather than taken from
  // TileCoordinates::boundsForTile so this file tests the query builder against the documented
  // formula instead of against another module's opinion of it.
  TileBounds boundsByFormula(int tile_x, int tile_y)
  {
    TileBounds bounds;
    bounds.min_x = (TILE_ORIGIN - (tile_x + 1)) * TILE_SIZE;
    bounds.max_x = (TILE_ORIGIN - tile_x) * TILE_SIZE;
    bounds.min_y = (TILE_ORIGIN - (tile_y + 1)) * TILE_SIZE;
    bounds.max_y = (TILE_ORIGIN - tile_y) * TILE_SIZE;
    return bounds;
  }

  TileBounds elwynnBounds()
  {
    return boundsByFormula(ELWYNN_TILE_X, ELWYNN_TILE_Y);
  }

  bool contains(std::string const& haystack, std::string const& needle)
  {
    return haystack.find(needle) != std::string::npos;
  }

  std::string lowered(std::string const& text)
  {
    std::string out;
    out.reserve(text.size());

    for (char c : text)
    {
      out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }

    return out;
  }

  bool containsNoCase(std::string const& haystack, std::string const& needle)
  {
    return contains(lowered(haystack), lowered(needle));
  }

  // Number of expressions in the SELECT list. The select lists built here contain no commas
  // outside the separators, so counting them is enough, and it is the only way to check that
  // the statement and the positional row decoder still agree on the shape of a row.
  std::size_t selectColumnCount(std::string const& sql)
  {
    std::size_t const start (sql.find("SELECT "));
    std::size_t const end (sql.find("\nFROM "));

    if (start == std::string::npos || end == std::string::npos || end <= start)
    {
      return 0;
    }

    std::string const list (sql.substr(start + 7, end - start - 7));

    return static_cast<std::size_t>(std::count(list.begin(), list.end(), ',')) + 1;
  }

  std::vector<std::string> numberedRow(std::size_t width)
  {
    std::vector<std::string> row;
    row.reserve(width);

    for (std::size_t i (0); i < width; ++i)
    {
      row.push_back(std::to_string(i + 1));
    }

    return row;
  }

  // Coordinates live in FLOAT columns, so nothing here compares them by equality even when the
  // value never went near a database.
  bool near(double a, double b, double tolerance = 1e-6)
  {
    return std::fabs(a - b) < tolerance;
  }

  // A minimal but self-consistent creature table, for the shapes no captured fixture has.
  std::vector<ColumnInfo> minimalCreatureColumns(bool with_wander)
  {
    std::vector<ColumnInfo> columns;
    int position (0);

    auto const add
      ( [&columns, &position] (char const* table, char const* column)
        {
          ColumnInfo info;
          info.table_name = table;
          info.column_name = column;
          info.ordinal_position = ++position;
          columns.push_back(info);
        }
      );

    add("creature", "guid");
    add("creature", "id");
    add("creature", "map");
    add("creature", "position_x");
    add("creature", "position_y");
    add("creature", "position_z");
    add("creature", "orientation");

    if (with_wander)
    {
      add("creature", "wander_distance");
    }

    return columns;
  }
}

TEST_CASE("the wander distance column comes from the schema, not from a literal"
         , "[spawnquery][sql][drift]")
{
  TileBounds const bounds (elwynnBounds());

  std::string const real (SpawnQuery::creatureSelectSql(modelFrom(REAL_FIXTURE), 0, bounds));
  std::string const drifted
    (SpawnQuery::creatureSelectSql(modelFrom(DRIFTED_FIXTURE), 0, bounds));

  // The whole reason the SchemaModel exists. Naming the wrong one here is an unknown-column
  // error against one database and correct against the other.
  CHECK(contains(real, "`wander_distance`"));
  CHECK_FALSE(containsNoCase(real, "spawndist"));

  CHECK(contains(drifted, "`spawndist`"));
  CHECK_FALSE(containsNoCase(drifted, "wander_distance"));

  // A builder that returned the same text for both schemas would pass every other assertion in
  // this file while being a hardcode with extra steps.
  CHECK(real != drifted);
}

TEST_CASE("spawn queries never filter on the core-derived zoneId or areaId"
         , "[spawnquery][sql][safety]")
{
  SchemaModel const schema (modelFrom(REAL_FIXTURE));
  TileBounds const bounds (elwynnBounds());

  // Both columns exist on this schema, so their absence from the SQL is a choice, not an
  // accident of the fixture.
  REQUIRE(schema.hasColumn("creature", "zoneId"));
  REQUIRE(schema.hasColumn("gameobject", "areaId"));

  std::string const creatures (SpawnQuery::creatureSelectSql(schema, 0, bounds));
  std::string const gameobjects (SpawnQuery::gameObjectSelectSql(schema, 0, bounds));

  CHECK_FALSE(containsNoCase(creatures, "zoneid"));
  CHECK_FALSE(containsNoCase(creatures, "areaid"));
  CHECK_FALSE(containsNoCase(gameobjects, "zoneid"));
  CHECK_FALSE(containsNoCase(gameobjects, "areaid"));
}

TEST_CASE("the map filter and both coordinate bounds reach the WHERE clause"
         , "[spawnquery][sql]")
{
  SchemaModel const schema (modelFrom(REAL_FIXTURE));
  TileBounds const bounds (elwynnBounds());

  std::string const creatures (SpawnQuery::creatureSelectSql(schema, 0, bounds));
  std::string const gameobjects (SpawnQuery::gameObjectSelectSql(schema, 571, bounds));

  CHECK(contains(creatures, "WHERE"));
  CHECK(contains(creatures, "`map` = 0"));
  CHECK(contains(gameobjects, "`map` = 571"));

  // Half-open on both axes: >= min, < max, so a spawn on a shared edge is read once.
  CHECK(contains(creatures, "`position_x` >= "));
  CHECK(contains(creatures, "`position_x` < "));
  CHECK(contains(creatures, "`position_y` >= "));
  CHECK(contains(creatures, "`position_y` < "));

  CHECK(contains(gameobjects, "`position_x` >= "));
  CHECK(contains(gameobjects, "`position_y` < "));

  // Ordering by guid keeps two reads of an unchanged tile comparable.
  CHECK(contains(creatures, "ORDER BY"));
}

TEST_CASE("bounds for tile (49,31) carry that tile's numbers", "[spawnquery][sql][tile]")
{
  TileBounds const bounds (elwynnBounds());

  // Sanity on the fixture itself: the verified real spawn must sit inside the bounds this test
  // then looks for in the SQL. Without this the assertions below would happily confirm the
  // wrong tile.
  CHECK(bounds.min_x <= ELWYNN_SPAWN_X);
  CHECK(ELWYNN_SPAWN_X < bounds.max_x);
  CHECK(bounds.min_y <= ELWYNN_SPAWN_Y);
  CHECK(ELWYNN_SPAWN_Y < bounds.max_y);

  // Increasing world x gives a decreasing tile index, so tile 49 is at negative x. A sign
  // error here still produces plausible-looking indices, which is why the numbers are pinned.
  CHECK(near(bounds.min_x, -18.0 * TILE_SIZE, 1e-4));
  CHECK(near(bounds.max_x, -17.0 * TILE_SIZE, 1e-4));
  CHECK(near(bounds.min_y, 0.0, 1e-4));
  CHECK(near(bounds.max_y, TILE_SIZE, 1e-4));
  CHECK(near(bounds.max_x - bounds.min_x, TILE_SIZE, 1e-4));

  std::string const sql (SpawnQuery::creatureSelectSql(modelFrom(REAL_FIXTURE), 0, bounds));

  CHECK(contains(sql, "-9599.9999"));   // 18 tiles west of the origin
  CHECK(contains(sql, "-9066.6666"));   // 17 tiles west of the origin
  CHECK(contains(sql, "0.000000"));     // the y origin, tile row 31/32 boundary
  CHECK(contains(sql, "533.3333"));

  // No exponent notation: it is legal SQL but unreadable in a log and unmatchable in a test.
  CHECK_FALSE(containsNoCase(sql, "e+"));

  // A different tile must produce different bounds, or the tile argument is being ignored.
  std::string const other
    (SpawnQuery::creatureSelectSql(modelFrom(REAL_FIXTURE), 0, boundsByFormula(48, 31)));

  CHECK(sql != other);
  CHECK_FALSE(contains(other, "-9599.9999"));
}

TEST_CASE("creatures are left joined to their addon so path_id comes back with them"
         , "[spawnquery][sql]")
{
  std::string const sql
    (SpawnQuery::creatureSelectSql(modelFrom(REAL_FIXTURE), 0, elwynnBounds()));

  CHECK(contains(sql, "LEFT JOIN `creature_addon`"));
  CHECK(contains(sql, "`path_id`"));

  // An INNER JOIN would silently drop every creature without an addon row, which is most of
  // them.
  CHECK_FALSE(containsNoCase(sql, "inner join"));

  // has_addon has to be its own column: a missing addon row and an addon row holding path_id 0
  // are indistinguishable otherwise, and only one of them means "no addon".
  CHECK(contains(sql, "has_addon"));
  CHECK(contains(sql, "IS NOT NULL"));
}

TEST_CASE("a schema without creature_addon still produces a readable row", "[spawnquery][sql]")
{
  // The select list keeps its width with literals rather than changing shape, so the
  // positional decoder never has to branch on the schema.
  SchemaModel const schema (minimalCreatureColumns(true));

  std::string const sql (SpawnQuery::creatureSelectSql(schema, 0, elwynnBounds()));

  CHECK_FALSE(containsNoCase(sql, "left join"));
  CHECK(contains(sql, "0 AS `path_id`"));
  CHECK(contains(sql, "0 AS `has_addon`"));

  // Columns this schema does not have are still selected as literals, at the same positions.
  CHECK(contains(sql, "AS `curhealth`"));
  CHECK(contains(sql, "AS `MovementType`"));
  CHECK(selectColumnCount(sql) == 22);
}

TEST_CASE("the gameobject query reads the quaternion, not euler angles", "[spawnquery][sql]")
{
  std::string const sql
    (SpawnQuery::gameObjectSelectSql(modelFrom(REAL_FIXTURE), 0, elwynnBounds()));

  CHECK(contains(sql, "`rotation0`"));
  CHECK(contains(sql, "`rotation1`"));
  CHECK(contains(sql, "`rotation2`"));
  CHECK(contains(sql, "`rotation3`"));
  CHECK(contains(sql, "`orientation`"));
  CHECK(contains(sql, "FROM `gameobject`"));

  // gameobject has no wander distance and no addon join of its own.
  CHECK_FALSE(containsNoCase(sql, "wander"));
  CHECK_FALSE(containsNoCase(sql, "join"));
}

TEST_CASE("the waypoint query never reads the core-managed wpguid", "[spawnquery][sql]")
{
  SchemaModel const schema (modelFrom(REAL_FIXTURE));

  REQUIRE(schema.hasColumn("waypoint_data", "wpguid"));

  std::string const sql (SpawnQuery::waypointSelectSql(schema, {90000040u, 12u}));

  CHECK_FALSE(containsNoCase(sql, "wpguid"));
  CHECK(contains(sql, "FROM `waypoint_data`"));
  // Wrapped in parentheses: Catch2's expression decomposer cannot handle a bare || inside an
  // assertion. Either ordering is acceptable since the set is unordered.
  CHECK((contains(sql, "IN (12, 90000040)") || contains(sql, "IN (90000040, 12)")));

  // point is 1-based and the core walks it in order, so the statement must impose that order
  // rather than leave it to the server.
  CHECK(contains(sql, "ORDER BY"));
  CHECK(contains(sql, "`point`"));
  CHECK(selectColumnCount(sql) == 10);

  // An empty id list must still be a legal statement rather than "IN ()".
  CHECK_FALSE(contains(SpawnQuery::waypointSelectSql(schema, {}), "IN ()"));
}

TEST_CASE("nothing but validated identifiers and numbers is interpolated"
         , "[spawnquery][safety]")
{
  // Column names cannot be bound as parameters, so the schema-resolved ones are interpolated.
  // They are refused rather than escaped when they are not plain identifiers.
  CHECK(SpawnQuery::isPlainIdentifier("wander_distance"));
  CHECK(SpawnQuery::isPlainIdentifier("spawndist"));
  CHECK(SpawnQuery::isPlainIdentifier("waypoint_data"));
  CHECK(SpawnQuery::isPlainIdentifier("a$b_9"));

  CHECK_FALSE(SpawnQuery::isPlainIdentifier(""));
  CHECK_FALSE(SpawnQuery::isPlainIdentifier("has space"));
  CHECK_FALSE(SpawnQuery::isPlainIdentifier("back`tick"));
  CHECK_FALSE(SpawnQuery::isPlainIdentifier("quote'mark"));
  CHECK_FALSE(SpawnQuery::isPlainIdentifier("wander_distance`; DROP TABLE `creature"));
  CHECK_FALSE(SpawnQuery::isPlainIdentifier("creature; --"));
  CHECK_FALSE(SpawnQuery::isPlainIdentifier(std::string(65, 'x')));

  // Every identifier either schema could hand the builder passes that rule.
  CHECK(SpawnQuery::isPlainIdentifier(modelFrom(REAL_FIXTURE).wanderDistanceColumn()));
  CHECK(SpawnQuery::isPlainIdentifier(modelFrom(DRIFTED_FIXTURE).wanderDistanceColumn()));

  // And the generated statements contain no string literal, no statement separator and no
  // comment introducer at all, so there is nothing for an injected value to break out of.
  SchemaModel const schema (modelFrom(REAL_FIXTURE));
  TileBounds const bounds (elwynnBounds());

  std::string const creatures (SpawnQuery::creatureSelectSql(schema, 0, bounds));
  std::string const gameobjects (SpawnQuery::gameObjectSelectSql(schema, 0, bounds));
  std::string const waypoints (SpawnQuery::waypointSelectSql(schema, {1u, 2u}));

  for (std::string const& sql : {creatures, gameobjects, waypoints})
  {
    CHECK(sql.find('\'') == std::string::npos);
    CHECK(sql.find('"') == std::string::npos);
    CHECK(sql.find(';') == std::string::npos);
    CHECK(sql.find("--") == std::string::npos);
    CHECK(sql.find("/*") == std::string::npos);
  }
}

TEST_CASE("an unusable schema or unusable bounds are refused, not papered over"
         , "[spawnquery][safety]")
{
  TileBounds const bounds (elwynnBounds());

  // No creature table at all.
  CHECK_THROWS_AS
    (SpawnQuery::creatureSelectSql(SchemaModel(), 0, bounds), SchemaCapabilityError);
  CHECK_THROWS_AS
    (SpawnQuery::gameObjectSelectSql(SchemaModel(), 0, bounds), SchemaCapabilityError);
  CHECK_THROWS_AS(SpawnQuery::waypointSelectSql(SchemaModel(), {1u}), SchemaCapabilityError);

  // A creature table carrying neither wander_distance nor spawndist. Guessing one would
  // produce a statement that fails at the server; guessing neither would silently drop the
  // wander radius on the next write.
  CHECK_THROWS_AS
    ( SpawnQuery::creatureSelectSql(SchemaModel(minimalCreatureColumns(false)), 0, bounds)
    , SchemaCapabilityError
    );

  SchemaModel const schema (modelFrom(REAL_FIXTURE));

  // Bounds that match nothing are a caller error, and an empty result would be read as an
  // empty tile rather than as a bug.
  TileBounds inverted (bounds);
  std::swap(inverted.min_x, inverted.max_x);
  CHECK_THROWS_AS(SpawnQuery::creatureSelectSql(schema, 0, inverted), std::invalid_argument);

  TileBounds not_a_number (bounds);
  not_a_number.min_y = std::numeric_limits<double>::quiet_NaN();
  CHECK_THROWS_AS
    (SpawnQuery::creatureSelectSql(schema, 0, not_a_number), std::invalid_argument);

  TileBounds infinite (bounds);
  infinite.max_x = std::numeric_limits<double>::infinity();
  CHECK_THROWS_AS(SpawnQuery::gameObjectSelectSql(schema, 0, infinite), std::invalid_argument);
}

TEST_CASE("a creature row decodes to the spawn it describes", "[spawnquery][parse]")
{
  // The reference changeset's spawn, as the server would hand it back.
  std::vector<std::string> const row
    { "9000004", "990001", "0", "1", "1", "0", "-1"
    , "-9512.345", "83.117", "58.271", "4.7124"
    , "120", "5.5", "0", "1", "0"
    , "2", "0", "0", "0"
    , "90000040", "1"
    };

  CreatureSpawn const spawn (SpawnQuery::parseCreatureRow(row));

  CHECK(spawn.guid == 9000004u);
  CHECK(spawn.id == 990001u);
  CHECK(spawn.map == 0);
  CHECK(spawn.spawn_mask == 1);
  CHECK(spawn.phase_mask == 1u);
  CHECK(spawn.model_id == 0u);

  // equipment_id is signed: -1 means "random", which an unsigned read would turn into 255.
  CHECK(spawn.equipment_id == -1);

  CHECK(near(spawn.position.x, -9512.345));
  CHECK(near(spawn.position.y, 83.117));
  CHECK(near(spawn.position.z, 58.271));
  CHECK(near(spawn.orientation, 4.7124));

  CHECK(spawn.spawn_time_secs == 120u);
  CHECK(near(spawn.wander_distance, 5.5));
  CHECK(spawn.cur_health == 1u);
  CHECK(spawn.movement_type == MovementType::WAYPOINT);

  CHECK(spawn.has_addon);
  CHECK(spawn.path_id == 90000040u);
}

TEST_CASE("a creature with no addon row is not mistaken for one with path 0"
         , "[spawnquery][parse]")
{
  std::vector<std::string> row (numberedRow(22));

  // What a LEFT JOIN that matched nothing looks like: both columns arrive as SQL NULL, which
  // the reader renders as an empty string.
  row[20] = "";
  row[21] = "0";

  CreatureSpawn const missing (SpawnQuery::parseCreatureRow(row));

  CHECK_FALSE(missing.has_addon);
  CHECK(missing.path_id == 0u);

  // An addon row that exists but binds no path is a different fact, and stays one.
  row[20] = "0";
  row[21] = "1";

  CreatureSpawn const present (SpawnQuery::parseCreatureRow(row));

  CHECK(present.has_addon);
  CHECK(present.path_id == 0u);
}

TEST_CASE("a short, empty or nonsense row decodes rather than crashing"
         , "[spawnquery][parse][safety]")
{
  // Truncated after map. Everything past the end of the row reads as 0 instead of off the end
  // of the vector.
  CreatureSpawn const truncated (SpawnQuery::parseCreatureRow({"42", "17", "0"}));

  CHECK(truncated.guid == 42u);
  CHECK(truncated.id == 17u);
  CHECK(truncated.map == 0);
  CHECK(truncated.cur_health == 0u);
  CHECK(truncated.movement_type == MovementType::IDLE);
  CHECK_FALSE(truncated.has_addon);
  CHECK(near(truncated.position.x, 0.0));

  CreatureSpawn const nothing (SpawnQuery::parseCreatureRow({}));
  CHECK(nothing.guid == 0u);

  GameObjectSpawn const no_gameobject (SpawnQuery::parseGameObjectRow({}));
  CHECK(no_gameobject.guid == 0u);

  WaypointNode const no_node (SpawnQuery::parseWaypointRow({}));
  CHECK(no_node.point == 0u);
  CHECK_FALSE(no_node.has_orientation);

  // Empty and unparseable fields are zeroes, not exceptions.
  std::vector<std::string> junk (22, std::string("not a number"));
  junk[0] = "";

  CreatureSpawn const garbage (SpawnQuery::parseCreatureRow(junk));

  CHECK(garbage.guid == 0u);
  CHECK(near(garbage.position.x, 0.0));
  CHECK(garbage.movement_type == MovementType::IDLE);

  // A guid wider than the column can hold saturates rather than wrapping to a small number
  // that collides with a real spawn.
  std::vector<std::string> huge (numberedRow(22));
  huge[0] = "99999999999999";

  CHECK(SpawnQuery::parseCreatureRow(huge).guid == 4294967295u);

  // An unknown movement type is read as idle rather than as an enumerator that does not exist.
  std::vector<std::string> odd (numberedRow(22));
  odd[16] = "9";

  CHECK(SpawnQuery::parseCreatureRow(odd).movement_type == MovementType::IDLE);
}

TEST_CASE("the select list and the row decoder agree on the shape of a row"
         , "[spawnquery][parse]")
{
  SchemaModel const schema (modelFrom(REAL_FIXTURE));
  TileBounds const bounds (elwynnBounds());

  // If a column is ever added to or removed from a select list without the decoder moving with
  // it, every field past that point silently shifts by one. These counts are the only thing
  // holding the two halves of the module together.
  CHECK(selectColumnCount(SpawnQuery::creatureSelectSql(schema, 0, bounds)) == 22);
  CHECK(selectColumnCount(SpawnQuery::gameObjectSelectSql(schema, 0, bounds)) == 16);
  CHECK(selectColumnCount(SpawnQuery::waypointSelectSql(schema, {1u})) == 10);

  // The last creature column really is has_addon: a row one field short reports no addon.
  std::vector<std::string> const full (numberedRow(22));

  CHECK(SpawnQuery::parseCreatureRow(full).has_addon);
  CHECK_FALSE(SpawnQuery::parseCreatureRow(numberedRow(21)).has_addon);
}

TEST_CASE("a gameobject row decodes with its quaternion intact", "[spawnquery][parse]")
{
  // rotation2 = sin(o/2), rotation3 = cos(o/2) for o = 3*pi/2.
  std::vector<std::string> const row
    { "9000010", "990002", "0", "1", "1"
    , "-9512.345", "83.117", "58.271", "4.7124"
    , "0", "0", "-0.707107", "0.707107"
    , "300", "100", "1"
    };

  GameObjectSpawn const spawn (SpawnQuery::parseGameObjectRow(row));

  CHECK(spawn.guid == 9000010u);
  CHECK(spawn.id == 990002u);
  CHECK(near(spawn.position.x, -9512.345));
  CHECK(near(spawn.orientation, 4.7124));

  // Read as stored, never recomputed from the orientation: the two can legitimately disagree
  // in real data and the database is the authority.
  CHECK(near(spawn.rotation.r0, 0.0));
  CHECK(near(spawn.rotation.r1, 0.0));
  CHECK(near(spawn.rotation.r2, -0.707107));
  CHECK(near(spawn.rotation.r3, 0.707107));

  CHECK(spawn.spawn_time_secs == 300);
  CHECK(spawn.anim_progress == 100u);
  CHECK(spawn.state == 1u);

  // A missing rotation3 is the identity rotation, not the zero quaternion: (0,0,0,0) is not a
  // rotation at all. A stored 0 is still read as 0, since cos(pi/2) is legitimately zero.
  CHECK(near(SpawnQuery::parseGameObjectRow({"1"}).rotation.r3, 1.0));

  std::vector<std::string> explicit_zero (row);
  explicit_zero[12] = "0";
  CHECK(near(SpawnQuery::parseGameObjectRow(explicit_zero).rotation.r3, 0.0));
}

TEST_CASE("a waypoint row keeps NULL orientation distinct from zero", "[spawnquery][parse]")
{
  // id, point, x, y, z, orientation, delay, move_type, action, action_chance.
  std::vector<std::string> row
    { "90000040", "3", "-9512.345", "83.117", "58.271", "", "1500", "1", "0", "100" };

  WaypointNode const null_orientation (SpawnQuery::parseWaypointRow(row));

  // point comes from column 1, not from the path id in column 0.
  CHECK(null_orientation.point == 3u);
  CHECK_FALSE(null_orientation.has_orientation);
  CHECK(near(null_orientation.orientation, 0.0));
  CHECK(null_orientation.delay_ms == 1500u);
  CHECK(null_orientation.move_type == WaypointMoveType::RUN);
  CHECK(null_orientation.action_chance == 100u);
  CHECK(near(null_orientation.position.y, 83.117));

  // The column is nullable and usually NULL, so "facing north" and "no facing" must not read
  // the same.
  row[5] = "0";
  WaypointNode const zero_orientation (SpawnQuery::parseWaypointRow(row));

  CHECK(zero_orientation.has_orientation);
  CHECK(near(zero_orientation.orientation, 0.0));

  row[5] = "3.14159";
  CHECK(near(SpawnQuery::parseWaypointRow(row).orientation, 3.14159));

  // move_type is a closed set of four; anything else walks.
  row[7] = "3";
  CHECK(SpawnQuery::parseWaypointRow(row).move_type == WaypointMoveType::TAKEOFF);

  row[7] = "77";
  CHECK(SpawnQuery::parseWaypointRow(row).move_type == WaypointMoveType::WALK);
}
