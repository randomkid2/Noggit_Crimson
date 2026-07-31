// This file is part of Noggit3, licensed under GNU General Public License (version 3).

// The pure half of the tile spawn reader: every SQL builder and every row decoder.
//
// Split out of SpawnQuery.cpp so that none of it sits in a translation unit that links the MySQL
// client. Building a statement and decoding a result row are functions of a SchemaModel, a set of
// bounds and a vector of strings -- no connection is involved in either -- and they are where the
// interesting failure modes live: the wrong column name, a filter on a core-derived column, a
// tile whose bounds do not match its index, a row that arrived shorter than the select list. All
// of that is now testable on a machine with no database client installed at all, which is the
// state most machines building this fork are in.
//
// SpawnQuery.cpp keeps only what genuinely needs a connection: loadTile, loadPath and the two
// max-guid helpers.

#include <noggit/database/SpawnQueryDetail.hpp>

#include <noggit/database/SpawnQuery.hpp>
#include <noggit/database/TileCoordinates.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <ios>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace Noggit::Database;

namespace
{
  constexpr char const* TABLE_CREATURE = "creature";
  constexpr char const* TABLE_CREATURE_ADDON = "creature_addon";
  constexpr char const* TABLE_CREATURE_TEMPLATE = "creature_template";
  constexpr char const* TABLE_CREATURE_TEMPLATE_MODEL = "creature_template_model";
  constexpr char const* TABLE_GAMEOBJECT = "gameobject";
  constexpr char const* TABLE_GAMEOBJECT_TEMPLATE = "gameobject_template";

  constexpr char const* ALIAS_CREATURE = "c";
  constexpr char const* ALIAS_ADDON = "a";
  constexpr char const* ALIAS_GAMEOBJECT = "g";
  constexpr char const* ALIAS_WAYPOINT = "w";

  // The joined template table, whichever statement it appears in. Single-letter aliases match the
  // rest of this file; the two statements never share one, so `t` can mean both templates.
  constexpr char const* ALIAS_TEMPLATE = "t";
  constexpr char const* ALIAS_TEMPLATE_MODEL = "m";

  constexpr char const* COLUMN_PATH_ID = "path_id";
  constexpr char const* COLUMN_POSITION_X = "position_x";
  constexpr char const* COLUMN_POSITION_Y = "position_y";
  constexpr char const* COLUMN_ENTRY = "entry";
  constexpr char const* COLUMN_NAME = "name";

  // creature_template_model, where it exists instead of modelid1..4.
  constexpr char const* COLUMN_CREATURE_ID = "CreatureID";
  constexpr char const* COLUMN_CREATURE_DISPLAY_ID = "CreatureDisplayID";
  constexpr char const* COLUMN_IDX = "Idx";

  // The template name is selected under one alias whatever the source column is called, so the
  // positional decoder and a reader of the query log both see the same name.
  constexpr char const* ALIAS_TEMPLATE_NAME = "template_name";
  constexpr char const* ALIAS_MODEL_CANDIDATE_PREFIX = "model_candidate_";

  // Model candidate columns emitted per creature, always, whatever the schema.
  //
  // Fixed width is what keeps the positional decoder free of schema branching, and four is the cap
  // creature_template.modelid1..4 imposes anyway -- so a creature_template_model carrying more
  // than four rows is capped here too, which costs nothing: the count only has to answer "was
  // there more than one", and four is already more than one.
  constexpr std::size_t MODEL_CANDIDATE_COUNT = 4;

  // Where the template columns land in each select list. Named rather than written as literals at
  // the decoder, unlike the older indices, because these two have to move together with
  // MODEL_CANDIDATE_COUNT: raising the cap without shifting the name index would decode the
  // fourth candidate as the name and the name as nothing.
  constexpr std::size_t CREATURE_MODEL_CANDIDATE_BASE = 22;
  constexpr std::size_t CREATURE_TEMPLATE_NAME_INDEX
    = CREATURE_MODEL_CANDIDATE_BASE + MODEL_CANDIDATE_COUNT;

  constexpr std::size_t GAMEOBJECT_DISPLAY_ID_INDEX = 16;
  constexpr std::size_t GAMEOBJECT_TYPE_INDEX = 17;
  constexpr std::size_t GAMEOBJECT_TEMPLATE_NAME_INDEX = 18;

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

  // A column from a LEFT JOINed template table.
  //
  // Two independent reasons to fall back to a literal -- the table is not joined at all, or it is
  // joined but lacks the column -- and both have to be checked here, because columnOr knows
  // nothing about whether the alias it is handed actually appears in the statement. Emitting
  // `t.displayId` for a statement with no `t` in it produces an unknown-column error on every
  // tile, which is the one failure mode this whole degrade-with-literals scheme exists to avoid.
  std::string joinedColumnOr
    ( SchemaModel const& schema
    , bool joined
    , std::string const& table
    , std::string const& column
    , std::string const& fallback = "0"
    )
  {
    if (!joined)
    {
      return fallback + " AS " + quoted(column);
    }

    return columnOr(schema, table, ALIAS_TEMPLATE, column, fallback);
  }

  // The template name, under a fixed alias whatever the source column is called or whether it is
  // there at all.
  //
  // NULL rather than an empty string literal for the fallback: this module emits no string literal
  // anywhere, which is both what makes the injection test meaningful and what keeps a schema-drift
  // report free of quoting questions. NULL decodes to an empty name, which is the truthful answer.
  std::string templateNameExpression
    (SchemaModel const& schema, bool joined, std::string const& table)
  {
    if (joined && schema.hasColumn(table, COLUMN_NAME))
    {
      return columnRef(ALIAS_TEMPLATE, COLUMN_NAME) + " AS " + quoted(ALIAS_TEMPLATE_NAME);
    }

    return "NULL AS " + quoted(ALIAS_TEMPLATE_NAME);
  }

  std::string modelCandidateAlias(std::size_t ordinal)
  {
    return quoted(ALIAS_MODEL_CANDIDATE_PREFIX + std::to_string(ordinal));
  }

  // One model candidate from creature_template_model, by rank.
  //
  // A correlated subquery rather than a join, because creature_template_model holds one row PER
  // MODEL: joining it would return each creature once per model row and silently multiply every
  // spawn in the tile -- four copies of a creature at the same coordinates, which renders as one
  // object and counts as four.
  //
  // LIMIT with an explicit ORDER BY, never a bare LIMIT: an unordered LIMIT 1 may legitimately
  // return a different row on each execution, and the point of resolving a display id at all is
  // that two reads of an unchanged tile draw the same model.
  std::string templateModelSubquery(SchemaModel const& schema, std::size_t rank)
  {
    bool const usable
      ( schema.hasColumn(TABLE_CREATURE_TEMPLATE_MODEL, COLUMN_CREATURE_ID)
        && schema.hasColumn(TABLE_CREATURE_TEMPLATE_MODEL, COLUMN_CREATURE_DISPLAY_ID)
      );

    if (!usable)
    {
      return "0";
    }

    // Idx is the template's own ordering. Where it is absent the display id is ordered on instead:
    // an arbitrary but stable order beats a nondeterministic one, and nothing here depends on the
    // order being meaningful -- only on it being the same order next time.
    std::string const order
      ( schema.hasColumn(TABLE_CREATURE_TEMPLATE_MODEL, COLUMN_IDX)
        ? COLUMN_IDX
        : COLUMN_CREATURE_DISPLAY_ID
      );

    // Zero display ids are filtered out here so the candidates arrive dense. Otherwise a template
    // whose first four rows all hold 0 would resolve to nothing while a fifth, usable row sat
    // unread past the cap.
    return "(SELECT " + columnRef(ALIAS_TEMPLATE_MODEL, COLUMN_CREATURE_DISPLAY_ID)
      + " FROM " + quoted(TABLE_CREATURE_TEMPLATE_MODEL) + " AS " + ALIAS_TEMPLATE_MODEL
      + " WHERE " + columnRef(ALIAS_TEMPLATE_MODEL, COLUMN_CREATURE_ID) + " = "
      + columnRef(ALIAS_CREATURE, "id")
      + " AND " + columnRef(ALIAS_TEMPLATE_MODEL, COLUMN_CREATURE_DISPLAY_ID) + " <> 0"
      + " ORDER BY " + columnRef(ALIAS_TEMPLATE_MODEL, order)
      + " LIMIT 1 OFFSET " + std::to_string(rank) + ")";
  }

  // MODEL_CANDIDATE_COUNT expressions, always, in the template's own order.
  std::vector<std::string> creatureModelCandidates
    (SchemaModel const& schema, bool join_template)
  {
    // creatureModelSource() throws when the schema matches neither known variant. That is the
    // right answer for a changeset emitter and the wrong one for a reader: refusing to open a tile
    // because no model could be resolved shows nothing at all, where showing every spawn as an
    // unresolved marker still lets the user work.
    //
    // Caught rather than pre-checked, so SchemaModel stays the single authority on which variant
    // this is. Repeating its two probes here is exactly how two copies of a rule come to disagree,
    // and this one already has a documented history of being got wrong -- the published references
    // claim creature_template_model coexists with modelid1..4 on 3.3.5, and it does not.
    CreatureModelSource source (CreatureModelSource::TEMPLATE_MODELID_COLUMNS);
    bool source_known (true);

    try
    {
      source = schema.creatureModelSource();
    }
    catch (SchemaCapabilityError const&)
    {
      source_known = false;
    }

    std::vector<std::string> expressions;
    expressions.reserve(MODEL_CANDIDATE_COUNT);

    for (std::size_t i (0); i < MODEL_CANDIDATE_COUNT; ++i)
    {
      std::string const alias (" AS " + modelCandidateAlias(i + 1));

      if (!source_known)
      {
        expressions.push_back("0" + alias);
        continue;
      }

      if (source == CreatureModelSource::TEMPLATE_MODEL_TABLE)
      {
        expressions.push_back(templateModelSubquery(schema, i) + alias);
        continue;
      }

      // Each modelid column is checked on its own rather than inferred from the source kind:
      // modelid1 being present says nothing about modelid3, and naming an absent one would fail
      // the whole query rather than lose one candidate.
      std::string const column ("modelid" + std::to_string(i + 1));

      if (join_template && schema.hasColumn(TABLE_CREATURE_TEMPLATE, column))
      {
        expressions.push_back(columnRef(ALIAS_TEMPLATE, column) + alias);
      }
      else
      {
        expressions.push_back("0" + alias);
      }
    }

    return expressions;
  }

  std::string boundsPredicate(std::string const& alias, TileBounds const& bounds)
  {
    requireUsableBounds(bounds);

    // (min, max] -- lower edge EXCLUSIVE, upper edge INCLUSIVE. This must match
    // TileCoordinates::tileForPosition exactly, and the interval is that way round because the
    // axis runs backwards: inverting floor(32 - x / TILE_SIZE) == i gives
    // (31-i)*TILE_SIZE < x <= (32-i)*TILE_SIZE.
    //
    // Getting this backwards is not a cosmetic off-by-one. With `>= min AND < max`, a spawn at
    // exactly x = 0.0 or y = 0.0 -- both exactly representable in a FLOAT column and common in
    // real data -- is excluded from the tile tileForPosition assigns it to and returned for the
    // neighbour instead, so it renders on the wrong ADT and its own tile looks empty. The
    // shift is uniform: for v = m * TILE_SIZE the owner is tile 32-m but `>=`/`<` matches
    // 31-m. A position at exactly +MAP_HALF_EXTENT would belong to no queryable tile at all.
    return columnRef(alias, COLUMN_POSITION_X) + " > " + formatBound(bounds.min_x)
      + " AND " + columnRef(alias, COLUMN_POSITION_X) + " <= " + formatBound(bounds.max_x)
      + " AND " + columnRef(alias, COLUMN_POSITION_Y) + " > " + formatBound(bounds.min_y)
      + " AND " + columnRef(alias, COLUMN_POSITION_Y) + " <= " + formatBound(bounds.max_y);
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
  namespace Detail
  {
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
    std::string const wander (Detail::requireIdentifier(schema.wanderDistanceColumn()));

    // creature_addon is where path_id lives, so the join is how a spawn learns which waypoint
    // path it belongs to. Where the table is missing the select list keeps its width with
    // literals rather than changing shape.
    bool const join_addon
      ( schema.hasTable(TABLE_CREATURE_ADDON)
        && schema.hasColumn(TABLE_CREATURE_ADDON, COLUMN_PATH_ID)
        && schema.hasColumn(TABLE_CREATURE_ADDON, "guid")
      );

    // creature_template carries the model to fall back on when creature.modelid is 0 -- which is
    // the overwhelming majority of real spawns -- and the name the UI labels them with. Without
    // this join a creature with modelid 0 can never be resolved to a model at all.
    //
    // Joined on the template's primary key, so it cannot multiply rows. LEFT, not INNER: a spawn
    // naming a template that does not exist is broken data the editor has to be able to show, and
    // an INNER JOIN would hide it and make the tile look emptier than it is.
    bool const join_template
      ( schema.hasTable(TABLE_CREATURE_TEMPLATE)
        && schema.hasColumn(TABLE_CREATURE_TEMPLATE, COLUMN_ENTRY)
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

    // The template columns close the select list rather than sitting beside creature.modelid, so
    // that every index the row decoder already uses is left where it was.
    for (std::string const& candidate : creatureModelCandidates(schema, join_template))
    {
      sql += ", " + candidate;
    }

    sql += ", " + templateNameExpression(schema, join_template, TABLE_CREATURE_TEMPLATE);

    sql += "\nFROM " + quoted(TABLE_CREATURE) + " AS " + ALIAS_CREATURE;

    if (join_addon)
    {
      sql += "\nLEFT JOIN " + quoted(TABLE_CREATURE_ADDON) + " AS " + ALIAS_ADDON
        + " ON " + columnRef(ALIAS_ADDON, "guid") + " = " + columnRef(ALIAS_CREATURE, "guid");
    }

    if (join_template)
    {
      sql += "\nLEFT JOIN " + quoted(TABLE_CREATURE_TEMPLATE) + " AS " + ALIAS_TEMPLATE
        + " ON " + columnRef(ALIAS_TEMPLATE, COLUMN_ENTRY) + " = "
        + columnRef(ALIAS_CREATURE, "id");
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

    // `gameobject` has no display column of any kind, so this join is the ONLY way a gameobject
    // resolves to a model. Without it nothing can be drawn for one, which is what blocked
    // rendering outright. Joined on the template's primary key and LEFT, for the same reasons as
    // the creature template join.
    bool const join_template
      ( schema.hasTable(TABLE_GAMEOBJECT_TEMPLATE)
        && schema.hasColumn(TABLE_GAMEOBJECT_TEMPLATE, COLUMN_ENTRY)
      );

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

    // displayId falls back to 0, which already means "nothing to draw", so an unresolvable
    // gameobject degrades to a marker rather than to a wrong model. `type` falls back to 0 (DOOR),
    // which reads as renderable -- deliberately, because the fallback only happens when displayId
    // fell back too and the spawn is excluded on that ground alone. Claiming a type this build
    // could not read is one of the six unrenderable ones would hide real objects.
    sql += ", " + joinedColumnOr(schema, join_template, TABLE_GAMEOBJECT_TEMPLATE, "displayId");
    sql += ", " + joinedColumnOr(schema, join_template, TABLE_GAMEOBJECT_TEMPLATE, "type");
    sql += ", " + templateNameExpression(schema, join_template, TABLE_GAMEOBJECT_TEMPLATE);

    sql += "\nFROM " + quoted(TABLE_GAMEOBJECT) + " AS " + ALIAS_GAMEOBJECT;

    if (join_template)
    {
      sql += "\nLEFT JOIN " + quoted(TABLE_GAMEOBJECT_TEMPLATE) + " AS " + ALIAS_TEMPLATE
        + " ON " + columnRef(ALIAS_TEMPLATE, COLUMN_ENTRY) + " = "
        + columnRef(ALIAS_GAMEOBJECT, "id");
    }

    sql += "\nWHERE " + columnRef(ALIAS_GAMEOBJECT, "map") + " = " + std::to_string(map)
      + "\n  AND " + boundsPredicate(ALIAS_GAMEOBJECT, bounds);

    sql += "\nORDER BY " + columnRef(ALIAS_GAMEOBJECT, "guid");

    return sql;
  }

  namespace
  {
    // Shared body of the two count builders.
    //
    // The map and bounds predicate is produced by the same boundsPredicate() the select builders
    // use, which is the point: a count that disagreed with the load it is meant to describe would
    // be worse than no count at all -- it would report a number the user then does not get.
    std::string countSql
      ( std::string const& table
      , std::string const& alias
      , std::uint16_t map
      , TileBounds const& bounds
      )
    {
      return "SELECT COUNT(*)\nFROM " + quoted(table) + " AS " + alias
        + "\nWHERE " + columnRef(alias, "map") + " = " + std::to_string(map)
        + "\n  AND " + boundsPredicate(alias, bounds);
    }
  }

  std::string creatureCountSql
    (SchemaModel const& schema, std::uint16_t map, TileBounds const& bounds)
  {
    if (!schema.hasTable(TABLE_CREATURE))
    {
      failSchema("there is no creature table");
    }

    return countSql(TABLE_CREATURE, ALIAS_CREATURE, map, bounds);
  }

  std::string gameObjectCountSql
    (SchemaModel const& schema, std::uint16_t map, TileBounds const& bounds)
  {
    if (!schema.hasTable(TABLE_GAMEOBJECT))
    {
      failSchema("there is no gameobject table");
    }

    return countSql(TABLE_GAMEOBJECT, ALIAS_GAMEOBJECT, map, bounds);
  }

  namespace Detail
  {
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

    // --- Row decoding ----------------------------------------------------------------------

    std::uint32_t rowUInt32(ResultRow const& row, std::size_t index)
    {
      return narrowed<std::uint32_t>(toInt64(fieldOr(row, index)));
    }

    CreatureSpawn parseCreatureRow(ResultRow const& row)
    {
      CreatureSpawn spawn;

      spawn.guid = rowUInt32(row, 0);
      spawn.id = rowUInt32(row, 1);
      spawn.map = toU16(row, 2);
      spawn.spawn_mask = toU8(row, 3);
      spawn.phase_mask = rowUInt32(row, 4);
      spawn.model_id = rowUInt32(row, 5);
      spawn.equipment_id = toI8(row, 6);

      spawn.position.x = toDouble(row, 7);
      spawn.position.y = toDouble(row, 8);
      spawn.position.z = toDouble(row, 9);
      spawn.orientation = toDouble(row, 10);

      spawn.spawn_time_secs = rowUInt32(row, 11);
      spawn.wander_distance = toDouble(row, 12);
      spawn.current_waypoint = rowUInt32(row, 13);
      spawn.cur_health = rowUInt32(row, 14);
      spawn.cur_mana = rowUInt32(row, 15);
      spawn.movement_type = toMovementType(row, 16);
      spawn.npc_flag = rowUInt32(row, 17);
      spawn.unit_flags = rowUInt32(row, 18);
      spawn.dynamic_flags = rowUInt32(row, 19);

      spawn.path_id = rowUInt32(row, 20);
      spawn.has_addon = toInt64(fieldOr(row, 21)) != 0;

      // Column 5 is creature.modelid; the four that follow has_addon are the template's candidates
      // in the template's own order.
      //
      // Resolution is deliberately done here rather than as a COALESCE chain in the statement. The
      // rule -- the spawn's own modelid wins, otherwise the first non-zero candidate, and say
      // whether there were others -- is the part worth testing, and in SQL it would be testable
      // only as text. It also has to produce the same answer for both schema variants, and the two
      // variants reach this point through completely different select expressions.
      std::vector<std::uint32_t> candidates;
      candidates.reserve(MODEL_CANDIDATE_COUNT);

      for (std::size_t i (0); i < MODEL_CANDIDATE_COUNT; ++i)
      {
        candidates.push_back(rowUInt32(row, CREATURE_MODEL_CANDIDATE_BASE + i));
      }

      spawn.template_info = SpawnDisplay::resolveCreatureTemplateInfo
        (spawn.model_id, candidates, fieldOr(row, CREATURE_TEMPLATE_NAME_INDEX));

      return spawn;
    }

    GameObjectSpawn parseGameObjectRow(ResultRow const& row)
    {
      GameObjectSpawn spawn;

      spawn.guid = rowUInt32(row, 0);
      spawn.id = rowUInt32(row, 1);
      spawn.map = toU16(row, 2);
      spawn.spawn_mask = toU8(row, 3);
      spawn.phase_mask = rowUInt32(row, 4);

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
      spawn.anim_progress = rowUInt32(row, 14);
      spawn.state = rowUInt32(row, 15);

      // From gameobject_template. There is no per-spawn alternative to fall back on and no
      // resolution rule to apply: the template's displayId is the only display id a gameobject
      // has, and 0 means there is nothing to draw.
      spawn.template_info.display_id = rowUInt32(row, GAMEOBJECT_DISPLAY_ID_INDEX);
      spawn.template_info.type = rowUInt32(row, GAMEOBJECT_TYPE_INDEX);
      spawn.template_info.name = fieldOr(row, GAMEOBJECT_TEMPLATE_NAME_INDEX);

      return spawn;
    }

    // The path id in column 0 is the caller's business; grouping reads it separately.
    WaypointNode parseWaypointRow(ResultRow const& row)
    {
      WaypointNode node;

      node.point = rowUInt32(row, 1);
      node.position.x = toDouble(row, 2);
      node.position.y = toDouble(row, 3);
      node.position.z = toDouble(row, 4);

      node.has_orientation = !isNull(row, 5);
      node.orientation = node.has_orientation ? toDouble(row, 5) : 0.0;

      node.delay_ms = rowUInt32(row, 6);
      node.move_type = toWaypointMoveType(row, 7);
      node.action = rowUInt32(row, 8);
      node.action_chance = rowUInt32(row, 9);

      return node;
    }
  }
}
