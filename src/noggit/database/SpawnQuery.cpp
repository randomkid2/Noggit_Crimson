// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/database/SpawnQuery.hpp>

#include <noggit/database/TileCoordinates.hpp>
#include <noggit/database/WorldDatabaseConnection.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <ios>
#include <limits>
#include <locale>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace Noggit::Database;

namespace
{
  constexpr char const* TABLE_CREATURE = "creature";
  constexpr char const* TABLE_CREATURE_ADDON = "creature_addon";
  constexpr char const* TABLE_GAMEOBJECT = "gameobject";

  constexpr char const* ALIAS_CREATURE = "c";
  constexpr char const* ALIAS_ADDON = "a";
  constexpr char const* ALIAS_GAMEOBJECT = "g";
  constexpr char const* ALIAS_WAYPOINT = "w";

  constexpr char const* COLUMN_PATH_ID = "path_id";
  constexpr char const* COLUMN_POSITION_X = "position_x";
  constexpr char const* COLUMN_POSITION_Y = "position_y";

  // Coordinates go into the query as decimal literals. Six decimals is far past what a FLOAT
  // column can hold at world magnitudes, so the bound is never the thing that loses a spawn,
  // and it keeps the text free of exponents -- which is what makes the generated SQL greppable
  // in a test and readable in a log.
  constexpr int BOUND_DECIMALS = 6;

  // Longest identifier MySQL accepts. Anything longer is not a column name that came out of
  // information_schema, so it is refused rather than truncated.
  constexpr std::size_t IDENTIFIER_MAX_LENGTH = 64;

  [[noreturn]] void failSchema(std::string const& what)
  {
    throw SchemaCapabilityError
      ("SpawnQuery cannot build a query against this database: " + what
       + ". Refusing to guess a column name, because a query built on a guess returns nothing"
         " rather than failing, and an empty tile reads as 'no spawns here'. Re-run"
         " /schema-check and update docs/schema-335.md if the target is legitimate.");
  }

  // The bounds are the only floating point values that reach the SQL text. A NaN would be
  // formatted as "nan" and produce a statement that is neither valid nor obviously wrong, and
  // inverted bounds match no row at all -- the silent-empty-tile failure this module exists to
  // avoid. Both are caller errors, so both are reported as such.
  void requireUsableBounds(TileBounds const& bounds)
  {
    bool const finite
      ( std::isfinite(bounds.min_x) && std::isfinite(bounds.max_x)
        && std::isfinite(bounds.min_y) && std::isfinite(bounds.max_y)
      );

    if (!finite)
    {
      throw std::invalid_argument
        ("SpawnQuery: tile bounds are not finite. Refusing to build a query from them.");
    }

    if (bounds.min_x > bounds.max_x || bounds.min_y > bounds.max_y)
    {
      throw std::invalid_argument
        ("SpawnQuery: tile bounds are inverted (min greater than max). Such a query matches no"
         " row, which is indistinguishable from an empty tile.");
    }
  }

  std::string formatBound(double value)
  {
    // Imbued with the classic locale on purpose: a comma decimal separator would produce an
    // extra column in the SQL rather than an error.
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::fixed << std::setprecision(BOUND_DECIMALS) << value;
    return out.str();
  }

  std::string quoted(std::string const& identifier)
  {
    return "`" + identifier + "`";
  }

  std::string columnRef(std::string const& alias, std::string const& column)
  {
    return alias + "." + quoted(column);
  }

  // A column the module cannot work without. Absent means this is not a world database in any
  // recognised shape.
  std::string requiredColumn
    ( SchemaModel const& schema
    , std::string const& table
    , std::string const& alias
    , std::string const& column
    )
  {
    if (!schema.hasColumn(table, column))
    {
      failSchema(table + " has no " + column + " column");
    }

    return columnRef(alias, column);
  }

  // A column the module can do without. Emitting a literal in its place keeps the select list
  // the same width whatever the schema, so the positional parser below never has to branch.
  std::string columnOr
    ( SchemaModel const& schema
    , std::string const& table
    , std::string const& alias
    , std::string const& column
    , std::string const& fallback = "0"
    )
  {
    if (!schema.hasColumn(table, column))
    {
      return fallback + " AS " + quoted(column);
    }

    return columnRef(alias, column);
  }

  std::string boundsPredicate(std::string const& alias, TileBounds const& bounds)
  {
    requireUsableBounds(bounds);

    // Half-open on both axes, matching TileCoordinates::boundsForTile: a position on a shared
    // edge belongs to exactly one tile and is never read twice.
    return columnRef(alias, COLUMN_POSITION_X) + " >= " + formatBound(bounds.min_x)
      + " AND " + columnRef(alias, COLUMN_POSITION_X) + " < " + formatBound(bounds.max_x)
      + " AND " + columnRef(alias, COLUMN_POSITION_Y) + " >= " + formatBound(bounds.min_y)
      + " AND " + columnRef(alias, COLUMN_POSITION_Y) + " < " + formatBound(bounds.max_y);
  }

  // --- Row decoding ----------------------------------------------------------------------
  // Every field is read through fieldOr, so a row shorter than the select list decodes to
  // zeroes instead of reading off the end. That cannot happen with the statements built here,
  // but these decode whatever the server hands back, and a crash in the editor because a
  // column was dropped underneath it is not an acceptable failure mode.

  std::string fieldOr(ResultRow const& row, std::size_t index)
  {
    return index < row.size() ? row[index] : std::string();
  }

  // Integer parsing is locale independent, unlike the floating point path below.
  std::int64_t toInt64(std::string const& text)
  {
    if (text.empty())
    {
      return 0;
    }

    char* end (nullptr);
    long long const value (std::strtoll(text.c_str(), &end, 10));

    return end == text.c_str() ? 0 : static_cast<std::int64_t>(value);
  }

  template<typename T>
  T narrowed(std::int64_t value)
  {
    constexpr std::int64_t low (static_cast<std::int64_t>(std::numeric_limits<T>::min()));
    constexpr std::int64_t high (static_cast<std::int64_t>(std::numeric_limits<T>::max()));

    return static_cast<T>(std::min(std::max(value, low), high));
  }

  double toDouble(std::string const& text)
  {
    if (text.empty())
    {
      return 0.0;
    }

    // Not std::strtod: that reads the decimal separator from the C locale, so a host running
    // under a comma-decimal locale would parse -9512.345 as -9512.
    std::istringstream in (text);
    in.imbue(std::locale::classic());

    double value (0.0);
    in >> value;

    return (in.fail() || !std::isfinite(value)) ? 0.0 : value;
  }

  std::uint32_t toU32(ResultRow const& row, std::size_t index)
  {
    return narrowed<std::uint32_t>(toInt64(fieldOr(row, index)));
  }

  std::uint16_t toU16(ResultRow const& row, std::size_t index)
  {
    return narrowed<std::uint16_t>(toInt64(fieldOr(row, index)));
  }

  std::uint8_t toU8(ResultRow const& row, std::size_t index)
  {
    return narrowed<std::uint8_t>(toInt64(fieldOr(row, index)));
  }

  std::int8_t toI8(ResultRow const& row, std::size_t index)
  {
    return narrowed<std::int8_t>(toInt64(fieldOr(row, index)));
  }

  std::int32_t toI32(ResultRow const& row, std::size_t index)
  {
    return narrowed<std::int32_t>(toInt64(fieldOr(row, index)));
  }

  double toDouble(ResultRow const& row, std::size_t index)
  {
    return toDouble(fieldOr(row, index));
  }

  // SQL NULL arrives as an empty string, which is how a nullable column is told apart from one
  // holding zero.
  bool isNull(ResultRow const& row, std::size_t index)
  {
    return fieldOr(row, index).empty();
  }

  MovementType toMovementType(ResultRow const& row, std::size_t index)
  {
    switch (toInt64(fieldOr(row, index)))
    {
      case 1: return MovementType::RANDOM;
      case 2: return MovementType::WAYPOINT;
      default: return MovementType::IDLE;
    }
  }

  WaypointMoveType toWaypointMoveType(ResultRow const& row, std::size_t index)
  {
    switch (toInt64(fieldOr(row, index)))
    {
      case 1: return WaypointMoveType::RUN;
      case 2: return WaypointMoveType::LAND;
      case 3: return WaypointMoveType::TAKEOFF;
      default: return WaypointMoveType::WALK;
    }
  }
}

namespace Noggit::Database::SpawnQuery
{
  // Identifiers cannot be bound as parameters, so the few that reach the SQL text -- the
  // schema-resolved wander distance column and waypoint table name -- are interpolated.
  // Rather than escape them, refuse anything that is not a plain identifier: that is a much
  // smaller rule to get right, and no legitimate column name needs more.
  //
  // Declared outside the header on purpose. It is not part of the module's contract, only of
  // its safety story, and the safety story is worth a test.
  bool isPlainIdentifier(std::string const& name)
  {
    if (name.empty() || name.size() > IDENTIFIER_MAX_LENGTH)
    {
      return false;
    }

    return std::all_of
      ( name.begin(), name.end()
      , [] (unsigned char c)
        {
          return std::isalnum(c) != 0 || c == '_' || c == '$';
        }
      );
  }

  std::string requireIdentifier(std::string const& name)
  {
    if (!isPlainIdentifier(name))
    {
      failSchema("the resolved identifier '" + name + "' is not a plain identifier");
    }

    return name;
  }

  std::string creatureSelectSql
    (SchemaModel const& schema, std::uint16_t map, TileBounds const& bounds)
  {
    if (!schema.hasTable(TABLE_CREATURE))
    {
      failSchema("there is no creature table");
    }

    // Throws when the schema has neither spelling, which is the correct outcome: a query
    // naming the wrong column fails loudly, but one that omits it silently loses every
    // creature's wander radius on the next write.
    std::string const wander (requireIdentifier(schema.wanderDistanceColumn()));

    // creature_addon is where path_id lives, so the join is how a spawn learns which waypoint
    // path it belongs to. Where the table is missing the select list keeps its width with
    // literals rather than changing shape.
    bool const join_addon
      ( schema.hasTable(TABLE_CREATURE_ADDON)
        && schema.hasColumn(TABLE_CREATURE_ADDON, COLUMN_PATH_ID)
        && schema.hasColumn(TABLE_CREATURE_ADDON, "guid")
      );

    std::string sql ("SELECT ");

    sql += requiredColumn(schema, TABLE_CREATURE, ALIAS_CREATURE, "guid") + ", ";
    sql += requiredColumn(schema, TABLE_CREATURE, ALIAS_CREATURE, "id") + ", ";
    sql += requiredColumn(schema, TABLE_CREATURE, ALIAS_CREATURE, "map") + ", ";
    sql += columnOr(schema, TABLE_CREATURE, ALIAS_CREATURE, "spawnMask", "1") + ", ";
    sql += columnOr(schema, TABLE_CREATURE, ALIAS_CREATURE, "phaseMask", "1") + ", ";
    sql += columnOr(schema, TABLE_CREATURE, ALIAS_CREATURE, "modelid") + ", ";
    sql += columnOr(schema, TABLE_CREATURE, ALIAS_CREATURE, "equipment_id") + ", ";
    sql += requiredColumn(schema, TABLE_CREATURE, ALIAS_CREATURE, COLUMN_POSITION_X) + ", ";
    sql += requiredColumn(schema, TABLE_CREATURE, ALIAS_CREATURE, COLUMN_POSITION_Y) + ", ";
    sql += requiredColumn(schema, TABLE_CREATURE, ALIAS_CREATURE, "position_z") + ", ";
    sql += requiredColumn(schema, TABLE_CREATURE, ALIAS_CREATURE, "orientation") + ", ";
    sql += columnOr(schema, TABLE_CREATURE, ALIAS_CREATURE, "spawntimesecs", "120") + ", ";
    sql += columnRef(ALIAS_CREATURE, wander) + ", ";
    sql += columnOr(schema, TABLE_CREATURE, ALIAS_CREATURE, "currentwaypoint") + ", ";
    sql += columnOr(schema, TABLE_CREATURE, ALIAS_CREATURE, "curhealth", "1") + ", ";
    sql += columnOr(schema, TABLE_CREATURE, ALIAS_CREATURE, "curmana") + ", ";
    sql += columnOr(schema, TABLE_CREATURE, ALIAS_CREATURE, "MovementType") + ", ";
    sql += columnOr(schema, TABLE_CREATURE, ALIAS_CREATURE, "npcflag") + ", ";
    sql += columnOr(schema, TABLE_CREATURE, ALIAS_CREATURE, "unit_flags") + ", ";
    sql += columnOr(schema, TABLE_CREATURE, ALIAS_CREATURE, "dynamicflags") + ", ";

    if (join_addon)
    {
      sql += columnRef(ALIAS_ADDON, COLUMN_PATH_ID) + ", ";

      // The presence flag has to be its own column: a LEFT JOIN that found no addon row and an
      // addon row carrying path_id 0 both read back as 0 otherwise, and only one of them means
      // "this spawn has no addon".
      sql += "(" + columnRef(ALIAS_ADDON, "guid") + " IS NOT NULL) AS `has_addon`";
    }
    else
    {
      sql += "0 AS `path_id`, 0 AS `has_addon`";
    }

    sql += "\nFROM " + quoted(TABLE_CREATURE) + " AS " + ALIAS_CREATURE;

    if (join_addon)
    {
      sql += "\nLEFT JOIN " + quoted(TABLE_CREATURE_ADDON) + " AS " + ALIAS_ADDON
        + " ON " + columnRef(ALIAS_ADDON, "guid") + " = " + columnRef(ALIAS_CREATURE, "guid");
    }

    // Filtered by map and world coordinates only. zoneId and areaId are core-derived and are
    // frequently 0 in real data, so a query that filtered on them would return nothing and
    // read as an empty tile.
    sql += "\nWHERE " + columnRef(ALIAS_CREATURE, "map") + " = " + std::to_string(map)
      + "\n  AND " + boundsPredicate(ALIAS_CREATURE, bounds);

    sql += "\nORDER BY " + columnRef(ALIAS_CREATURE, "guid");

    return sql;
  }

  std::string gameObjectSelectSql
    (SchemaModel const& schema, std::uint16_t map, TileBounds const& bounds)
  {
    if (!schema.hasTable(TABLE_GAMEOBJECT))
    {
      failSchema("there is no gameobject table");
    }

    std::string sql ("SELECT ");

    sql += requiredColumn(schema, TABLE_GAMEOBJECT, ALIAS_GAMEOBJECT, "guid") + ", ";
    sql += requiredColumn(schema, TABLE_GAMEOBJECT, ALIAS_GAMEOBJECT, "id") + ", ";
    sql += requiredColumn(schema, TABLE_GAMEOBJECT, ALIAS_GAMEOBJECT, "map") + ", ";
    sql += columnOr(schema, TABLE_GAMEOBJECT, ALIAS_GAMEOBJECT, "spawnMask", "1") + ", ";
    sql += columnOr(schema, TABLE_GAMEOBJECT, ALIAS_GAMEOBJECT, "phaseMask", "1") + ", ";
    sql += requiredColumn(schema, TABLE_GAMEOBJECT, ALIAS_GAMEOBJECT, COLUMN_POSITION_X) + ", ";
    sql += requiredColumn(schema, TABLE_GAMEOBJECT, ALIAS_GAMEOBJECT, COLUMN_POSITION_Y) + ", ";
    sql += requiredColumn(schema, TABLE_GAMEOBJECT, ALIAS_GAMEOBJECT, "position_z") + ", ";
    sql += requiredColumn(schema, TABLE_GAMEOBJECT, ALIAS_GAMEOBJECT, "orientation") + ", ";

    // rotation0..3 is a quaternion, not Euler angles. rotation3 falls back to 1 rather than 0
    // because (0,0,0,0) is not a rotation at all, while (0,0,0,1) is the identity.
    sql += columnOr(schema, TABLE_GAMEOBJECT, ALIAS_GAMEOBJECT, "rotation0") + ", ";
    sql += columnOr(schema, TABLE_GAMEOBJECT, ALIAS_GAMEOBJECT, "rotation1") + ", ";
    sql += columnOr(schema, TABLE_GAMEOBJECT, ALIAS_GAMEOBJECT, "rotation2") + ", ";
    sql += columnOr(schema, TABLE_GAMEOBJECT, ALIAS_GAMEOBJECT, "rotation3", "1") + ", ";

    sql += columnOr(schema, TABLE_GAMEOBJECT, ALIAS_GAMEOBJECT, "spawntimesecs", "300") + ", ";
    sql += columnOr(schema, TABLE_GAMEOBJECT, ALIAS_GAMEOBJECT, "animprogress", "100") + ", ";
    sql += columnOr(schema, TABLE_GAMEOBJECT, ALIAS_GAMEOBJECT, "state", "1");

    sql += "\nFROM " + quoted(TABLE_GAMEOBJECT) + " AS " + ALIAS_GAMEOBJECT;

    sql += "\nWHERE " + columnRef(ALIAS_GAMEOBJECT, "map") + " = " + std::to_string(map)
      + "\n  AND " + boundsPredicate(ALIAS_GAMEOBJECT, bounds);

    sql += "\nORDER BY " + columnRef(ALIAS_GAMEOBJECT, "guid");

    return sql;
  }

  // Nodes for a set of paths, ordered so a single pass groups them. Not part of the header
  // contract, but pure and worth testing: the id list is the one place a caller-supplied value
  // list reaches the SQL, and wpguid must never appear in it.
  std::string waypointSelectSql
    (SchemaModel const& schema, std::vector<std::uint32_t> const& path_ids)
  {
    // Throws when no known waypoint table exists.
    std::string const table (requireIdentifier(schema.waypointPathTable()));

    std::string sql ("SELECT ");

    sql += requiredColumn(schema, table, ALIAS_WAYPOINT, "id") + ", ";
    sql += requiredColumn(schema, table, ALIAS_WAYPOINT, "point") + ", ";
    sql += requiredColumn(schema, table, ALIAS_WAYPOINT, COLUMN_POSITION_X) + ", ";
    sql += requiredColumn(schema, table, ALIAS_WAYPOINT, COLUMN_POSITION_Y) + ", ";
    sql += requiredColumn(schema, table, ALIAS_WAYPOINT, "position_z") + ", ";

    // Nullable and usually NULL. NULL in place of a missing column keeps has_orientation
    // false, which is the truthful answer either way.
    sql += columnOr(schema, table, ALIAS_WAYPOINT, "orientation", "NULL") + ", ";
    sql += columnOr(schema, table, ALIAS_WAYPOINT, "delay") + ", ";
    sql += columnOr(schema, table, ALIAS_WAYPOINT, "move_type") + ", ";
    sql += columnOr(schema, table, ALIAS_WAYPOINT, "action") + ", ";
    sql += columnOr(schema, table, ALIAS_WAYPOINT, "action_chance", "100");

    // wpguid is deliberately absent: it is core-managed, and reading it invites writing it.

    sql += "\nFROM " + quoted(table) + " AS " + ALIAS_WAYPOINT;

    sql += "\nWHERE " + columnRef(ALIAS_WAYPOINT, "id") + " IN (";

    if (path_ids.empty())
    {
      // An empty IN list is a syntax error in MySQL. NULL matches nothing, which is the
      // meaning intended, though callers are expected not to ask.
      sql += "NULL";
    }
    else
    {
      for (std::size_t i (0); i < path_ids.size(); ++i)
      {
        sql += (i == 0 ? "" : ", ") + std::to_string(path_ids[i]);
      }
    }

    sql += ")";

    // Grouping in the reader depends on this order, and the core walks points in order.
    sql += "\nORDER BY " + columnRef(ALIAS_WAYPOINT, "id") + ", "
      + columnRef(ALIAS_WAYPOINT, "point");

    return sql;
  }

  // --- Row decoding, exposed so it can be tested without a database ------------------------

  CreatureSpawn parseCreatureRow(ResultRow const& row)
  {
    CreatureSpawn spawn;

    spawn.guid = toU32(row, 0);
    spawn.id = toU32(row, 1);
    spawn.map = toU16(row, 2);
    spawn.spawn_mask = toU8(row, 3);
    spawn.phase_mask = toU32(row, 4);
    spawn.model_id = toU32(row, 5);
    spawn.equipment_id = toI8(row, 6);

    spawn.position.x = toDouble(row, 7);
    spawn.position.y = toDouble(row, 8);
    spawn.position.z = toDouble(row, 9);
    spawn.orientation = toDouble(row, 10);

    spawn.spawn_time_secs = toU32(row, 11);
    spawn.wander_distance = toDouble(row, 12);
    spawn.current_waypoint = toU32(row, 13);
    spawn.cur_health = toU32(row, 14);
    spawn.cur_mana = toU32(row, 15);
    spawn.movement_type = toMovementType(row, 16);
    spawn.npc_flag = toU32(row, 17);
    spawn.unit_flags = toU32(row, 18);
    spawn.dynamic_flags = toU32(row, 19);

    spawn.path_id = toU32(row, 20);
    spawn.has_addon = toInt64(fieldOr(row, 21)) != 0;

    return spawn;
  }

  GameObjectSpawn parseGameObjectRow(ResultRow const& row)
  {
    GameObjectSpawn spawn;

    spawn.guid = toU32(row, 0);
    spawn.id = toU32(row, 1);
    spawn.map = toU16(row, 2);
    spawn.spawn_mask = toU8(row, 3);
    spawn.phase_mask = toU32(row, 4);

    spawn.position.x = toDouble(row, 5);
    spawn.position.y = toDouble(row, 6);
    spawn.position.z = toDouble(row, 7);
    spawn.orientation = toDouble(row, 8);

    // Missing scalar fields default to 0, but a quaternion does not: (0,0,0,0) is not a
    // rotation at all, and zeroing r3 turns a merely truncated row into one that fails
    // unit-length validation. Only overwrite the identity default when the whole quaternion is
    // present, so a short row yields a usable object rather than a degenerate one.
    if (row.size() > 12)
    {
      spawn.rotation.r0 = toDouble(row, 9);
      spawn.rotation.r1 = toDouble(row, 10);
      spawn.rotation.r2 = toDouble(row, 11);
      spawn.rotation.r3 = toDouble(row, 12);
    }

    spawn.spawn_time_secs = toI32(row, 13);
    spawn.anim_progress = toU32(row, 14);
    spawn.state = toU32(row, 15);

    return spawn;
  }

  // The path id in column 0 is the caller's business; grouping reads it separately.
  WaypointNode parseWaypointRow(ResultRow const& row)
  {
    WaypointNode node;

    node.point = toU32(row, 1);
    node.position.x = toDouble(row, 2);
    node.position.y = toDouble(row, 3);
    node.position.z = toDouble(row, 4);

    node.has_orientation = !isNull(row, 5);
    node.orientation = node.has_orientation ? toDouble(row, 5) : 0.0;

    node.delay_ms = toU32(row, 6);
    node.move_type = toWaypointMoveType(row, 7);
    node.action = toU32(row, 8);
    node.action_chance = toU32(row, 9);

    return node;
  }

  // --- Reading ---------------------------------------------------------------------------

  namespace
  {
    std::map<std::uint32_t, WaypointPath> groupWaypointRows(ResultRows const& rows)
    {
      std::map<std::uint32_t, WaypointPath> paths;

      for (ResultRow const& row : rows)
      {
        std::uint32_t const id (toU32(row, 0));

        WaypointPath& path (paths[id]);
        path.id = id;
        path.nodes.push_back(parseWaypointRow(row));
      }

      // The statement already orders by point, but the sort is kept so a caller handing over
      // rows from anywhere else still gets a walkable path rather than a scrambled one.
      for (auto& entry : paths)
      {
        std::stable_sort
          ( entry.second.nodes.begin(), entry.second.nodes.end()
          , [] (WaypointNode const& a, WaypointNode const& b)
            {
              return a.point < b.point;
            }
          );
      }

      return paths;
    }

    std::uint32_t maxGuidOf(WorldDatabaseConnection const& connection, std::string const& table)
    {
      // COALESCE rather than a NULL check on the caller's side: an empty table yields NULL,
      // which the reader renders as an empty string and would decode to 0 anyway. Saying it in
      // the statement makes the intent visible in a query log.
      ResultRows const rows
        (connection.query("SELECT COALESCE(MAX(`guid`), 0) FROM `" + table + "`"));

      if (rows.empty())
      {
        return 0;
      }

      return toU32(rows.front(), 0);
    }
  }

  TileSpawns loadTile
    ( WorldDatabaseConnection const& connection
    , SchemaModel const& schema
    , std::uint16_t map
    , TileIndex const& tile
    )
  {
    if (!TileCoordinates::isValidTile(tile))
    {
      throw std::invalid_argument
        ("SpawnQuery::loadTile: tile (" + std::to_string(tile.x) + ", "
         + std::to_string(tile.y) + ") is outside the 64x64 map grid.");
    }

    TileSpawns spawns;
    spawns.tile = tile;
    spawns.map = map;

    TileBounds const bounds (TileCoordinates::boundsForTile(tile));

    ResultRows const creature_rows (connection.query(creatureSelectSql(schema, map, bounds)));
    spawns.creatures.reserve(creature_rows.size());

    for (ResultRow const& row : creature_rows)
    {
      spawns.creatures.push_back(parseCreatureRow(row));
    }

    ResultRows const gameobject_rows
      (connection.query(gameObjectSelectSql(schema, map, bounds)));
    spawns.gameobjects.reserve(gameobject_rows.size());

    for (ResultRow const& row : gameobject_rows)
    {
      spawns.gameobjects.push_back(parseGameObjectRow(row));
    }

    // Paths are collected from the creatures standing in this tile, not from waypoint
    // coordinates: a patrol route commonly leaves the tile its owner spawns in, and selecting
    // by node position would both miss those paths and load fragments of paths belonging to
    // creatures elsewhere.
    std::vector<std::uint32_t> path_ids;

    for (CreatureSpawn const& creature : spawns.creatures)
    {
      if (creature.has_addon && creature.path_id != 0)
      {
        path_ids.push_back(creature.path_id);
      }
    }

    std::sort(path_ids.begin(), path_ids.end());
    path_ids.erase(std::unique(path_ids.begin(), path_ids.end()), path_ids.end());

    if (!path_ids.empty())
    {
      std::map<std::uint32_t, WaypointPath> const paths
        (groupWaypointRows(connection.query(waypointSelectSql(schema, path_ids))));

      spawns.paths.reserve(paths.size());

      for (auto const& entry : paths)
      {
        spawns.paths.push_back(entry.second);
      }
    }

    return spawns;
  }

  WaypointPath loadPath
    ( WorldDatabaseConnection const& connection
    , SchemaModel const& schema
    , std::uint32_t path_id
    )
  {
    WaypointPath path;
    path.id = path_id;

    std::map<std::uint32_t, WaypointPath> const paths
      (groupWaypointRows(connection.query(waypointSelectSql(schema, {path_id}))));

    auto const found (paths.find(path_id));

    // A path id with no rows is not an error: it is a creature_addon pointing at a path that
    // was never authored, which the editor has to be able to show rather than throw on.
    return found == paths.end() ? path : found->second;
  }

  std::uint32_t maxCreatureGuid(WorldDatabaseConnection const& connection)
  {
    return maxGuidOf(connection, TABLE_CREATURE);
  }

  std::uint32_t maxGameObjectGuid(WorldDatabaseConnection const& connection)
  {
    return maxGuidOf(connection, TABLE_GAMEOBJECT);
  }
}
