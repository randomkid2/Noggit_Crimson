// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_DATABASE_SCHEMAMODEL_HPP
#define NOGGIT_DATABASE_SCHEMAMODEL_HPP

#include <noggit/database/ColumnInfo.hpp>

#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace Noggit::Database
{
  // Raised when the schema answers a capability question in no recognised way.
  //
  // This is deliberately fatal rather than falling back to a default. Guessing "probably
  // wander_distance" against a schema that has neither column is how every changeset the tool
  // emits ends up silently wrong, which is the exact failure this class exists to prevent.
  class SchemaCapabilityError : public std::runtime_error
  {
    public:
      explicit SchemaCapabilityError(std::string const& message)
        : std::runtime_error(message) {}
  };

  // How creature_addon / creature_template_addon encode stand state, mount and sheath.
  enum class AddonPoseEncoding
  {
    DISCRETE_COLUMNS,   // StandState, AnimTier, VisFlags, SheathState, PvPFlags, MountCreatureID
    PACKED_BYTES        // bytes1, bytes2
  };

  // Where a creature's display id comes from.
  enum class CreatureModelSource
  {
    TEMPLATE_MODELID_COLUMNS,   // creature_template.modelid1..4
    TEMPLATE_MODEL_TABLE        // creature_template_model
  };

  // Answers "what does *this* database call the thing I need" from introspected column
  // metadata.
  //
  // Contains no I/O, no SQL and no Qt, on purpose. That keeps it testable against captured
  // fixtures with no database and no Qt present, and it is the only reason this layer could be
  // built before the rest of the toolchain was ready.
  //
  // Lookups are case-insensitive: MySQL identifier case behaviour varies with platform and
  // lower_case_table_names, so a case-sensitive comparison works on one machine and fails on
  // the next.
  class SchemaModel
  {
    public:
      SchemaModel() = default;
      explicit SchemaModel(std::vector<ColumnInfo> columns);

      bool hasTable(std::string const& table) const;
      bool hasColumn(std::string const& table, std::string const& column) const;

      // 1-based, matching information_schema. Returns 0 when the column is absent.
      int columnPosition(std::string const& table, std::string const& column) const;

      // Number of columns in `table` whose name begins with `prefix`.
      std::size_t countColumnsWithPrefix(std::string const& table, std::string const& prefix) const;

      std::size_t tableCount() const { return _tables.size(); }
      std::size_t columnCount() const { return _column_count; }

      // --- Resolved names and shapes -------------------------------------------------------
      // Each throws SchemaCapabilityError when the schema matches no known variant.

      // "wander_distance" on TrinityCore 3.3.5, "spawndist" on AzerothCore and older cores.
      std::string wanderDistanceColumn() const;

      // "version_db_world" where present, otherwise "version". Both exist in the wild; the
      // reference TDB 335.25101 database has only the latter.
      std::string versionTable() const;

      // "pool_members" once creature and gameobject pooling was unified, otherwise
      // "pool_creature".
      std::string poolMembersTable() const;

      // The table holding waypoint path nodes. "waypoint_data" everywhere seen so far.
      std::string waypointPathTable() const;

      AddonPoseEncoding addonPoseEncoding() const;
      CreatureModelSource creatureModelSource() const;

      // smart_scripts grew a fifth event param and a fourth target param over time. Emitting
      // the wrong count produces a column-count mismatch at best and silent misalignment at
      // worst.
      std::size_t smartScriptsEventParamCount() const;
      std::size_t smartScriptsTargetParamCount() const;

    private:
      static std::string normalised(std::string const& s);

      [[noreturn]] static void fail(std::string const& question, std::string const& tried);

      // table -> (column -> ordinal position). Keys are normalised.
      std::map<std::string, std::map<std::string, int>> _tables;
      std::size_t _column_count = 0;
  };
}

#endif
