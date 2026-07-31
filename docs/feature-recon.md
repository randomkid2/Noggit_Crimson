# Feature recon — ten map-making features

Read-only reconnaissance over the whole tree, one pass per feature, every claim cited to
`file:line`. Recorded because the headline result is *how much already exists*: three features are
wholly or mostly implemented already, and every remaining one is smaller than it looks because the
machinery underneath is built.

**Do not start any of these without reading its row first.** The expensive mistake here is building
something that is already in the Editor menu.

| # | Feature | Size | Verdict |
|---|---|---|---|
| 1 | Scatter brush | small | **Already exists in Lua.** `scripts/prop_placer.lua` |
| 2 | Auto-texture by slope/height | medium | Genuinely new rule layer; alpha write path already built |
| 3 | UID collision repair | small | **Detection *and* repair already run.** Only reporting is missing |
| 4 | ADT seam stitching | small | **Height stitching already done and in the menu.** Alpha missing |
| 5 | Missing asset report | medium | Existence API already exists and is used in 9 places |
| 6 | WDL generation | — | **Already fully implemented, with a menu item.** Build nothing |
| 7 | game_tele from bookmarks | done | Built. `GameTeleBuilder` + Assist menu action |
| 8 | Waypoint editor | medium | Logic layer done and tested; UI only |
| 9 | Erosion brush | small | No erosion exists, but every *part* does. One new kernel |
| 10 | Vertex-colour AO baking | medium | One pure function + a hook into `VertexPainterTool` |

## The three that are already built

**#6 WDL generation — do not build this.** Two WDL writers and a reader are in the tree, plus a UI
action labelled **"Generate new WDL"**. All inherited Noggit Red code, untouched by this branch
(`git diff HEAD -- src/noggit/map_horizon.cpp` is empty). The correct work is verification, not a
generator.

**#3 UID collision repair — the premise is half wrong.** Collisions are not silently ignored; they
are silently **auto-repaired in memory** on every instance load
(`world_model_instances_storage.cpp:34-63` for M2, `:83-112` for WMO, via `unsafe_uid_is_used` at
`:306`). What is missing is that the collision set is compressed into a single `bool` and never
recorded — so you cannot see what was renumbered. Write a **recorder and a report**, not a detector.

**#1 Scatter brush — one already ships, in Lua.** `scripts/prop_placer.lua` reads the brush radius,
holds three model slots with min/max scale, thins placements with simplex noise and a spacing
distance, and randomises model, scale and yaw. What a C++ version would genuinely add:

- **Undo.** The Lua path calls `World::addM2(..., action=false)`
  (`script_global.cpp:35`), which skips `registerObjectAdded`
  (`world_model_instances_storage.cpp:51`). `ScriptingTool::onTick` never opens an action either.
- Terrain-slope alignment (Lua does yaw only) — though `World::rotate_model_to_ground_normal`
  (`World.cpp:439`) already exists.
- Tilt jitter — though `object_paste_params` (`object_paste_params.hpp:7`) already has min/max
  rotation, tilt and scale, **with a full UI** at `ObjectEditor.cpp:399-406, 447-482`.

Note `ObjectTool` already owns a brush radius with a drawn cursor circle that currently drives only
range-selection (`MapView.cpp:4267`) — a radius with no placement behaviour attached. The hook is
sitting there.

## The rest, with the reusable parts named

**#4 Seam stitching.** `MapChunk::fixGapLeft` / `fixGapAbove` (`MapChunk.cpp:1837-1860`) copy a
neighbour's edge column and call `registerChunkUpdate(ChunkUpdateFlags::VERTEX)`, already wired to a
menu action. Heights are done. **Alpha-map stitching does not exist at all**, and the survey found
three defects in the existing paths.

**#9 Erosion.** No erosion code anywhere. But `MapChunk::blurTerrain` (`MapChunk.cpp:1110-1194`) is
already a neighbourhood-averaging filter that samples through `World::GetVertex`, so it reads across
chunk *and* tile boundaries, and already does inverse-distance weighting and `glm::mix`. The new
code is one kernel; the sampling, blending, undo and save are all reusable verbatim.

**#5 Missing asset report.** `BlizzardArchive::ClientData::exists(FileKey)`
(`ClientData.hpp:166`, impl `ClientData.cpp:575-591`) already does exactly the non-throwing
existence check needed, tries the project directory before the archives, and is already used in
nine places.

**#2 Auto-texture.** The rule and sampling layers are genuinely new — no slope/angle machinery
exists. Everything below is built: `TextureSet` owns the alphamaps and the whole write path.

**#10 AO baking.** Do **not** build a new Tool subclass — `VertexPainterTool`/`ShaderTool` already
own `editing_mode::mccv`. One pure occlusion estimator (~120-200 lines) plus a small UI hook.

**#8 Waypoint editor.** The one real blocker is that paths are not carried into the scene: add a
`ScenePath { WaypointPath path; bool dirty; }` vector to `TileSpawnScene`
(`SpawnSceneCache.hpp:121`). `SpawnQuery::loadPath` and `ChangesetBuilder::addWaypointPath` are
already written and tested.

## Registering a new Tool, if one is needed

The `_tools` vector index must equal the `editing_mode` value (`MapView.cpp:3843`), so a new tool
**must be appended last**: add the enumerator after `area_trigger = 14`
(`tool_enums.hpp:79`), append the `_tools.emplace_back(...)` after `AreaTriggerTool`
(`MapView.cpp:3304`), pick an icon from `FontNoggit.hpp:74`, and call `registerTool` from
`setupUi`. The toolbar button then appears for free. `VertexPainterTool.cpp` is a 164-line minimal
example.
