// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_DATABASE_CHANGESETBUILDER_HPP
#define NOGGIT_DATABASE_CHANGESETBUILDER_HPP

#include <noggit/database/SchemaModel.hpp>
#include <noggit/database/SpawnTypes.hpp>

#include <string>
#include <vector>

namespace Noggit::Database
{
  // Builds a reviewable TDB-style .sql changeset.
  //
  // This is the only way spawn edits leave the editor. There is no path that writes to a live
  // database, by construction: this class produces text.
  //
  // Every emitted statement must satisfy three properties, all enforced by tests against
  // tools/dev-db/03_example_changeset.sql as the reference shape:
  //
  //   1. Idempotent -- DELETE precedes INSERT for every table touched, so applying twice
  //      leaves identical rows and raises no error.
  //   2. Variable-driven -- GUIDs declared once at the top as @CGUID/@OGUID/@PATH so a
  //      reviewer can retarget the whole changeset by editing the header.
  //   3. Column-explicit -- never positional, and no emitted column name is assumed to exist.
  //
  //      Names that DIFFER between core generations are resolved through the SchemaModel: the
  //      wander-distance column, the creature_addon pose columns, and the waypoint path table.
  //      A database using spawndist or bytes1 therefore gets correct SQL rather than silently
  //      wrong SQL.
  //
  //      Names that are stable across every supported generation appear as literals. That is
  //      deliberate, but it is not a guarantee they exist on an arbitrary target, so build()
  //      verifies every column name it has emitted against the schema before returning and
  //      throws if any is absent. Without that check a schema the READ path tolerates -- the
  //      reader treats a dozen columns as optional -- could produce a changeset that dies with
  //      ERROR 1054 at apply time, after its DELETE statements had already committed.
  //
  // zoneId and areaId are never emitted: the core derives them.
  class ChangesetBuilder
  {
    public:
      struct Options
      {
        // Prepended as SQL comments. Provenance for whoever reviews the file.
        std::string description;

        // Convention only, not enforced by the core: path_id = guid * 10.
        std::uint32_t path_id_multiplier = 10;

        // Refuse to emit anything that fails SpawnValidation. Off would let the editor produce
        // rows the server will reject or silently correct at load.
        bool reject_invalid = true;
      };

      ChangesetBuilder(SchemaModel schema, Options options = {});

      void addCreature(CreatureSpawn const& spawn);
      void addGameObject(GameObjectSpawn const& spawn);
      void addWaypointPath(WaypointPath const& path);

      // Marks an existing spawn for removal. Emits the DELETE without a matching INSERT.
      void removeCreature(std::uint32_t guid);
      void removeGameObject(std::uint32_t guid);
      void removeWaypointPath(std::uint32_t path_id);

      bool empty() const;

      // Problems found while adding. Non-empty means build() will throw when
      // Options::reject_invalid is set.
      std::vector<ValidationIssue> const& issues() const { return _issues; }

      // The complete .sql text.
      //
      // Throws ChangesetError when reject_invalid is set and any error-severity issue was
      // recorded. Refusing to produce a broken changeset is the point; a warning-only issue
      // is emitted as a SQL comment beside the statement it concerns.
      std::string build() const;

    private:
      SchemaModel _schema;
      Options _options;

      std::vector<CreatureSpawn> _creatures;
      std::vector<GameObjectSpawn> _gameobjects;
      std::vector<WaypointPath> _paths;
      std::vector<std::uint32_t> _removed_creatures;
      std::vector<std::uint32_t> _removed_gameobjects;
      std::vector<std::uint32_t> _removed_paths;
      std::vector<ValidationIssue> _issues;
  };

  class ChangesetError : public std::runtime_error
  {
    public:
      explicit ChangesetError(std::string const& message)
        : std::runtime_error(message) {}
  };

  namespace SqlFormat
  {
    // Formats a coordinate for SQL.
    //
    // The target columns are FLOAT. Emitting full double precision implies an accuracy the
    // column cannot hold and makes changesets needlessly noisy in review; emitting too few
    // digits moves the spawn. Six decimals is comfortably inside float precision at world
    // magnitudes and round-trips within tolerance.
    std::string coordinate(double value);

    // Escapes a string for single-quoted SQL. Used for names and comments, never for
    // identifiers -- identifiers are validated, not escaped.
    std::string quote(std::string const& value);
  }
}

#endif
