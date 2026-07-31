# Screenshots

**There are none yet.** This directory is a placeholder so that the README can link somewhere honest
rather than reference an image that does not exist.

Do not add a screenshot until it shows the real thing running. A mock-up, a cropped upstream
screenshot, or a render from a different branch is worse than nothing here — the README's whole
claim to trust is that it does not oversell.

## What is worth capturing, in priority order

1. **Database spawns drawn over terrain.** The one image that shows what this fork is for: creature
   and gameobject spawns from a TrinityCore world database standing on the ADT they belong to.
   Capture it with the **Database Spawns** dock open on the right so the list and the viewport are
   visible together.
2. **The spawn tile picker** (`Ui::SpawnTilePicker`), showing the three-state loaded markers. This
   is the one piece of UI whose behaviour is not obvious from a description: a picked tile can be
   cached and editable without being drawn, because Noggit only streams roughly 5x5 around the
   camera.
3. **An emitted changeset**, side by side with the pending-changes list that produced it. This is
   the safety story made concrete — the output is a file you read, not a write that already happened.
4. **Ground Effect Sets** editor (Tools menu), with a set open and its doodads resolved.
5. **Alpha Map Integrity** report on a tile with real findings.
6. **Before/after** for the erosion brush, automatic texturing, and the ambient occlusion bake.
   These three are only meaningful as pairs.

## Rules for anything committed here

- **No client assets.** A screenshot of the editor showing terrain and models is a screenshot of a
  running program, not a redistribution of game data, but do not commit extracted textures, model
  previews, or DBC dumps as image files.
- **No infrastructure detail visible in the frame.** Check the window title, the status bar, the
  settings panel and any file path in the shot for schema names, hostnames, usernames, or absolute
  paths on your machine. Crop or blank them. The whole point of keeping `db-policy.json` and
  `docs/environment.md` gitignored is defeated by a screenshot of the connection dialog.
- **PNG**, reasonable dimensions (1600px wide is plenty), and name files for what they show, e.g.
  `spawn-overlay.png`, not `screenshot1.png`.
- Link them from `README.md` only once they exist, and update the Status table there — it currently
  states plainly that there are no screenshots.
