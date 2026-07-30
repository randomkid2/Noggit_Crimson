# M0 plan — schema introspection layer

**Status: awaiting approval.** No implementation code until this is signed off.

## The one idea that changes the schedule

Qt blocks *Noggit*. It does not have to block **M0's core**.

The riskiest part of M0 — deciding "does this database use `StandState` or `bytes1`" — is pure
data-in, data-out. It needs no Qt, no database, and no Connector/C++. If the layer is split so
that logic has zero dependencies, it can be written, tested and finished **now**, against the
two fixtures already captured. Only a thin settings adapter genuinely needs Qt.

| Layer | Depends on | Blocked? |
|---|---|---|
| `SchemaModel` — capability decisions from column metadata | nothing | **no** |
| `SchemaIntrospector` — fills metadata from `information_schema` | Connector/C++ | **no** (staged and configuring) |
| `WorldDatabaseConnection` — connection lifetime, write refusal | Connector/C++ | **no** |
| `DatabaseSettings` — reads credentials from `QSettings` | Qt | yes, and it is ~40 lines |

So M0 is roughly 90% unblocked. That is the main reason to do it in this order.

## Files

Following the newer convention actually used in `src/noggit/project` — lowercase directory,
PascalCase files, `Noggit::<Dir>` namespace, `NOGGIT_<NAME>_HPP` include guards. Note the tree
mixes older `snake_case.hpp` files in; new code follows the newer style.

```
src/noggit/database/
  ColumnInfo.hpp                 POD: table, ordinal, name, type, nullable, default, extra
  SchemaModel.hpp/.cpp           the capability model. No Qt, no SQL, no I/O.
  SchemaIntrospector.hpp/.cpp    information_schema -> vector<ColumnInfo>
  WorldDatabaseConnection.hpp/.cpp   one held connection; read-only vs dev-write modes
  DatabaseSettings.hpp/.cpp      QSettings adapter (the only Qt-dependent file)
  SchemaFixture.hpp/.cpp         loads the .tsv fixtures; shared by tests

tests/
  CMakeLists.txt                 Catch2 via FetchContent; links NO Qt
  SchemaModelTests.cpp
  SchemaIntrospectorTests.cpp    requires the dev DB; skipped if unreachable
```

## `SchemaModel` — the crux

Resolves names rather than exposing booleans, so callers cannot forget to branch:

```cpp
namespace Noggit::Database
{
  enum class AddonPoseEncoding { DISCRETE_COLUMNS, PACKED_BYTES };
  enum class CreatureModelSource { TEMPLATE_MODELID_COLUMNS, TEMPLATE_MODEL_TABLE };

  class SchemaModel
  {
    public:
      explicit SchemaModel (std::vector<ColumnInfo> columns);

      bool hasTable (std::string const& table) const;
      bool hasColumn (std::string const& table, std::string const& column) const;

      std::string wanderDistanceColumn() const;   // "wander_distance" | "spawndist"
      std::string versionTable() const;           // "version" | "version_db_world"
      std::string poolMembersTable() const;       // "pool_members" | "pool_creature"
      std::string waypointPathTable() const;      // "waypoint_data" | ...
      AddonPoseEncoding addonPoseEncoding() const;
      CreatureModelSource creatureModelSource() const;
      int smartScriptsEventParamCount() const;
      int smartScriptsTargetParamCount() const;

    private:
      std::unordered_map<std::string, std::vector<ColumnInfo>> _by_table;
  };
}
```

**Three rules that make this worth having:**

1. **Never guess.** If neither `wander_distance` nor `spawndist` is present, `throw`. Returning
   a default here is how you silently corrupt every changeset the tool ever emits — the very
   failure mode this layer exists to prevent. Loud beats plausible.
2. **Case-insensitive lookup.** MySQL identifier case behaviour varies by platform and
   `lower_case_table_names`. Comparing case-sensitively works on this machine and breaks on
   someone else's.
3. **No I/O and no Qt, ever.** The moment this class reads a file or a setting, it stops being
   testable in isolation and M0 becomes Qt-blocked again.

## `WorldDatabaseConnection`

Replaces, not extends, the existing `src/mysql/mysql.cpp` approach. Its `connect()` opens a
fresh connection per call *and* issues `CREATE DATABASE IF NOT EXISTS` every time — unusable
for per-tile spawn queries and a live-write hazard in its own right.

- Holds one connection for its lifetime.
- Two explicit modes: `ReadOnly` against any schema, `DevWrite` against the configured dev
  schema only. `DevWrite` construction against any other schema name fails.
- Every mutating call checks the mode. **This is enforced in code, independent of DB grants and
  of the guard hook** — three layers, each assuming the others may fail.
- Never issues DDL against a schema it did not create in dev mode.
- Migrate the five existing UID functions onto it and delete the old `connect()`. Leaving both
  alive means the hazardous path stays reachable.

Version detection probes `version_db_world` **then** `version`, exposing the raw `db_version`
string without parsing it into an ordering — capability lookups decide behaviour, never a
version comparison.

## Tests

Catch2 via FetchContent (header-only, lowest friction; no test framework exists in the tree
today). `tests/` links neither Qt nor Noggit — it compiles `SchemaModel.cpp`, `ColumnInfo.hpp`
and `SchemaFixture.cpp` directly, which is what lets it run before Qt lands.

Required cases:

- Every accessor, asserted against **both** fixtures, with **different** expected answers.
  `schema-tdb335-25101.tsv` must yield `wander_distance` / `DISCRETE_COLUMNS` /
  `TEMPLATE_MODELID_COLUMNS` / 5 / 4 / `pool_members` / `version`;
  `schema-alt-drifted.tsv` must yield `spawndist` / `PACKED_BYTES` / `TEMPLATE_MODEL_TABLE` /
  4 / 3 / `pool_creature` / `version_db_world`.
- A fixture with the discriminating columns stripped **throws**, per rule 1.
- Identifier casing varied in a fixture still resolves.
- `DevWrite` against a non-dev schema name is refused at construction.
- `SchemaIntrospector` against the live dev DB produces a model equal to the golden fixture —
  this is what proves the introspection query and the fixture format agree, and it is why the
  fixture is captured rather than hand-written.

## Definition of done

- `tests/` builds and **passes with Qt absent**; output pasted.
- Both fixtures asserted, producing different answers where the fixture README's table says
  they must. A run that returns identical answers for both is a failure even if one is correct.
- Unknown-schema throw covered.
- `DevWrite` refusal covered.
- Old `connect()` deleted, UID functions migrated, `/build` clean once Qt is present; output
  pasted.
- `/schema-check both` still green.
- `ATTRIBUTION.md` modification table updated; every new file carries the GPL header.

## Three decisions for you

1. **Catch2 vs GoogleTest.** Recommend **Catch2** — header-only, single FetchContent line, no
   separate build. GoogleTest is better for heavy mocking, which this does not need.
2. **Fix `src/mysql/mysql.cpp` in place, or replace it?** Recommend **replace and migrate the
   UID functions**, then delete the old path. Fixing in place leaves the per-call
   `CREATE DATABASE` pattern alive and reachable.
3. **`USE_SQL` or a new CMake option?** Recommend **reuse `USE_SQL`** — it already gates exactly
   this dependency set and is documented in the README. A second option means two ways to get a
   half-configured build.

## Not in M0

Rendering, tile queries, changeset emission, waypoint UI, anything touching the viewport. M0
ends when the schema layer is trustworthy and proven, because everything downstream inherits its
correctness.

## Known first task once Qt lands

`src/mysql/mysql.cpp` includes `<driver.h>` and `<prepared_statement.h>`, but
`MYSQLCPPCONN_INCLUDE` resolves to the *parent* of `cppconn/` — confirmed: with Connector/C++
26.7.0 staged, CMake resolved it to `<Noggit3libs>/mysql/connector`. The commented-out
`<cppconn/driver.h>` form is correct. The file also uses `QMessageBox` and `std::stringstream`
without including them. Left unfixed deliberately: it cannot be compiled yet, and this project
does not claim fixes it has not built.
