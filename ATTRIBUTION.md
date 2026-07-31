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
| 2026-07-30 | Added `docs/`, `tools/dev-db/`, `ATTRIBUTION.md` — TrinityCore 3.3.5 database-editing groundwork. No existing source files modified. |
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
| 2026-07-31 | `src/noggit/MapView.cpp` — three dev-bridge commands: `menus` lists every menu entry the running application actually offers, `tools` lists the registered tools and flags any whose list position does not equal its own `editing_mode` value, and `trigger <substring>` activates a menu entry (refusing an ambiguous substring rather than guessing). Compile and link evidence proves a `QAction` was constructed; it does not prove it was added to a menu a user can open, and it does not prove that activating it survives. Both gaps had to be closed by asking the running process. The tool-index invariant in particular lives in the agreement between `tool_enums.hpp` and `createGUI`, where nothing checks it. |
| 2026-07-31 | Added `src/noggit/ui/SpawnTilePicker.{hpp,cpp}` and extended `src/noggit/ui/DatabaseSpawnPanel.{hpp,cpp}` — multi-tile spawn loading. The picker **subclasses upstream's `minimap_widget`** rather than drawing a second grid; the base already implements click-select, shift 3×3 brush, ctrl-deselect, drag-paint and the map's own minimap. Its "loaded" marker is three-state (`NONE`/`CACHED_ONLY`/`DRAWN`), because the renderer only draws spawns for tiles it is already walking (`WorldRender.cpp:429` iterates `_loaded_tiles_buffer`) and Noggit streams roughly 5×5 around the camera — so picking a distant block loads spawns that are editable and in the changeset but not on screen. Marking that honestly, rather than painting every picked cell as loaded, is the difference between a useful load and one that reports success at an empty viewport. A poll timer refreshes the classification because which ADTs are streamed changes as the camera flies. |
| 2026-07-31 | `src/noggit/MapView.{h,cpp}`, `src/noggit/ui/tools/ViewportGizmo/ViewportGizmo.{hpp,cpp}`, `src/noggit/database/SpawnSceneCache.{hpp,cpp}` — axis-drag translate/rotate gizmo for database spawns. A spawn's `ModelInstance` is deliberately **not** passed to `handleTransformGizmo`: that compiles, since `ModelInstance` is a `SceneObject`, but it calls `updateTilesEntry(..., model_update::add)`, which reaches `adt->add_model()` and would register a server-side spawn into `MapTile::object_instances` — the ADT MDDF save path this fork keeps DB spawns structurally out of. It would also key undo on `uid`, which is 0 for every spawn, and write `pos`/`dir` without touching the source row, so the spawn would move on screen while the changeset stayed empty. A separate `handleDetachedGizmo` seam avoids all three; rotation routes through `SpawnSceneCache::rotateTo` so the gameobject `rotation0..3` handling is not bypassed. |
| 2026-07-31 | `src/noggit/MapView.{h,cpp}` — `loadDatabaseSpawnsForTiles` takes an explicit tile list, and `loadDatabaseSpawns` became a thin wrapper over it. The panel had reimplemented the connection setup, schema introspection, unsaved-changes warning, count threshold and the OpenGL `makeCurrent` + `scoped_setter` guard; a second copy of that is what made a multi-tile load fail a camera-tile check it had no reason to care about. Callers pass `::TileIndex` in ADT `(x, z)` order and the transposition to the server's `(x, y)` happens inside, so a caller cannot get the two frames the wrong way round. |
| 2026-07-31 | `src/noggit/MapView.cpp` — dev-bridge `mccvcheck`, which round-trips a vertex-colour edit on a chunk with no MCCV block and reports `hasMCCV` and `header_flags.flags.has_mccv` at each step. It exists because the defect it checks for is invisible on screen: a chunk with no MCCV block already reads as neutral white, so restoring the colour array is a no-op and an undo that leaves the flags set looks perfect. Measured against the running editor over tile 31,49: `before=0,0 after_edit=1,1 after_undo=0,0 after_redo=1,1`. The command restores the world before returning. |
| 2026-07-31 | `src/noggit/application/ApplicationEntry.cpp`, `src/noggit/ui/windows/noggitWindow/NoggitWindow.{hpp,cpp}` — `--project`, `--map` and `--goto` open a project and enter a map with no window interaction, so the dev bridge can drive a real session instead of one assembled in a test. Every addition sits inside `#ifdef NOGGIT_DEV_BRIDGE_ENABLED` and is absent from a default build, for one reason: `openMapUnattended` calls `enterMapAt` with `uid_fix_mode::none` rather than `check_uid_then_enter_map`, which may raise the UID fix window — a modal, and a modal is a hang when there is no human present. That makes it fine for looking at a tile and wrong for editing one, since a session that skipped the UID check and then saved could reuse unique IDs; the header carries that warning and the session logs it on entry. `--goto` takes TrinityCore server coordinates and converts them through `SpawnPlacement::positionFor`, the same tested seam the bridge's own `goto` uses, rather than by hand. The project-open path mirrors `NoggitProjectSelectionWindow` minus the window and is kept in step with it by hand, which the comment says out loud. |
| 2026-07-31 | Added `src/noggit/AssetScan.{hpp,cpp}` and `tests/AssetScanTests.cpp`, plus the Assist ▸ `Report missing assets...` entry in `src/noggit/MapView.cpp` — every model, world model and texture the loaded tiles reference, and which of those the client data chain cannot produce. The module's split runs *through* the header rather than between two of them: everything with an out-of-line definition depends on nothing but the STL, while the world walk is a set of templates purely so `World.h`, `MapTile.h`, `Model.h`, `WMO.h` and `ClientData.hpp` stay out of it — that is what lets the aggregation half link into the standalone test target with no client install, no Qt and no build-system conditionals. Probes are memoised per distinct asset because `ClientData::exists` walks every open archive on a miss and a map resolves a few hundred distinct `.blp` names across tens of thousands of references; without it the scan presents as a hang. Paths are lower-cased by an explicit ASCII range test rather than `::tolower`, which has undefined behaviour for the bytes ≥ 0x80 that ADT texture lists and DBC string tables really carry — `ClientData::normalizeFilenameInternal` has exactly that bug and this does not reproduce it. Terrain textures are read through a plain 16×16 chunk loop rather than `World::for_all_chunks_on_tile`, which calls `mapIndex.setChanged` on entry and would put unsaved-change prompts in front of a user who only asked a question. `Missing` and `Unreadable` are kept apart because the fix differs — ship the file versus replace the corrupt one — even though both render as nothing in game. |
| 2026-07-31 | Added `src/noggit/UidCollisionLog.{hpp,cpp}` and `tests/UidCollisionLogTests.cpp`; `src/noggit/world_model_instances_storage.{hpp,cpp}` record each repair and `src/noggit/World.h` gains a narrow `uidCollisionLog()` accessor, reachable from Assist ▸ `UID collision report...`. Noggit already renumbers duplicate unique IDs in memory as instances load, and that repair is deliberately untouched — `newGUID()` is still called exactly once per collision and its result is still what the instance is stored under, because the renumbered uid is what the tile is then saved with and changing it would move real object placements. What was missing is the evidence: the whole collision set collapsed into one `_uid_duplicates_found` bool that `MapView.cpp:4670` turns into a single modal naming nothing, so a mapper hit by a `uid.ini` desync is told that "a" uid was in use and cannot learn which objects were affected, in which tile, or how many. The record is taken before the uid is overwritten and before the instance is moved, both of which destroy what it is about. The log holds its own mutex so a reader on the UI thread never waits on the storage lock, which instance loading holds for the whole of an add; it caps at 4096 records and keeps the *first*, since the earliest collisions identify where the desync came from and later ones repeat it; and it is deliberately not emptied by the storage's `clear()`, whose only route is `MapIndex::fixUIDs` — the operation a user runs *because of* these collisions, so wiping the evidence there would destroy it at the moment they went looking for it. The header is STL-only, which is what admits the recorder to the standalone test target and why the tile is kept as plain indices rather than a `TileIndex`; an out-of-grid position is recorded as "no tile" rather than clamped to one it does not belong to. `World.h` exposes the log rather than the storage because widening access to reach it would hand every caller the instance maps as well. |
| 2026-07-31 | `src/noggit/scripting/scripting_tool.{hpp,cpp}`, `src/noggit/tools/ScriptingTool.cpp` — **supersedes the `ScriptingTool::onTick` left-mouse gate recorded in the scripted-placement row above.** Gating `onTick` the way every terrain brush does broke the scripting brush outright: `sendBrushEvent` drives an edge detector comparing the previous button state against the current one, so returning early whenever the button was up meant the "up" state was never observed — `on_left_click` fired once per session, and `on_left_release` and every right-button callback never fired at all. `onTick` now calls it unconditionally and `sendBrushEvent` self-gates instead: idle with nothing pending it dispatches nothing, allocates no event object and opens no undo action, and it opens its own action with the `eLMB`/`eRMB` modality only when a callback is about to run, so a stroke is still one undo step rather than one per frame. The action moved into `sendBrushEvent` because the release edge lands on the tick *after* `MapView` closed the caller's LMB-modal action, so there is provably none open for it and a world edit with no action running dereferences a null `NOGGIT_CUR_ACTION`; `beginAction` returns the running action untouched when there is one (`ActionManager.cpp:64-65`), so a caller that opens its own keeps ownership and none of this fires. An action opened on a tick with no button held is closed immediately — it carries no modality controller, `endActionOnModalityMismatch` returns early on `eNONE`, and it could therefore never be closed at all, leaving `undo()` to assert with it still running. The per-tick contract is stated in the header, and the fallback for a caller that ignores it disarms itself the moment it observes one idle tick. |
| 2026-07-31 | `src/mysql/mysql.{h,cpp}` — removed the `CREATE DATABASE IF NOT EXISTS` that upstream ran on **every** `connect()`, and bound the whole seam to the nominated writable schema. As shipped, enabling UID storage and pointing Noggit at a production world database would have it create a schema and a table there before doing anything else: the project's central safety claim falsified by the application itself, in inherited code rather than in anything this fork wrote. Two changes, in this order. The `CREATE DATABASE` is gone outright, so a schema that does not exist is now a reported connection failure instead of something conjured into being. And nothing opens a connection at all unless `project/mysql/db` is exactly `project/mysql/dev_schema`, which puts the surviving `CREATE TABLE`, the `INSERT` and the `UPDATE` beyond reach of any other schema. The policy is deliberately not reimplemented here — the schema comes from `Database::DatabaseSettings` and the enforcement from `WorldDatabaseConnection`'s `DEV_WRITE` guard, the same path the fork's own database layer is already bound by, because a second parallel policy is a second thing to get wrong; the explicit check ahead of it is redundant on purpose, firing before a socket is opened and phrased in terms of UID storage rather than changesets. `testConnection` now opens `READ_ONLY`: a button whose entire job is to test reachability must not be the thing that writes to a server, and it correctly still reports success against a live database that UID storage itself will refuse. This also supersedes the `<driver.h>` include fix recorded above, by removing the connector headers from the file entirely — every statement goes through `WorldDatabaseConnection` now. Upstream's connect-per-call remains, unrestructured and documented: it is tolerable only because these five functions run a handful of times per session, and it is exactly why the fork's per-tile spawn queries never extended this seam. `mysql.h` also gained an include guard in place of `#pragma once`, per the repo's own rule. |
| 2026-07-31 | `docs/schema-335.md` — removed infrastructure detail from the provenance block ahead of publication. The precise server build and its locality, the measured database's table count, the core revision hash and the name of the particular TrinityCore fork it was cross-checked against, and the enumeration of one operator's four custom tables all described a private machine rather than the 3.3.5 schema; the note comparing it against "a second, heavily customised world database on the same server" disclosed a database inventory outright. The column-level facts are untouched — they are the schema ground truth this document exists for, and they remain measured from `information_schema` rather than quoted from anywhere. What replaces the removed material states plainly the limitation those details had been carrying implicitly: the measured `world` is not a pristine TDB install, so its table *list* and *count* say nothing about stock TDB 335.25101 and only the column facts do. The `playercreateinfo_spell_custom` / `spell_custom_attr` trap is kept, being a fact about stock TrinityCore rather than about anyone's server. |
| 2026-07-31 | Removed `resources/font_awesome.otf`, `resources/segoeui.ttf` and `resources/segoeuisb.ttf`, and rebuilt `src/noggit/ui/FontAwesome.{hpp,cpp}` and `resources/resources.qrc` around their absence. None of the three could lawfully be published from this repository. `font_awesome.otf` is Font Awesome 5 **Pro** Regular 5.14.0 — a paid product whose licence forbids redistributing the font file and specifically forbids committing it to a public repository — and the two Segoe UI faces are Microsoft's, whose name table permits use only as part of a licensed Microsoft product. All three were inherited from upstream, which does not make publishing them someone else's act. `noggit_font.ttf` stays, being Noggit's own icon font. Font Awesome is now resolved at runtime from `<install dir>/fonts` (and the working directory) under any of the usual Free or Pro file names, or from an installed system family preferring the solid face; Segoe UI is resolved by family name through the new `Noggit::Ui::UiFonts` instead of being loaded from a bundled file. With none of them present the application still runs: `FontAwesomeIconEngine` paints a standard Qt icon where one fits the meaning and a short text label otherwise, so every button stays identifiable. That degradation is real and visible, and is documented in the file rather than papered over — Font Awesome Free is SIL OFL 1.1 and may be installed freely, but part of the codepoint list in `FontAwesome::Icons` is Pro-only and stays blank even with Free installed. Nothing in this path downloads a font. `FontAwesome.hpp` also gained an include guard in place of `#pragma once`, per the repo's own rule. |
| 2026-07-31 | `.gitignore` — this repository ignores everything (`/**`) and whitelists back explicitly, and the inherited whitelist un-ignored directory *contents* (`!/src/**`) but never the directories themselves. `/**` matches the bare `src` entry too, so git prunes the directory and never looks inside it; the tracked files there survive only because gitignore does not apply to already-tracked files, which means a **new** file under any whitelisted directory is silently invisible to `git add`. That bit this fork the first time a new source file failed to stage, and it was still latent for `scripts/` (23 Lua/TypeScript files inherited from upstream, tracked but never un-ignored). Both are fixed by un-ignoring each directory alongside its contents. Added whitelist entries for `docs/`, `tools/` and `tests/`, then re-ignored — last matching pattern wins — the generated, machine-specific and secret-bearing files listed under "Not published" below. Ahead of publication two hard re-ignore blocks were added on top: build and editor artefacts, because the broad `!/bin/**` and `!/src/**` whitelists mean an in-source build lands somewhere git is already looking; and every WoW client data extension (`*.mpq`, `*.dbc`, `*.m2`, `*.wmo`, `*.adt`, `*.blp`, …) across the whole tree *including* `tests/` and `src/`, so a fixture cannot be added without noticing. That second block is a copyright control rather than tidiness: one committed `.blp` is redistribution of Blizzard assets, and it is permanent once pushed. |
| 2026-07-31 | `README.md` — rewritten as this fork's front page ahead of publication: what it adds over upstream, the never-writes-to-a-live-database constraint stated before anything else, requirements, quick start, an honest status-and-limitations section, and credit to upstream at the top rather than the bottom. Upstream's original text is **kept verbatim** rather than replaced, demoted under a `# Upstream documentation` heading that retains its LICENSE, BUILDING, SUBMODULES and CODING GUIDELINES sections unaltered. That is deliberate on both counts: the coding guidelines are still the rules this repository holds itself to, and the build instructions are still the ones the upstream project publishes, so dropping either would leave the fork's own documentation as the only surviving record of them and quietly attribute upstream's work to this fork. The new material says plainly that anyone who only wants a map editor should use upstream instead. |

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

Everything under `src/external/` is third-party and carries its own terms, and so are
`include/utf8.h` and `include/win/`. `src/External` is explicitly exempt from this project's
coding rules per the README.

**Scope and method of this audit.** Every entry below was determined *from the files present in
this working tree* — a `LICENSE`/`COPYING` file, or a licence grant in the component's own
sources — and cites the path that proves it. Nothing here was inferred from a library's
reputation or looked up in an upstream repository. Where the tree does not state the terms, the
row says **UNRESOLVED** rather than guessing. Audited 2026-07-31 against the tree as it stands;
re-run it whenever a submodule pointer or a vendored copy moves.

"GPL-3.0 compatible" means the component may be combined and redistributed as part of a GPL-3.0
work. Permissive terms (MIT, BSD, zlib, libpng, Boost) are. LGPL-2.1 is, via LGPL-2.1 §3, which
permits relicensing a copy under GPL v2 *or any later version*. LGPL-3.0 is, being GPLv3 with
added permissions. GPL-3.0-or-later is. **GPL-2.0-only would not be** — nothing found in this
tree carries GPL-2.0-only terms.

### Vendored under `src/external/`

| Component | Location in tree | Licence | Evidence in tree | GPL-3.0 compatible |
|---|---|---|---|---|
| framelesshelper | `src/external/framelesshelper` | MIT | `framelesshelper/LICENSE` | Yes |
| imguizmo | `src/external/imguizmo` | MIT | `imguizmo/LICENSE` | Yes |
| rapidfuzz-cpp | `src/external/rapidfuzz-cpp` | MIT | `rapidfuzz-cpp/LICENSE` | Yes |
| tsl (robin-map/robin-set) | `src/external/tsl` | MIT | Full MIT grant heading all four headers, e.g. `tsl/robin_map.h:1-23` | Yes |
| Dear ImGui | `src/external/qtimgui/imgui` | MIT | `qtimgui/imgui/LICENSE.txt` | Yes |
| NodeEditor | `src/external/NodeEditor` | BSD-3-Clause | `NodeEditor/LICENSE` | Yes — requires Qt5 ≥ 5.10 |
| tracy | `src/external/tracy` | BSD-3-Clause | `tracy/LICENSE` (states 3-clause BSD on its face) | Yes |
| libnoise | `src/external/libnoise` | LGPL-2.1 | `libnoise/LICENSE.md` | Yes (LGPL-2.1 §3) |
| QtAdvancedDockingSystem | `src/external/QtAdvancedDockingSystem` | LGPL-2.1 | `QtAdvancedDockingSystem/LICENSE`, `gnu-lgpl-v2.1.md` | Yes (LGPL-2.1 §3) |
| qt-color-widgets | `src/external/qt-color-widgets` | LGPL-3.0-or-later | `qt-color-widgets/COPYING`, plus a per-file grant in every source, e.g. `src/abstract_widget_list.cpp:1-22` | Yes |
| qtgradienteditor | `src/external/qtgradienteditor` | LGPL-2.1 with the Nokia Qt LGPL Exception 1.1 | Per-file Qt licence block, e.g. `qtgradienteditor/qtgradienteditor.cpp:1-41` | Yes (LGPL-2.1 §3) — see caveat below |
| **glm** | `src/external/glm` (v0.9.9.8, per `glm/detail/setup.hpp:11`) | **UNRESOLVED** | None. No `LICENSE`/`COPYING`/`copying.txt`/`manual.md`; no licence grant in any header. The only copyright notice in the whole directory is a Sun Microsystems permission notice in `glm/ext/scalar_ulp.inl:1-6`, which covers that one file. | **Unresolved** |
| **imguipiemenu** | `src/external/imguipiemenu` | **UNRESOLVED** | None. Two files (`PieMenu.hpp`, `PieMenu.cpp`), no licence file, no header, no attributed author. | **Unresolved** |
| **qtimgui** (the Qt wrapper itself, not the bundled Dear ImGui) | `src/external/qtimgui/{QtImGui,ImGuiRenderer}.{h,cpp}` | **UNRESOLVED** | None. `LICENSE.txt` exists only under `qtimgui/imgui/`; the wrapper sources carry no grant. | **Unresolved** |
| **PNG2BLP** (the top-level converter, not its bundled libraries) | `src/external/PNG2BLP/*.{h,cpp}` | **UNRESOLVED** | None. No licence file, no header on any of the ten top-level sources. | **Unresolved** |

### Third-party libraries nested inside PNG2BLP

`src/external/PNG2BLP` vendors five further libraries. These were not previously recorded at
all, and one of them is copyleft.

| Component | Location in tree | Licence | Evidence in tree | GPL-3.0 compatible |
|---|---|---|---|---|
| zlib | `PNG2BLP/zlib` (v1.2.11) | zlib | `zlib/zlib.h:1-21` | Yes |
| libpng | `PNG2BLP/libpng` | libpng licence | `libpng/LICENSE` | Yes |
| png++ | `PNG2BLP/pngpp` | BSD-3-Clause | `pngpp/COPYING`, `pngpp/AUTHORS` | Yes |
| libtxc_dxtn | `PNG2BLP/libtxc_dxtn` | MIT | `libtxc_dxtn/txc_dxtn.h:1-23` | Yes |
| **libimagequant** | `PNG2BLP/libimagequant` (v2.12.2) | **GPL-3.0-or-later** | `libimagequant/blur.c:1-18` states the GPLv3-or-later grant in full. `pam.h:1-14` adds a separate permissive notice from Jef Poskanzer / Greg Roelofs for that file. | Yes — but see below |

`libimagequant` being GPLv3-or-later is fine for this project (the whole work is GPL-3.0 already)
and it is *not* a blocker, but it is worth stating plainly: the built binary is unambiguously
copyleft, and the previous summary — which implied the bundle was permissive and LGPL — was
wrong about that.

### Third-party outside `src/external/`

| Component | Location in tree | Licence | Evidence in tree | GPL-3.0 compatible |
|---|---|---|---|---|
| UTF8-CPP (Nemanja Trifunovic) | `include/utf8.h` | Boost Software License 1.0 | `include/utf8.h:1-30` | Yes |
| **StackWalker** (Jochen Kalmbach) | `include/win/StackWalker.{h,cpp}` — compiled via `error_handling.cpp` | **UNRESOLVED** | None. No licence text in either file; the header records only a 2005 CodeProject release history. | **Unresolved** |

### Git submodules

These are referenced by commit, not copied into this repository, but a user who clones with
`--recursive` receives them and the built binary links them.

| Component | Location in tree | Licence | Evidence in tree | GPL-3.0 compatible |
|---|---|---|---|---|
| **blizzard-archive-library** | `src/external/blizzard-archive-library` (submodule) | **UNRESOLVED** | None. No `LICENSE`/`COPYING`; no grant in any source. `README.md` is a stale copy of the sibling library's, so even the identity of the project is unstated. | **Unresolved** |
| **blizzard-database-library** | `src/external/blizzard-database-library` (submodule) | **UNRESOLVED** for the library as a whole | None at top level. One nested component *is* stated: ByteStream (Pablo Albiol) is LGPL-3.0-or-later per `include/external/ByteStream.h:1-18` and `src/external/ByteStream.cpp:1-18`. That covers those two files only. | **Unresolved** |
| build-dependencies (`cmake/`) | `cmake` (submodule) | **UNRESOLVED** | Build scripts only; no licence file. Not linked into the binary. | **Unresolved** (low risk — build tooling) |
| `dist/definitions`, `dist/listfile`, `dist/themes` | submodules | **UNRESOLVED** | Data, not code. No licence file. | **Unresolved** (data, not linked) |

### Fetched at build time — not distributed by this repository

`FetchContent` pulls these during configure. Publishing this repository's *source* does not
distribute them; shipping a **built binary** does, and their obligations attach at that point.
None of them is present in this tree, so none can be audited from it.

| Component | Fetched from | Licence |
|---|---|---|
| StormLib (Ladislav Zezula) | `gitlab.com/prophecy-rp/dependencies.git` (`cmake/FindStormLib.cmake`) | Not determinable from this tree |
| CascLib (Ladislav Zezula) | `gitlab.com/prophecy-rp/dependencies.git` (`cmake/FindCascLib.cmake`) | Not determinable from this tree |
| Lua 5.x | `gitlab.com/prophecy-rp/dependencies.git` (`cmake/FindLua.cmake`) | Not determinable from this tree |
| sol2 | `github.com/tswow/sol2` (`cmake/FindSol2.cmake`) | Not determinable from this tree |
| FastNoise2 | `github.com/tswow/FastNoise2` (`cmake/FindFastNoise2.cmake`) | Not determinable from this tree |
| lodepng | `github.com/lvandeve/lodepng` (`cmake/Findlodepng.cmake`) | Not determinable from this tree |
| nlohmann/json | `github.com/ArthurSonzogni/nlohmann_json_cmake_fetchcontent` (`cmake/FindJson.cmake`) | Not determinable from this tree; declares `cmake_minimum_required` below 3.5 |
| Qt5 | supplied by the user | Not distributed here |

> [!warning] Unresolved before publishing
> Seven components carry **no licence statement anywhere in this tree**, so their terms are
> genuinely unknown here — not "probably MIT":
>
> `glm`, `imguipiemenu`, `qtimgui` (wrapper), `PNG2BLP` (top level), `StackWalker`,
> `blizzard-archive-library`, `blizzard-database-library`.
>
> All seven are compiled into `noggit.exe`. Every one is inherited from upstream rather than
> introduced by this fork, which does not make it someone else's problem once you publish.
> Each must be resolved against its own upstream and its licence text committed alongside it.
> Until then the safe reading is that this repository redistributes code whose terms it cannot
> evidence.
>
> Two further gaps are obligations rather than unknowns — the terms are stated, but the licence
> texts they point the reader at were not vendored:
> `libimagequant`'s sources say "See COPYRIGHT file for license" and no `COPYRIGHT` file exists;
> `qtgradienteditor`'s Qt block points at `LICENSE.LGPL` and `LGPL_EXCEPTION.txt`, neither of
> which is present. Copy both in.
>
> One wording caveat on `qtgradienteditor`: its 2009 Nokia header opens with "No Commercial
> Usage / This file contains pre-release code and may not be distributed." That sentence belongs
> to the Technology Preview option of Qt's three-way licence block, and the same block offers
> LGPL-2.1 as an explicit alternative ("Alternatively, this file may be used under the terms of
> the GNU Lesser General Public License version 2.1"). This project relies on the LGPL-2.1
> option. Anyone reading only the first lines will think otherwise, so do not delete this note.

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
| `tools/dev-db/.generated/**` | Generated credentials, filled bootstrap, structure dumps |
| `tools/dev-db/*.local.sql` | Bootstrap with real passwords substituted |

Each has a committed `.example` twin, so a fresh clone is usable without them.

Pre-push check:

```bash
git ls-files | grep -E "environment\.md|db-policy\.json|dev-db\.config\.json|\.local\.sql|\.generated/"
```

That must print nothing. Also confirm `tools/dev-db/01_bootstrap_root.sql` still contains its
two `__CHANGE_ME__` placeholders rather than real passwords.
