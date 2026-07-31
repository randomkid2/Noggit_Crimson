# Noggit Red — TrinityCore 3.3.5a database-backed editing

Fork of [Marlamin/noggit-red](https://github.com/Marlamin/noggit-red) (GPL-3.0, C++, Qt5,
OpenGL, CMake, MSVC). Upstream base: `6f0776d4` (2025-01-17). Work branch:
`feature/tc335-db-editing`.

**Goal:** render and edit TrinityCore 3.3.5a world-DB spawns on the ADT view — creature and
gameobject placement, a visual waypoint editor, a chunk mover, and an AtlasForge-style
tile-centric staged-edit workflow adapted to server-side data.

## HARD RULES

1. **Never write to a live database.** The only writable schema is the one named in
   `tools/dev-db/db-policy.json` (`writableSchema`, normally `noggit_dev_world`). Every schema
   in that file's `protectedSchemas` is read-only. Reads are fine; writes are not.
   Enforced by `.claude/hooks/guard-db.ps1` (PreToolUse, exits 2). The hook is the real
   guard — `settings.json` allow/deny lists are bypassable by compound commands.
   Policy lives in gitignored config, not in source, because this repo is intended for public
   release and a schema inventory is infrastructure detail. Read the config when you need the
   list; do not paste it into tracked files.
2. **All DB edits are emitted as a reviewable `.sql` changeset** (TDB style: `DELETE` then
   `INSERT`, `@GUID` variables), optionally applied to `noggit_dev_world`. There is no
   "write directly to the server" path, ever.
3. **Never hardcode a column name or column order.** Verify against `information_schema` at
   runtime. See `docs/schema-335.md` for measured ground truth and the specific places
   published references are wrong.
4. **Version detection must probe both `version_db_world` AND `version`.** The reference DB
   here has only `version` (`db_version = 'TDB 335.25101'`).
5. **TrinityCore 3.3.5a only** (client `3.3.5a.12340`). Keep a thin adapter seam so
   AzerothCore can be added later; implement TC-3.3.5 defaults only for now.
6. **Use the existing MySQL Connector/C++ (`cppconn`) path. Do NOT add Qt QMYSQL.**
7. **Show evidence, never assert success.** Paste the compiler output, the
   `information_schema` diff, the applied-SQL result. "It should work" is not a result.

## Existing MySQL seam — read before touching it

`src/mysql/mysql.{h,cpp}`, gated behind CMake `USE_SQL` → `-DUSE_MYSQL_UID_STORAGE`.
It is **not** a database layer. It is five free functions persisting one `UIDs`
(`_map_id`, `UID`) table. Known problems, all of which M0 must address:

- `connect()` runs `CREATE DATABASE IF NOT EXISTS` on **every call** — Noggit currently
  attempts DDL against whatever server its settings point at. This is rule 1 violated by
  upstream code.
- A fresh connection is opened per call. Unusable for per-tile spawn queries.
- Credentials sit in `QSettings` (Windows registry) in plaintext under `project/mysql/*`.
- `mysql.cpp` does `#include <driver.h>`, but CMake's `FIND_PATH(MYSQLCPPCONN_INCLUDE NAMES
  cppconn/driver.h)` and the README both resolve to the *parent* of `cppconn/`. These
  contradict; the commented-out `<cppconn/driver.h>` line is the correct one. The file also
  uses `QMessageBox` and `std::stringstream` without including them. Assume this path does
  not currently build.
- `mysql.h` uses `#pragma once`, against the repo's own guideline.

## Build

Full prerequisites, gotchas and dev-DB setup are in `docs/setup.md`. Four things bite in order:

1. **CMake 4.x needs `"-DCMAKE_POLICY_VERSION_MINIMUM=3.5"`, quoted.** Unquoted the `.5` is
   stripped and every subproject fails with a message blaming the subproject.
2. **Qt must be ≥ 5.10**, 5.15.2 recommended. `NodeEditor` requires 5.10; below that
   `Qt5::OpenGL` is never defined and the failure appears at *generate*, not configure.
3. **Submodules first** — `cmake/` is itself a submodule.
4. **Never build `INSTALL`, and do not set `CMAKE_INSTALL_PREFIX`.** `cmake/win32_pack.cmake`
   (pulled in by `includePlatform("pack")`, `CMakeLists.txt:503`) installs `bin/shaders`,
   `bin/fonts`, `bin/noggit_template.conf` and four DLLs from a top-level `bin/` that is neither
   tracked (`git ls-files bin/` is empty) nor generated. The target dies with
   `file INSTALL cannot find "…/bin/shaders"`. Three of those DLLs are not even linked any more.
   Run the editor out of `build/bin/<config>/`.

```bash
git submodule update --init --recursive
```

Configure (Qt path must match one compiler version only):

```bash
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 "-DCMAKE_POLICY_VERSION_MINIMUM=3.5" -DCMAKE_PREFIX_PATH="<Qt>/msvc2019_64"
```

Add for the DB build:

```bash
-DUSE_SQL=ON -DMYSQL_LIBRARY="<...>/libmysql.lib" -DMYSQLCPPCONN_INCLUDE="<...>/include/jdbc" -DMYSQLCPPCONN_LIBRARY="<...>/mysqlcppconn.lib"
```

Then `cmake --build . --config RelWithDebInfo --target ALL_BUILD`. The exe lands in
`build/bin/<config>/` and is run from there. There is no install step.

`windeployqt` already runs as a POST_BUILD step (`cmake/windeployqt.cmake`), so the Qt DLLs and the
platform plugin are placed automatically; `definitions/`, `noggit-definitions/` and `themes/` are
copied by POST_BUILD steps too. Only `listfile.csv` needs a manual
`cp dist/listfile/listfile.csv build/bin/<config>/`, plus the connector DLL for `USE_SQL` builds,
which `windeployqt` cannot know about. Measured runtime dependencies are listed in `docs/setup.md`;
two widely repeated instructions are wrong for this configuration and were checked with
`dumpbin /DEPENDENTS`: **there is no `lua51.dll`** (sol2 links Lua 5.4 statically) and
**`Qt5OpenGL.dll` is never imported** (NodeEditor needs the module at configure time only).

## Coding guidelines

From the README, with one caveat: the README specifies `/src/Noggit` PascalCase directories,
but the actual tree is `src/noggit` lowercase. **Match the surrounding code**, not the
aspirational rule, when adding files to existing directories.

- Include guards, never `#pragma once`. `.hpp` headers, `.cpp` implementations.
- PascalCase file and type names; camelCase methods; `_snake_case` private members;
  `snake_case` locals; `SCREAMING_CASE` constants and macros.
- Namespaces are PascalCase and mirror the directory layout.
- `<>` includes, ordered: local, library, STL. Forward-declare in headers.

## This repo is intended for public release

It is a GPL-3.0 fork and will be published with upstream credit. Consequences for day-to-day
work — see `ATTRIBUTION.md` for the full picture:

- **Every new source file needs the project's GPL header.** 337 existing files carry it.
- **Nothing stays proprietary.** GPL-3.0 is inherited; new code cannot be closed or relicensed.
- **Never commit infrastructure detail.** Database names, absolute paths, hostnames, and
  security posture go in the gitignored `*.local.*` / `db-policy.json` / `environment.md`
  files, each of which has a committed `.example` twin. If you need the protected-schema list,
  read the config — do not paste it into a tracked file.
- **No TrinityCore source code.** TrinityCore is GPL-2.0, incompatible with GPL-3.0. Schema
  *facts* (column names, order, types, what the server derives) are fine and are what
  `docs/schema-335.md` records. Implementation code is not.
- **No client data.** No MPQ, DBC, model, or texture ever enters the repo.
- **AtlasForge is conceptual inspiration only** — closed-source, binary-only, no shared code.
  Do not imply affiliation.
- **Log modifications** in the `ATTRIBUTION.md` change table (GPL-3.0 §5(a)).

## Docs

| File | Contents |
|---|---|
| `docs/schema-335.md` | Measured 3.3.5 schema + where published references are wrong |
| `docs/setup.md` | Build prerequisites, gotchas, dev-DB setup — the shareable guide |
| `docs/environment.md` | **Local only, gitignored.** This machine's specifics |
| `docs/milestones.md` | M0–M5 with a definition of done each |
| `ATTRIBUTION.md` | Licensing, upstream credit, pre-release checks |

## /compact policy

Preserve across compaction: the HARD RULES; the protected-schema list; that the reference DB
is TDB 335.25101 with a `version` table (not `version_db_world`); the corrections table in
`docs/schema-335.md`; the four known defects in the existing `USE_SQL` seam; the current
milestone and its definition of done. Discard: file listings, tool probe output, superseded
plans.

## Ask before

`git push`, any network fetch, any `docker` command, anything touching a protected schema,
and any use of `--dangerously-skip-permissions` (which disables permissions entirely — only
PreToolUse hooks survive it, so it must never be used in this repo).
