// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/database/SchemaModel.hpp>

#include <algorithm>
#include <cctype>

using namespace Noggit::Database;

namespace
{
  constexpr char const* TABLE_CREATURE = "creature";
  constexpr char const* TABLE_CREATURE_ADDON = "creature_addon";
  constexpr char const* TABLE_CREATURE_TEMPLATE = "creature_template";
  constexpr char const* TABLE_CREATURE_TEMPLATE_MODEL = "creature_template_model";
  constexpr char const* TABLE_SMART_SCRIPTS = "smart_scripts";

  constexpr char const* COLUMN_WANDER_DISTANCE = "wander_distance";
  constexpr char const* COLUMN_SPAWNDIST = "spawndist";

  constexpr char const* PREFIX_EVENT_PARAM = "event_param";
  constexpr char const* PREFIX_TARGET_PARAM = "target_param";
}

SchemaModel::SchemaModel(std::vector<ColumnInfo> columns)
{
  for (ColumnInfo const& column : columns)
  {
    if (column.table_name.empty() || column.column_name.empty())
    {
      continue;
    }

    _tables[normalised(column.table_name)][normalised(column.column_name)] = column.ordinal_position;
    ++_column_count;
  }
}

std::string SchemaModel::normalised(std::string const& s)
{
  std::string out;
  out.reserve(s.size());

  for (char c : s)
  {
    out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }

  return out;
}

void SchemaModel::fail(std::string const& question, std::string const& tried)
{
  throw SchemaCapabilityError
    ("Cannot determine " + question + " for this database. Tried: " + tried
     + ". This schema is not a recognised TrinityCore 3.3.5 variant; refusing to guess, because"
       " guessing here corrupts every emitted changeset. Re-run /schema-check and update"
       " docs/schema-335.md if the target is legitimate.");
}

bool SchemaModel::hasTable(std::string const& table) const
{
  return _tables.find(normalised(table)) != _tables.end();
}

bool SchemaModel::hasColumn(std::string const& table, std::string const& column) const
{
  auto const table_it = _tables.find(normalised(table));

  if (table_it == _tables.end())
  {
    return false;
  }

  return table_it->second.find(normalised(column)) != table_it->second.end();
}

int SchemaModel::columnPosition(std::string const& table, std::string const& column) const
{
  auto const table_it = _tables.find(normalised(table));

  if (table_it == _tables.end())
  {
    return 0;
  }

  auto const column_it = table_it->second.find(normalised(column));

  return column_it == table_it->second.end() ? 0 : column_it->second;
}

std::size_t SchemaModel::countColumnsWithPrefix
  (std::string const& table, std::string const& prefix) const
{
  auto const table_it = _tables.find(normalised(table));

  if (table_it == _tables.end())
  {
    return 0;
  }

  std::string const needle (normalised(prefix));

  return static_cast<std::size_t>
    (std::count_if
      ( table_it->second.begin(), table_it->second.end()
      , [&needle] (auto const& entry)
        {
          return entry.first.rfind(needle, 0) == 0;
        }
      )
    );
}

std::string SchemaModel::wanderDistanceColumn() const
{
  if (hasColumn(TABLE_CREATURE, COLUMN_WANDER_DISTANCE))
  {
    return COLUMN_WANDER_DISTANCE;
  }

  if (hasColumn(TABLE_CREATURE, COLUMN_SPAWNDIST))
  {
    return COLUMN_SPAWNDIST;
  }

  fail("the creature wander-distance column", "creature.wander_distance, creature.spawndist");
}

std::string SchemaModel::versionTable() const
{
  // Probe the newer name first. The reference TDB 335.25101 database has only `version`, so
  // checking a single table is not enough -- this is one of the places published references
  // are wrong.
  if (hasTable("version_db_world"))
  {
    return "version_db_world";
  }

  if (hasTable("version"))
  {
    return "version";
  }

  fail("the database version table", "version_db_world, version");
}

std::string SchemaModel::poolMembersTable() const
{
  if (hasTable("pool_members"))
  {
    return "pool_members";
  }

  if (hasTable("pool_creature"))
  {
    return "pool_creature";
  }

  fail("the pool membership table", "pool_members, pool_creature");
}

std::string SchemaModel::waypointPathTable() const
{
  if (hasTable("waypoint_data"))
  {
    return "waypoint_data";
  }

  fail("the waypoint path table", "waypoint_data");
}

AddonPoseEncoding SchemaModel::addonPoseEncoding() const
{
  // Newer cores split bytes1/bytes2 into named columns. Check for a discrete column first:
  // a schema mid-migration could plausibly carry both, and the named columns are the ones
  // the core actually reads when present.
  if (hasColumn(TABLE_CREATURE_ADDON, "StandState"))
  {
    return AddonPoseEncoding::DISCRETE_COLUMNS;
  }

  if (hasColumn(TABLE_CREATURE_ADDON, "bytes1"))
  {
    return AddonPoseEncoding::PACKED_BYTES;
  }

  fail("the creature_addon pose encoding", "creature_addon.StandState, creature_addon.bytes1");
}

CreatureModelSource SchemaModel::creatureModelSource() const
{
  if (hasTable(TABLE_CREATURE_TEMPLATE_MODEL))
  {
    return CreatureModelSource::TEMPLATE_MODEL_TABLE;
  }

  if (hasColumn(TABLE_CREATURE_TEMPLATE, "modelid1"))
  {
    return CreatureModelSource::TEMPLATE_MODELID_COLUMNS;
  }

  fail
    ( "the creature model source"
    , "creature_template_model table, creature_template.modelid1"
    );
}

std::size_t SchemaModel::smartScriptsEventParamCount() const
{
  std::size_t const count (countColumnsWithPrefix(TABLE_SMART_SCRIPTS, PREFIX_EVENT_PARAM));

  if (count == 0)
  {
    fail("the smart_scripts event parameter count", "smart_scripts.event_param*");
  }

  return count;
}

std::size_t SchemaModel::smartScriptsTargetParamCount() const
{
  std::size_t const count (countColumnsWithPrefix(TABLE_SMART_SCRIPTS, PREFIX_TARGET_PARAM));

  if (count == 0)
  {
    fail("the smart_scripts target parameter count", "smart_scripts.target_param*");
  }

  return count;
}
