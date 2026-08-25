# Design notes

Why this fork is shaped the way it is. These are the decisions that are hard to infer from the
code, plus the handful of facts that have each cost somebody a day.

[`CONTRIBUTING.md`](CONTRIBUTING.md) states the project rules; this file explains the reasoning
behind them. [`setup.md`](setup.md) covers building and running. [`schema-335.md`](schema-335.md)
is the measured database ground truth. [`milestones.md`](milestones.md) records what was built
when, and what evidence backed each claim.

## What this fork is

**Noggit Crimson** — a fork of [Marlamin/noggit-red](https://github.com/Marlamin/noggit-red)
("Noggit Red", upstream base `6f0776d4`, 2025-01-17) that renders and edits **TrinityCore 3.3.5a
world-database spawns on the ADT view**: creature and gameobject placement, and a tile-centric
staged-edit workflow adapted to server-side data.

Two further goals from the original plan — a **visual waypoint editor** and a **chunk mover** —
are only half built. Both have a complete, unit-tested logic layer (`waypoint_data` modelling and
emission; `ChunkTransform`'s translate and rotate planning) and **neither has a user interface**.
Do not read the paragraph above as a feature list; the current one is in
[`../README.md`](../README.md).

Everything database-related is opt-in behind the CMake `USE_SQL` option, which is `OFF` by
default. With it off, four sources are dropped from the build and the editor is ordinary Noggit
Red plus this fork's terrain work. That gating is not a packaging convenience — it is what keeps
the majority of the new logic (tile maths, spawn types, changeset emission, the SQL builders, the
spawn scene cache) free of Qt, OpenGL and the connector, and therefore exhaustively testable on a
machine with no MySQL at all. See [Tests](setup.md#tests).

## Scope: TrinityCore 3.3.5a, with a seam

Target is TrinityCore 3.3.5a against client build 3.3.5a (12340). Only TC 3.3.5 defaults are
implemented, deliberately — an untested AzerothCore code path would be worse than none, because it
would look like support.

What exists instead is a seam, so the port stays cheap:

- **`ConnectionConfig` is a plain struct with no Qt in it.** Reading those values out of
  `QSettings` is `DatabaseSettings`' job, at the application boundary. That single decision is why
  the whole database layer can be built and tested without Qt present.
- **`SchemaModel` answers "what does *this* database call the thing I need"** from introspected
  column metadata, with no I/O and no SQL of its own. The variation points it already models are
  the ones that differ across cores: `wander_distance` vs `spawndist`, `pool_members` vs
  `pool_creature`, `version_db_world` vs `version`, discrete addon pose columns vs packed
  `bytes1`/`bytes2`, `creature_template.modelid1..4` vs a `creature_template_model` table, and the
  event/target parameter counts of `smart_scripts`.

Adding a core means teaching `SchemaModel` another variant and testing it against a real database
of that flavour. It does not mean touching the query, emission or rendering layers.

Note that the connector choice is fixed: the tree uses **MySQL Connector/C++'s legacy JDBC
(`cppconn`) API**, which is what upstream already links and what `src/mysql/` was written against.
Qt's `QMYSQL` driver is not used and should not be introduced — a second connection stack would
mean a second place for the write guard described below to be absent.

## Data safety: the editor produces text, not writes

The central design constraint is that **there is no code path which opens a live world database
read/write**, and there must never be one — not behind a flag, not behind a confirmation dialog.
All edits leave the editor as a reviewable `.sql` changeset. The only schema the application will
execute a statement against is the one nominated as writable, normally a disposable
`noggit_dev_world`.

### Why changesets rather than direct writes

The obvious design — edit in the viewport, `UPDATE` the row — was rejected for four reasons, and
they are worth stating because the changeset path costs real ergonomics:

1. **A world database is somebody's live server.** The blast radius of a bug in a graphical
   editor writing directly to `creature` is a wrecked production world with no diff to inspect.
   Text can be read before it is applied.
2. **Review is the normal workflow in this ecosystem.** TDB-style `.sql` files with a `DELETE`
   then `INSERT` per table and `@CGUID`-style variables at the top are what server developers
   already exchange, apply, revert and put in version control. Emitting that format means the
   tool's output drops into an existing process instead of demanding a new one.
3. **A file survives the tool.** A changeset can be applied by hand, applied months later, or
   applied to a server the editor cannot reach. A direct-write editor's output only exists inside
   whatever session produced it.
4. **It makes the safety claim checkable.** "The application cannot write to your world database"
   is a property that can be established by grepping the source. "The application only writes when
   you meant it to" cannot.

`ChangesetBuilder` is the only way spawn edits leave the editor, and it produces a `std::string`.
Every statement it emits satisfies three properties, tested against
`tools/dev-db/03_example_changeset.sql` as the reference shape:

- **Idempotent** — `DELETE` precedes `INSERT` for every table touched, so applying twice leaves
  identical rows and raises no error. `creature_addon` is the deliberate exception: it is upserted
  rather than cleared, because the read path only takes `path_id` and a row-exists flag from it, and
  a `DELETE` would silently discard the mount, stand state, sheath state, emote and auras the
  editor never read and cannot rewrite.
- **Variable-driven** — GUIDs are declared once at the top as `@CGUID` / `@OGUID` / `@PATH`, and
  referenced as `@CGUID+7` and so on. A reviewer retargets the whole file by editing the header.
- **Column-explicit** — never positional, and no emitted column name is assumed to exist. Names
  that vary across cores are resolved through `SchemaModel`; names that are stable appear as
  literals, but `build()` still verifies **every** name it emitted against the schema before
  returning. Without that check, a schema the read path tolerates (the reader treats a dozen
  columns as optional) could produce a changeset that dies with `ERROR 1054` at apply time —
  *after* its `DELETE` statements had already committed.

`zoneId` and `areaId` are never emitted: the core derives them.

### Where the guard actually lives

`WorldDatabaseConnection` is the single point at which a statement reaches a server, and it is
built around `AccessMode`:

- **`READ_ONLY` is the posture for anything pointed at a real server.** Reading a live world
  database is fine; only writing is not. The constructor additionally asks for
  `SET SESSION TRANSACTION READ ONLY`, wrapped in a `try` because not every server honours it —
  belt as well as braces, so a connection handed to code that forgets the mode check is still
  read-only at the session level.
- **`DEV_WRITE` refuses construction** unless `config.schema` is exactly the configured writable
  schema. Refusing at construction rather than at first write means the mistake cannot be made at
  all, instead of being made and then caught.
- **`execute()` re-checks the same condition before every individual statement**, rather than
  trusting the construction-time result. `setSchema` could have been called since, and this is the
  last line before a socket.
- **`executeScript()` splits a changeset into statements and then routes each through
  `execute()`.** Splitting first and guarding second is deliberate: it means there is exactly one
  place a statement reaches the server, so the batch entry point cannot become a bypass. Order and
  connection identity both matter here, because the changesets `SET @CGUID := …` and refer to it
  later, and a session variable only survives on the connection that set it.

Also load-bearing, and easy to undo by accident: `WorldDatabaseConnection` exposes **no
prepared-statement API**. Callers concatenate, which is safe only because every value spliced into
a statement is either a number or an identifier that came out of `information_schema`. If a
free-text value ever needs to reach the server, that is the moment to add binding — not to reach
for a quoting helper.

This is one of three layers, not the whole of the protection. The other two, and the honest limits
of all of them, are in [How the write protection is
layered](setup.md#how-the-write-protection-is-layered).

## The schema is discovered, never assumed

**Never hardcode a database column name or ordinal position.** Verify against
`information_schema` at runtime, through `SchemaIntrospector` and `SchemaModel`.

This is not defensive habit, it is a response to measurement. `docs/schema-335.md` records where
published references for 3.3.5 disagree with a real TDB database, column by column, with the
severity of believing each one. The reference database used to build this fork is **TDB
335.25101**.

Two consequences worth knowing before you write a query:

- **Version detection must probe both `version_db_world` *and* `version`.** Published references
  say to read `version_db_world`. On the reference database that table **does not exist** — there
  is only `version`, carrying `core_version`, `core_revision`, `db_version` and `cache_id`. Both
  names are in the wild. `SchemaModel::versionTable()` prefers `version_db_world` where present and
  falls back to `version`; `DoctorConnectionChecks` reports the schema as unusable only when
  neither is found.
- **An unrecognised schema is fatal, not defaulted.** `SchemaModel` throws
  `SchemaCapabilityError` when a capability question has no recognised answer, rather than
  guessing. Guessing "probably `wander_distance`" against a database that has neither column is
  precisely how every changeset the tool emits ends up silently wrong, which is the failure the
  class exists to prevent.

Lookups are case-insensitive throughout, because MySQL identifier case behaviour varies with
platform and `lower_case_table_names` — a case-sensitive comparison works on one machine and fails
on the next.

## The coordinate-frame trap

**This is the single most dangerous thing in the codebase.** Read it before touching anything that
handles a tile index.

There are two structures named `TileIndex`, in different frames, and they are **transposed**:

| | Type | `x` derived from | second field |
|---|---|---|---|
| Noggit's own | `::TileIndex` (`src/noggit/TileIndex.hpp`) | world **y** | `z`, from world **x** |
| This fork's database layer | `Noggit::Database::TileIndex` (`src/noggit/database/TileCoordinates.hpp`) | world **x** | `y`, from world **y** |

Noggit works in an internal frame where `pos.x = ZEROPOINT - world_y` and
`pos.z = ZEROPOINT - world_x`, which is also ADT-filename order, `<map>_<x>_<z>.adt`. The database
layer works in server order, straight off the `position_x` / `position_y` columns. So `x` names
both fields while meaning opposite axes.

Worked example: the Elwynn Forest tile Noggit loads as `Azeroth_31_49.adt` is
`::TileIndex{x=31, z=49}` and `Database::TileIndex{x=49, y=31}`.

The reason this is worth a section rather than a comment is the failure mode. Assigning one to the
other field by field:

- compiles;
- keeps both indices inside 0..63, so it passes `isValidTile`;
- returns a plausible, non-empty result set;
- from a tile roughly **9.6 km away on both axes**, with no error anywhere.

Only tiles on the diagonal `x == y` are immune, which includes a tempting number of test cases.

**Use the named conversions, never field assignment.** `toAdtFileIndex(TileIndex)` and
`fromAdtFileIndex(AdtFileIndex)` in `TileCoordinates.hpp` exist so the transposition is impossible
to perform by accident, and they are covered in `tests/TileCoordinatesTests.cpp` — the transpose
itself, the exact-inverse round trip in both directions, and agreement with `tileForPosition` on
known real spawn coordinates. Every crossing in the tree goes through them: `SpawnPlacement`,
`SpawnSceneCache`, `MapView`, `DatabaseSpawnPanel` and `SpawnTilePicker`.

Three related facts in the same file, each of which has its own quiet failure mode:

- **Increasing world x yields a *decreasing* tile index.** `blockX = floor(32 - x / 533.33333)`.
  Getting the sign wrong still produces plausible-looking indices, which is why the formula is
  tested against known real spawns rather than only against synthetic values.
- **Tile bounds are the half-open interval `(min, max]`** — lower edge exclusive, upper edge
  inclusive. That is the opposite of the usual convention, and it follows from the inverted axis:
  inverting `floor(32 - x / TILE_SIZE) == i` gives `(31-i)*TILE_SIZE < x <= (32-i)*TILE_SIZE`, so
  `min_x` belongs to the neighbour with the *higher* index. A SQL predicate wants
  `> min AND <= max`. Writing `>= min AND < max` silently attributes every tile-edge spawn to the
  wrong tile.
- **All computation is done in `double` even though the columns are `FLOAT`.** A `FLOAT` holds
  about seven significant digits, so at the ~17000 yard map edge the representable step is roughly
  0.002 yards. Narrow once, on write, never mid-calculation. For the same reason the emitter uses
  six decimals for coordinates (more digits than the column can hold at world magnitudes) but
  **nine significant digits for rotation components** — `FLT_MAX_DIGITS10` is 9, and six decimals
  on a value near 1.0 is eight float steps of slack, enough to rewrite a row nobody touched.

## The upstream MySQL seam (`src/mysql/`)

Worth understanding before you touch it, because its name oversells it.

`src/mysql/mysql.{h,cpp}` is gated behind CMake `USE_SQL` → `-DUSE_MYSQL_UID_STORAGE`. It is **not
a database layer**. It is five free functions persisting one `UIDs(_map_id, UID)` table so that
map UID counters survive between sessions. The actual database layer is `src/noggit/database/`.

Upstream's version carried four defects. Their current state:

| Defect | Status |
|---|---|
| `connect()` ran `CREATE DATABASE IF NOT EXISTS` **on every call**, against whatever host `project/mysql/server` named | **Fixed.** `CREATE DATABASE` is gone entirely. |
| A fresh connection opened per call | **Still true, deliberately.** |
| Credentials in `QSettings` (Windows registry) in plaintext under `project/mysql/*` | **Still true.** Known limitation. |
| `#include <driver.h>`, plus `QMessageBox` / `std::stringstream` / `std::unique_ptr` used without including them | **Fixed.** |

Each is worth a sentence:

**The DDL.** Pointing upstream Noggit at a production world database with the feature enabled
would create a schema and a table there before doing anything else — the project's central safety
claim falsified by the application itself. `CREATE DATABASE` was removed outright: the schema must
already exist, and if it does not the connection fails and is reported rather than the schema being
conjured into existence. The `CREATE TABLE IF NOT EXISTS UIDs` survives, because UID storage needs
it, but it is now reachable only after the configured schema has been established as the sanctioned
one. Crucially the policy is **not reimplemented** in `src/mysql/` — the schema comes from
`Database::DatabaseSettings` and the enforcement from `WorldDatabaseConnection`'s `DEV_WRITE`
guard, the same path the fork's own layer is bound by. A second, parallel policy would be a second
thing to get wrong. There is an additional explicit check before the connection call, redundant
against that guard on purpose: it costs nothing, it fires before a socket is opened, and it
produces a message about UID storage rather than about changesets.

*Behaviour change for upstream users:* if your `UIDs` table lives in a differently-named schema,
UID persistence stops until `project/mysql/dev_schema` points at it. The refusal reason goes to
`log.txt` and is surfaced by **Noggit ▸ Settings ▸ MySQL ▸ Test Connection**, which is itself
`READ_ONLY` now — a test button must not be the thing that writes to a server.

**Connect-per-call.** Left alone. It is upstream's design and it is tolerable only because these
five functions run a handful of times per session, at map load and map save. It is also exactly
why the fork's spawn queries use `WorldDatabaseConnection` directly instead of extending this
seam: per-tile spawn queries through a connect-per-call path would be unusable. The old path was
not extended, because leaving it alive leaves it reachable.

**Credentials.** Unchanged and unfixed. Treat the MySQL account Noggit is configured with as
compromised-by-storage — which is another argument for it being `noggit_ro`.

**The include path.** `mysql.cpp` shipped with `#include <driver.h>` while CMake's
`FIND_PATH(MYSQLCPPCONN_INCLUDE NAMES cppconn/driver.h)` resolves to the *parent* of `cppconn/`;
`driver.h` lives in `cppconn/`, not beside it. That path could never build against a stock
Connector/C++ layout. Rather than re-point the include, the connector headers were removed from
that file **entirely** — every statement now goes through `WorldDatabaseConnection`, whose header
forward-declares `sql::Connection`. Verified by a successful link against Connector/C++ 26.7.0.
`mysql.h` also used `#pragma once` against the repo's own guideline; it now has an include guard.

## Conventions

Full list in [`CONTRIBUTING.md`](CONTRIBUTING.md#coding-style). Two points that are context rather
than rule:

**The README describes a tree that does not exist.** Upstream's `README.md` specifies
`/src/Noggit` with PascalCase directories; the actual tree is `src/noggit`, lowercase. **Match the
surrounding code**, not the aspirational rule, when adding files to an existing directory. Renaming
the tree to match the README would produce a diff against upstream that no one could review.

**The naming scheme carries information, so it is worth following exactly**: include guards never
`#pragma once`; `.hpp` headers and `.cpp` implementations; PascalCase files and types; camelCase
methods; `_snake_case` private members; `snake_case` locals; `SCREAMING_CASE` constants and macros;
PascalCase namespaces mirroring the directory layout; `<>` includes ordered local, library, STL;
forward declarations in headers. `src/external/` is third-party and exempt from all of it.

## Build traps

All of these are documented in full in [`setup.md`](setup.md), with the exact error text each one
produces. Listed here only so you know they exist before you lose an afternoon:

- [**Submodules first.**](setup.md#what-a-fresh-clone-does-not-contain) `cmake/` is itself a
  submodule, so *nothing configures at all* without `git submodule update --init --recursive`.
- [**Quote `"-DCMAKE_POLICY_VERSION_MINIMUM=3.5"` on CMake
  4.x.**](setup.md#cmake-4x-policy-flag) Unquoted, some shells strip the `.5`, the cache holds
  `3`, and every subproject fails with a message that blames the subproject rather than the flag.
- [**Qt must be ≥ 5.10.**](setup.md#qt-version-floor) `NodeEditor` requires it; below that
  `Qt5::OpenGL` is never defined, and the failure appears at *generate*, not configure — so
  `Configuring done` has told you nothing about Qt.
- [**`--target INSTALL` is broken**](setup.md#the-install-target-is-broken) on a fresh clone, and
  has nothing to do with this fork. Run out of `build/bin/<config>/`.
- [**Two widely repeated runtime instructions are
  wrong**](setup.md#what-the-binary-actually-needs-at-runtime) for this configuration, checked
  with `dumpbin /DEPENDENTS`: there is **no `lua51.dll`** (sol2 links Lua 5.4 statically) and
  **`Qt5OpenGL.dll` is never imported** (NodeEditor needs the module at configure time only; Qt
  5.15 puts `QOpenGLWidget` in `Qt5Widgets`). `windeployqt` runs as a POST_BUILD step and gets
  every Qt entry right on its own; the connector DLL is the one thing it cannot know about.

## Licensing constraints that shape the code

Detail in [`ATTRIBUTION.md`](../ATTRIBUTION.md); the parts that affect what you can write:

- **No TrinityCore source code, ever.** TrinityCore is GPL-2.0, which is *not* compatible with
  GPL-3.0. Schema *facts* — column names, ordinal positions, types, defaults, which values the
  server derives — are fine, and are exactly what `schema-335.md` records. Implementation code is
  not. This is why the schema doc reads as measurements rather than as citations.
- **No client data.** No MPQ, DBC, model, texture or extracted asset enters the repository, in any
  form, including test fixtures. Display IDs are integers rather than content, which is why real
  ones appear in `tools/dev-db/02_seed_synthetic.sql`; everything around them there is invented.
- **Every new source file carries the GPL header**, and every modification gets a row in the
  `ATTRIBUTION.md` change table. That table is a GPL-3.0 §5(a) obligation to upstream, not
  bookkeeping.
- **No infrastructure detail in tracked files.** Database names, hostnames, absolute paths and
  security posture live in the gitignored configs, each with a committed `.example` twin.
