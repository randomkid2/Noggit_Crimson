// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_DATABASE_SPAWNQUERYDETAIL_HPP
#define NOGGIT_DATABASE_SPAWNQUERYDETAIL_HPP

#include <noggit/database/SchemaModel.hpp>
#include <noggit/database/SpawnTypes.hpp>
#include <noggit/database/WorldDatabaseConnection.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Noggit::Database::SpawnQuery
{
  // The parts of the tile reader that are not part of its contract: the identifier rule its SQL
  // builders rest on, the one builder no caller outside the module needs, and the row decoders.
  //
  // These previously had external linkage in SpawnQuery.cpp and were declared nowhere, so
  // SpawnQueryTests.cpp reached them by re-declaring five of them by hand. That is a second,
  // unchecked source of truth for their signatures: the two copies stayed in step only because
  // ResultRow happens to be a typedef for std::vector<std::string>, and a change to either the
  // alias or a parameter would have produced a link error in one configuration and a silently
  // different overload in another. Declaring them once here is what makes the compiler check the
  // match.
  //
  // Everything below is a pure function of its arguments. None of it opens, holds or touches a
  // WorldDatabaseConnection, and SpawnQueryDetail.cpp calls nothing from the MySQL client, so
  // that translation unit -- and every test over it -- builds with no database client present.
  // WorldDatabaseConnection.hpp is included for the ResultRow alias alone; that header names no
  // connector type either, which is the reason the alias can be used here at all.
  namespace Detail
  {
    // Identifiers cannot be bound as query parameters, so the few that reach the SQL text -- the
    // schema-resolved wander distance column, the waypoint table name -- are interpolated.
    // Rather than escape them, anything that is not a plain identifier is refused: a far smaller
    // rule to get right, and no name that came out of information_schema needs more.
    bool isPlainIdentifier(std::string const& name);

    // Returns `name` unchanged, or throws SchemaCapabilityError. Refusing beats sanitising: a
    // name that needs sanitising did not come from information_schema, so something upstream is
    // already wrong and no amount of quoting makes the result trustworthy.
    std::string requireIdentifier(std::string const& name);

    // Nodes for a set of paths, ordered so a single pass groups them.
    //
    // Kept out of SpawnQuery.hpp because no caller builds this statement itself, but the id list
    // is the one place a caller-supplied value list reaches the SQL text and wpguid must never
    // appear in the select list, so both are worth a test.
    std::string waypointSelectSql
      (SchemaModel const& schema, std::vector<std::uint32_t> const& path_ids);

    // Positional decoders for the select lists this module builds. Positional because the
    // builders keep the select list a fixed width whatever the schema, substituting literals for
    // absent columns; the price is that a column added to a select list without moving the
    // decoder shifts every field after it, which is what the column-count tests exist to catch.
    //
    // Every field is read defensively, so a row shorter than the select list decodes to zeroes
    // rather than reading off the end of the vector. That cannot happen with the statements built
    // here, but these decode whatever the server actually hands back.
    //
    // parseCreatureRow also applies the display id resolution rule, over the four template model
    // candidates the select list ends with. It is done here rather than in SQL because the rule has
    // to give the same answer for both schema variants, and the two reach these columns through
    // entirely different select expressions -- modelid1..4 on one, correlated subqueries over
    // creature_template_model on the other. SpawnDisplay::resolveCreatureTemplateInfo holds the
    // rule itself and is tested directly.
    CreatureSpawn parseCreatureRow(ResultRow const& row);
    GameObjectSpawn parseGameObjectRow(ResultRow const& row);
    WaypointNode parseWaypointRow(ResultRow const& row);

    // One unsigned field, saturating rather than wrapping: a guid too wide for the column has to
    // stay obviously wrong instead of becoming a small number that collides with a real spawn.
    //
    // Declared alongside the decoders because the reading half of the module needs the same
    // primitive -- for the path id a waypoint row is grouped by, and for MAX(guid) -- and a
    // second copy of it there would be exactly the duplication this header removes.
    std::uint32_t rowUInt32(ResultRow const& row, std::size_t index);
  }
}

#endif
