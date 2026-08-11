# Milestones — historical planning record

> [!important] This is a historical document, not a status page.
>
> It records the milestone briefs this fork was planned against and the evidence pasted as each
> piece landed, in the order it happened. **Individual sections below were written at a point in
> time and were accurate then; several have since been overtaken.** Where a section's figures or
> scope disagree with the summary immediately below, the summary is the current one.
>
> For what the project does and what is actually verified today, read
> [`../README.md`](../README.md) — specifically its Status table. For how the code is shaped and
> why, read [`design-notes.md`](design-notes.md).
>
> The planning process it describes was real: each milestone was gated on a written plan, and
> every definition of done required **pasted evidence** rather than an assertion that something
> built. That standard is why the sections below read the way they do.

## Current status — supersedes the per-milestone notes below

Measured on this tree, and the figures here are the ones to trust:

```
test cases:    422 |    417 passed | 5 skipped
assertions: 276964 | 276964 passed
```

`ctest -LE needs-database` reports `100% tests passed, 0 tests failed out of 33`. The five skips
are the live-database cases with no credentials in the environment; that is the correct result,
not a regression.

| | Milestone | State |
|---|---|---|
| — | Groundwork: guardrails, measured schema, dev-DB tooling | **done** (2026-07-30) |
| — | Dev database live and seeded, 29/29 assertions | **done** (2026-07-30) |
| M0a | Capability model + test harness | **done** |
| M0b | Live introspector + connection layer | **done** |
| M0c | QSettings adapter | **done** — links into `noggit.exe` |
| M0c | UID migration off the old `connect()` | **remaining** |
| M1 | Tile math + spawn query (logic) | **done** |
| M1 | Spawn overlay rendering | **done and confirmed in the editor** — still awaits the screenshot and the `noggit_ro` read-only proof, so the definition of done is not formally met |
| M2 | Changeset emission + staging UI | **done** — `DatabaseSpawnPanel`, `SpawnTilePicker`. Never applied twice against a real server. |
| M3 | Waypoint path model + emission | **done** — **no visual editor exists**; there is no waypoint UI in the tree |
| M4 | Chunk transform plan (translate + rotate) | **done** — **terrain half not built** |
| M5 | Doctor, archive/recovery, coordinate round-trip | **done** — `DoctorReport` renders, but **no dialog surfaces it** |

Beyond the original M0–M5 plan, two things shipped that these briefs do not describe:

- **Client ▸ Patch Client** — MPQ patch building with a referenced-asset dependency walk. The
  briefs below list this as explicitly out of scope; that is no longer true. See
  [Scope changes since the briefs were written](#scope-changes-since-the-briefs-were-written).
- **The map-making tools** — ground effect set editor, automatic texturing, thermal erosion, AO
  baking, alpha-map integrity, missing-asset and UID-collision reports. These came out of
  [`feature-recon.md`](feature-recon.md), not out of the database plan.

Still genuinely outstanding: the waypoint visual editor, the terrain half of the chunk mover, a
Doctor dialog over `DoctorReport::render()`, the UID migration off the old `connect()`, and the
`noggit_ro` read-only demonstration.

### M1 rendering — next steps, in order

A read-only survey of the render, model, DBC and spawn paths — every finding cited to file:line —
established the architecture and turned up two blockers that must be cleared before anything can
be drawn.

**Steps 1–4 are now done.** Evidence, 2026-07-30:

- `cmake --build build --config RelWithDebInfo --target noggit` → exit 0,
  `noggit.vcxproj -> build\bin\RelWithDebInfo\noggit.exe` (23,416,832 bytes). `DisplayResolver.obj`
  names all three DBC globals (`DisplayResolver.cpp:312,333`) and the link resolves, which is the
  actual proof step 1 is cleared — before this, the first translation unit to touch
  `gGameObjectDisplayInfoDB` was an unresolved external. Zero warnings attributable to any new or
  changed database file.
- `noggit_schema_tests` builds clean under `/W4 /permissive-`; **217 cases, 2702 assertions, all
  passing**, 5 skipped (the live-DB cases, no password in the environment). `ctest`: 26/26 passed,
  so all five new tags (`modelpath`, `display`, `creature`, `gameobject`, `parse`) match real cases
  rather than silently matching nothing.
- New: `ModelPathFixup` (anchored, case-insensitive, Qt-free `.mdx`/`.mdl` → `.m2`; the existing
  six copies in-tree are enumerated with file:line in its header, three of them defective) and
  `DisplayResolver` (caches both DBC chains, throws nothing, bounds-checks the string-table
  offset that `DBCFile::Record::getString` only guards with an `assert` RelWithDebInfo compiles
  out).
- `tests/CMakeLists.txt` is not globbed, unlike the root build — new sources must be added to it
  by hand or they are silently untested.

**Step 5, the draw hook, is code-complete and built.** Evidence, 2026-07-30:

- `--target noggit` → exit 0, `noggit.exe` 23,461,376 bytes. **No new compiler warnings**: the only
  three reported against `WorldRender.cpp` are the pre-existing C4018/C4267 pair, shifted from lines
  326/870/940 to 327/917/987 by the inserted block.
- **220 cases, 3098 assertions, all passing**; `ctest` 27/27.
- Added `SpawnSceneCache` (`src/noggit/database/`), which turns `TileSpawns` into `ModelInstance`
  objects held per ADT tile, plus skip counts by reason so an empty overlay is explainable rather
  than indistinguishable from an empty tile.
- The overlay appends into the same `models_to_draw` the MDDF path fills, so it costs one instanced
  draw call per model and no shader of its own. `MapTile::object_instances` is never touched, which
  is what makes "DB spawns never enter MDDF/MODF" structural rather than remembered.
- View menu: a `Database spawns` toggle plus a separate `Load database spawns` action. The query is
  explicit and synchronous in the Qt handler; the render path issues no query and walks no DBC.

Two defects found and fixed while doing it, both pre-existing:

1. **`ValidationIssue::Severity::ERROR` could not compile in any translation unit that reaches a
   Windows header.** `<wingdi.h>` does `#define ERROR 0`, making the declaration
   `enum class Severity { WARNING, 0 }`. It went unnoticed because `SpawnTypes.hpp` was included
   only by the database layer and its Qt-free test target; the first UI file to include it broke the
   build. Renamed to `BLOCKING` across all 20 uses, with a comment saying why so it is not "restored".
2. **`toAdtFileIndex` / `fromAdtFileIndex` had no test at all** — the one conversion in this layer
   that fails silently rather than loudly, and the subject of warnings in three separate headers.
   Now pinned by three cases (`[adt]`, 396 assertions) over deliberately asymmetric fixtures, since
   any `x == y` fixture passes under a helper that does nothing.

**The overlay is confirmed working in the viewport.** Run of 2026-07-30 against `noggit_dev_world`:

```
MapView.cpp: Database spawns loaded from schema "noggit_dev_world"
             over 30 loaded tile(s): 5 spawn(s) across 30 tile(s)
```

Five, not four, and that is correct: `03_example_changeset.sql` adds creature guid `9000004` on
top of the four seed fixtures. Verified directly against the schema. Zero skips — every spawn
resolved through template join → display id → DBC chain → model path. The same run confirms all
three new DBCs open against a stock client (`GameObjectDisplayInfo`, `CreatureDisplayInfo`,
`CreatureModelData`), which is blockers 1 and 3 proven on real data rather than merely linked.

Three defects found by that run, all fixed:

1. **Hard crash on the second load.** The cache owns `scoped_model_reference`s; releasing the last
   one destroys a `Model` → `ModelRender` → OpenGL vertex arrays, and `OpenGL::Scoped`'s destructor
   throws when no context is current. Thrown from a destructor that is `terminate`, not a catchable
   error, and it cannot reproduce on the first load because there is nothing to release. Fixed by
   binding the context in `loadDatabaseSpawns` and `~MapView`.
2. **Everything beyond 300 yards was culled.** `ModelInstance::isInRenderDist` applies a size
   ladder keyed on `size_cat`, which is only populated from the MDDF/WDT path and is therefore 0
   for every database spawn. Half an ADT tile. Now culled by view distance directly.
3. **The multi-tile load was an unguarded footgun.** Split into a this-tile action and an
   all-loaded-tiles action with a pre-flight `COUNT` and a confirmation above 2000 spawns.

**Creature models render solid black.** This is upstream behaviour, not a defect in this work:
`Model.cpp:353` substitutes `tileset/generic/black.blp` for every texture with `type != 0`, and
`NO_REPLACIBLE_TEXTURES_HACK` is not defined anywhere in the tree, so that branch is unconditional.
Creature skins are exactly that case — they come from `CreatureDisplayInfo.TextureVariation` at
runtime rather than from the M2. Upstream Noggit draws only doodads and WMOs, which carry embedded
type-0 textures, so this has never mattered before; the spawn overlay is the first thing in Noggit
to render a creature. Placement, scale and orientation are unaffected. See below for what fixing it
requires.

**Remaining for the M1 definition of done:** the read-only proof as `noggit_ro`. Needs that
account's password, so it is the user's to run.

If the fixtures appear in the right places but uniformly mis-facing,
`SpawnPlacement::YAW_OFFSET_DEGREES` is the cause — its header flags M2 local-forward `+X` as the
one link in the derivation that is unverified, and it will be off by exactly 90, 180 or 270.

1. **`gGameObjectDisplayInfoDB` is declared but never defined.** `src/noggit/DBC.h:403` declares
   it; `DBC.cpp` defines 19 globals and this is not one of them, and it is absent from `OpenDBs`.
   It links today only because nothing references it — **the first line of code that touches it is
   an unresolved external**. Two one-line additions to `DBC.cpp` fix it. The field indices the
   existing wrapper declares were checked against `dist/definitions/GameObjectDisplayInfo.dbd`
   and are correct.
2. **`TileSpawns` carries no template-derived display id.** `creature_template.modelid1..4` and
   `gameobject_template.displayId` are *detected* by `SchemaModel` but never selected, and
   gameobjects have no per-spawn display column at all. So a gameobject can never be resolved to
   a model, and neither can a creature with `modelid = 0`. The query layer needs extending first;
   this blocks rendering rather than polishing it.
3. **`CreatureDisplayInfo` and `CreatureModelData` have no C++ wrapper.** Field layouts verified
   from `dist/definitions` for build 3.3.5.12340: `CreatureDisplayInfo` ID=0, **ModelID=1**,
   CreatureModelScale=4; `CreatureModelData` ID=0, Flags=1, **ModelName=2** — note *2*, not 1,
   because `Flags` precedes it in WotLK. Guessing there reinterprets an integer as a string offset.
4. **`DBCFile::getByID` is a linear scan** (`DBCFile.cpp:144-153`) and throws on miss, and
   `getString` is raw pointer arithmetic guarded only by an `assert` — compiled out in
   RelWithDebInfo. A resolver must cache and must bounds-check rather than call per frame.
5. **Draw hook.** `WorldRender::draw` gathers M2 instances into
   `robin_map<Model*, vector<mat4>> models_to_draw` and issues one instanced call per model
   (`rendering/WorldRender.cpp:801-892`). The per-tile index it walks is
   `MapTile::object_instances`. `activeTool()->preRender()/postRender()`
   (`MapView.cpp:2812-2827`) are the tool-level hooks. DB spawns must never enter MDDF/MODF.

### The `USE_SQL=OFF` build — fixed

Previously broken, and it mattered: this is a public fork, and most people cloning it will not have
MySQL Connector/C++ installed, so the default configuration was the one that did not build.

`collect_files(noggit_root_sources src/noggit TRUE "*.cpp")` globs recursively, so everything under
`src/noggit/database` compiled unconditionally, while the connector's include directory was added
to the target only when the three `FIND_*` calls succeeded. The result:

```
WorldDatabaseConnection.cpp(5,10): error C1083:
  Cannot open include file: 'cppconn/driver.h': No such file or directory
```

Established by configuring and building `-DUSE_SQL=OFF`, not by reading the CMake. Fixed by
excluding the four connector-dependent sources — `WorldDatabaseConnection`, `SchemaIntrospector`,
`SpawnQuery`, `DoctorConnectionChecks` — when the connector is absent. The other three compile
cleanly on their own, since `WorldDatabaseConnection.hpp` forward-declares `sql::Connection` so
consumers need no connector headers, but they *call* into it, so leaving them in without it trades
a compile error for unresolved externals. All four go together.

Everything else in the layer stays compiled in every configuration: the schema model, tile maths,
changeset emission, the spawn scene cache, and the SQL builders in `SpawnQueryDetail.cpp`. Only the
code that actually talks to a server is gated.

### Creature skin textures — implemented, not yet seen

Creature spawns rendered as black silhouettes because a creature M2 carries no skin of its own: the
mesh is shared and the image is supplied per display id. Noggit's loader replaces every such
texture with `tileset/generic/black.blp` (`Model.cpp:353`) and — the part that actually blocked a
fix — **flattens the texture type to -1**, so nothing downstream could tell which slots had been
replaceable.

The approach taken avoids the two routes that looked obvious and are not:

- **Not** by enabling the `NO_REPLACIBLE_TEXTURES_HACK` branch. That code is stale and would not
  compile: it does `_replaceTextures.emplace(type, _textureFilenames[i])`, but
  `scoped_blp_texture_reference` has no single-argument constructor. It also runs into a real bug
  in `ModelRender.cpp:1023`, where the guard `_specialTextures[tex] >= _replaceTextures.size()`
  compares a **map key against a container size** — `_replaceTextures` is keyed by texture type, so
  a type-11 skin with one entry gives `11 >= 1` and binds nothing. Both are left untouched.
- **Not** by giving each display id its own `Model`. `AsyncObjectMultimap` keys models on
  `(context, FileKey)`, so that would mean changing storage shared by every model, WMO and texture
  in the application.

Instead: `Model` now records `_replaceable_texture_types` — the original M2 type per slot, which
nothing else reads, so recording it changes no behaviour. `DisplayResolver` resolves
`CreatureDisplayInfo.TextureVariation[0..2]` into `.blp` paths beside the model. And database
spawns draw in their own pass, grouped by **(model, display id)**, with that display's textures
bound for the duration of the call and restored immediately after — because `Model::_textures`
belongs to the shared model, and a skin left applied would repaint every other user of it.

Grouping by display id rather than by model is the point: one wolf model serves wolves of several
colours, so grouping by model alone would draw them all with whichever skin was bound last.

**Not visually confirmed.** It builds clean in both configurations and breaks no test, but
verifying it needs someone to open a populated tile and look. If creatures are still black, the
first thing to check is whether `TextureVariation` is populated for those display ids; if they are
wrongly skinned, the type↔slot matching in the spawn pass is where to look.

### Known follow-ups from the M4–M5 review

Real, judged non-urgent, recorded so they are not rediscovered as surprises.

1. **`ChangesetArchive::collect` stores a narrow `std::string` filename.** A name the active code
   page cannot represent (a UTF-8 name copied from another machine) is converted with replacement
   characters, so `list()` reports a name that no longer opens, `load()` fails on it, and
   `prune()` throws on `remove()` — permanently blocking pruning while that file sits in the root.
   Fix is to keep the native `std::filesystem::path` in the stored entry.
2. **`ChangesetArchive::load` sizes the file then reads that many bytes**, so the short-read guard
   only detects shrinkage. A file that *grew* between the stat and the read is returned silently
   truncated — the exact failure the guard's comment claims to make impossible.
3. **`ChangesetArchive::collect` fails the whole archive on one unsizable entry.** An unrelated
   backup vanishing mid-scan (a concurrent `prune`) makes `store()` refuse to archive a changeset
   that had nothing wrong with it. A not-found error should skip the entry, as `prune` already does.
4. **`isDefaultRotation`, `TWO_PI`, `ORIENTATION_AGREEMENT_TOLERANCE` and `yawSeparation` are
   duplicated across three translation units** (`ChangesetBuilder`, `ChunkTransform`,
   `GmCommands`), with `TWO_PI` written as two different literals. Each copy is tested only
   against itself, so changing the identity test in one place silently desynchronises the emitter,
   the transform planner and the `.go` command. They belong in `TileCoordinates`, which owns
   `Quaternion`. `addIssue`/`addError`/`addWarning` are likewise duplicated between `SpawnTypes`
   and `ChunkTransform`.
5. **Case folding in `DoctorReport` uses locale-sensitive `std::tolower`.** Under a Turkish
   `LC_CTYPE` (Qt calls `setlocale(LC_ALL, "")` at startup) the configured writable schema
   compares unequal to itself and the loud live-database alarm fires against the *correct* schema.
   Fold ASCII by hand, as `ChangesetArchive::sanitiseLabel` already does.
6. **Two assertions in `GmCommandsTests` are tautologies** over a hardcoded literal rather than
   over emitted output, so on a machine with no comma-decimal locale the test reports having
   verified locale independence while verifying nothing.

### Known follow-ups from the M1–M4 review

Found by adversarial review, judged real but deliberately deferred. Neither is a correctness
risk today; both are recorded so they are not rediscovered as surprises.

1. **`rotation0..3` are formatted at six decimals**, the same precision as world coordinates.
   That is 11 significant digits at world magnitude but only six where `|v| <= 1`, against a
   `float` step of 5.96e-8. Re-emitting an *untouched* gameobject therefore rewrites the stored
   bytes of its rotation columns by ~4e-7 — about eight float steps, roughly 1e-6 radians of
   yaw. Harmless in the world, but it shows up as a spurious diff in changeset review, which is
   precisely what the `-0.0` handling elsewhere exists to avoid. Fix is to format the rotation
   components with a significant-digit precision rather than reusing the coordinate helper.
2. **`SpawnQuery.cpp` gives external linkage to six functions its header does not declare**
   (the pure SQL builders and row decoders), and `SpawnQueryTests.cpp` keeps a hand-written
   parallel copy of five of those signatures to reach them. They only stay in step by accident
   of `ResultRow` being a typedef. It also means the pure builders sit inside a translation unit
   that links the database connector, so they are gated behind Connector/C++ despite needing no
   database. Fix is to move them into their own translation unit with a small internal header.
   **Done** — they live in `SpawnQueryDetail.{hpp,cpp}`, which links no database client, so the
   spawn-query and SQL-builder cases now run on a machine with no MySQL installed.

## Groundwork — done

- `docs/schema-335.md`, `docs/environment.md`, this file.
- **The write guard lives in the shipped code, not in a convention.**
  `Noggit::Database::WorldDatabaseConnection` refuses to construct a `DEV_WRITE` connection
  unless `config.schema` equals the configured writable schema, and re-checks that same equality
  inside `execute()` before every individual statement rather than trusting the construction-time
  result — `setSchema` could have been called since. `READ_ONLY` connections are rejected by
  `execute()` outright and additionally ask the server for `SET SESSION TRANSACTION READ ONLY`
  where it is honoured. `executeScript` splits a changeset and routes every statement back
  through `execute()`, so applying a whole file cannot become a way around the guard.
- **The one inherited write path is bound by the same policy.** `src/mysql/mysql.cpp` — upstream's
  UID storage — no longer issues the `CREATE DATABASE IF NOT EXISTS` it ran on *every*
  `connect()`, and opens no connection at all unless `project/mysql/db` is exactly
  `project/mysql/dev_schema`. That check is redundant against `WorldDatabaseConnection`'s guard on
  purpose: it fires before a socket is opened and reports in terms of UID storage. The policy is
  not reimplemented there — a second parallel policy would be a second thing to get wrong.
- `tools/dev-db/` — root bootstrap, structure-copy seeder, synthetic fixtures, schema-check.
  `seed-dev-db.ps1` refuses to run when its configured dev schema is a protected name or equals
  the source schema (`seed-dev-db.ps1:40,46`).
- `schema-check.ps1 -Target source`: **29/29 assertions hold** against `world` (TDB 335.25101).
- `noggit_dev_world` bootstrapped, structure copied (26 tables), fixtures seeded.
  `schema-check.ps1 -Target dev`: **29/29 hold**. `noggit_ro` write attempt refused with
  `ERROR 1142`.
- Submodules initialised.

## Prerequisites before M0

These block compilation and are not code problems. See `docs/environment.md`.

1. ~~**Submodules**~~ — done. `cmake/` is itself a submodule, so nothing configures without it.
2. ~~**Root DB bootstrap**~~ — done. `noggit_dev_world` is live and seeded.
3. **CMake policy** — `CMakeLists.txt:9` is bumped from 3.3 to 3.5, but CMake 4.1.2 still needs
   `"-DCMAKE_POLICY_VERSION_MINIMUM=3.5"` on every configure because `nlohmann/json` (fetched to
   `build/_deps/json-src`) declares below 3.5 and gets re-fetched, so patching it does not
   stick. **Quote the argument** — unquoted, the `.5` is stripped and the cache holds `3`,
   failing in every subproject with a misleading message.
4. **Qt — the one real blocker.** Tested and confirmed insufficient. With the policy flag,
   configure completes in ~37s and fails only at generate on two Qt targets:
   `src/external/NodeEditor/CMakeLists.txt:37` requires `Qt5 5.10`, so `Qt5::OpenGL` is never
   defined, and `framelesshelper` wants `Qt::GuiPrivate`. Nothing else is outstanding for a
   base build. Install **Qt 5.15.2 msvc2019_64**.
5. ~~**MySQL Connector/C++**~~ — done. 26.7.0 staged in the sibling `Noggit3libs` layout;
   `-DUSE_SQL=ON` resolves all three `FIND_*` calls with no `-D` paths. Current Connector/C++
   still ships the legacy `cppconn` API. See `docs/setup.md`.

   Confirmed while doing it: `MYSQLCPPCONN_INCLUDE` resolves to the *parent* of `cppconn/`, so
   `src/mysql/mysql.cpp`'s `#include <driver.h>` will not compile. The commented-out
   `<cppconn/driver.h>` form is correct. Left unfixed until it can actually be built.

## M0 — Schema introspection layer

The highest-risk milestone, because column drift baked into C++ corrupts every changeset
downstream and is the hardest thing to retrofit. Nothing else should start before it works.

**Build:**

- A connection layer that holds one connection rather than opening one per call. The existing
  `connect()` in `src/mysql/mysql.cpp` reconnects on every invocation and issues
  `CREATE DATABASE IF NOT EXISTS` each time — unusable for per-tile queries and a live-DB write
  hazard. Do not extend it; replace it, and migrate the UID functions onto the new layer.
- A `DbSchema` type that introspects `information_schema` on connect and answers: does table T
  exist, does column T.C exist, what is its ordinal position and type.
- Version detection probing **both** `version_db_world` and `version`.
- A capability model, not a version-string switch. Ask "does `creature_addon` have `bytes1` or
  `StandState`", never "is this TDB ≥ N".
- Connection modes: read-only against a live schema, read/write against `noggit_dev_world`
  only. Refuse a write when the target schema is not the configured dev schema — enforced in
  the application's own code, so it holds independently of DB grants and of any external tooling.
- Catch2 or GoogleTest via FetchContent, and a `tests/` CMake target. There is no test
  framework in the tree today.

**Done when:**

- Unit tests cover the capability model against both a real introspection result and a
  fabricated master-branch-shaped one, proving the branch is taken correctly.
- A test asserts a write to a non-dev schema is refused by the layer itself, with the DB grants
  irrelevant.
- `schema-check.ps1 -Target both` passes; output pasted.
- The build is clean; compiler output pasted.

## M1 — Spawn read + tile overlay

**Build:**

- Given an open ADT tile, load `creature` and `gameobject` rows for it. World→tile:
  `blockX = floor(32 - x/533.33333)`, `blockY = floor(32 - y/533.33333)` — verified against
  real spawns in tile 49_31, and it works directly on TrinityCore coordinates.
- Query by computed bounds, not by `zoneId`/`areaId`: those are core-derived and frequently 0.
- Resolve `displayId` → model path: creatures via `CreatureDisplayInfo.dbc` →
  `CreatureModelData.dbc`; gameobjects via `GameObjectDisplayInfo.dbc`. DBCs come from
  a stock 3.3.5a client (path recorded in `docs/environment.md`; use an unmodified client, not
  one carrying custom patches, or the DBC baseline is wrong). Creature models come from
  `creature.modelid` when non-zero, otherwise
  `creature_template.modelid1..4` — `creature_template_model` does not exist here.
- Render at world coordinates. Gameobject rotation is the `rotation0..3` quaternion, not Euler.
- Handle the unrenderable cases without breaking: `displayId = 0`, gameobject types 6, 11, 12,
  13, 15, 18.

**Done when:**

- The three fixture creatures and one fixture gameobject in tile 49_31 render at their seeded
  coordinates, and a screenshot shows it.
- A tile with no spawns loads without error.
- Tile-boundary spawns appear in exactly one tile — a test covers the boundary arithmetic.
- Read path is provably read-only: run it as `noggit_ro` and show it working.

## M2 — Spawn edit + changeset emission

**Build:**

- AtlasForge-style staging: ghost placement, Edit and Demolish modes, a reviewable Pending
  Changes list. Nothing is committed implicitly.
- Commit emits a TDB-style `.sql` changeset — `DELETE` then `INSERT`, `@GUID` variables —
  covering `creature`, `creature_addon`, `gameobject`. Use the core's own `WORLD_INS_CREATURE`
  column list as the template.
- Emit `zoneId`/`areaId` as 0. Never author `wpguid`.
- Respect the core's validation: `MovementType` 0 requires `wander_distance = 0`; type 1
  requires `> 0`.
- Optional rehearsal against `noggit_dev_world`. No path to a live schema exists.
- Every emitted file is backed up with a timestamp.

**Done when:**

- A generated changeset applies cleanly to `noggit_dev_world` with **zero MySQL warnings**;
  output pasted.
- It is idempotent — applying twice produces the same rows and no errors.
- Round-trip: every written value re-reads identically, coordinates included. `float` columns
  will not hold a `double`; a shifted coordinate is a defect.
- The changeset is reviewed statically against `docs/schema-335.md` *before* it is run, because a
  statically wrong changeset does not need a rehearsal. The recurring mistakes: `bytes1`/`bytes2`
  on `creature_addon` or `creature_template_addon` (3.3.5 splits both into `MountCreatureID`,
  `StandState`, `AnimTier`, `VisFlags`, `SheathState`, `PvPFlags`); `spawndist` instead of
  `wander_distance`; references to `waypoints`, `script_waypoint`, `creature_template_model`,
  `pool_creature`, `pool_gameobject` or `version_db_world`, none of which exist on the reference
  database; `smart_scripts` assuming 4 event and 3 target parameters when there are 5 and 4;
  a non-zero `zoneId`/`areaId` in an `INSERT INTO creature`, which the core derives; an authored
  `wpguid`, which the core manages; and a `MovementType`/`wander_distance` pair the core rejects
  (type 0 needs `wander_distance = 0`, type 1 needs `> 0`).
- The rehearsal starts from a known state — reseed, then `schema-check.ps1 -Target dev` — so a
  clean apply is not an artefact of what the schema happened to contain.

## M3 — Waypoint editor

**Scope is smaller than the source brief claims.** Of the three waypoint systems it describes,
only one table exists on this DB:

| Table | Status |
|---|---|
| `waypoint_data` | present — the target |
| `waypoints` | absent |
| `script_waypoint` | absent |

So there is no creature-AI-dependent table branching to implement. Still resolve the target through the
M0 capability layer, because another TDB or an AzerothCore target may reintroduce `waypoints`.

**Build:**

- Visualise and edit a path: nodes, ordering, `delay`, `move_type` (0 WALK, 1 RUN, 2 LAND,
  3 TAKEOFF), `action`, `action_chance`. `point` starts at 1.
- Bind a spawn to a path via `creature_addon.path_id` with `creature.MovementType = 2`.
- `path_id = guid * 10` is a TDB convention only, not enforced by the core. Default to it, make
  it configurable, and detect collisions.
- Read `creature_template.AIName` and surface it — a `SmartAI` creature behaves differently
  even though the path table is the same.

**Done when:**

- The 5-node fixture path on creature `9000003` loads, renders in order, and round-trips
  through an emitted changeset.
- Reordering and deleting nodes keeps `point` contiguous from 1.
- A test asserts the target table is chosen through capability lookup, not a literal.
- `schema-check.ps1` still passes.

## M4 — Chunk mover

**Build:**

- Move a chunk or tile's terrain and placements, updating dependent spawn coordinates.
- Terrain and DB edits stage and commit together — a moved chunk with unmoved spawns is a
  broken world.
- Dependents to follow: `creature`, `gameobject`, `waypoint_data` nodes on paths belonging to
  moved creatures, `game_tele` entries inside the region.

**Done when:**

- Moving the fixture tile relocates all four fixture spawns and all five waypoint nodes by the
  same delta, verified numerically.
- Terrain and DB changes appear in one changeset; a partial failure rolls back both.
- Spawns crossing a tile boundary as a result are reassigned correctly.

## M5 — Doctor, recovery, coordinate round-trip

**Build:**

- **Doctor**: on startup validate DB connectivity, detect version via both tables, run the
  `schema-check.ps1` assertions in-process, confirm the client data path, and report plainly.
  Warn loudly when the connected schema is not the configured dev schema.
- **Debug bundle**: logs plus the last changeset, for support.
- **Recovery**: timestamped backups of every emitted `.sql`, restorable.
- **Coordinate round-trip**: paste a coordinate to move the camera; produce `.go` / `.tele`
  strings from a selected spawn. Manual entry only — no live server connection.

**Done when:**

- Doctor correctly reports: a healthy dev DB; a missing dev DB; a live schema connected by
  mistake; a schema failing an assertion. All four demonstrated.
- A deleted changeset is recoverable from backup.
- A round-tripped coordinate returns to the same tile and position within float precision.

## Scope changes since the briefs were written

**Client-side MPQ patch building was out of scope and is now built.** The brief below reasoned
that AtlasForge's `patch-Z.MPQ` model had no analog here because the server-side equivalent is the
emitted `.sql` changeset. That reasoning holds for *spawn* data and is unchanged — but it does not
cover the client-side assets Noggit was already editing, and those still have to reach the client
somehow. **Client ▸ Patch Client** now writes the project folder, plus the assets its terrain
references, into an MPQ patch. It requires `patches/0001`; see
[`setup.md`](setup.md#applying-patches0001) and the `ATTRIBUTION.md` change table.

The rest of the original out-of-scope list stands.

## Explicitly out of scope

- ~~Client-side MPQ patch building.~~ **Built** — see above.
- Custom WMO embedded-doodad (`MODD`/`MODN`) editing — a client-asset concern, and Noggit
  already places WMOs in ADTs.
- Anything touching `TaxiPath.dbc` / `TaxiPathNode.dbc` — DBC-driven, not world-DB data.
- AzerothCore support. Keep the adapter seam; implement TrinityCore 3.3.5 only. AC keeps
  `spawndist`, so the capability layer is what makes adding it cheap later.
- Any write path to a live schema, at any milestone, for any reason.
