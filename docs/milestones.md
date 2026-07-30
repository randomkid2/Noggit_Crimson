# Milestones

Each milestone is gated: run `/milestone Mn`, produce a plan, get it approved, then implement.
No implementation code before an approved plan.

Every definition of done requires **pasted evidence**. "Builds clean" without compiler output
is not a completed milestone.

## Status

| | Milestone | State |
|---|---|---|
| — | Groundwork: guardrails, measured schema, dev-DB tooling | **done** (2026-07-30) |
| — | Dev database live and seeded, 29/29 assertions | **done** (2026-07-30) |
| M0a | Capability model + test harness | **done** — 9/9 tests, no Qt required |
| M0b | Live introspector + connection layer | next, unblocked |
| M0c | QSettings adapter, UID migration | blocked on Qt |
| M1 | Spawn read + tile overlay | not started |
| M2 | Spawn edit + changeset emission | not started |
| M3 | Waypoint editor | not started |
| M4 | Chunk mover | not started |
| M5 | Doctor, recovery, coordinate round-trip | not started |

## Groundwork — done

- `CLAUDE.md`, `docs/schema-335.md`, `docs/environment.md`, this file.
- `.claude/hooks/guard-db.ps1` — allowlist DB guard. 22/22 self-test cases pass, including
  compound-command bypass and the `mysql` binary/schema name collision.
- `.claude/settings.json`, five slash commands, three subagents.
- `tools/dev-db/` — root bootstrap, structure-copy seeder, synthetic fixtures, schema-check.
- `/schema-check source`: **29/29 assertions hold** against `world` (TDB 335.25101).
- `noggit_dev_world` bootstrapped, structure copied (26 tables), fixtures seeded.
  `/schema-check dev`: **29/29 hold**. `noggit_ro` write attempt refused with `ERROR 1142`.
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
  code, independent of DB grants and of the hook.
- Catch2 or GoogleTest via FetchContent, and a `tests/` CMake target. There is no test
  framework in the tree today.

**Done when:**

- Unit tests cover the capability model against both a real introspection result and a
  fabricated master-branch-shaped one, proving the branch is taken correctly.
- A test asserts a write to a non-dev schema is refused by the layer itself, with the DB grants
  irrelevant.
- `/schema-check both` passes; output pasted.
- `/build` clean; output pasted.

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
- `db-verifier` reports PASS on all four of its steps.

## M3 — Waypoint editor

**Scope is smaller than the source brief claims.** Of the three waypoint systems it describes,
only one table exists on this DB:

| Table | Status |
|---|---|
| `waypoint_data` | present — the target |
| `waypoints` | absent |
| `script_waypoint` | absent |

So there is no AI-dependent table branching to implement. Still resolve the target through the
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
- `/schema-check` still passes.

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
  `/schema-check` assertions in-process, confirm the client data path, and report plainly.
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

## Explicitly out of scope

- Client-side MPQ patch building. AtlasForge's `patch-Z.MPQ` model has no analog here; the
  server-side equivalent is the emitted `.sql` changeset.
- Custom WMO embedded-doodad (`MODD`/`MODN`) editing — a client-asset concern, and Noggit
  already places WMOs in ADTs.
- Anything touching `TaxiPath.dbc` / `TaxiPathNode.dbc` — DBC-driven, not world-DB data.
- AzerothCore support. Keep the adapter seam; implement TrinityCore 3.3.5 only. AC keeps
  `spawndist`, so the capability layer is what makes adding it cheap later.
- Any write path to a live schema, at any milestone, for any reason.
