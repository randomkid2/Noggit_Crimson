# Noggit Crimson — TrinityCore 3.3.5a edition

[![Licence: GPL-3.0](https://img.shields.io/badge/licence-GPL--3.0-blue.svg)](COPYING)

**Noggit Crimson is an unofficial fork of
[[Marlamin/noggit-red]([https://github.com/Marlamin/noggit-red](https://gitlab.com/prophecy-rp/noggit-red))](https://gitlab.com/prophecy-rp/noggit-red) — "Noggit Red" — that adds
TrinityCore 3.3.5a world-database editing to the Noggit map editor. It is
[GPL-3.0](COPYING), inherited from upstream and unchangeable.**

Upstream Noggit Red edits the *client* side of a World of Warcraft map: terrain, textures, water,
M2 and WMO placements inside ADT files. Noggit Crimson keeps all of that untouched and adds the
*server* side next to it — the `creature` and `gameobject` spawns your TrinityCore world database
holds for the same tile — so you can see and move them in the same viewport as the terrain they
stand on.

| | |
|---|---|
| Product | **Noggit Crimson** — this fork, this repository |
| Upstream | **Noggit Red** — [Marlamin/noggit-red](https://github.com/Marlamin/noggit-red) |
| Forked at | `6f0776d4` (2025-01-17) |
| Licence | **GPL-3.0**, inherited and unchangeable — see [COPYING](COPYING) |
| Target | TrinityCore 3.3.5a only, client build `3.3.5a.12340` |
| Platform | Windows / MSVC is the tested configuration |

Everything upstream Noggit Red does, this still does. If you only want a map editor, use upstream —
it is the same program, better tested, and without the extra build dependency.

---

## Read this first: what it will and will not send to your database

Being precise here matters more than being reassuring, because a database you did not mean to
touch does not un-touch itself.

### What Noggit Crimson sends

- **No `CREATE DATABASE` statement exists anywhere in the source.** Noggit never creates a schema.
  If the one you configured does not exist, the connection fails and reports it.
- **Every world-data edit is emitted as a reviewable `.sql` changeset**, in TrinityCore's own TDB
  style: `SET @CGUID := …` variables at the top, then `DELETE` before `INSERT` for every table
  touched, with explicit column lists rather than positional values. The file is written next to
  your project with a timestamp. Changesets are idempotent by construction — every `INSERT` is
  either preceded by a `DELETE` for the same key or is an upsert, which the test suite asserts
  structurally over the emitted text (it has not been re-applied twice against a real server). See
  [`tools/dev-db/03_example_changeset.sql`](tools/dev-db/03_example_changeset.sql) for the shape.
- **Reading spawns is read-only, twice over.** Every query that loads `creature`, `gameobject` and
  their satellites runs on a `WorldDatabaseConnection` opened in `READ_ONLY` mode, which throws
  rather than executing a statement, *and* which asks the server for
  `SET SESSION TRANSACTION READ ONLY` on connect where the server honours it.

### The two paths that do write

Both go through `Noggit::Database::WorldDatabaseConnection` in `DEV_WRITE` mode. That class
refuses to construct — and then refuses again immediately before every individual statement —
unless the connection's schema is exactly the one you nominated as writable in
**Noggit ▸ Settings ▸ MySQL ▸ Dev schema** (`project/mysql/dev_schema`, default
`noggit_dev_world`).

1. **"Also apply to the dev schema"** — an unchecked checkbox beside Save in the spawn panel. Tick
   it and the changeset Noggit just wrote is also executed, against that one schema. The `.sql`
   file is written either way; this is a rehearsal convenience, not the delivery mechanism.
2. **The optional `USE_SQL` UID-storage feature** inherited from upstream, which keeps a `UIDs`
   table of the highest object UID per map. Its ``CREATE TABLE IF NOT EXISTS `UIDs` ``
   (`src/mysql/mysql.cpp`) is the **only** DDL statement left in the codebase, and it, its
   `INSERT` and its `UPDATE` sit behind the same guard. Upstream ran that `CREATE TABLE` — and a
   `CREATE DATABASE IF NOT EXISTS` alongside it — against whatever server the settings named, on
   every single connection. See [Fixes to inherited code](#fixes-to-inherited-code).

### What that is *not*

This is an application-level guarantee about the statements Noggit itself sends. It is not a
substitute for database grants, and it is worth reading the limits before relying on it:

- **It does not stop you nominating a live schema as the writable one.** Nothing checks that the
  name you typed is disposable.
- **It does not stop you applying an emitted changeset to a live server by hand** — that is the
  intended way to use them.
- **So grant, don't trust.** Give the account you configure `SELECT` on whatever you read spawns
  from, and write access to the dev schema *only*. That layer does not depend on our code being
  correct, which is why it is the one that matters.
- **The guarantee is established by code inspection, a whole-tree DDL grep and a clean compile —
  not by observing a real server refuse a connection.** No dev database was reachable when the
  fix was made, so the refusal path has never been watched executing.
- **One weakness in the wiring rather than in the guard.** At the "apply to dev schema" call site
  (`src/noggit/MapView.cpp`), the writable-schema argument is handed the same value as the
  connection's schema, so that particular comparison is trivially true. The protection there rests
  on the line above it, which sets the schema from the configured writable one, rather than on the
  guard re-deriving it independently. The outcome is correct; the check is weaker than it looks.

The layered setup is described in
[`docs/setup.md`](docs/setup.md#how-the-write-protection-is-layered): grants first, then
[`tools/dev-db/db-policy.example.json`](tools/dev-db/db-policy.example.json), which names the one
writable schema and lists the schemas that are off limits. Copy it to `db-policy.json` and fill in
your own names — that file is gitignored, because a list of your live databases is not something to
publish. Nothing in this repository names anyone's databases; the committed examples use
placeholder names.

---

## What this fork adds

Read [`docs/milestones.md`](docs/milestones.md) and the change table in
[`ATTRIBUTION.md`](ATTRIBUTION.md) for the full record, including the defects found and fixed along
the way. The headline additions:

### Database spawn editing

- **Render TrinityCore spawns on the ADT view.** `creature` and `gameobject` rows for a tile are
  read, resolved `displayId` → model through `CreatureDisplayInfo` → `CreatureModelData` (creatures)
  or `GameObjectDisplayInfo` (gameobjects), and drawn at their server coordinates. Gameobject
  orientation uses the `rotation0..3` quaternion, not Euler angles.
- **Select, move and rotate** spawns with an axis-drag gizmo, or set orientation numerically in the
  spawn panel. Rotating a gameobject rewrites `rotation0..3` as well as `orientation`, because the
  core reads the latter and the client renders the former — writing one without the other gives you
  a spawn that faces differently in game than it did in the editor. (The gizmo compiles and its
  code path is reachable, but nobody has confirmed the handles draw on screen — see
  [Status](#status--please-read-before-you-rely-on-this).)
- **Multi-tile loading.** A minimap tile picker (built on Noggit's own minimap widget) lets you pick
  a block of tiles; loading counts the spawns first and asks before pulling in a large set.
- **A pending-changes panel** — load, list, select, move, save or discard. Nothing is committed
  implicitly, and *saving* means writing a `.sql` file. An unchecked "Also apply to the dev schema"
  box beside Save can additionally execute that file against your nominated dev schema, and only
  that schema.
- **Database spawns never enter the ADT save path.** They are appended to the renderer's draw list
  directly and are deliberately kept out of `MapTile::object_instances`, so a server-side spawn can
  never be written into a client-side MDDF/MODF chunk. That is structural, not a rule someone has to
  remember.
- **Capability-based schema handling.** Column names and positions are read from
  `information_schema` at runtime rather than hardcoded, and version detection probes both
  `version_db_world` and `version`. [`docs/schema-335.md`](docs/schema-335.md) records the measured
  layout and the specific places published references disagree with a real database.
- **Export bookmarks as `game_tele` rows** (Assist ▸ Export bookmarks as game_tele SQL), and produce
  `.go` / `.tele` command strings from a selected spawn.

### Map-making tools (no database required)

These work in a plain `-DUSE_SQL=OFF` build:

- **Ground effect set editor** (Tools ▸ Ground Effect Sets) — Noggit could already *assign* an
  existing ground effect set and derive the per-cell layer map from your alpha maps; it could not
  *create* one. This writes `GroundEffectTexture.dbc` and `GroundEffectDoodad.dbc` into the
  **project's** `DBFilesClient/` as a patch. The client installation is never touched. See
  [`docs/ground-effects.md`](docs/ground-effects.md).
- **Automatic texturing by slope and height** (Tools ▸ Automatic Texturing) — rule-driven, with a
  deterministic total order over rules rather than first-match, so the result does not depend on the
  order you happened to add them in.
- **Thermal erosion brush** — angle-of-repose relaxation, which is mass-conserving and has a fixed
  point, so a stroke settles rather than running away.
- **Ambient occlusion baking into vertex colours** (Tools ▸ Bake Ambient Occlusion) — horizon-sampled
  AO written into MCCV.
- **Alpha map integrity report** (Tools ▸ Alpha Map Integrity) — finds and repairs alpha-map states
  an ADT can legally store but no renderer can display: layer weights that do not sum to full
  coverage, layers referencing textures with no weight anywhere, and duplicate layers.
- **Missing asset report** and a **UID collision report** (Assist menu). Noggit already
  auto-repaired UID collisions in memory on every load, but compressed the result to a single
  `bool`; this records and reports what was renumbered.

### Client patch building

**Client ▸ Patch Client** writes your project folder into an MPQ patch the game client will load.
Two things about it are worth stating plainly, because both are load-bearing:

- **It packs referenced assets, not just terrain.** A byte-level walk over the project's ADTs
  follows the three links whose absence produces a model that loads in the editor and fails in
  game: WMO **group files**, M2 **`.skin`** files for every `nViews`, and the MOSB **skybox**.
  Base-client assets are excluded by default and gated behind a checkbox.
- **It requires [`patches/0001`](patches/0001-blizzard-archive-mpq-backslash-names.patch) to be
  applied.** That patch lives in a submodule this project cannot push to. Without it the archives
  written here are **silently ignored by the client** — valid header, correct payload, correct
  name, and MPQ viewers still list the contents, because they read the `(listfile)` rather than
  hashing the stored name. Applying it is a required build step, not an optional one. See
  [Quick start](#quick-start).

The archive produced this way has been confirmed working in game against a 3.3.5a client. The
report the packer prints distinguishes a file it could not parse at all from one it parsed
partially, and names the chunk and byte offset where it stopped.

### Fixes to inherited code

Several upstream defects were fixed as a by-product, and they matter whether or not you use the
database half:

- **`src/mysql/mysql.cpp` issued DDL against an arbitrary server.** Upstream's `connect()` ran
  `CREATE DATABASE IF NOT EXISTS <project/mysql/db>` followed by a `CREATE TABLE IF NOT EXISTS` for
  the `UIDs` table on *every call*, against whatever host the settings pointed at, before doing
  anything else. `CREATE DATABASE` is deleted outright; the surviving `CREATE TABLE` is reachable only
  through the `DEV_WRITE` guard described above. The five public signatures are unchanged, so no
  caller needed editing.
- **`testConnection()` used to be the write path too.** It is now `READ_ONLY`, and separately
  reports whether UID storage is permitted — so you can see a connection succeed while storage is
  refused, rather than having a connectivity-test button be the thing that writes to a server.
- `USE_SQL=ON` could not build at all (`#include <driver.h>` against an include path that resolves to
  the parent of `cppconn/`, plus `QMessageBox` and `std::stringstream` used without being included).
  Fixed by routing everything through `WorldDatabaseConnection`, whose header forward-declares
  `sql::Connection` — the connector headers are gone from `mysql.cpp` entirely rather than merely
  re-pointed.
- **Two unredistributable fonts were committed** — `resources/font_awesome.otf` (Font Awesome 5
  **Pro**, whose licence forbids exactly this) and Microsoft's `segoeui.ttf` / `segoeuisb.ttf`.
  Removed, and both are now resolved at runtime instead. See [Fonts](#fonts-are-not-bundled).
- `FontAwesome.cpp` threw `std::runtime_error` from inside `QIconEngine::paint` when its font was
  missing — an exception raised inside Qt's paint loop. It now falls back rather than throwing.
- `gGameObjectDisplayInfoDB` was declared but never defined — the first line of code to touch it was
  an unresolved external.
- Scripted object placement (`add_m2`, `add_wmo`, `model::remove`) skipped undo registration, so the
  Lua scatter brush painted models that Ctrl+Z could not touch.
- Vertex-colour undo restored the colour array but not the MCCV flags, so undoing a paint on a chunk
  that previously had no MCCV block still caused an MCCV block to be written on save.
- `ActionManager::endAction` could leave a dead action open if `finish()` threw, after which undo
  stopped working for the rest of the session.
- On a fresh install, four MySQL settings defaulted to `"127.0.0.1"`, putting a hostname into the
  user, password and database fields and making the port parse as 0.

The full list, with the reasoning for each, is the change table in [`ATTRIBUTION.md`](ATTRIBUTION.md).

---

## Status — please read before you rely on this

**This is work in progress, and some of it has never been used by a human.** Being specific about
which parts:

| | |
|---|---|
| **Confirmed in the game client** | **Client ▸ Patch Client**, with `patches/0001` applied. An ADT naming `Farm.wmo` produced an archive containing `world\wmo\azeroth\buildings\human_farm\farm.wmo` alongside `farm_000.wmo` and `farm_001.wmo`, with zero forward-slash entries, and `SFileHasFile` — which hashes the name, as the client does — found all three. Confirmed loading in game against a 3.3.5a client. |
| **Confirmed in the running editor** | Spawn overlay rendering. A run against a seeded dev schema loaded 5 spawns across 30 tiles with zero resolution failures, and confirmed all three DBC chains open against a stock client. Menu entries and tool registration were verified by driving the running process, not inferred from the fact that they compile. |
| **Built and tested, not visually confirmed** | Creature skin textures. Creature M2s carry no skin of their own — the image comes from `CreatureDisplayInfo.TextureVariation` per display id — and Noggit's loader replaced every such texture with black. The fix builds clean and breaks no test, but nobody has opened a populated tile and looked. If creatures render black, that is where to start. |
| **Compiles, path is reachable, never seen working** | The **spawn axis-drag gizmo**. `MapView::handleSpawnGizmo` is wired to the same preconditions as the shift-click pick and hands off to the ImGuizmo-backed transform gizmo, so nothing about the code path is speculative — but no one has confirmed the handles actually render on screen, let alone dragged one. Treat "move a spawn by dragging it" as unproven. Setting the coordinates numerically in the spawn panel is the path that has been exercised. |
| **Logic complete and unit-tested, no UI yet** | The waypoint editor (`waypoint_data` model and emission — no visual editor), the terrain half of the chunk mover, and the environment "Doctor" (`DoctorReport` renders, but there is no dialog over it). |
| **Not finished** | The M1 definition of done also requires demonstrating the read path working through a `SELECT`-only account, and a screenshot. Neither has been done. |
| **Never run against live MySQL** | The `DEV_WRITE` refusal path. It is verified by reading the code and by a clean compile; it has not been watched refusing a real connection. |
| **Menu-verified, output not signed off** | The map-making tools — ground effect set editor, automatic texturing, thermal erosion, AO baking, alpha-map integrity. They build clean in both configurations, their pure logic is unit-tested, their menu entries and tool registration were verified in the running editor, and each was put through an adversarial review that found and fixed real defects. What is **not** recorded anywhere is somebody opening a map, running each one, and confirming the *result* looks right. Treat the output as unproven. |
| **Screenshots** | None yet. See [`docs/screenshots/`](docs/screenshots/). |

The test suite is a standalone Catch2 target that links neither Qt nor Noggit and needs no running
database, plus a handful of live-database integration cases that skip cleanly without credentials.
The current suite is **422 test cases**. **Without a dev database you will see
`test cases: 422 | 417 passed | 5 skipped` — that is the correct result on this tree, not a
regression**, and the same run reports 276,964 assertions, all passing. That is the measured
figure, and `ctest -LE needs-database` reports `100% tests passed, 0 tests failed out of 33`
alongside it. With a dev database configured, the five gated cases are expected to run and pass as
well; nobody has a reachable one to confirm it with right now. A machine with no Connector/C++ at
all compiles the six cases in `tests/DatabaseIntegrationTests.cpp` out of the binary entirely and
should report 416 — that figure is arithmetic from the measured 422, not a separate measurement.
See [`docs/setup.md`](docs/setup.md#expected-results-without-a-database) for why each mechanism
exists.

```bash
cmake -S tests -B build-tests -G "Visual Studio 17 2022" -A x64
cmake --build build-tests --config Release
build-tests/Release/noggit_schema_tests.exe
```

There is **no CI**. The `.gitlab-ci.yml` in the root is upstream's GitLab template and does not run
here. Nothing in this repository is verified by an automated build on push; the evidence is what is
pasted into `docs/milestones.md`.

Not planned, deliberately: `TaxiPath.dbc` / `TaxiPathNode.dbc` editing, custom WMO embedded-doodad
(`MODD`/`MODN`) editing, AzerothCore support (the capability layer exists to make it cheap to add
later, but only TrinityCore 3.3.5 is implemented), and any write path to a schema other than the one
you nominate as writable. There will never be a "write directly to the server" button.

Client-side MPQ patch building **was** on that list and is no longer — Client ▸ Patch Client now
writes the project folder, and the assets its terrain references, into an MPQ patch. See
[Client patch building](#client-patch-building) below.

---

## No game data is included

No MPQ or CASC archives, no DBC or DB2 files, no models, textures, maps, sounds, or extracted
assets. **You supply your own legally obtained 3.3.5a client.** The editor reads DBCs from an
unmodified client installation you point it at; use a stock client rather than one carrying custom
patches, or the DBC baseline is wrong.

Two things in the tree are *derived from* the game and should be named rather than glossed over,
because "no game data, not ever" would be a claim you could falsify in thirty seconds:

- **`dist/noggit-definitions/`** — four upstream-supplied CSV mapping tables, inherited unchanged
  from Noggit Red and copied next to the executable at build time.
  `AreatriggerDescriptions.csv` (1,219 rows) labels area-trigger IDs with Blizzard zone and trigger
  names; `light_dbc_names.csv` (378 rows) names `Light.dbc` entries and, per its own README, was
  extracted from the legacy `.lit` files that shipped until the TBC beta; `ZoneLight.*.csv` and
  `ZoneLightPoint.*.csv` carry zone-light values the 3.3.5 client hardcodes. These are factual
  identifiers and labels — without them the editor shows you raw integers — not art, geometry,
  audio or anything the client renders. They are Blizzard's naming all the same, and they are here
  because upstream put them here.
- **`tools/dev-db/02_seed_synthetic.sql`** uses real creature and gameobject *display IDs*, because
  those are integer keys into the client's DBC files and have to be real for `displayId` → model
  resolution to be testable at all. Every accompanying value — names, radii, coordinates, entries,
  GUIDs — is invented, and all IDs sit far above the ranges TDB uses.

Nothing else derived from the client is here, and nothing else will be added.

---

## Fonts are not bundled

Upstream committed `resources/font_awesome.otf` — that is Font Awesome 5 **Pro**, whose licence
specifically forbids putting the file in a public repository — along with Microsoft's `segoeui.ttf`
and `segoeuisb.ttf`. All three are gone from this fork's working tree, and the Qt resource entries
that named them are gone with them.

**The consequence is visible the moment you run it.** Out of the box, every icon button falls back
to either a Qt standard icon (where one honestly fits the meaning — save, close, play, delete) or a
short text label in a rounded outline box. The interface is completely usable; it is just plain.

To restore the icons, do **one** of:

- install a Font Awesome desktop font system-wide — any family whose name begins "Font Awesome" is
  picked up, Solid face preferred; or
- drop a font file into a `fonts` folder next to `noggit.exe`. `fa-solid-900.ttf` from the free
  **Font Awesome Free for Desktop** download (SIL OFL 1.1, redistributable, no cost) is the easy
  option; your own licensed `Font Awesome 5 Pro-Regular-400.otf` also works. The full list of file
  names checked is in the comment block at the top of `src/noggit/ui/FontAwesome.cpp`.

Note that some icons in the enum come from the paid **Pro** set and have no Free equivalent — those
stay blank with Free installed. Only a licensed Pro file restores the complete set.

**Segoe UI** is likewise no longer bundled. The interface font is now resolved by *family name*
from `QFontDatabase` — Segoe UI first, then Noto Sans, DejaVu Sans, Liberation Sans, Arial, then
whatever Qt reports as the system font — so a machine without it simply gets another sans-serif.

> **If you fork this further:** deleting the files clears the working tree, not the history. The
> blobs are still reachable from the inherited upstream commits, so a redistribution has to rewrite
> history or start from a squashed root commit to be clean.

---

## Requirements

Full detail, including the reason each one exists, is in [`docs/setup.md`](docs/setup.md). The short
version:

| Component | Requirement | Notes |
|---|---|---|
| MSVC | VS 2019 or 2022, x64 | |
| CMake | 3.5+ | CMake **4.x** needs a policy flag — see below. |
| Qt | **5.15.2** `msvc2019_64` recommended | Hard floor is **5.10**. Install one Qt version and one compiler only. |
| Submodules | initialised | `cmake/` is itself a submodule, so nothing configures without this. |
| MySQL Server | 8.0 / 8.4 | Only for `-DUSE_SQL=ON`; supplies `libmysql.lib`. |
| MySQL Connector/C++ | legacy JDBC (`cppconn`) API | Only for `-DUSE_SQL=ON`. Ships under `include/jdbc/`. |
| Font Awesome | *optional*, runtime only | Not bundled. Without it the icons are text labels — see [Fonts](#fonts-are-not-bundled). |
| A 3.3.5a client | yours, legally obtained | Runtime only. No game data ships here. |

Qt 5.9 and earlier **cannot** build this tree regardless of what older Noggit binaries suggest:
`src/external/NodeEditor` requires Qt 5.10 and `framelesshelper` links `Qt::GuiPrivate`. The failure
appears at *generate*, not configure, so a successful "Configuring done" has told you nothing about
Qt yet.

The Qt online installer wants an account; `aqtinstall` fetches the same packages from the same
mirrors and does not. [`docs/setup.md`](docs/setup.md#installing-qt-without-a-qt-account) has the
exact command.

---

## Quick start

**1. Clone with submodules.** Six of them, and `cmake/` is itself one, so nothing configures at all
without this step:

```bash
git clone --recursive <this-repo>
cd <this-repo>
git submodule update --init --recursive
```

**2. Apply `patches/0001` — this is a required build step, not an optional one.**

```bash
cd src/external/blizzard-archive-library
git am ../../../patches/0001-blizzard-archive-mpq-backslash-names.patch
cd ../../..
```

`src/external/blizzard-archive-library` is a submodule pointing at
`gitlab.com/T1ti/blizzard-archive-library`, which this project cannot push to, so the fix cannot be
delivered through the submodule pointer — the parent must record a commit that exists on that
remote, or `git submodule update` fails for everyone who clones. The patch is therefore carried
here and applied by hand. **Without it, Client ▸ Patch Client writes archives the game client
silently ignores**, because MPQ resolves a file by hashing its stored name and the writer stored
forward slashes where the client asks with backslashes. Nothing reports an error; the patch simply
does nothing in game.

Applying it leaves that submodule showing as modified in `git status`. **That is expected and
correct** — do not "fix" it by resetting the submodule.

**3. Configure.** **Quote the policy flag** — unquoted, some shells strip the `.5`, the cache ends up
holding `3`, and every subproject then fails with a message that blames the subproject rather than
the flag:

```bash
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 \
  "-DCMAKE_POLICY_VERSION_MINIMUM=3.5" \
  "-DCMAKE_PREFIX_PATH=<QtDir>/5.15.2/msvc2019_64"
```

Add these for the database build (or drop the `-D` paths entirely and use the sibling
`../Noggit3libs/mysql` layout described in `docs/setup.md`, which CMake already searches):

```bash
-DUSE_SQL=ON
-DMYSQL_LIBRARY="<MySQL Server>/lib/libmysql.lib"
-DMYSQLCPPCONN_INCLUDE="<Connector>/include/jdbc"
-DMYSQLCPPCONN_LIBRARY="<Connector>/lib64/vs14/mysqlcppconn.lib"
```

**4. Build.** Build the `noggit` target (or `ALL_BUILD`) — **never `INSTALL`**, which is broken
upstream, see the note below:

```bash
cmake --build build --config RelWithDebInfo --target noggit
```

The executable lands in **`build/bin/RelWithDebInfo/`**, and you run it from there.

> **Do not run `--target INSTALL`.** It fails on a fresh clone, and this is inherited, not something
> the fork broke. `cmake/win32_pack.cmake` (reached via `includePlatform("pack")`) installs from a
> top-level `bin/` directory — `bin/shaders`, `bin/fonts`, `bin/noggit_template.conf`,
> `bin/freetype6.dll`, `bin/StormLib.dll`, `bin/glew32.dll`, `bin/zlib1.dll` — that the repository
> does not contain and the build does not generate. `git ls-files bin/` prints nothing, and the
> target stops with `CMake Error … file INSTALL cannot find "…/bin/shaders"`. Those entries are
> leftovers from a much older build system: `freetype6`, `glew32` and `zlib1` are not linked by the
> current tree at all, and the shaders are compiled into the Qt resource bundle
> (`resources/resources.qrc`, prefix `/shader`). There is no `CMAKE_INSTALL_PREFIX` worth setting.
> To distribute a build, zip `build/bin/<config>/`.

**One thing you must copy by hand:** the listfile. CMake's copy step for it is commented out, and
the application looks for `listfile.csv` in its working directory.

```bash
cp dist/listfile/listfile.csv build/bin/RelWithDebInfo/
```

Optionally, also drop a Font Awesome file into `build/bin/RelWithDebInfo/fonts/` — without one the
icons fall back to text labels. See [Fonts are not bundled](#fonts-are-not-bundled).

Everything else is already there. `windeployqt` runs **automatically** as a POST_BUILD step
(`cmake/windeployqt.cmake`), so the Qt DLLs *and* the platform plugin — without which the binary
exits immediately with no useful message — are placed for you; `definitions/`, `noggit-definitions/`
and `themes/` are copied from the submodules by POST_BUILD steps too. Only run `windeployqt`
yourself if you moved the executable somewhere else or your generator skipped the step:

```bash
<QtDir>/5.15.2/msvc2019_64/bin/windeployqt.exe --release --no-translations \
  build/bin/RelWithDebInfo/noggit.exe
```

For a `USE_SQL` build, also copy `mysqlcppconn-<n>-vs14.dll` (which lives one level *above* the
`vs14/` directory holding the import library) and `libmysql.dll` next to the executable —
`windeployqt` cannot know about those.

A healthy start writes `log.txt` beside the executable, naming the OpenGL version and renderer and
whether the listfile and DBC definitions were found. If no window appears, read that file first.

Two widely repeated instructions are **wrong** for this configuration, checked with
`dumpbin /DEPENDENTS`: there is no `lua51.dll` to copy (sol2 links Lua 5.4 statically), and
`Qt5OpenGL.dll` is never imported at runtime (NodeEditor needs the module at configure time only).

### Enabling the database features

`-DUSE_SQL=OFF` is the default and is fully supported — everything in "Map-making tools" above works
without a database, and without MySQL Connector/C++ installed. If you built with `-DUSE_SQL=ON`,
enable the feature and set the connection details in **Noggit ▸ Settings ▸ MySQL**, including the
**Dev schema** field, which names the only schema this application will open read/write. The **Test
Connection** button reports the server version, whether the connection works, and — separately —
whether UID storage is permitted, so you can see a connection succeed while writes are refused.

Setting up a disposable dev schema of your own is covered in
[`docs/setup.md`](docs/setup.md#dev-database). In outline: copy the two `.example` config files and
edit them, run `tools/dev-db/01_bootstrap_root.sql` (after replacing its two `__CHANGE_ME__`
placeholders with passwords you choose) to create the schema and a read-only and a read-write
account, then seed it. The seeder copies table *structure* from your live schema and adds synthetic
fixtures — **no live data is ever copied**. Structure is copied rather than hand-written on purpose:
a hand-written schema only proves the documentation agrees with itself, so real column drift would
stay invisible.

---

## Documentation

| File | Contents |
|---|---|
| [`docs/setup.md`](docs/setup.md) | Build prerequisites, the gotchas in the order they bite, dev-database setup |
| [`docs/design-notes.md`](docs/design-notes.md) | Why the fork is shaped the way it is — the decisions that are hard to infer from the code |
| [`docs/schema-335.md`](docs/schema-335.md) | TrinityCore 3.3.5a world schema measured from `information_schema`, and eleven places published references are wrong |
| [`patches/README.md`](patches/README.md) | The required `0001` submodule patch, why it cannot ship as a submodule commit, and how to verify the pointer |
| [`docs/milestones.md`](docs/milestones.md) | **Historical planning record.** The milestone briefs and the evidence pasted as each landed. Superseded in places — the README Status table is current. |
| [`docs/ground-effects.md`](docs/ground-effects.md) | How ground effects work in 3.3.5 and what the editor adds |
| [`docs/feature-recon.md`](docs/feature-recon.md) | **Point-in-time survey** of what existed in the tree before the map-making work. Most of it has since been built. |
| [`docs/CONTRIBUTING.md`](docs/CONTRIBUTING.md) | Coding rules, the licence header every new file needs, and what evidence a change is expected to carry |
| [`ATTRIBUTION.md`](ATTRIBUTION.md) | Licensing, upstream credit, third-party components, pre-release checks |

---

## Licence

**GPL-3.0**, the full text of which is in [COPYING](COPYING).

Noggit Red is GPL-3.0. This fork is a derivative work, so it must be distributed under GPL-3.0 with
corresponding source. There is **no** option to relicense, close, or dual-licence it, and that
applies to every new file added here. Every source file added by this fork carries the project's
header:

```cpp
// This file is part of Noggit3, licensed under GNU General Public License (version 3).
```

**Every upstream file this fork modifies is recorded**, with the date and the reason, in the
GPL-3.0 §5(a) statement-of-modification table in [`ATTRIBUTION.md`](ATTRIBUTION.md). Upstream's own
README text is retained verbatim below, under [Upstream documentation](#upstream-documentation),
rather than replaced — where this fork has measured something different, a note says so and the
original is left standing. Bundled third-party code under `src/external/` carries its own terms,
listed in the same file.

[`docs/schema-335.md`](docs/schema-335.md) documents the TrinityCore 3.3.5a world schema **as
measured from a live `information_schema`** on a TDB 335.25101 `world` database (MySQL 8.4),
cross-checked against a TrinityCore 3.3.5a source checkout: column names, ordinal positions, types
and defaults, plus a table of eleven places where the published references this project started from
are wrong or stale. It deliberately records no server, host, credential, core-revision or
database-inventory detail, and describes only stock TrinityCore tables. The measured database is not
a pristine TDB install, so **re-measure your own target with `/schema-check` before relying on any
of it.**

**No TrinityCore source code is copied into this project** — TrinityCore is GPL-2.0, which is not
compatible with GPL-3.0, so the distinction is deliberate. Schema *facts* are not code.

## Credits

**Noggit Red is [Marlamin's](https://github.com/Marlamin/noggit-red), not ours.** This fork exists
because that project is good enough to build on, and every part of the editor you will actually spend
your time in — terrain, texturing, water, object placement, the node editor, the asset browser, the
scripting layer — is theirs.

Noggit predates that repository too: the source headers still say **Noggit3**, and the lineage runs
back through earlier Noggit and Noggit SDL releases by authors who appear in no git history here.
Credit the project, not the commit log.

`noggit-red` spans 1441 commits from 2020-10-09 to 2025-01-17. Principal contributors by commit
count:

> T1ti · Skarn · sshumakov3 · Alister · Martin Benjamins · Balkron · p620 · EIntemporel · Kaev ·
> ihm-tswow · Intemporel · BalkronPainter · Felfired · Havric · DennisWG · Varen · BinarySpace

Also: StormLib and CascLib by Ladislav Zezula, and the libraries vendored under `src/external/` —
see [`ATTRIBUTION.md`](ATTRIBUTION.md) for the full list with licences.

Some of the workflow design here — tile-centric editing, staged pending changes, an explicit
build/commit step, an environment "Doctor" — is **conceptual inspiration** taken from AtlasForge
(RetroGooby), a closed-source editor for WoW 1.12.1. **No AtlasForge code is included, and none
could be**: it ships as a binary with no published source. Only observable UX ideas were
reimplemented independently, against a different game version and a fundamentally different data
layer — AtlasForge builds client-side MPQ patches, whereas this emits server-side database
changesets.

## Not affiliated with anyone

This project is **not affiliated with, endorsed by, or connected to Blizzard Entertainment,
the TrinityCore project, or AtlasForge/RetroGooby**. World of Warcraft and all related assets are
trademarks and copyright of Blizzard Entertainment. No game content is distributed here. This is an
independent, non-commercial fan tool.

---
---

# Upstream documentation

*The remainder of this file is Noggit Red's own README, preserved. Where this fork has measured
something different, a note says so — the original text is left in place.*

## LICENSE

This software is open source software licensed under GPL3, as found in the COPYING file.

## BUILDING

This project requires CMake to be built.

It also requires the following libraries:

* OpenGL
* StormLib (by Ladislav Zezula)
* CascLib (by Ladislav Zezula)
* Qt5
* Lua5.x

On Windows you only need to install Qt5 yourself, the rest of the dependencies are pulled through
FetchContent automatically. Supporting for Linux and Mac for this feature is coming in the future.
In case FetchContent is not available (e.g. no internet connection), the find scripts will look for
system installed libraries.

Further following libraries are required for MySQL GUID Storage builds:

* LibMySQL
* MySQLCPPConn

See below for detailed instructions.

> **Fork note:** Lua is fetched and linked *statically* by sol2 (`lua54.lib`), so there is no Lua DLL
> to deploy. See `docs/setup.md`.

### Windows

Text in `<brackets>` below are up to your choice but shall be replaced with the same choice every
time the same text is contained.

#### MSVC++

Any recent version of Microsoft Visual C++ should work. Be sure to remember which version you chose
as later on you will have to pick corresponding versions for other dependencies.

#### CMake

Any recent CMake 3.x version should work. Just take the latest.

> **Fork note:** CMake **4.x** additionally requires `"-DCMAKE_POLICY_VERSION_MINIMUM=3.5"`, quoted,
> on every configure — `nlohmann/json` is re-fetched by FetchContent and declares a minimum below
> 3.5, so patching it in place does not persist.

#### Qt5

Install Qt5 to `<Qt-install>`, downloading a pre-built package from
https://www.qt.io/download-open-source/#section-2.

Note that during installation you only need **one** version of Qt and also only **one** compiler
version. If download size is noticably large (more than a few hundred MB), you're probably
downloading way too much.

> **Fork note:** the floor is **Qt 5.10** (NodeEditor), and 5.15.2 `msvc2019_64` is what this fork is
> built and tested against. `aqtinstall` avoids needing a Qt account.

#### StormLib

This step is only required if pulling the dependency from FetchContent is not available. Download
StormLib from https://github.com/ladislav-zezula/StormLib (any recent version).

* open CMake GUI
* set `CMAKE_INSTALL_PREFIX` (path) to `<Stormlib-install>` (folder should not yet exist). No other
  things should need to be configured.
* open solution with visual studio
* build ALL_BUILD
* build INSTALL
* Repeat for both release and debug.

#### MySQL (Optional)

Optional, required for MySQL GUID Storage builds.
Download MySQL server https://dev.mysql.com/downloads/installer/
and MySQL C++ Connector https://dev.mysql.com/downloads/connector/cpp/

* open CMake GUI
* enable `USE_SQL`
* set `MYSQL_LIBRARY` (path) to `libmysql.lib` from your MYSQL server install,
  e.g. `"C:/Program Files/MySQL/MySQL Server 8.0/lib/libmysql.lib"`
* set `MYSQLCPPCONN_INCLUDE` (path) to the folder containing `cppconn/driver.h` from your MYSQL
  Connector C++ install, e.g. `"C:/Program Files/MySQL/Connector C++ 8.0/include/jdbc"`
* set `MYSQLCPPCONN_LIBRARY` (path) to `mysqlcppconn.lib` from your MYSQL Connector C++ install,
  e.g. `"C:/Program Files/MySQL/Connector C++ 8.0/lib64/vs14/mysqlcppconn.lib"`
* Don't forget to set your SQL settings and enable the feature in the noggit settings menu to use it.

> **Fork note:** in this fork `USE_SQL` also enables the TrinityCore world-database features, not
> only UID storage. `dev.mysql.com` returns 403 to scripted downloads — fetch the connector zip with
> a browser.

#### Noggit

* open CMake GUI
* set `CMAKE_PREFIX_PATH` (path) to `"<Qt-install>;<Stormlib-install>"`, e.g.
  `"C:/Qt/5.6/msvc2015;D:/StormLib/install"`
* set `BOOST_ROOT` (path) to `<boost-install>`, e.g. `"C:/local/boost_1_60_0"`
* (**unlikely to be required:**) move the libraries of Boost from where they are into
  `BOOST_ROOT/lib` so that CMake finds them automatically or set `BOOST_LIBRARYDIR` to where your
  lib are (.dll and .lib). Again, this is **highly** unlikely to be required.
* set `CMAKE_INSTALL_PREFIX` (path) to an empty destination, e.g.
  `"C:/Users/blurb/Documents/noggitinstall"`
* configure, generate
* open solution with visual studio
* build ALL_BUILD
* build INSTALL

> **Fork note:** the last two lines are stale. **Do not build `INSTALL`** — `cmake/win32_pack.cmake`
> installs from a top-level `bin/` directory that is not in the repository and is not generated, so
> the target fails with `file INSTALL cannot find "…/bin/shaders"`. `CMAKE_INSTALL_PREFIX` therefore
> does nothing useful either. Build `ALL_BUILD` and run the editor out of `build/bin/<config>/`,
> which is already complete apart from `listfile.csv`. See [Quick start](#quick-start).

To launch noggit you will need the following DLLs from Qt loadable. Install them in the system, or
copy them from `C:/Qt/X.X/msvcXXXX/bin` into the directory containing noggit.exe, i.e.
`CMAKE_INSTALL_PREFIX` configured.

* release: Qt5Core, Qt5OpenGL, Qt5Widgets, Qt5Gui
* debug: Qt5Cored, Qt5OpenGLd, Qt5Widgetsd, Qt5Guid

> **Fork note:** `windeployqt` is wired in as a POST_BUILD step in this tree, so you do not normally
> copy anything by hand — it resolves the Qt DLLs *and* the platform plugin, which the list above
> omits and without which the binary exits silently. Measured with `dumpbin /DEPENDENTS`, the actual
> runtime set is Qt5Core, Qt5Gui, Qt5Widgets, Qt5Network, Qt5Xml, Qt5Multimedia and Qt5Quick —
> `Qt5OpenGL.dll` is *not* imported.

### Linux

On **Ubuntu** you can install the building requirements using:

```bash
sudo apt install freeglut3-dev libboost-all-dev qt5-default libstorm-dev
```

Compile and build using:

```bash
mkdir build
cd build
cmake ..
make -j $(nproc)
```

Instead of `make -j $(nproc)` you may want to pick a bigger number than `$(nproc)`, e.g. the number
of `CPU cores * 1.5`.

If the build pass correctly without errors, you can go into build/bin/ and run noggit. Note that
`make install` will probably work but is not tested, and nobody has built distributable packages in
years.

> **Fork note:** the fork's additions are not tested on Linux. Nothing in them is Windows-specific by
> design, but the only configuration anyone has built here is MSVC x64.

## SUBMODULES

To check out the submodule commits this repository records — which is what you want after cloning,
after switching branches, or whenever `git status` reports a submodule as modified:

```bash
git submodule update --init --recursive
```

> **Fork note — do not use `--remote` here.** Upstream documented
> `git submodule update --recursive --remote`, and in this fork that command is destructive.
> `--remote` ignores the commit the repository records and takes each submodule's remote tip
> instead, which **silently discards the patches in `patches/`** — including `0001`, without which
> `Client ▸ Patch Client` writes MPQ archives the game client ignores without any error. The
> command exits 0 and prints reassuring "checked out" lines while doing it, so nothing tells you it
> happened. If you have already run it, reapply the patches as described in `patches/README.md`.

## CODING GUIDELINES

File naming rules:

`.hpp` - is used for header files (C++ language).

`.h` - is used **only** for header files or modules written in C language.

`.c` - is used **only** for implementation files or modules written in C language.

`.cpp` - is used for project implementation files.

`.inl` - is used for include files providing template instantiations.

`.ui` - is used for QT UI definitions (output of QtDesigner/QtCreator).

### Project structure

`/src/Noggit` - is the main directory hosting .cpp, .hpp, .inl, .ui files of the project.

Within this directory the subdirs should correspond to namespace names (case sensitive).

File names should use PascalCase (e.g. `FooBan.hpp`) and either correspond to the type defined in
the file, or represent sematics of the module.

`/src/External` - is the directory of hosting included libraries and subprojects. This is external or
modified external code, so no rules from Noggit project apply to its content.

`/src/Glsl` - is the directory to store .glsl shaders for the OpenGL renderer. It is not recommended,
but not strictly prohibited to inline shader code as strings to `.cpp` implementation files.

> **Fork note:** the actual tree is `src/noggit` and `src/external`, lowercase. When adding files to
> an existing directory, match the surrounding code rather than the aspirational rule above.

### Code style

Following is an example for file `src/Noggit/Ui/FooBan.hpp`.

```cpp
#ifndef INCLUDE_GUARD_BASED_ON_FILENAME
#define INCLUDE_GUARD_BASED_ON_FILENAME
// We do not use #pragma once in headers as it is technically not cross-platform.
// Use include guards instead. For example, CLion IDE creates them automatically on .hpp file creation.

// <> are prefered for includes.
// Local imports go here
#include <SomeLocalFile.hpp>

// Lib imports go here
#include <external/SomeLibCode.hpp>

// STL imports go here
#include <string>
#include <mutex>
#include <vector> // etc

// Forward declarations in headers are encouraged. That prevents type leaking into bigger scopes
// Also reduces compile time
namespace Parent::SomeOtherChild
{
  class ForwardDeclaredClass;
}

// Namespaces are defined as PascalCase names. Namespace concatenation for nested namespaces
// is adviced, but not strictly enforced.
namespace Parent::Child
{
  // types are name in PascalCase,
  class Test : public TestBase
  {
    public:
      Test();

      int x; // public fields like that are discourged, but occur here and there through the project.
      // Subject to refactoring.

      // methods are named in camelCase.
      // trivial getter methods are declared in the header file.
      int somePrivateMember() { return _some_private_member; } const;

      // trivial setters are declared in the header file. Preceded by "set" prefix.
      void setSomePrivateMember(int a) { _some_private_member = a; };

    // private members are snake lower case, separated by underscore, preceded by underscore to indicate they're private.
    private:
      int _some_private_member;
      ForwardDeclaredClass* _some_other_private_member_using_forward_decl;
      std::mutex _mutex;

    // static methods

    private:
      static void someStaticMethod();

  };
}

#endif
```

Following is an example for file `src/Noggit/Ui/FooBan.cpp`.

```cpp
// the header of this .cpp comes first
// <> are prefered for includes.
#include <Noggit/Ui/FooBan.hpp>

// same order of includes as in header.

using namespace Parent::Child;

Test::Test()
: TestBase("some_arg")
, _some_private_member(0)
, _some_other_private_member_using_forward_decl(new ForwardDeclaredClass()) // do not forget to import ForwardDeclaredClass in .cpp
{
// body of ctor
}

void Test::someStaticMethod()
{
// local variables are named in snake_case, no preceding underscore.
int local_var = 0;

// preceding underscore is used on variables that are used for RAII patterns, such as scoped stuff (e.g. a scoped mutex)
std::lock_guard<std::mutex> _lock (_mutex); // _lock is never accessed later, it just needs to live as long as the scope lives.
// So, it has an underscore prefix.

someFunc(local_var); // free floating functions use the same naming rules as methods
}
```

Additional examples:

```cpp
constexpr unsigned SOME_CONSTANT = 10; // constants are named in SCREAMING_CASE
#define SOME_MACRO // macro definitions are named in SCREAMING_CASE
```
