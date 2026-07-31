# Setup

What you need, in the order it breaks if you skip it. Machine-specific notes for a particular
install belong in `docs/environment.md`, which is gitignored — this file is the shareable one.

## Prerequisites

| Component | Requirement | Notes |
|---|---|---|
| MSVC | VS 2019 or 2022, x64 | `cl`/`msbuild` need a Developer prompt, or just use the VS generator. |
| CMake | 3.5+ | If you have CMake **4.x**, see the policy flag below — it fails before anything else. |
| Qt | **5.15.2**, `msvc2019_64` | Hard floor is 5.10; see "Qt version floor". Pick one Qt version and one compiler only. **No Qt account needed** — see below. |
| MySQL Server | 8.0 or 8.4 | Supplies `libmysql.lib` and hosts the dev schema. |
| MySQL Connector/C++ | 8.0+ | Only needed for `-DUSE_SQL=ON`. Ships as a ZIP, no installer required. |
| Submodules | initialised | `cmake/` is itself a submodule, so nothing configures without this. |

```bash
git submodule update --init --recursive
```

## CMake 4.x policy flag

CMake 4 removed support for `cmake_minimum_required` below 3.5. The top-level `CMakeLists.txt`
has been raised to 3.5, but the flag is **still required**, because `nlohmann/json` is pulled by
FetchContent into `build/_deps/json-src`, declares below 3.5 itself, and is re-fetched on every
clean configure — so patching it in place does not persist.

```bash
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 "-DCMAKE_POLICY_VERSION_MINIMUM=3.5" "-DCMAKE_PREFIX_PATH=<Qt>/msvc2019_64"
```

**Quote the flag.** Unquoted, some shells strip the `.5`, the cache ends up holding `3`, and
every subproject then fails with a message that blames the subproject rather than the flag.
If you see `Invalid CMAKE_POLICY_VERSION_MINIMUM value "3"`, that is this.

## Installing Qt without a Qt account

The official Qt online installer requires an account. `aqtinstall` fetches the same packages from
the same mirrors and does not:

```bash
python -m pip install aqtinstall
python -m aqt install-qt windows desktop 5.15.2 win64_msvc2019_64 -O C:/Qt
```

That completes in about 90 seconds and yields `Qt5Core`, `Qt5Widgets`, `Qt5Gui`, `Qt5OpenGL`,
`Qt5OpenGLExtensions`, `Qt5Network`, `Qt5Xml` and `Qt5Multimedia` — every component this project's
`find_package` calls need.

Do **not** pass `-m qtmultimedia`: in 5.15.2 Multimedia ships in the base package and naming it as
a module fails with "packages were not found while parsing XML of package information". Check what
a version actually offers with `aqt list-qt windows desktop --modules 5.15.2 win64_msvc2019_64`.

Then point CMake at it — `<QtDir>` is whatever you passed to `-O` above:

```bash
"-DCMAKE_PREFIX_PATH=<QtDir>/5.15.2/msvc2019_64"
```

## Qt version floor

Qt 5.9 and earlier **cannot** build this tree, regardless of what older prebuilt Noggit binaries
suggest. Two submodules put the floor above it:

- `src/external/NodeEditor/CMakeLists.txt:37` — `find_package(Qt5 5.10 ...)`. Below 5.10 this
  silently fails and `Qt5::OpenGL` is never defined.
- `src/external/framelesshelper` — links `Qt::GuiPrivate`.

The failure surfaces at **generate**, not configure, so a configure that says
`Configuring done` has told you nothing about Qt yet. Symptom is
`Target "nodes" links to Qt5::OpenGL but the target was not found`.

## MySQL Connector/C++ for `USE_SQL`

Noggit needs the **legacy JDBC (`cppconn`) API**, not the X DevAPI. Current Connector/C++
(**26.7.0** — MySQL moved to year-based versioning, so there is no 9.x/10.x) still ships it
under `include/jdbc/`. Verified against 26.7.0.

Download `mysql-connector-c++-<version>-winx64.zip` from
<https://dev.mysql.com/downloads/connector/cpp/>. Fetch it with a browser: `dev.mysql.com`
returns 403 to scripted requests regardless of user-agent.

The zip layout, which is not quite what the README implies:

```
include/jdbc/cppconn/driver.h          <- what FIND_PATH looks for
lib64/vs14/mysqlcppconn.lib            <- import library
lib64/mysqlcppconn-<n>-vs14.dll        <- runtime DLL, one level ABOVE vs14/
```

**Easiest setup — no `-D` flags at all.** CMake already searches
`${CMAKE_SOURCE_DIR}/../Noggit3libs/mysql`, so build this sibling layout and everything resolves
itself. `FIND_PATH` wants `cppconn/driver.h` *directly* under `connector/`, so copy the
**contents** of `include/jdbc` in — not the `jdbc` folder itself:

```
<repo>/../Noggit3libs/
  mysql/
    libmysql.lib                  from your MySQL Server lib/ directory
    connector/
      cppconn/…                   contents of the zip's include/jdbc/
      mysqlcppconn.lib            from lib64/vs14/
      mysqlcppconn-<n>-vs14.dll   from lib64/  (copy next to noggit.exe at runtime)
```

Then `cmake … -DUSE_SQL=ON` and nothing else. Explicit paths still work if you prefer:

```bash
-DUSE_SQL=ON
-DMYSQL_LIBRARY="<MySQL Server>/lib/libmysql.lib"
-DMYSQLCPPCONN_INCLUDE="<Connector>/include/jdbc"
-DMYSQLCPPCONN_LIBRARY="<Connector>/lib64/vs14/mysqlcppconn.lib"
```

If the paths are wrong, CMake stops with `MySQL lib or connector not found` at *configure*
time — so a configure that gets as far as Qt errors has already proven your MySQL paths good.

> [!warning] The `USE_SQL` path has a known include contradiction
> `src/mysql/mysql.cpp` does `#include <driver.h>`, but CMake's
> `FIND_PATH(MYSQLCPPCONN_INCLUDE NAMES cppconn/driver.h)` and the README both resolve to the
> *parent* of `cppconn/`. The commented-out `<cppconn/driver.h>` line is the correct form.
> The file also uses `QMessageBox` and `std::stringstream` without including them. Assume this
> path needs fixing before it builds. See `CLAUDE.md` for the full list of defects here.

## Running it

CMake copies the Qt DLLs next to `noggit.exe` itself, but not the Qt *plugins* — without the
platform plugin the binary exits immediately with no useful message. Deploy them once:

```bash
<QtDir>/5.15.2/msvc2019_64/bin/windeployqt.exe --release --no-translations build/bin/RelWithDebInfo/noggit.exe
```

For a `USE_SQL` build also copy the connector runtime next to the executable. Note it lives one
level **above** the `vs14/` directory that holds the import library:

```
<Connector>/lib64/mysqlcppconn-<n>-vs14.dll
<MySQL Server>/lib/libmysql.dll
```

A healthy start writes `log.txt` beside the executable, reporting the build revision, the
OpenGL version and renderer, and whether the listfile and DBC definitions were found. If the
window never appears, read that file first — it names the missing piece.

### What the binary actually needs at runtime

Measured with `dumpbin /DEPENDENTS` on a `RelWithDebInfo` `USE_SQL=ON` build rather than taken
from the README, because two of the commonly repeated instructions are wrong for this
configuration:

```
OPENGL32 · USER32 · SHELL32 · GDI32 · dwmapi · d2d1 · ADVAPI32 · KERNEL32 · IMM32 ·
WININET · WS2_32 · MSVCP140 · VCRUNTIME140(_1) · api-ms-win-crt-*     (system)
Qt5Core · Qt5Gui · Qt5Widgets · Qt5Network · Qt5Xml · Qt5Multimedia · Qt5Quick
mysqlcppconn-<n>-vs14                                                 (USE_SQL only)
```

- **No Lua DLL.** `sol2` fetches Lua and links it statically (`SOL2_BUILD_LUA=TRUE`,
  `lua54.lib`), so there is nothing to copy. Instructions to place `lua51.dll` beside the
  executable date from an older build system — the version is wrong and the file is not needed.
- **No `Qt5OpenGL.dll`.** `src/external/NodeEditor` requires the `Qt5::OpenGL` *module* at
  configure time, which is why Qt ≥ 5.10 is a hard floor, but nothing imports the library at
  runtime: Qt 5.15 puts `QOpenGLWidget` in `Qt5Widgets`. Deploying it does no harm; omitting it
  does none either.

`windeployqt` gets all of the Qt entries right on its own. The connector DLL is the one it
cannot know about.

## Dev database

This project **never writes to a live database**. All edits are emitted as reviewable `.sql`
changesets, optionally rehearsed against a disposable schema. Setting that schema up:

**1. Configure.** Copy both examples and edit:

```bash
cp tools/dev-db/db-policy.example.json      tools/dev-db/db-policy.json
cp tools/dev-db/dev-db.config.example.json  tools/dev-db/dev-db.config.json
```

`db-policy.json` lists every live database on your server. Both files are gitignored — they
describe your infrastructure, which is not something to publish.

**2. Bootstrap.** Replace the two `__CHANGE_ME__` placeholders in
`tools/dev-db/01_bootstrap_root.sql` with passwords you choose, then run it as root:

```bash
mysql -u root -p < tools/dev-db/01_bootstrap_root.sql
```

This needs `CREATE USER` and `GRANT`, which a typical worldserver DB account does not have.
It creates `noggit_dev_world` plus two accounts: `noggit_ro` (SELECT only) and `noggit_rw`
(full rights on the dev schema and nowhere else).

**3. Seed.** Export the `noggit_rw` password and run:

```bash
powershell -NoProfile -File tools/dev-db/seed-dev-db.ps1
```

This copies table *structure* from your live schema and adds synthetic fixtures. **No live data
is ever copied.** Structure is copied rather than hand-written on purpose: a hand-written schema
only proves the documentation agrees with itself, so real column drift stays invisible.

**4. Verify.**

```bash
powershell -NoProfile -File tools/dev-db/schema-check.ps1 -Target both
```

Every assertion exists because a published reference got it wrong or because the column name
differs between the 3.3.5 and master branches. A failure means `docs/schema-335.md` is stale for
your target, or your target is a different core — not that your database needs fixing.

## How the write protection is layered

Strongest first. Each layer assumes the ones above it may fail.

1. **Database grants.** `noggit_ro` holds `SELECT` only. Verified by attempting an `INSERT`
   and confirming the server refuses it. This does not depend on our code being correct.
2. **In-script assertions.** `seed-dev-db.ps1` refuses to run if its configured dev schema is a
   protected name or equals the source schema.
3. **The `PreToolUse` hook.** `.claude/hooks/guard-db.ps1` inspects agent shell commands.
   `-Test` runs 22 self-test cases. It reads **command lines only** — not the contents of a
   `.sql` file, not statements issued from inside a script, and nothing the compiled
   application does at runtime. Treat it as catching obvious mistakes, not as permission.

The guard's core rule is that any state-changing statement not explicitly naming the writable
schema is refused. That holds even with an empty `protectedSchemas` list, so an incomplete list
costs you a clearer error message, not your safety.
