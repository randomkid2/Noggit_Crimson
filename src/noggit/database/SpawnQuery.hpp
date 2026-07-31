// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_DATABASE_SPAWNQUERY_HPP
#define NOGGIT_DATABASE_SPAWNQUERY_HPP

#include <noggit/database/SchemaModel.hpp>
#include <noggit/database/SpawnTypes.hpp>
#include <noggit/database/TileCoordinates.hpp>

#include <cstddef>
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
  //
  // The module is split across two translation units, and not along the line this header
  // suggests: SpawnQueryDetail.cpp holds every SQL builder and every row decoder, including the
  // two builders declared below, because none of them needs a connection and keeping them out of
  // the unit that links the MySQL client is what lets them be tested without one. SpawnQuery.cpp
  // holds only loadTile, loadPath and the max-guid helpers. The internal surface those pure
  // functions share is declared in SpawnQueryDetail.hpp, not here: it is not part of this
  // contract.
  namespace SpawnQuery
  {
    // Column names are resolved through the SchemaModel rather than written as literals, so a
    // database using spawndist instead of wander_distance is read correctly rather than
    // failing with an unknown-column error.
    //
    // Both statements LEFT JOIN their template table, because that is the only place a display id
    // can come from: `gameobject` has no display column at all, and `creature.modelid` is 0 for
    // most real spawns and means "use the template's". Which template columns hold the model is
    // itself a schema question -- modelid1..4 or creature_template_model -- and is answered by
    // SchemaModel::creatureModelSource(). A template table or column that is missing degrades to a
    // literal in the select list rather than failing the query, so a tile still opens on a schema
    // this build only half recognises.
    //
    // Exposed because a caller may want to log or rehearse the statement, and because these are
    // pure: given a schema and a set of bounds they need no database at all.
    std::string creatureSelectSql
      (SchemaModel const& schema, std::uint16_t map, TileBounds const& bounds);

    std::string gameObjectSelectSql
      (SchemaModel const& schema, std::uint16_t map, TileBounds const& bounds);

    // How many spawns a region holds, without fetching any of them.
    //
    // These exist so the editor can say "this is 4,812 spawns, continue?" before committing to
    // the expensive part. That matters more than it sounds: the cost of loading a tile is not the
    // query, it is that every resolved spawn builds a ModelInstance and queues an asynchronous M2
    // load, so a dense city tile set can queue thousands of model loads and look like a hang.
    //
    // No joins and no ORDER BY -- a count needs neither, and a template join would make the
    // pre-flight check as expensive as the thing it is meant to guard. The map and bounds
    // predicate is identical to the select builders' by construction, so the count and the
    // subsequent load cannot disagree about which rows are in scope.
    std::string creatureCountSql
      (SchemaModel const& schema, std::uint16_t map, TileBounds const& bounds);

    std::string gameObjectCountSql
      (SchemaModel const& schema, std::uint16_t map, TileBounds const& bounds);

    // Total spawns in one tile. Two cheap COUNT queries, no rows fetched.
    std::size_t countTile
      ( WorldDatabaseConnection const& connection
      , SchemaModel const& schema
      , std::uint16_t map
      , TileIndex const& tile
      );

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
