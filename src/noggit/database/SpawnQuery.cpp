// This file is part of Noggit3, licensed under GNU General Public License (version 3).

// The half of the tile spawn reader that needs a connection: issue the statements, group what
// comes back.
//
// Every SQL builder and every row decoder lives in SpawnQueryDetail.cpp instead, declared in
// SpawnQueryDetail.hpp. They are pure functions and they are where the interesting failure modes
// are, so they are kept out of this translation unit -- which links the MySQL client -- and can be
// tested on a machine that has none.

#include <noggit/database/SpawnQuery.hpp>

#include <noggit/database/SpawnQueryDetail.hpp>
#include <noggit/database/TileCoordinates.hpp>
#include <noggit/database/WorldDatabaseConnection.hpp>

#include <algorithm>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

using namespace Noggit::Database;

namespace
{
  constexpr char const* TABLE_CREATURE = "creature";
  constexpr char const* TABLE_GAMEOBJECT = "gameobject";
}

namespace Noggit::Database::SpawnQuery
{
  namespace
  {
    std::map<std::uint32_t, WaypointPath> groupWaypointRows(ResultRows const& rows)
    {
      std::map<std::uint32_t, WaypointPath> paths;

      for (ResultRow const& row : rows)
      {
        std::uint32_t const id (Detail::rowUInt32(row, 0));

        WaypointPath& path (paths[id]);
        path.id = id;
        path.nodes.push_back(Detail::parseWaypointRow(row));
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

      return Detail::rowUInt32(rows.front(), 0);
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
      spawns.creatures.push_back(Detail::parseCreatureRow(row));
    }

    ResultRows const gameobject_rows
      (connection.query(gameObjectSelectSql(schema, map, bounds)));
    spawns.gameobjects.reserve(gameobject_rows.size());

    for (ResultRow const& row : gameobject_rows)
    {
      spawns.gameobjects.push_back(Detail::parseGameObjectRow(row));
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
        (groupWaypointRows(connection.query(Detail::waypointSelectSql(schema, path_ids))));

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
      (groupWaypointRows(connection.query(Detail::waypointSelectSql(schema, {path_id}))));

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
