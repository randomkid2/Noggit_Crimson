# Environment — template

Copy this to `docs/environment.md` and fill it in for your machine. **`environment.md` is
gitignored and must stay that way** — it records absolute paths, database names and credential
locations, which are infrastructure detail and do not belong in a public repository. This template
is the tracked twin: it carries the structure and none of the values.

Record what you measured, not what you assume. Every heading below exists because something in the
build or the database layer depends on it and fails unhelpfully when it is wrong.

## Toolchain

- Compiler and version (e.g. MSVC 19.x from Visual Studio 2022), CMake version, generator.
- Qt version and install root. 5.15.2 recommended; **5.10 is a hard floor** — below it
  `Qt5::OpenGL` is never defined and `src/external/NodeEditor` fails at *generate*, not configure.
- MySQL Connector/C++ location, and whether it exposes the legacy `cppconn` API.
- Note that `MYSQLCPPCONN_INCLUDE` must resolve to the **parent** of `cppconn/`, not to
  `cppconn/` itself.

## Databases

- Server host, port, version.
- Which schema is the **writable dev schema** — this must match `writableSchema` in
  `tools/dev-db/db-policy.json`.
- Which schemas are **protected** (read-only). Everything the server hosts that is not the dev
  schema belongs here. Do not enumerate these anywhere tracked.

### Credentials for the dev accounts

- Account names and what each is granted. Keep the *values* in environment variables or a
  password manager, not in this file — record only where they live.
- The read-only account is what proves the M1 read path is genuinely read-only, so it needs
  `SELECT` and nothing else.

### Reference schema

- Which schema is the untouched reference, and its `db_version`.
- **Which version table it actually has.** Probe both `version_db_world` and `version`; a TDB
  release may have only one. Recording the wrong one makes version detection fail silently.

### Credentials

- Environment variable names the tooling reads, and the fallback if any.

## Client data

- Path to a **stock, unmodified** 3.3.5a client (build 3.3.5.12340) and its locale.
- Confirm the MPQ chain is complete and that **no custom `patch-*` archives are injected** — a
  client carrying custom content is not a valid DBC baseline, and
  `CreatureDisplayInfo` / `CreatureModelData` / `GameObjectDisplayInfo` are read from it.
- MPQ load order, if yours is non-standard.
- If you have more than one client, say explicitly which is the baseline and which must not be
  used.

## Other Noggit installs on this machine

Only worth recording if there are several, so a stale binary is never mistaken for the built one.

## TrinityCore source

- Path, fork and revision, if a checkout is available for reading schema *facts*.
- **No TrinityCore source code may be copied into this repository.** TrinityCore is GPL-2.0 and
  incompatible with this fork's GPL-3.0. Column names, ordering, types and what the server derives
  are facts and are fine; they belong in `docs/schema-335.md`.

## What your write protections do and do not cover

Record honestly which layers stand between you and a live database on *this* machine, and where
each one stops. Anything a layer cannot intercept is a risk you are carrying, not one that has
been removed.

- **Database grants.** Which account each tool connects as, and what each is granted. This is the
  only layer that does not depend on this project's code being correct.
- **`tools/dev-db/seed-dev-db.ps1`.** Refuses to run when the configured dev schema is a
  protected name or equals the source schema. It cannot vet a statement you run by hand.
- **The application.** `WorldDatabaseConnection` refuses a `DEV_WRITE` connection unless
  `config.schema` equals the configured writable schema, and re-checks that before every
  statement; reads open `READ_ONLY`, which `execute()` rejects outright. None of that stops you
  *nominating* a live schema as the writable one — that choice is recorded above and is yours to
  get right — nor from applying an emitted changeset yourself.

## Deliberate omissions

Anything you chose not to configure, and why, so it is not rediscovered as a surprise.

## Staged dependencies (this machine)

Where prebuilt libraries live, if they are not in the layout `CMakeLists.txt` searches by default.
