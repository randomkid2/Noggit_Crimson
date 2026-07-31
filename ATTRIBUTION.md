# Attribution, licensing, and release compliance

This is a **fork**, and it is intended for public release. This file records what it derives
from, what it bundles, and what must not end up in it.

Not legal advice. The licence obligations below are the ones the licence texts state on their
face; verify anything load-bearing before you publish.

## This fork

| | |
|---|---|
| Upstream | [Marlamin/noggit-red](https://github.com/Marlamin/noggit-red) |
| Forked at | `6f0776d4` (2025-01-17) |
| Licence | **GPL-3.0** — unchanged, and it cannot be changed |
| Branch | `feature/tc335-db-editing` |

Noggit Red is GPL-3.0 (see `COPYING`). This fork is a derivative work, so it **must** be
distributed under GPL-3.0 with corresponding source. There is no option to relicense, close, or
dual-licence it, and that applies to every new file added here too.

### Statement of modification (GPL-3.0 §5(a))

GPL-3.0 requires modified files to carry prominent notices stating that they were changed and
when. Changes made in this fork:

| Date | Change |
|---|---|
| 2026-07-30 | `CMakeLists.txt` — raised `CMAKE_MINIMUM_REQUIRED` from 3.3 to 3.5 for CMake 4.x compatibility |
| 2026-07-30 | Added `docs/`, `tools/dev-db/`, `.claude/`, `CLAUDE.md`, `ATTRIBUTION.md` — TrinityCore 3.3.5 database-editing groundwork. No existing source files modified. |
| 2026-07-30 | Added `tests/fixtures/` — captured and synthetic `information_schema` metadata for schema-drift tests. No game or server content. |
| 2026-07-30 | Added `docs/plans/M0-schema-layer.md` — design for the database schema introspection layer. |
| 2026-07-30 | Added `src/noggit/database/` (`ColumnInfo.hpp`, `SchemaModel.hpp/.cpp`) — schema capability model. First new application source in this fork; carries the project GPL header. |
| 2026-07-30 | Added `tests/` — standalone Catch2 target for the schema layer. The repository had no test framework before this. |
| 2026-07-30 | Added `src/noggit/database/` connection and introspection layer (`ConnectionConfig.hpp`, `WorldDatabaseConnection.hpp/.cpp`, `SchemaIntrospector.hpp/.cpp`) plus live integration tests. |
| 2026-07-30 | Added the Qt-free logic layer for M1–M4: `TileCoordinates`, `SpawnTypes`, `SpawnQuery`, `ChangesetBuilder`, with 98 test cases. |
| 2026-07-30 | Added `DatabaseSettings.hpp/.cpp` — QSettings adapter. Now compiled and linked into `noggit.exe`. |
| 2026-07-30 | `src/mysql/mysql.cpp` — fixed two upstream defects that prevented `USE_SQL` from ever building: `#include <driver.h>` corrected to `<cppconn/driver.h>` (CMake and the README both resolve the include path to the parent of `cppconn/`), and added the missing `<QtWidgets/QMessageBox>`, `<memory>`, `<sstream>`, `<string>`. Verified by a successful link. |
| 2026-07-30 | Added M4/M5 modules: `ChunkTransform`, `GmCommands`, `ChangesetArchive`, `DoctorReport` + `DoctorConnectionChecks`, and split the pure SQL builders and row decoders into `SpawnQueryDetail`. |
| 2026-07-30 | Added `SqlFormat::rotationComponent` — quaternion components need significant-digit precision, not the coordinate formatter's fixed decimals. |
| 2026-07-30 | `src/noggit/DBC.h`, `src/noggit/DBC.cpp` — defined `gGameObjectDisplayInfoDB`, which upstream declares but never defines, so the first code to touch it was an unresolved external. Added `CreatureDisplayInfoDB` and `CreatureModelDataDB` wrappers (field layouts verified against `dist/definitions` for 3.3.5.12340), opened via a per-file helper so one missing DBC cannot abort the other eighteen as the single upstream `try` block does. |
| 2026-07-30 | `src/noggit/ui/windows/settingsPanel/SettingsPanel.{h,cpp}` — added the dev-schema field and the read-only safety notice, built in code rather than in the shared `.ui` form to avoid a merge conflict in generated XML. Also fixed four upstream QSettings defaults that fell back to `"127.0.0.1"` for the user, password, database and port keys, which put a hostname in three text fields and made the port parse as 0 on a fresh install. |
| 2026-07-30 | Added `DisplayResolver`, `ModelPathFixup`, `SpawnSceneCache` — display-id → model resolution, an anchored Qt-free `.mdx`/`.mdl` → `.m2` rewrite, and the per-tile drawable spawn store. |
| 2026-07-30 | `src/noggit/rendering/WorldRender.{hpp,cpp}`, `src/noggit/MapView.{h,cpp}` — database spawn overlay. Spawn transforms are appended to the existing `models_to_draw` map, so the overlay reuses the instanced M2 path and adds no shader or draw call; `MapTile::object_instances` is deliberately untouched so database spawns cannot reach the ADT save path. Adds a View-menu toggle and an explicit load action. |
| 2026-07-30 | `src/noggit/database/SpawnTypes.hpp` and 20 use sites — renamed `ValidationIssue::Severity::ERROR` to `BLOCKING`. `<wingdi.h>` defines `ERROR` as `0`, so the enum could not compile in any translation unit reaching a Windows header; the defect was latent only because the header had never been included from UI code. |
| 2026-07-30 | `src/noggit/MapView.cpp` — bound an OpenGL context around every mutation and destruction of the spawn cache. Releasing the last reference to a `Model` destroys its GL vertex arrays, and `OpenGL::Scoped`'s destructor throws when no context is current, which terminates the process from a destructor. Reproduced as a hard crash on the second load; the same guard `~MapView` already applies to its tools. |
| 2026-07-30 | Split the spawn load into `Load spawns (this tile)` and `Load spawns (all loaded tiles)...`, the latter running a pre-flight `COUNT` and confirming above 2000 spawns. Added `creatureCountSql` / `gameObjectCountSql` / `countTile`. Against a populated world database the multi-tile load can queue thousands of asynchronous model loads and present as a hang. |
| 2026-07-30 | Database spawns are culled by view distance directly rather than through `ModelInstance::isInRenderDist`, whose size ladder culls anything with `size_cat == 0` beyond 300 yards. `size_cat` is only ever populated from the MDDF/WDT path, so every database spawn would vanish at barely half an ADT tile. |
| 2026-07-31 | `src/noggit/Model.{h,cpp}` — record `_replaceable_texture_types`, the original M2 texture type per slot. The loader flattens every replaceable slot to `-1` and points it at `black.blp`, destroying the only evidence of which slots a creature's display id is meant to fill. Nothing existing reads the new member, so behaviour is unchanged. |
| 2026-07-31 | `src/noggit/rendering/WorldRender.cpp` — database spawns draw in their own pass grouped by (model, display id), binding that display's skins for the call and restoring them immediately. Creature skins come from `CreatureDisplayInfo.TextureVariation`, not from the M2, and `Model::_textures` is shared, so a skin left applied would repaint every other user of that model. Reuses the existing M2 program and `ModelRender::draw`; no shader or pass logic is duplicated. |
| 2026-07-31 | Added `src/noggit/DevBridge.{hpp,cpp}`, `tools/dev-bridge/`, `docs/dev-bridge.md` — a loopback command socket for driving the editor from a script, so rendering can be verified without a human at the window. Gated twice: excluded from the source list unless `-DNOGGIT_DEV_BRIDGE=ON` (default OFF), and listens only when `NOGGIT_BRIDGE_PORT` is set, bound to `QHostAddress::LocalHost`. Uses the already-linked `Qt5::Network`; no new dependency. |
| 2026-07-31 | `src/noggit/MapView.{h,cpp}` — `loadDatabaseSpawns` now returns its outcome as text and takes an `interactive` flag, so the menu raises dialogs while a script gets the same line back. Added `handleBridgeCommand`, which implements the bridge commands here rather than in `DevBridge` because they need the camera, world and spawn cache — making `DevBridge` a friend would have traded a clean seam for a shortcut. |
| 2026-07-31 | `src/noggit/rendering/ModelRender.cpp` — fixed the replaceable-texture lookup. `_replaceTextures` is a `std::map` keyed by texture TYPE, but the guard compared that key against the map's `size()`, so a type-11 monster skin in a map holding one entry gave `11 >= 1`, logged "index out of range" and bound nothing. Now a `find()`. Unreachable today because `Model.cpp` forces every `_specialTextures` entry to -1; fixed so it is not a trap for whoever enables that path. |
| 2026-07-31 | `src/noggit/database/DoctorReport.cpp` — case folding now ASCII by hand instead of `std::tolower`. Qt calls `setlocale(LC_ALL, "")` at startup, so under a Turkish `LC_CTYPE` an 'I' folds to a dotless 'i' and the configured writable schema stops comparing equal to itself — firing the loud live-database alarm against the *correct* schema. |
| 2026-07-31 | `tests/GmCommandsTests.cpp` — the locale-independence case asserted `expected.find('.')` against a string literal it had written itself, so both assertions held regardless of what the emitter produced. On a machine with no comma-decimal locale installed the case then returned early and reported verifying locale independence while verifying nothing. Now asserted against the emitted string. |
| 2026-07-31 | `src/noggit/database/SpawnSceneCache.{hpp,cpp}`, `DatabaseSpawnPanel`, `WorldRender.cpp`, `MapView.{h,cpp}` — spawn rotation, selection highlight and camera focus. Rotating a gameobject rewrites `rotation0..3` as well as `orientation`, because the core reads the latter and the client renders the former; writing one without the other produces a spawn that faces differently in game than in the editor. |
| 2026-07-31 | `src/noggit/scripting/script_global.cpp`, `script_model.cpp`, `src/noggit/tools/ScriptingTool.cpp` — made scripted object placement and removal undoable. `add_m2`, `add_wmo` and `model::remove` passed `action=false`, which skips the `registerObjectAdded`/`Removed` calls, so the scatter brush shipped in `scripts/prop_placer.lua` could paint hundreds of models that Ctrl+Z could not touch. `ScriptingTool::onTick` now opens an action with the `eLMB` modality controller so one brush stroke is one undo step rather than one per frame, and is gated on the left mouse button as every other brush tool is — without that gate the brush fired for as long as a chunk stayed *selected*, which outlives the click. |
| 2026-07-31 | Added `src/noggit/database/GameTeleBuilder.{hpp,cpp}` and `tests/GameTeleBuilderTests.cpp` — exports project bookmarks as `game_tele` rows, reachable from Assist ▸ Export bookmarks as game_tele SQL. Pure, no Qt or database. The camera-yaw-to-server-orientation conversion is a 180° turn rather than a units change, because the two frames disagree about axis direction; derived in the header and pinned at four cardinals so a simplification that drops it fails a test instead of shipping teleports that face backwards. |
| 2026-07-31 | Added `src/noggit/ui/DatabaseSpawnPanel.{hpp,cpp}` — the spawn editing surface: load, list, select, move mode, pending changes, save and discard. `MapView::mousePressEvent` takes the click ahead of the active tool while move mode is on, so placing a creature does not also raise terrain under it. |
| 2026-07-31 | `SpawnSceneCache` now keeps the source `creature`/`gameobject` row per entry and tracks a dirty flag, so an edit can be written back. Moves convert to server coordinates through `SpawnPlacement::serverPositionFor`, the tested inverse of the load path, so an untouched spawn re-emits the coordinates it was read with. |
| 2026-07-31 | `WorldDatabaseConnection::executeScript` — runs a whole changeset. `execute()` sends one statement, so a multi-statement changeset was rejected wholesale: safe, but it made every emitted file unapplicable. Splits on statement boundaries and runs them in order on one connection, because `SET @CGUID` only survives on the connection that set it. Routes every statement through `execute()`, so the DEV_WRITE guard cannot be bypassed by the new entry point. |
| 2026-07-31 | Added `src/noggit/ui/GroundEffectSetEditor.{hpp,cpp}` — authoring for ground effect sets, reachable from Tools ▸ Ground Effect Sets. Noggit could already assign an existing effect id to a texture layer, derive the per-cell layer map from the alpha maps (`TextureSet::updateDoodadMapping`) and paint the exclusion stencil, but it could not *create* a set: `GroundEffectsTool` only ever calls `getByID`/`CheckIfIdExists`. This writes `GroundEffectTexture.dbc` and `GroundEffectDoodad.dbc` through `DBCFile::save()`, which emits them under the project's own `DBFilesClient/` — a patch, never the client install. Also bulk-assigns a set to every layer using the selected texture across a tile or all loaded tiles. |
| 2026-07-31 | `src/noggit/tools/TexturingTool.{hpp,cpp}`, `src/noggit/MapView.{h,cpp}` — added `selectedTexturePath()` and `cameraPosition()` accessors so the ground effect editor can target the working texture without being handed the texturing widget. |
| 2026-07-31 | `CMakeLists.txt` — excluded the four connector-dependent database sources from the build when MySQL Connector/C++ is absent. `collect_files` globs `src/noggit` recursively while the connector include path is added only when found, so a plain `-DUSE_SQL=OFF` build failed with `WorldDatabaseConnection.cpp(5,10): error C1083: Cannot open include file: 'cppconn/driver.h'`. Confirmed by building it, not by inspection. This restores the no-SQL configuration, which matters for a public fork whose users mostly will not have Connector/C++ installed. |
| 2026-07-31 | Added `src/noggit/TerrainRules.{hpp,cpp}` and `tests/TerrainRulesTests.cpp` — rule-driven automatic texturing by slope and height. Pure: no Qt, no OpenGL, no MapChunk; the world walk is templated so no Noggit header enters the header. Rules resolve by an explicit total order (priority, then specificity, then strength, then texture name, then list index) rather than first-match, because first-match makes the result depend on the order the user happened to add rules in and is unreproducible across sessions. |
| 2026-07-31 | Added `src/noggit/ErosionKernel.{hpp,cpp}` and `tests/ErosionKernelTests.cpp` — thermal erosion (angle-of-repose relaxation) on a plain height lattice. Thermal rather than hydraulic on purpose: hydraulic erosion is a simulation you stop rather than one that finishes, whereas a surface already at or below repose is a fixed point, so the kernel is idempotent and mass-conserving — both properties are asserted, not assumed. |
| 2026-07-31 | Added `src/noggit/AmbientOcclusion.{hpp,cpp}` and `tests/AmbientOcclusionTests.cpp` — horizon-sampled ambient occlusion baked into terrain vertex colours (MCCV). Deliberately does not include `MapHeaders.h` for `TILESIZE`/`UNITSIZE`: that header opens with `<glm/vec3.hpp>` and the standalone test target has no glm include path, so the two constants are restated with their source cited. |
| 2026-07-31 | Added `src/noggit/AlphaIntegrity.{hpp,cpp}` and `tests/AlphaIntegrityTests.cpp` — detects and repairs alpha-map states an ADT can legally store but no renderer can display: layer weights that do not sum to full coverage, layers referencing textures that carry no weight anywhere, and duplicate texture layers. Distinct from the two fixes already in the tree (`Alphamap::readNotCompressed`'s row/column-63 duplication and `TextureSet::convertToBigAlpha`), which it documents rather than repeats. |
| 2026-07-31 | `tests/CMakeLists.txt` — wired the four modules above into the standalone test target. That file is hand-maintained while the root `CMakeLists.txt` globs `src/noggit` recursively, so a new module builds into the application automatically but is silently untested until it is added here. |
| 2026-07-31 | Added `src/noggit/tools/ErosionTool.{hpp,cpp}` and `src/noggit/ui/ErosionToolSettings.{hpp,cpp}` — the erosion brush over `ErosionKernel`, plus `editing_mode::erosion` in `tool_enums.hpp`. The enumerator is appended rather than inserted because `MapView` indexes its tool vector by these values directly, so a tool's position in `MapView::createGUI` must equal its enumerator. `onTick` is gated on the left mouse button, which is safe here and was not in `ScriptingTool`: nothing in this path is an edge detector, so a skipped tick is equivalent to a tick with nothing to do. `deltaTime` is deliberately unused — thermal erosion has a fixed point, so frame rate changes how quickly a stroke settles and not what it settles to. |
| 2026-07-31 | Added `src/noggit/ui/AutoTextureDialog.{hpp,cpp}`, `src/noggit/ui/AmbientOcclusionDialog.{hpp,cpp}` and `src/noggit/ui/AlphaIntegrityReport.{hpp,cpp}` — the interactive surfaces over `TerrainRules`, `AmbientOcclusion` and `AlphaIntegrity`. All three are modeless dialogs on the Tools menu next to the ground effect editor rather than entries under Assist: each owns a scope selector and reports what it examined before it writes, while every Assist entry acts on the ADT under the camera the instant it is clicked. `AmbientOcclusionDialog` is a dialog rather than a `Tool` because `editing_mode::mccv` already belongs to `VertexPainterTool` and `ShaderTool`. Note `Noggit::AlphaIntegrityReport` (the per-chunk value struct) and `Noggit::Ui::AlphaIntegrityReport` (the window) differ only by namespace; both headers call the collision out. |
| 2026-07-31 | `src/noggit/MapView.cpp` — registered the four modules above. `ErosionTool` is **appended** to the `_tools` sequence in `createGUI` and nothing above it moves: `MapView` indexes that vector with `editing_mode` values directly (`selectedTexturePath`, and the `paint`/`object` resets in the destructor), so a tool's position must equal its enumerator and inserting anywhere earlier silently hands every later tool the wrong slot. The three dialogs share the separator the ground effect editor already opened rather than adding their own. |
| 2026-07-31 | `src/noggit/Action.{hpp,cpp}`, `src/noggit/MapChunk.{h,cpp}` — vertex-colour undo now restores the MCCV *flags*, not just the colours. `initMCCV` sets both `hasMCCV` and `header_flags.flags.has_mccv`; undo restored only the `mccv[]` array, so painting vertex colour on a chunk that had none and pressing Ctrl+Z looked correct — a chunk with no MCCV block already reads as neutral white, so the colour restore is a visual no-op — while `MapChunk::save` (which gates the whole block on the runtime flag, not the header bit) then wrote an MCCV block and a non-zero `ofsMCCV` into an ADT that never had either. Pre-existing and tree-wide: `MapChunk.cpp:891` and `:936` are the shipping vertex-paint entry points. The ambient-occlusion bake did not introduce it, it made it a bulk operation. Fixed by snapshotting both flags in each direction and adding `MapChunk::setHasMccv`, the counterpart to `initMCCV`, which can only ever *set* the flag while undo has to *clear* it. |
| 2026-07-31 | `src/noggit/ActionManager.cpp` — `endAction` clears `_cur_action` before the first thing that can throw. `Action::finish()` allocates the redo snapshot, so a bulk operation could leave it throwing `std::bad_alloc` with `_cur_action` still set; nothing ever cleared it again, and `beginAction` returns the running action rather than starting a new one, so from that point every stroke in the session joined a dead action that could never be closed and undo stopped working entirely. The action is deliberately left on the stack on failure: its undo data was recorded as the edits happened and is intact, which is the half that matters after an operation blows up. |
| 2026-07-31 | `src/noggit/ui/AutoTextureDialog.cpp`, `AlphaIntegrityReport.cpp`, `AmbientOcclusionDialog.cpp`, `tools/ErosionTool.cpp`, `ErosionKernel.{hpp,cpp}` — twelve defects found by adversarial review of the four new surfaces, each confirmed by two independent refutation attempts before being fixed. The ones worth naming: the AO bake held raw `MapTile*`/`MapChunk*` across `QProgressDialog::setValue`, which pumps the event loop and lets `MapView::tick()` unload those very tiles; the auto-texture dialog resolved "this ADT tile" from the live camera at apply time, so it could paint a tile the preview never examined; the erosion lattice used half the terrain's x resolution, making every second x-edge read twice the true slope; and the alpha report's row-selection camera focus aimed at the sky. |

Keep this table current. When application code lands under `src/`, every new file needs the
project's GPL header, matching the 337 existing files that carry it:

```cpp
// This file is part of Noggit3, licensed under GNU General Public License (version 3).
```

## Upstream authorship

Noggit predates this repository — the source headers refer to **Noggit3**, and the lineage runs
back through earlier Noggit and Noggit SDL releases by authors not represented in this repo's
git history. Credit the project, not just the commit log.

`noggit-red` itself spans 1441 commits from 2020-10-09 to 2025-01-17. Principal contributors by
commit count:

> T1ti · Skarn · sshumakov3 · Alister · Martin Benjamins · Balkron · p620 · EIntemporel ·
> Kaev · ihm-tswow · Intemporel · BalkronPainter · Felfired · Havric · DennisWG · Varen ·
> BinarySpace

Regenerate with `git shortlog -sn 6f0776d4`. Before publishing, check upstream for an
`AUTHORS`/`CONTRIBUTORS` file added after the fork point and prefer it over this list.

## Bundled third-party code

Everything under `src/external/` is third-party and carries its own terms. `src/External` is
explicitly exempt from this project's coding rules per the README.

| Component | Licence | Notes |
|---|---|---|
| framelesshelper | MIT | |
| imguizmo | MIT | |
| rapidfuzz-cpp | MIT | |
| NodeEditor | BSD | Requires Qt5 ≥ 5.10 |
| tracy | BSD | |
| libnoise | LGPL | GPL-3.0 compatible |
| qt-color-widgets | LGPL | GPL-3.0 compatible |
| QtAdvancedDockingSystem | LGPL | GPL-3.0 compatible |
| StormLib | (fetched) | Ladislav Zezula |
| CascLib | (fetched) | Ladislav Zezula |
| nlohmann/json | (fetched) | MIT; declares `cmake_minimum_required` below 3.5 |

> [!warning] Action required before publishing
> These bundled components have **no licence file at their top level**, so their terms cannot be
> confirmed from the tree as it stands:
>
> `blizzard-archive-library`, `blizzard-database-library`, `glm`, `imguipiemenu`, `PNG2BLP`,
> `qtgradienteditor`, `qtimgui`, `tsl`
>
> Most are well-known permissive libraries (glm is MIT/Happy Bunny, tsl/robin-map is MIT,
> qtimgui is MIT), but "probably MIT" is not a licence audit. Resolve each from its upstream
> repository and record it above before release. This is inherited from upstream, not
> introduced here — which does not make it someone else's problem once you publish.

## AtlasForge — independent reimplementation, no shared code

Some of the workflow design here is **inspired by** AtlasForge (RetroGooby), a closed-source
Windows editor for WoW 1.12.1: tile-centric editing, ghost placement with a staged pending-change
list, an explicit build/commit step, and an environment "Doctor".

**No AtlasForge code is included, and none could be.** It is distributed as a binary-only
release with no published source. Only observable UX concepts were reimplemented, independently,
against a different game version (3.3.5a) and a fundamentally different data layer — AtlasForge
builds client-side MPQ patches, whereas this writes server-side database changesets. There is no
analog to `patch-Z.MPQ` here.

If you describe the lineage publicly, describe it as conceptual inspiration. Do not imply
affiliation, endorsement, or shared implementation.

## TrinityCore — schema facts, not source code

`docs/schema-335.md` documents the TrinityCore 3.3.5 world database layout. **No TrinityCore
source code is copied into this project.** TrinityCore is GPL-2.0, which is not compatible with
GPL-3.0, so this distinction matters and is deliberate.

What the documentation contains is factual: column names, ordinal positions, types, and defaults
read from `information_schema` on a running database, plus semantic notes about which columns the
server derives rather than reads. Database column names are an interface. Where the core's own
`INSERT` column list is reproduced, it is reproduced as a factual list of identifiers, with its
location cited so a reader can verify it.

Keep it that way. Do not paste TrinityCore implementation code into this repository.

## Blizzard content — none, ever

No game client data is included or may be: no MPQ archives, no DBC/DB2 files, no models,
textures, maps, or extracted assets. Users supply their own legally obtained 3.3.5a client.

The synthetic fixtures in `tools/dev-db/02_seed_synthetic.sql` use real creature and gameobject
**display IDs**. These are integer keys into the client's DBC files, not content — they have to
be real for `displayId` → model resolution to be testable at all. Every accompanying value
(names, bounding radii, coordinates, entries, GUIDs) is invented, and all IDs sit far above the
ranges TDB uses.

## Not published — verify before every release

These are gitignored because they describe private infrastructure or hold secrets. Confirm none
are tracked before pushing:

| Path | Why |
|---|---|
| `docs/environment.md` | One machine's toolchain, database inventory, and security posture |
| `tools/dev-db/db-policy.json` | Names the operator's live databases |
| `tools/dev-db/dev-db.config.json` | Machine-specific paths and connection details |
| `.claude/settings.local.json` | Local filesystem paths |
| `tools/dev-db/.generated/**` | Generated credentials, filled bootstrap, structure dumps |
| `tools/dev-db/*.local.sql` | Bootstrap with real passwords substituted |

Each has a committed `.example` twin, so a fresh clone is usable without them.

Pre-push check:

```bash
git ls-files | grep -E "environment\.md|db-policy\.json|dev-db\.config\.json|settings\.local\.json|\.local\.sql|\.generated/"
```

That must print nothing. Also confirm `tools/dev-db/01_bootstrap_root.sql` still contains its
two `__CHANGE_ME__` placeholders rather than real passwords.
