// This file is part of Noggit3, licensed under GNU General Public License (version 3).

// Pure tests for the tile spawn reader.
//
// Nothing here touches a database, and nothing here needs one to be installed. The SQL builders
// are pure functions of a SchemaModel and a set of bounds, and the row decoders are pure functions
// of a result row, which is exactly what makes the interesting failure modes -- the wrong column
// name, a filter on a derived column, a tile whose bounds do not match its index, a row that came
// back short -- testable on a machine with no MySQL at all. All of it is defined in
// SpawnQueryDetail.cpp, which links no database client, so this file needs only that object and
// the schema model. Live behaviour is covered separately by the integration suite.
//
// The internal entry points come from SpawnQueryDetail.hpp. They used to be re-declared by hand at
// the top of this file, which was a second and unchecked source of truth for their signatures:
// they matched the definitions in SpawnQuery.cpp only because ResultRow is a typedef for
// std::vector<std::string>, and a changed parameter type would have shown up as a link error at
// best. Including the real header is what makes the compiler check them.

#include <catch2/catch_test_macros.hpp>

#include <FixtureLoader.hpp>
#include <noggit/database/SchemaModel.hpp>
#include <noggit/database/SpawnQuery.hpp>
#include <noggit/database/SpawnQueryDetail.hpp>
#include <noggit/database/SpawnTypes.hpp>
#include <noggit/database/TileCoordinates.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
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

  // Width of each select list, and the one place the expected widths are written down.
  //
  // 22 spawn and addon columns, then four template model candidates, then the template name. The
  // template columns were appended rather than placed beside creature.modelid precisely so that
  // every pre-existing decoder index stayed put.
  constexpr std::size_t CREATURE_SELECT_WIDTH = 27;
  constexpr std::size_t GAMEOBJECT_SELECT_WIDTH = 19;
  constexpr std::size_t WAYPOINT_SELECT_WIDTH = 10;

  // Decoder positions the assertions below poke at directly.
  constexpr std::size_t CREATURE_MODELID_INDEX = 5;
  constexpr std::size_t CREATURE_CANDIDATE_BASE = 22;
  constexpr std::size_t CREATURE_TEMPLATE_NAME_INDEX = 26;

  constexpr std::size_t GAMEOBJECT_DISPLAY_ID_INDEX = 16;
  constexpr std::size_t GAMEOBJECT_TYPE_INDEX = 17;
  constexpr std::size_t GAMEOBJECT_TEMPLATE_NAME_INDEX = 18;

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

  // A creature table plus a creature_template carrying exactly the named model columns, so that
  // each modelid can be checked on its own rather than inferred from modelid1's presence.
  std::vector<ColumnInfo> creatureColumnsWithTemplateModels
    (std::vector<std::string> const& model_columns)
  {
    std::vector<ColumnInfo> columns (minimalCreatureColumns(true));
    int position (0);

    auto const add
      ( [&columns, &position] (char const* table, std::string const& column)
        {
          ColumnInfo info;
          info.table_name = table;
          info.column_name = column;
          info.ordinal_position = ++position;
          columns.push_back(info);
        }
      );

    add("creature_template", "entry");
    add("creature_template", "name");

    for (std::string const& column : model_columns)
    {
      add("creature_template", column);
    }

    return columns;
  }

  // Rows at the real select-list width, all zeroes, ready to have the interesting fields set.
  // Built full width so a test never exercises the short-row path by accident -- that path has its
  // own case and conflating the two would let a shifted index pass.
  std::vector<std::string> creatureRow()
  {
    return std::vector<std::string>(CREATURE_SELECT_WIDTH, std::string("0"));
  }

  std::vector<std::string> gameObjectRow()
  {
    return std::vector<std::string>(GAMEOBJECT_SELECT_WIDTH, std::string("0"));
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

TEST_CASE("the creature model source comes from the schema, not from a literal"
         , "[spawnquery][sql][drift][display]")
{
  TileBounds const bounds (elwynnBounds());

  SchemaModel const real_schema (modelFrom(REAL_FIXTURE));
  SchemaModel const drifted_schema (modelFrom(DRIFTED_FIXTURE));

  // The fixtures take opposite branches here, which is the only reason this can be tested at all.
  REQUIRE(real_schema.creatureModelSource() == CreatureModelSource::TEMPLATE_MODELID_COLUMNS);
  REQUIRE(drifted_schema.creatureModelSource() == CreatureModelSource::TEMPLATE_MODEL_TABLE);

  std::string const real (SpawnQuery::creatureSelectSql(real_schema, 0, bounds));
  std::string const drifted (SpawnQuery::creatureSelectSql(drifted_schema, 0, bounds));

  // 3.3.5: four columns on creature_template, joined on its primary key.
  CHECK(contains(real, "LEFT JOIN `creature_template` AS t ON t.`entry` = c.`id`"));
  CHECK(contains(real, "t.`modelid1` AS `model_candidate_1`"));
  CHECK(contains(real, "t.`modelid2` AS `model_candidate_2`"));
  CHECK(contains(real, "t.`modelid3` AS `model_candidate_3`"));
  CHECK(contains(real, "t.`modelid4` AS `model_candidate_4`"));
  CHECK(contains(real, "t.`name` AS `template_name`"));
  CHECK_FALSE(containsNoCase(real, "creature_template_model"));

  // The drifted variant: creature_template_model, reached by correlated subquery.
  CHECK(contains(drifted, "`creature_template_model`"));
  CHECK(contains(drifted, "m.`CreatureDisplayID`"));
  CHECK(contains(drifted, "m.`CreatureID` = c.`id`"));
  CHECK(contains(drifted, "t.`name` AS `template_name`"));

  // No template modelid column is named. Checked against the template alias rather than against
  // the bare word, because `creature.modelid` -- the per-spawn override -- is still selected here
  // and is a different column with a different meaning.
  CHECK(contains(drifted, "c.`modelid`"));
  CHECK_FALSE(contains(drifted, "t.`modelid"));

  // A subquery, never a join. creature_template_model holds one row per model, so joining it
  // returns each creature once per model and multiplies every spawn in the tile -- four copies at
  // identical coordinates, which look like one object and count as four.
  CHECK_FALSE(containsNoCase(drifted, "join `creature_template_model`"));

  // An explicit ORDER BY inside each subquery. A bare LIMIT 1 may return a different row on each
  // execution, which would make the resolved model depend on the server's mood.
  CHECK(contains(drifted, "ORDER BY m.`Idx`"));
  CHECK(contains(drifted, "LIMIT 1 OFFSET 0"));
  CHECK(contains(drifted, "LIMIT 1 OFFSET 3"));

  // A builder returning the same text for both schemas would be a hardcode with extra steps.
  CHECK(real != drifted);
}

TEST_CASE("each template model column is resolved on its own", "[spawnquery][sql][display]")
{
  TileBounds const bounds (elwynnBounds());

  // modelid1 being present says nothing about modelid3. Inferring the rest from the source kind
  // would name a column that is not there and fail every tile read, and inferring nothing would
  // lose a model the template really does offer.
  SchemaModel const partial (creatureColumnsWithTemplateModels({"modelid1", "modelid2"}));

  std::string const sql (SpawnQuery::creatureSelectSql(partial, 0, bounds));

  CHECK(contains(sql, "t.`modelid1` AS `model_candidate_1`"));
  CHECK(contains(sql, "t.`modelid2` AS `model_candidate_2`"));
  CHECK(contains(sql, "0 AS `model_candidate_3`"));
  CHECK(contains(sql, "0 AS `model_candidate_4`"));
  CHECK_FALSE(contains(sql, "t.`modelid3`"));
  CHECK(selectColumnCount(sql) == CREATURE_SELECT_WIDTH);

  // A template with a name column but no model columns at all still contributes the name, so the
  // UI can label a spawn it cannot draw.
  SchemaModel const nameless (creatureColumnsWithTemplateModels({}));
  std::string const name_only (SpawnQuery::creatureSelectSql(nameless, 0, bounds));

  CHECK(contains(name_only, "t.`name` AS `template_name`"));
  CHECK(contains(name_only, "0 AS `model_candidate_1`"));
  CHECK(selectColumnCount(name_only) == CREATURE_SELECT_WIDTH);
}

TEST_CASE("the gameobject display id comes from the template, or degrades to nothing"
         , "[spawnquery][sql][drift][display]")
{
  TileBounds const bounds (elwynnBounds());

  SchemaModel const real_schema (modelFrom(REAL_FIXTURE));
  SchemaModel const drifted_schema (modelFrom(DRIFTED_FIXTURE));

  // The drifted fixture has no gameobject_template at all, which is what makes it the degradation
  // case rather than a second spelling.
  REQUIRE(real_schema.hasColumn("gameobject_template", "displayId"));
  REQUIRE_FALSE(drifted_schema.hasTable("gameobject_template"));

  std::string const real (SpawnQuery::gameObjectSelectSql(real_schema, 0, bounds));
  std::string const drifted (SpawnQuery::gameObjectSelectSql(drifted_schema, 0, bounds));

  CHECK(contains(real, "LEFT JOIN `gameobject_template` AS t ON t.`entry` = g.`id`"));
  CHECK(contains(real, "t.`displayId`"));
  CHECK(contains(real, "t.`type`"));
  CHECK(contains(real, "t.`name` AS `template_name`"));

  // Missing template: literals in the select list, no join, same width. A gameobject then resolves
  // to display id 0 and is drawn as a marker, which beats refusing to open the tile.
  CHECK_FALSE(containsNoCase(drifted, "gameobject_template"));
  CHECK_FALSE(containsNoCase(drifted, "join"));
  CHECK(contains(drifted, "0 AS `displayId`"));
  CHECK(contains(drifted, "0 AS `type`"));
  CHECK(contains(drifted, "NULL AS `template_name`"));

  CHECK(selectColumnCount(real) == GAMEOBJECT_SELECT_WIDTH);
  CHECK(selectColumnCount(drifted) == GAMEOBJECT_SELECT_WIDTH);
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

  // (min, max] on both axes: lower edge EXCLUSIVE, upper edge INCLUSIVE, because the axis runs
  // backwards. Asserted as exact operator text rather than "some comparison is present",
  // because the earlier version of this test checked only for ">= " and "< " and so passed
  // against a predicate that used the opposite interval to tileForPosition -- which put every
  // tile-edge spawn on the wrong ADT.
  CHECK(contains(creatures, "`position_x` > "));
  CHECK(contains(creatures, "`position_x` <= "));
  CHECK(contains(creatures, "`position_y` > "));
  CHECK(contains(creatures, "`position_y` <= "));

  CHECK(contains(gameobjects, "`position_x` > "));
  CHECK(contains(gameobjects, "`position_y` <= "));

  // The inverted forms must be absent outright. ">= " contains "> " as a substring, so the
  // checks above cannot by themselves distinguish the two conventions.
  CHECK_FALSE(contains(creatures, "`position_x` >= "));
  CHECK_FALSE(contains(creatures, "`position_y` >= "));
  CHECK_FALSE(contains(gameobjects, "`position_x` >= "));

  // "< " likewise appears inside "<= ", so absence has to be checked on the full comparison.
  CHECK_FALSE(contains(creatures, "`position_x` < "));
  CHECK_FALSE(contains(creatures, "`position_y` < "));
  CHECK_FALSE(contains(gameobjects, "`position_y` < "));

  // Ordering by guid keeps two reads of an unchanged tile comparable.
  CHECK(contains(creatures, "ORDER BY"));
}

TEST_CASE("the query interval owns a tile edge exactly where tileForPosition does"
         , "[spawnquery][sql][tile][bounds]")
{
  // Direct regression test for a real defect: the predicate was built as [min, max) while
  // tileForPosition owns (min, max], so a spawn at exactly x = 0.0 or y = 0.0 -- both exactly
  // representable in a FLOAT column and common in real data -- was excluded from its own tile
  // and returned for the neighbour. The tile the editor opened looked empty and the spawn
  // rendered on the wrong ADT.
  //
  // Asserted over the interval rather than the SQL text so it holds whatever the operators are
  // spelled as.
  auto ownedBy = [] (TileIndex const& tile, double x, double y)
  {
    TileBounds const b (TileCoordinates::boundsForTile(tile));
    return x > b.min_x && x <= b.max_x && y > b.min_y && y <= b.max_y;
  };

  for (double const edge : {0.0, TILE_SIZE, -TILE_SIZE, 2.0 * TILE_SIZE})
  {
    CAPTURE(edge);

    TileIndex const owner (TileCoordinates::tileForPosition(edge, edge));

    // The tile tileForPosition names must be the tile whose interval contains the value.
    CHECK(ownedBy(owner, edge, edge));

    // And exactly one tile may claim it. The neighbour on the far side of the shared edge is
    // the one with the HIGHER index, because the axis runs backwards.
    CHECK_FALSE(ownedBy(TileIndex{owner.x + 1, owner.y + 1}, edge, edge));
    CHECK_FALSE(ownedBy(TileIndex{owner.x - 1, owner.y - 1}, edge, edge));
  }
}

TEST_CASE("bounds for tile (49,31) carry that tile's numbers", "[spawnquery][sql][tile]")
{
  TileBounds const bounds (elwynnBounds());

  // Sanity on the fixture itself: the verified real spawn must sit inside the bounds this test
  // then looks for in the SQL. Without this the assertions below would happily confirm the
  // wrong tile.
  // Expressed as (min, max] -- lower exclusive, upper inclusive -- to match both
  // TileCoordinates::boundsForTile and the emitted predicate.
  CHECK(bounds.min_x < ELWYNN_SPAWN_X);
  CHECK(ELWYNN_SPAWN_X <= bounds.max_x);
  CHECK(bounds.min_y < ELWYNN_SPAWN_Y);
  CHECK(ELWYNN_SPAWN_Y <= bounds.max_y);

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

  // The template columns degrade the same way. This schema has no creature_template at all, so
  // creatureModelSource() cannot answer -- and the reader must still open the tile and show every
  // spawn as unresolved rather than refuse the query, which is what a bare rethrow would do.
  CHECK(contains(sql, "0 AS `model_candidate_1`"));
  CHECK(contains(sql, "0 AS `model_candidate_4`"));
  CHECK(contains(sql, "NULL AS `template_name`"));

  CHECK(selectColumnCount(sql) == CREATURE_SELECT_WIDTH);
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

  // gameobject has no wander distance.
  CHECK_FALSE(containsNoCase(sql, "wander"));

  // Exactly one join, to gameobject_template. It used to be asserted that there was no join at
  // all, which was true and is now wrong: `gameobject` carries no display column of any kind, so
  // this join is the only thing that can resolve a gameobject to a model.
  CHECK(contains(sql, "LEFT JOIN `gameobject_template`"));
  CHECK_FALSE(containsNoCase(sql, "inner join"));
  CHECK_FALSE(containsNoCase(sql, "creature"));
}

TEST_CASE("the waypoint query never reads the core-managed wpguid", "[spawnquery][sql]")
{
  SchemaModel const schema (modelFrom(REAL_FIXTURE));

  REQUIRE(schema.hasColumn("waypoint_data", "wpguid"));

  std::string const sql (SpawnQuery::Detail::waypointSelectSql(schema, {90000040u, 12u}));

  CHECK_FALSE(containsNoCase(sql, "wpguid"));
  CHECK(contains(sql, "FROM `waypoint_data`"));
  // Wrapped in parentheses: Catch2's expression decomposer cannot handle a bare || inside an
  // assertion. Either ordering is acceptable since the set is unordered.
  CHECK((contains(sql, "IN (12, 90000040)") || contains(sql, "IN (90000040, 12)")));

  // point is 1-based and the core walks it in order, so the statement must impose that order
  // rather than leave it to the server.
  CHECK(contains(sql, "ORDER BY"));
  CHECK(contains(sql, "`point`"));
  CHECK(selectColumnCount(sql) == WAYPOINT_SELECT_WIDTH);

  // An empty id list must still be a legal statement rather than "IN ()".
  CHECK_FALSE(contains(SpawnQuery::Detail::waypointSelectSql(schema, {}), "IN ()"));
}

TEST_CASE("nothing but validated identifiers and numbers is interpolated"
         , "[spawnquery][safety]")
{
  // Column names cannot be bound as parameters, so the schema-resolved ones are interpolated.
  // They are refused rather than escaped when they are not plain identifiers.
  CHECK(SpawnQuery::Detail::isPlainIdentifier("wander_distance"));
  CHECK(SpawnQuery::Detail::isPlainIdentifier("spawndist"));
  CHECK(SpawnQuery::Detail::isPlainIdentifier("waypoint_data"));
  CHECK(SpawnQuery::Detail::isPlainIdentifier("a$b_9"));

  CHECK_FALSE(SpawnQuery::Detail::isPlainIdentifier(""));
  CHECK_FALSE(SpawnQuery::Detail::isPlainIdentifier("has space"));
  CHECK_FALSE(SpawnQuery::Detail::isPlainIdentifier("back`tick"));
  CHECK_FALSE(SpawnQuery::Detail::isPlainIdentifier("quote'mark"));
  CHECK_FALSE(SpawnQuery::Detail::isPlainIdentifier("wander_distance`; DROP TABLE `creature"));
  CHECK_FALSE(SpawnQuery::Detail::isPlainIdentifier("creature; --"));
  CHECK_FALSE(SpawnQuery::Detail::isPlainIdentifier(std::string(65, 'x')));

  // Every identifier either schema could hand the builder passes that rule.
  CHECK(SpawnQuery::Detail::isPlainIdentifier(modelFrom(REAL_FIXTURE).wanderDistanceColumn()));
  CHECK(SpawnQuery::Detail::isPlainIdentifier(modelFrom(DRIFTED_FIXTURE).wanderDistanceColumn()));

  // And the generated statements contain no string literal, no statement separator and no
  // comment introducer at all, so there is nothing for an injected value to break out of.
  SchemaModel const schema (modelFrom(REAL_FIXTURE));
  TileBounds const bounds (elwynnBounds());

  std::string const creatures (SpawnQuery::creatureSelectSql(schema, 0, bounds));
  std::string const gameobjects (SpawnQuery::gameObjectSelectSql(schema, 0, bounds));
  std::string const waypoints (SpawnQuery::Detail::waypointSelectSql(schema, {1u, 2u}));

  // The drifted creature statement is included because it is the only one that emits correlated
  // subqueries over creature_template_model, and a subquery is the easiest place for a stray
  // separator or comment introducer to appear unnoticed.
  std::string const drifted_creatures
    (SpawnQuery::creatureSelectSql(modelFrom(DRIFTED_FIXTURE), 0, bounds));

  for (std::string const& sql : {creatures, gameobjects, waypoints, drifted_creatures})
  {
    CHECK(sql.find('\'') == std::string::npos);
    CHECK(sql.find('"') == std::string::npos);
    CHECK(sql.find(';') == std::string::npos);
    CHECK(sql.find("--") == std::string::npos);
    CHECK(sql.find("/*") == std::string::npos);
  }
}

TEST_CASE("a resolved identifier that is not plain is refused rather than escaped"
         , "[spawnquery][safety]")
{
  // requireIdentifier is the gate every interpolated name passes through, and it is the one
  // internal function this file could not reach while it was re-declaring them by hand: five of
  // the six were copied out, and the one left out was the one whose failure mode is worst.
  CHECK(SpawnQuery::Detail::requireIdentifier("wander_distance") == "wander_distance");
  CHECK(SpawnQuery::Detail::requireIdentifier("spawndist") == "spawndist");
  CHECK(SpawnQuery::Detail::requireIdentifier(std::string(64, 'x')).size() == 64);

  // Refused, never sanitised. A name that needs sanitising did not come out of
  // information_schema, so the schema it was resolved from is not trustworthy and quoting it
  // would only hide that.
  CHECK_THROWS_AS(SpawnQuery::Detail::requireIdentifier(""), SchemaCapabilityError);
  CHECK_THROWS_AS(SpawnQuery::Detail::requireIdentifier("has space"), SchemaCapabilityError);
  CHECK_THROWS_AS
    ( SpawnQuery::Detail::requireIdentifier("wander_distance`; DROP TABLE `creature")
    , SchemaCapabilityError
    );
  CHECK_THROWS_AS
    (SpawnQuery::Detail::requireIdentifier(std::string(65, 'x')), SchemaCapabilityError);

  // The message has to name what it rejected: a schema-drift report that does not say which
  // identifier failed is unactionable.
  try
  {
    SpawnQuery::Detail::requireIdentifier("no good");
    FAIL("requireIdentifier accepted an identifier containing a space");
  }
  catch (SchemaCapabilityError const& error)
  {
    CHECK(contains(error.what(), "no good"));
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
  CHECK_THROWS_AS(SpawnQuery::Detail::waypointSelectSql(SchemaModel(), {1u}), SchemaCapabilityError);

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
  // The reference changeset's spawn, as the server would hand it back: 22 spawn and addon columns,
  // then the four template model candidates, then the template name.
  std::vector<std::string> const row
    { "9000004", "990001", "0", "1", "1", "0", "-1"
    , "-9512.345", "83.117", "58.271", "4.7124"
    , "120", "5.5", "0", "1", "0"
    , "2", "0", "0", "0"
    , "90000040", "1"
    , "17188", "0", "0", "0"
    , "Hogger"
    };

  REQUIRE(row.size() == CREATURE_SELECT_WIDTH);

  CreatureSpawn const spawn (SpawnQuery::Detail::parseCreatureRow(row));

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

  // modelid is 0, so the template's first model is what renders. Before this existed the spawn
  // resolved to nothing and could not be drawn at all -- which is most real spawns, since
  // creature.modelid is 0 unless a GM overrode it.
  CHECK(spawn.template_info.display_id == 17188u);
  CHECK(spawn.template_info.display_id_origin == DisplayIdOrigin::TEMPLATE);
  CHECK(spawn.template_info.template_model_count == 1u);
  CHECK_FALSE(spawn.template_info.templateOffersAlternatives());
  CHECK(spawn.template_info.name == "Hogger");
  CHECK(SpawnDisplay::isRenderable(spawn));
}

TEST_CASE("the resolved display id prefers the spawn's own modelid over the template's"
         , "[spawnquery][parse][display]")
{
  // The rule, at the seam where the reader applies it: creature.modelid when non-zero, the
  // template's first non-zero model when it is 0. Getting this backwards renders every
  // GM-overridden spawn as the wrong creature, and nothing errors.
  std::vector<std::string> row (creatureRow());
  row[CREATURE_CANDIDATE_BASE] = "17188";
  row[CREATURE_TEMPLATE_NAME_INDEX] = "Hogger";

  SECTION("modelid 0 defers to the template")
  {
    row[CREATURE_MODELID_INDEX] = "0";

    CreatureSpawn const spawn (SpawnQuery::Detail::parseCreatureRow(row));

    CHECK(spawn.model_id == 0u);
    CHECK(spawn.template_info.display_id == 17188u);
    CHECK(spawn.template_info.display_id_origin == DisplayIdOrigin::TEMPLATE);
  }

  SECTION("a non-zero modelid wins")
  {
    row[CREATURE_MODELID_INDEX] = "448";

    CreatureSpawn const spawn (SpawnQuery::Detail::parseCreatureRow(row));

    CHECK(spawn.model_id == 448u);
    CHECK(spawn.template_info.display_id == 448u);
    CHECK(spawn.template_info.display_id_origin == DisplayIdOrigin::SPAWN);

    // The template's model is still recorded as existing. The editor needs it to answer "what
    // would this look like if the override were removed", and removing the override means writing
    // creature.modelid = 0, not writing the template's id into it.
    CHECK(spawn.template_info.template_model_count == 1u);
  }

  SECTION("neither side names a model")
  {
    row[CREATURE_MODELID_INDEX] = "0";
    row[CREATURE_CANDIDATE_BASE] = "0";

    CreatureSpawn const spawn (SpawnQuery::Detail::parseCreatureRow(row));

    CHECK(spawn.template_info.display_id == 0u);
    CHECK(spawn.template_info.display_id_origin == DisplayIdOrigin::UNRESOLVED);
    CHECK_FALSE(SpawnDisplay::isRenderable(spawn));
  }
}

TEST_CASE("a template offering several models is resolved deterministically"
         , "[spawnquery][parse][display]")
{
  // TrinityCore rolls one of modelid1..4 per spawn at runtime. The editor cannot: a tile that drew
  // a different model on each open would be unusable, and a changeset diff that moved with a dice
  // roll would be unreviewable. So the first non-zero wins, every time, and the fact that others
  // existed is recorded rather than lost.
  std::vector<std::string> row (creatureRow());
  row[CREATURE_MODELID_INDEX] = "0";
  row[CREATURE_CANDIDATE_BASE + 0] = "0";       // an unused slot ahead of the real ones
  row[CREATURE_CANDIDATE_BASE + 1] = "1400";
  row[CREATURE_CANDIDATE_BASE + 2] = "1401";
  row[CREATURE_CANDIDATE_BASE + 3] = "1402";

  CreatureSpawn const spawn (SpawnQuery::Detail::parseCreatureRow(row));

  CHECK(spawn.template_info.display_id == 1400u);
  CHECK(spawn.template_info.display_id_origin == DisplayIdOrigin::TEMPLATE);
  CHECK(spawn.template_info.template_model_count == 3u);
  CHECK(spawn.template_info.templateOffersAlternatives());

  // Repeated decoding gives the same answer. Trivially true of a pure function, and asserted
  // anyway because "deterministic" is the requirement and a future COALESCE-in-SQL or RAND()-based
  // implementation would fail here rather than in a viewport six months later.
  CHECK(SpawnQuery::Detail::parseCreatureRow(row).template_info.display_id
        == spawn.template_info.display_id);
}

TEST_CASE("a gameobject row carries its template's display id, type and name"
         , "[spawnquery][parse][display]")
{
  std::vector<std::string> row (gameObjectRow());
  row[0] = "9000010";
  row[1] = "990002";
  row[12] = "1";                                  // rotation3, so the quaternion stays a rotation
  row[GAMEOBJECT_DISPLAY_ID_INDEX] = "259";
  row[GAMEOBJECT_TYPE_INDEX] = "3";               // CHEST
  row[GAMEOBJECT_TEMPLATE_NAME_INDEX] = "Battered Chest";

  GameObjectSpawn const spawn (SpawnQuery::Detail::parseGameObjectRow(row));

  // There is no per-spawn display column on `gameobject` at all, so this is the only display id a
  // gameobject has. Without it nothing could be drawn for one, whatever else was read.
  CHECK(spawn.template_info.display_id == 259u);
  CHECK(spawn.template_info.type == 3u);
  CHECK(spawn.template_info.name == "Battered Chest");
  CHECK(SpawnDisplay::isRenderable(spawn));
}

TEST_CASE("a gameobject with no renderable model is flagged rather than drawn"
         , "[spawnquery][parse][display]")
{
  // The six types that carry no model the editor can draw, from docs/schema-335.md. A trap or a
  // ritual site has a perfectly valid displayId and is invisible in the client; drawing it puts
  // objects in the viewport that no player ever sees.
  for (std::uint32_t const type : {6u, 11u, 12u, 13u, 15u, 18u})
  {
    CAPTURE(type);

    std::vector<std::string> row (gameObjectRow());
    row[12] = "1";
    row[GAMEOBJECT_DISPLAY_ID_INDEX] = "259";
    row[GAMEOBJECT_TYPE_INDEX] = std::to_string(type);

    GameObjectSpawn const spawn (SpawnQuery::Detail::parseGameObjectRow(row));

    // The type and the display id are both still read. Skipping is the renderer's decision, and
    // the UI still has to list the object and let it be edited.
    CHECK(spawn.template_info.type == type);
    CHECK(spawn.template_info.display_id == 259u);
    CHECK_FALSE(SpawnDisplay::isRenderable(spawn));
  }

  // displayId 0 is the other way to have nothing to draw, and it is legal and common.
  std::vector<std::string> no_display (gameObjectRow());
  no_display[12] = "1";
  no_display[GAMEOBJECT_TYPE_INDEX] = "3";        // CHEST, perfectly renderable
  no_display[GAMEOBJECT_DISPLAY_ID_INDEX] = "0";

  CHECK_FALSE(SpawnDisplay::isRenderable(SpawnQuery::Detail::parseGameObjectRow(no_display)));

  // And a DOOR with a display id is drawn, so the check above is not just rejecting everything.
  std::vector<std::string> door (gameObjectRow());
  door[12] = "1";
  door[GAMEOBJECT_TYPE_INDEX] = "0";
  door[GAMEOBJECT_DISPLAY_ID_INDEX] = "259";

  CHECK(SpawnDisplay::isRenderable(SpawnQuery::Detail::parseGameObjectRow(door)));
}

TEST_CASE("a creature with no addon row is not mistaken for one with path 0"
         , "[spawnquery][parse]")
{
  std::vector<std::string> row (numberedRow(CREATURE_SELECT_WIDTH));

  // What a LEFT JOIN that matched nothing looks like: both columns arrive as SQL NULL, which
  // the reader renders as an empty string.
  row[20] = "";
  row[21] = "0";

  CreatureSpawn const missing (SpawnQuery::Detail::parseCreatureRow(row));

  CHECK_FALSE(missing.has_addon);
  CHECK(missing.path_id == 0u);

  // An addon row that exists but binds no path is a different fact, and stays one.
  row[20] = "0";
  row[21] = "1";

  CreatureSpawn const present (SpawnQuery::Detail::parseCreatureRow(row));

  CHECK(present.has_addon);
  CHECK(present.path_id == 0u);
}

TEST_CASE("a short, empty or nonsense row decodes rather than crashing"
         , "[spawnquery][parse][safety]")
{
  // Truncated after map. Everything past the end of the row reads as 0 instead of off the end
  // of the vector.
  CreatureSpawn const truncated (SpawnQuery::Detail::parseCreatureRow({"42", "17", "0"}));

  CHECK(truncated.guid == 42u);
  CHECK(truncated.id == 17u);
  CHECK(truncated.map == 0);
  CHECK(truncated.cur_health == 0u);
  CHECK(truncated.movement_type == MovementType::IDLE);
  CHECK_FALSE(truncated.has_addon);
  CHECK(near(truncated.position.x, 0.0));

  // Nothing to resolve a model from, so nothing is claimed. The renderer reads display_id 0 as
  // "draw a marker", which is the only honest answer for a row that arrived truncated.
  CHECK(truncated.template_info.display_id == 0u);
  CHECK(truncated.template_info.display_id_origin == DisplayIdOrigin::UNRESOLVED);
  CHECK(truncated.template_info.name.empty());
  CHECK(truncated.template_info.template_model_count == 0u);

  CreatureSpawn const nothing (SpawnQuery::Detail::parseCreatureRow({}));
  CHECK(nothing.guid == 0u);

  GameObjectSpawn const no_gameobject (SpawnQuery::Detail::parseGameObjectRow({}));
  CHECK(no_gameobject.guid == 0u);

  WaypointNode const no_node (SpawnQuery::Detail::parseWaypointRow({}));
  CHECK(no_node.point == 0u);
  CHECK_FALSE(no_node.has_orientation);

  // Empty and unparseable fields are zeroes, not exceptions.
  std::vector<std::string> junk (CREATURE_SELECT_WIDTH, std::string("not a number"));
  junk[0] = "";

  CreatureSpawn const garbage (SpawnQuery::Detail::parseCreatureRow(junk));

  CHECK(garbage.guid == 0u);
  CHECK(near(garbage.position.x, 0.0));
  CHECK(garbage.movement_type == MovementType::IDLE);

  // Unparseable model candidates resolve to nothing rather than to a garbage display id, and the
  // name field is taken verbatim -- it is a label, not a number, and the database is free to hold
  // anything in it.
  CHECK(garbage.template_info.display_id == 0u);
  CHECK(garbage.template_info.display_id_origin == DisplayIdOrigin::UNRESOLVED);
  CHECK(garbage.template_info.name == "not a number");

  // A guid wider than the column can hold saturates rather than wrapping to a small number
  // that collides with a real spawn.
  std::vector<std::string> huge (numberedRow(CREATURE_SELECT_WIDTH));
  huge[0] = "99999999999999";

  CHECK(SpawnQuery::Detail::parseCreatureRow(huge).guid == 4294967295u);

  // An unknown movement type is read as idle rather than as an enumerator that does not exist.
  std::vector<std::string> odd (numberedRow(CREATURE_SELECT_WIDTH));
  odd[16] = "9";

  CHECK(SpawnQuery::Detail::parseCreatureRow(odd).movement_type == MovementType::IDLE);
}

TEST_CASE("an unsigned field saturates rather than wrapping", "[spawnquery][parse][safety]")
{
  // The one decoding primitive both halves of the module use: the decoders read every unsigned
  // column through it, and the reading half uses it for the path id a waypoint row is grouped by
  // and for MAX(guid). That second use is why saturation rather than wrapping matters -- a
  // wrapped maximum would hand out guids that are already in use.
  std::vector<std::string> const row
    {"", "0", "4294967295", "4294967296", "99999999999999", "-1", "12x"};

  CHECK(SpawnQuery::Detail::rowUInt32(row, 0) == 0u);            // SQL NULL
  CHECK(SpawnQuery::Detail::rowUInt32(row, 1) == 0u);
  CHECK(SpawnQuery::Detail::rowUInt32(row, 2) == 4294967295u);   // the column's own maximum
  CHECK(SpawnQuery::Detail::rowUInt32(row, 3) == 4294967295u);   // one past it, saturated
  CHECK(SpawnQuery::Detail::rowUInt32(row, 4) == 4294967295u);
  CHECK(SpawnQuery::Detail::rowUInt32(row, 5) == 0u);            // clamped, not wrapped to ~4e9
  CHECK(SpawnQuery::Detail::rowUInt32(row, 6) == 12u);           // trailing junk ignored
  CHECK(SpawnQuery::Detail::rowUInt32(row, 99) == 0u);           // past the end of the row
}

TEST_CASE("the select list and the row decoder agree on the shape of a row"
         , "[spawnquery][parse]")
{
  SchemaModel const schema (modelFrom(REAL_FIXTURE));
  TileBounds const bounds (elwynnBounds());

  // If a column is ever added to or removed from a select list without the decoder moving with
  // it, every field past that point silently shifts by one. These counts are the only thing
  // holding the two halves of the module together.
  CHECK(selectColumnCount(SpawnQuery::creatureSelectSql(schema, 0, bounds))
        == CREATURE_SELECT_WIDTH);
  CHECK(selectColumnCount(SpawnQuery::gameObjectSelectSql(schema, 0, bounds))
        == GAMEOBJECT_SELECT_WIDTH);
  CHECK(selectColumnCount(SpawnQuery::Detail::waypointSelectSql(schema, {1u}))
        == WAYPOINT_SELECT_WIDTH);

  // The drifted schema must produce the same widths, or the positional decoder is reading one
  // schema's row with the other's offsets. It reaches the model candidates through four correlated
  // subqueries instead of four columns, which is exactly the sort of substitution that changes a
  // width without anyone noticing.
  SchemaModel const drifted (modelFrom(DRIFTED_FIXTURE));

  CHECK(selectColumnCount(SpawnQuery::creatureSelectSql(drifted, 0, bounds))
        == CREATURE_SELECT_WIDTH);
  CHECK(selectColumnCount(SpawnQuery::gameObjectSelectSql(drifted, 0, bounds))
        == GAMEOBJECT_SELECT_WIDTH);

  // has_addon is still column 21, ahead of the appended template columns: a row that stops one
  // field short of it reports no addon.
  std::vector<std::string> const full (numberedRow(CREATURE_SELECT_WIDTH));

  CHECK(SpawnQuery::Detail::parseCreatureRow(full).has_addon);
  CHECK_FALSE(SpawnQuery::Detail::parseCreatureRow(numberedRow(21)).has_addon);

  // And the last three gameobject columns really are the template's. numberedRow fills field i
  // with i+1, so a shift of one shows up as an off-by-one value rather than as a zero.
  GameObjectSpawn const numbered
    (SpawnQuery::Detail::parseGameObjectRow(numberedRow(GAMEOBJECT_SELECT_WIDTH)));

  CHECK(numbered.template_info.display_id == GAMEOBJECT_DISPLAY_ID_INDEX + 1);
  CHECK(numbered.template_info.type == GAMEOBJECT_TYPE_INDEX + 1);
  CHECK(numbered.template_info.name == std::to_string(GAMEOBJECT_TEMPLATE_NAME_INDEX + 1));
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

  GameObjectSpawn const spawn (SpawnQuery::Detail::parseGameObjectRow(row));

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
  CHECK(near(SpawnQuery::Detail::parseGameObjectRow({"1"}).rotation.r3, 1.0));

  std::vector<std::string> explicit_zero (row);
  explicit_zero[12] = "0";
  CHECK(near(SpawnQuery::Detail::parseGameObjectRow(explicit_zero).rotation.r3, 0.0));
}

TEST_CASE("a waypoint row keeps NULL orientation distinct from zero", "[spawnquery][parse]")
{
  // id, point, x, y, z, orientation, delay, move_type, action, action_chance.
  std::vector<std::string> row
    { "90000040", "3", "-9512.345", "83.117", "58.271", "", "1500", "1", "0", "100" };

  WaypointNode const null_orientation (SpawnQuery::Detail::parseWaypointRow(row));

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
  WaypointNode const zero_orientation (SpawnQuery::Detail::parseWaypointRow(row));

  CHECK(zero_orientation.has_orientation);
  CHECK(near(zero_orientation.orientation, 0.0));

  row[5] = "3.14159";
  CHECK(near(SpawnQuery::Detail::parseWaypointRow(row).orientation, 3.14159));

  // move_type is a closed set of four; anything else walks.
  row[7] = "3";
  CHECK(SpawnQuery::Detail::parseWaypointRow(row).move_type == WaypointMoveType::TAKEOFF);

  row[7] = "77";
  CHECK(SpawnQuery::Detail::parseWaypointRow(row).move_type == WaypointMoveType::WALK);
}

// --- Pre-flight count builders ----------------------------------------------------------
//
// These exist so the editor can report "this is N spawns, continue?" before committing to a load
// whose real cost is not the query but the model loads each spawn queues. That makes exactly one
// property load-bearing: the count must describe the same rows the subsequent load returns. A
// count that disagreed with its load would be worse than no count, because the user would approve
// one number and receive another.

TEST_CASE("count builders select over the same map and bounds as the loaders"
         , "[spawnquery][sql][count]")
{
  TileBounds const bounds (elwynnBounds());
  SchemaModel const schema (modelFrom(REAL_FIXTURE));

  std::string const creature_count (SpawnQuery::creatureCountSql(schema, 0, bounds));
  std::string const creature_select (SpawnQuery::creatureSelectSql(schema, 0, bounds));

  CHECK(containsNoCase(creature_count, "count(*)"));

  // The bounds predicate is the part that must agree, so it is compared literally rather than
  // by spot-checking a number: the interval is half-open in the unusual direction ((min, max],
  // because the axis runs backwards) and a count built with `>=`/`<` would quietly disagree
  // with the loader at exactly the tile edges, which is where real spawns sit.
  auto const where_of = [] (std::string const& sql)
  {
    std::size_t const at (sql.find("WHERE "));
    REQUIRE(at != std::string::npos);

    std::size_t const end (sql.find("\nORDER BY", at));
    return sql.substr(at, end == std::string::npos ? std::string::npos : end - at);
  };

  CHECK(where_of(creature_count) == where_of(creature_select));

  std::string const object_count (SpawnQuery::gameObjectCountSql(schema, 0, bounds));
  std::string const object_select (SpawnQuery::gameObjectSelectSql(schema, 0, bounds));

  CHECK(containsNoCase(object_count, "count(*)"));
  CHECK(where_of(object_count) == where_of(object_select));
}

TEST_CASE("count builders carry no join and no ordering", "[spawnquery][sql][count]")
{
  TileBounds const bounds (elwynnBounds());
  SchemaModel const schema (modelFrom(REAL_FIXTURE));

  for (std::string const& sql : { SpawnQuery::creatureCountSql(schema, 0, bounds)
                                , SpawnQuery::gameObjectCountSql(schema, 0, bounds)
                                })
  {
    // A template join would make the pre-flight check as expensive as the load it guards, which
    // defeats the point of asking first. ORDER BY on a COUNT is pure waste.
    CHECK_FALSE(containsNoCase(sql, "join"));
    CHECK_FALSE(containsNoCase(sql, "order by"));

    // No row-bearing columns: this must never turn into a select that fetches what it counts.
    CHECK_FALSE(containsNoCase(sql, "position_z"));
    CHECK_FALSE(containsNoCase(sql, "guid,"));
  }
}

TEST_CASE("count builders still respect the schema-resolved map filter"
         , "[spawnquery][sql][count]")
{
  TileBounds const bounds (elwynnBounds());
  SchemaModel const schema (modelFrom(REAL_FIXTURE));

  // A count that ignored the map id would report every continent's spawns for one tile index,
  // since tile indices repeat on every map.
  CHECK(contains(SpawnQuery::creatureCountSql(schema, 571, bounds), "= 571"));
  CHECK(contains(SpawnQuery::gameObjectCountSql(schema, 571, bounds), "= 571"));
}
