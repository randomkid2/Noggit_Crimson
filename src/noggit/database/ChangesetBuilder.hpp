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
  //      Spawn CREATION is the second exception, and it is a property of the problem rather
  //      than of this class. A row the database has never seen has no guid until the file is
  //      applied, so the new-spawn sections allocate from MAX(guid) at apply time and are
  //      append-only: nothing identifies "the row this file created last time", so there is no
  //      key a DELETE could name. Applying the file twice therefore creates the spawns twice.
  //      The alternative -- deleting by (entry, map, position) -- would delete a pre-existing
  //      duplicate somebody else authored, which is data loss in exchange for tidiness. The
  //      emitted file says all of this in its own header so the reviewer is told, not trusted
  //      to know.
  //   2. Variable-driven -- GUIDs declared once at the top as @CGUID/@OGUID/@PATH so a
  //      reviewer can retarget the whole changeset by editing the header. Created spawns get
  //      @CGUID_NEW/@OGUID_NEW, which are SET from MAX(guid) rather than from a literal --
  //      see addNewCreature.
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

      // Adds a spawn that does not exist in the target database yet.
      //
      // > [!important] `spawn.guid` is IGNORED, and that is the whole point
      // > The editor has to name a spawn it has just placed -- to select it, move it, list it --
      // > before any database has issued a guid for it, so SpawnSceneCache hands out a
      // > provisional one. Emitting that number would either collide with a real row or, worse,
      // > silently overwrite one: creature.guid and gameobject.guid are independent primary key
      // > sequences, both counting up from 1, and neither has any reserved range.
      // >
      // > So the guid is allocated where it can be allocated correctly -- at apply time, on the
      // > server, from MAX(guid) of the table concerned. The file declares
      // >
      // >     SET @CGUID_NEW := (SELECT IFNULL(MAX(`guid`), 0) FROM `creature`);
      // >
      // > and the created rows are @CGUID_NEW+1, +2, ... in the order they were added here. Two
      // > sequences, two variables, because the two tables are two key spaces.
      //
      // The cost is idempotency, and it is unavoidable: see the class comment. These sections
      // are append-only and applying the file twice creates the spawns twice.
      //
      // Nothing an addon row would hold is invented for a created creature. A creature arrives
      // here with has_addon false unless the caller authored a waypoint binding, and only then
      // is a creature_addon row written -- carrying the path the caller gave and the neutral
      // pose, never a mount, emote or aura this editor did not author.
      void addNewCreature(CreatureSpawn const& spawn);
      void addNewGameObject(GameObjectSpawn const& spawn);

      // Marks an existing spawn for removal. Emits the DELETE without a matching INSERT.
      //
      // Only ever for a row that is actually in the database. A guid the editor invented for a
      // spawn it created has no row behind it, so a DELETE naming it would either do nothing or
      // -- if the sequence has since reached that number -- remove somebody else's spawn.
      // SpawnSceneCache is what keeps the two apart; see SpawnSceneEntry::is_new.
      //
      // Removing a creature removes its creature_addon row with it. That is the one case where
      // clearing the addon is right: the creature row it belonged to is going away, so what
      // would be left is an orphan rather than anybody's data.
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

      // Kept apart from _creatures / _gameobjects rather than distinguished by a flag: these are
      // emitted from a different variable, in their own section, with no DELETE in front of them.
      // A single list with a predicate would put "is this row keyed by @CGUID or @CGUID_NEW" at
      // every emit site, and getting that wrong writes over an existing spawn.
      std::vector<CreatureSpawn> _new_creatures;
      std::vector<GameObjectSpawn> _new_gameobjects;

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
