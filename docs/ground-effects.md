# Ground effects

What ground effects are, what Noggit already did, and what this fork added.

## How they work in 3.3.5

Four pieces, and only the last one was missing from Noggit:

| Piece | Where it lives | Meaning |
|---|---|---|
| Effect id per texture layer | `MCLY.effectID` in the ADT | Which `GroundEffectTexture.dbc` row this layer uses |
| Layer map | `MCNK.doodadMapping[8]` — 128 bits | Which of the 4 layers' effect shows in each of the 8×8 cells |
| Exclusion stencil | `MCNK.doodadStencil[8]` — 64 bits | Cells where effects are suppressed entirely |
| The set itself | `GroundEffectTexture.dbc` → `GroundEffectDoodad.dbc` | Up to 4 doodad `.m2` files with weights, plus density and terrain type |

## What Noggit already did

More than the third-party tools suggest, and one part better:

- Both MCNK maps are modelled and written on save — [MapChunk.cpp:1517](../src/noggit/MapChunk.cpp)
- [`TextureSet::updateDoodadMapping()`](../src/noggit/texture_set.cpp) **derives the layer map from the
  alpha maps automatically**. Standalone editors make you paint that 8×8 grid by hand per chunk;
  Noggit computes it from the texture you already painted, which is the whole tedious part gone.
- The exclusion stencil is paintable with full undo — `texture_set.cpp`, `Action.cpp`
- `GroundEffectsTool` (inside the Texturing tool) shows effect sets, weights, density, terrain
  type, doodad previews, and placement/exclusion overlays

## What was missing, and is now added

**You could not create a ground effect set.** `GroundEffectsTool` only ever calls `getByID` and
`CheckIfIdExists` — it never writes a DBC. Making a new effect meant leaving Noggit for a
standalone DBC editor and coming back.

**Tools ▸ Ground Effect Sets** closes that:

- Lists every set in `GroundEffectTexture.dbc`, resolved to doodad filenames
- **New** / **Duplicate**, then fill in up to four `.m2` paths with weights, density and terrain type
- **Save DBCs to project** writes both `GroundEffectTexture.dbc` and `GroundEffectDoodad.dbc` into
  the project's own `DBFilesClient/` via `DBCFile::save()`. **The client installation is never
  touched** — the result is a patch, and the stock client remains the DBC baseline that creature
  display-id resolution depends on.
- **Apply** assigns the selected set to every layer using the texture currently selected in the
  Texturing tool, across this tile or all loaded tiles

Notes on the behaviour, because they are decisions rather than accidents:

- New ids start at **50000**. Blizzard's rows sit well below that in 3.3.5, so a custom set is
  recognisable on sight and a later client patch cannot collide with one of yours.
- A doodad file already present in `GroundEffectDoodad.dbc` is **reused**, not appended again;
  otherwise every save would grow the file with duplicates of the same path.
- A weight of 0 disables a slot without clearing its path.
- Editing a set that already exists rewrites that row in place. Only genuinely new ids append.

## Verification status

The editor builds in both configurations, its menu entry and dialog were confirmed present in the
running editor, and the DBC write path is unit-tested. **Nobody has written a set, loaded the
resulting project patch in the game client, and confirmed the doodads appear.** Until someone
does, treat the in-game result as unproven — see the Status table in [`../README.md`](../README.md).

## Still not done

- **Export / import** (the `.MGE` equivalent) — reapplying a tile's assignments after repainting
  terrain, matched by texture. Not started.
- The older `GroundEffectsTool` is still bolted onto the Texturing tool. The new editor
  deliberately does not merge into it: that one is a per-chunk brush, this is a library editor.
