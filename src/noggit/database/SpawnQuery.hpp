// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_DATABASE_SPAWNQUERY_HPP
#define NOGGIT_DATABASE_SPAWNQUERY_HPP

#include <noggit/database/SchemaModel.hpp>
#include <noggit/database/SpawnTypes.hpp>
#include <noggit/database/TileCoordinates.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace Noggit::Database
{
  class WorldDatabaseConnection;

  // Reads the spawns anchored to one ADT tile.
  //
  // Queries by computed world-coordinate bounds, never by zoneId or areaId: those are
  // core-derived and are frequently 0 in real data, so filtering on them silently returns
  // nothing.
  //
  // Read-only. Works through an account holding nothing but SELECT.
  namespace SpawnQuery
  {
    // Column names are resolved through the SchemaModel rather than written as literals, so a
    // database using spawndist instead of wander_distance is read correctly rather than
    // failing with an unknown-column error.
    std::string creatureSelectSql
      (SchemaModel const& schema, std::uint16_t map, TileBounds const& bounds);

    std::string gameObjectSelectSql
      (SchemaModel const& schema, std::uint16_t map, TileBounds const& bounds);

    // Loads creatures, gameobjects and every waypoint path belonging to a creature in the
    // tile. A path is included when a creature in the tile references it through
    // creature_addon.path_id, not by the path's own coordinates -- a patrol route may lead
    // well outside the tile its owner stands in.
    TileSpawns loadTile
      ( WorldDatabaseConnection const& connection
      , SchemaModel const& schema
      , std::uint16_t map
      , TileIndex const& tile
      );

    // One waypoint path by id, ordered by point.
    WaypointPath loadPath
      ( WorldDatabaseConnection const& connection
      , SchemaModel const& schema
      , std::uint32_t path_id
      );

    // Highest guid currently in use, for allocating new spawns without collision.
    std::uint32_t maxCreatureGuid(WorldDatabaseConnection const& connection);
    std::uint32_t maxGameObjectGuid(WorldDatabaseConnection const& connection);
  }
}

#endif
