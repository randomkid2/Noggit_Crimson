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
