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
  //
  //      creature_addon is the deliberate exception: it is upserted, not cleared. The read
  //      path takes only path_id and a row-exists flag from it, so a DELETE there would throw
  //      away the mount, stand state, sheath state, emote and auras this class never read and
  //      cannot rewrite. It writes the one column the editor authored and leaves the rest of
  //      an existing row alone. Applying twice is still a no-op. See the section comment in
  //      build() for the full argument.
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

    // Formats one component of the rotation0..3 quaternion for SQL.
    //
    // Separate from coordinate() because the two sit at opposite ends of a float's dynamic
    // range, and a count of DECIMALS means different things there. Six decimals is about eleven
    // significant digits at a world coordinate's magnitude of ~1e4, but only six where
    // |v| <= 1 -- which is where every quaternion component lives, and six is not enough to
    // name a float uniquely. A gameobject read back with rotation2 = 0.4794255495071411
    // re-emits at six decimals as 0.479426 and reloads as 0.47942599654197693, eight float
    // steps away: a changeset that edited nothing still rewrites the stored bytes and shows up
    // as a diff in review, which is exactly what the "-0" handling next door exists to avoid.
    //
    // Nine SIGNIFICANT digits name any IEEE binary32 value uniquely, so this works to a
    // significant-digit precision rather than a fixed number of decimals. It stays in fixed
    // notation, so no exponent reaches the SQL, and it keeps coordinate()'s promise that no
    // value is ever emitted as "-0".
    std::string rotationComponent(double value);

    // Escapes a string for single-quoted SQL. Used for names and comments, never for
    // identifiers -- identifiers are validated, not escaped.
    std::string quote(std::string const& value);
  }
}

#endif
