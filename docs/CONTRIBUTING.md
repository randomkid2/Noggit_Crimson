# Contributing

Thanks for looking. This is a GPL-3.0 fork of
[Marlamin/noggit-red](https://github.com/Marlamin/noggit-red); read
[`ATTRIBUTION.md`](../ATTRIBUTION.md) before your first patch, because several of the rules below
are licence obligations rather than preferences.

Build instructions live in [`setup.md`](setup.md). If a build step is wrong or incomplete, that is
a bug worth reporting on its own.

> This file lives in `docs/` rather than at the repository root on purpose. The root `.gitignore`
> starts with `/**` and whitelists paths explicitly, so a **new file added at the top level is
> silently invisible to `git add`**. `docs/` is whitelisted. GitHub finds a contributing guide
> here just as well.

## Before you start

- **There is no CI.** `.gitlab-ci.yml` is upstream's GitLab template and does not run here.
  Nothing is verified on push. Whatever you claim about your patch, you have to show.
- **Show evidence, do not assert success.** Paste the compiler output, the test run, the
  `information_schema` diff. "It should work" is not a result. That standard is applied to
  everything already in the tree and it is why `docs/milestones.md` reads the way it does.

## Hard rules

These are not style points. A patch that breaks one of them cannot be merged.

### 1. Never write to a live database

There is no code path that opens a live world database read/write, and there must never be one —
not behind a flag, not behind a confirmation dialog. All edits are emitted as reviewable `.sql`
changesets. The only schema the application may ever open read/write is the configured dev
schema. If your change needs to touch a database to be tested, test it against your own
disposable schema.

### 2. Never commit infrastructure detail

Database names, absolute paths, hostnames, credentials, and security posture are not published.
They live in gitignored files, each with a committed `.example` twin:

| Gitignored | Committed twin |
|---|---|
| `docs/environment.md` | `docs/environment.example.md` |
| `tools/dev-db/db-policy.json` | `tools/dev-db/db-policy.example.json` |
| `tools/dev-db/dev-db.config.json` | `tools/dev-db/dev-db.config.example.json` |
| `tools/dev-db/.generated/**`, `tools/dev-db/*.local.sql` | — |

If you need the protected-schema list, read the config. Do not paste it into a tracked file.
Before opening a PR:

```bash
git ls-files | grep -E "environment\.md|db-policy\.json|dev-db\.config\.json|\.local\.sql|\.generated/"
```

That must print nothing, and `tools/dev-db/01_bootstrap_root.sql` must still contain its two
`__CHANGE_ME__` placeholders rather than real passwords.

### 3. No TrinityCore source code

TrinityCore is GPL-2.0, which is **not** compatible with GPL-3.0. Schema *facts* — column names,
ordinal positions, types, defaults, and which columns the server derives rather than reads — are
fine, and are exactly what `docs/schema-335.md` records. Implementation code is not. Cite the
location of anything you reproduce so a reader can verify it against their own database.

### 4. No client data, ever

No MPQ archives, no DBC/DB2 files, no models, textures, maps, or extracted assets. Not in the
repository, not in a test fixture, not in a screenshot's accompanying files. Contributors and
users supply their own legally obtained 3.3.5a client.

Display IDs are integers, not content, and appear in `tools/dev-db/02_seed_synthetic.sql` because
`displayId` → model resolution cannot be tested without real ones. Everything around them there
is invented.

### 5. Every new source file carries the GPL header

First line, exactly:

```cpp
// This file is part of Noggit3, licensed under GNU General Public License (version 3).
```

337 existing files carry it. Yours makes 338.

### 6. Log your change in ATTRIBUTION.md

GPL-3.0 §5(a) requires modified files to carry prominent notice of what changed and when. This
project satisfies that with the change table in [`ATTRIBUTION.md`](../ATTRIBUTION.md). Add a row.
Say *why*, not just *what* — the existing rows are the standard: several of them are the only
record of a defect that would otherwise look like an arbitrary edit.

### 7. AtlasForge is conceptual inspiration only

It is closed-source and binary-only; no code is shared and none could be. Do not imply
affiliation, endorsement, or shared implementation anywhere in code, comments, or docs.

## Coding style

From upstream's `README.md`, with one caveat: the README specifies `/src/Noggit` PascalCase
directories, but the actual tree is `src/noggit` lowercase. **Match the surrounding code**, not
the aspirational rule, when adding files to existing directories.

- **Include guards, never `#pragma once`.**
- `.hpp` for C++ headers, `.cpp` for implementations, `.h`/`.c` only for C.
- PascalCase file and type names; camelCase methods; `_snake_case` private members;
  `snake_case` locals; `SCREAMING_CASE` constants and macros.
- Namespaces are PascalCase and mirror the directory layout.
- `<>` includes, ordered: local, library, STL. Forward-declare in headers.
- `src/external/` is third-party and exempt from all of the above.

Two structural conventions that are easy to break by accident:

- **Never hardcode a database column name or column order.** Verify against `information_schema`
  at runtime. `docs/schema-335.md` records where published references are wrong.
- **Version detection probes both `version_db_world` and `version`.** The reference database used
  here has only `version`.

## Tests

`tests/` is a standalone CMake project; it links neither Qt nor Noggit and runs on a machine with
no MySQL at all. See [`setup.md`](setup.md#tests) for how to build and run it, and for the
expected counts when no database is present — **a lower test count with zero failures is the
correct outcome there, not a regression.**

```bash
cmake -S tests -B build-tests -G "Visual Studio 17 2022" -A x64
cmake --build build-tests --config Release
ctest --test-dir build-tests -C Release -LE needs-database --output-on-failure
```

`tests/CMakeLists.txt` is **hand-maintained**, while the root `CMakeLists.txt` globs `src/noggit`
recursively. A new module therefore builds into the application automatically but stays silently
untested until you add it. Adding a module means, in `tests/CMakeLists.txt`:

1. its `.cpp` in `NOGGIT_DB_SOURCES` — only if it is Qt-free, OpenGL-free and connector-free;
2. your test file in `NOGGIT_DB_TESTS`;
3. any new Catch2 tag in `NOGGIT_TEST_TAGS` — **but only if an always-compiled file carries it.**
   A tag carried only by connector-gated files matches zero cases on a machine without the
   connector, and Catch2 exits `2` for "no tests ran", which is not the configured
   `SKIP_RETURN_CODE` of `4`. ctest would then report a failure on a machine behaving correctly.

Prefer pure modules. The largest half of this fork's logic — tile maths, spawn types, changeset
emission, SQL builders, terrain rules, erosion, ambient occlusion, alpha integrity — has no Qt and
no database dependency precisely so it can be tested exhaustively. Keep it that way where you can.

## Gotchas that have bitten people here

- **New top-level files are invisible to git.** `.gitignore` line 1 is `/**`; every path is
  whitelisted explicitly. Adding a new top-level directory or root file means adding both
  `!/thing/` *and* `!/thing/**` — un-ignoring the contents alone does not work, because `/**`
  matches the bare directory entry and git then never looks inside it. Existing files survive only
  because gitignore does not apply to already-tracked files. Check with `git status` before you
  assume a file was staged.
- **`cmake --build … --target INSTALL` fails.** `cmake/win32_pack.cmake` installs from a top-level
  `bin/` directory that is not in the repository. Run out of `build/bin/<config>/` instead. See
  [`setup.md`](setup.md#the-install-target-is-broken).
- **`NOGGIT_DEV_BRIDGE` must stay `OFF`.** It compiles a loopback TCP command socket into the
  binary. It is excluded from the source list entirely when off, and it must never be on in
  anything distributed.
- **`ERROR` is a macro.** `<wingdi.h>` defines `ERROR` as `0`, so an enumerator by that name
  cannot compile in any translation unit that reaches a Windows header. This already cost one
  rename across 20 use sites.
- **Tool order is load-bearing.** `MapView` indexes its `_tools` vector with `editing_mode` values
  directly, so a tool's position in `MapView::createGUI` must equal its enumerator. Append; never
  insert.

## Submitting

1. Branch from `feature/tc335-db-editing`.
2. Build **both** configurations if you touched anything under `src/noggit/database/` or the
   CMake files: `-DUSE_SQL=OFF` (the default, and what most users have) and `-DUSE_SQL=ON`.
3. Run the test suite and paste the result.
4. Add your `ATTRIBUTION.md` row.
5. Re-run the tracked-file check in rule 2.

Bug reports are more useful with `log.txt` (written beside the executable), your Qt and CMake
versions, and whether you built with `USE_SQL`.
