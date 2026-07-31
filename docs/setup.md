# Setup

What you need, in the order it breaks if you skip it. Machine-specific notes for a particular
install belong in `docs/environment.md`, which is gitignored — this file is the shareable one.

If you only want to read one thing, read [Quick start](#quick-start-default-build-no-database).
The database features are **opt-in and off by default**; you do not need MySQL to build or run
this editor.

This file covers *how* to build and run. For *why* the database layer is shaped the way it is —
the changeset-only write model, the schema-discovery rule, and the transposed tile-index trap that
moves things 9.6 km without an error — see [`design-notes.md`](design-notes.md).

## Platform

**Windows x64 with MSVC is the only configuration this fork has been built on.** Measured:
Visual Studio 2022 (v17), Qt 5.15.2 `msvc2019_64`, generator `Visual Studio 17 2022 -A x64`,
config `RelWithDebInfo`, both with and without `USE_SQL`.

Upstream's `README.md` carries Ubuntu instructions and a `.gitlab-ci.yml` that builds with gcc,
so a Linux build may well work — but that CI job is `only: web` (manual trigger), its package
list is years stale (`qt5-default` no longer exists on current Ubuntu, and Boost is no longer a
dependency), and nothing in this fork has been tested there. Treat Linux as unverified.

Two things in this fork are Windows-only outright:

- the dev-database tooling under `tools/dev-db/` is PowerShell;
- `windeployqt` and `include/win/StackWalker.cpp` are Windows paths in the upstream build.

Nothing in the C++ that this fork adds is Windows-specific. If you get it building on Linux,
that is a welcome patch — just do not assume the docs below apply unchanged.

## Prerequisites

| Component | Requirement | Notes |
|---|---|---|
| MSVC | VS 2019 or 2022, x64 | `cl`/`msbuild` need a Developer prompt, or just use the VS generator. |
| CMake | 3.5+ | If you have CMake **4.x**, see the policy flag below — it fails before anything else. |
| Qt | **5.15.2**, `msvc2019_64` | Hard floor is 5.10; see "Qt version floor". Pick one Qt version and one compiler only. **No Qt account needed** — see below. |
| Git | any | Required for submodules, and CMake uses it for the revision string. |
| Network | first configure only | Six submodules and seven FetchContent dependencies are fetched. See below. |
| MySQL Server | *optional* — 8.0 or 8.4 | **Only for `-DUSE_SQL=ON`.** Supplies `libmysql.lib` and hosts the dev schema. |
| MySQL Connector/C++ | *optional* — 8.0+ | **Only for `-DUSE_SQL=ON`.** Ships as a ZIP, no installer required. |

## Quick start (default build, no database)

This is the normal path and what most people want. `USE_SQL` defaults to `OFF`; the build then
contains no MySQL code at all and links no connector.

```bash
git clone <this repo> noggit
cd noggit
git submodule update --init --recursive

cmake -S . -B build -G "Visual Studio 17 2022" -A x64 \
  "-DCMAKE_POLICY_VERSION_MINIMUM=3.5" \
  "-DCMAKE_PREFIX_PATH=C:/Qt/5.15.2/msvc2019_64"

cmake --build build --config RelWithDebInfo --target ALL_BUILD
```

The executable lands in `build/bin/RelWithDebInfo/noggit.exe`. Before the first run, copy the
listfile next to it — see [Running it](#running-it).

**Do not run `--target INSTALL`.** It is broken in the tree as it stands; see
[The INSTALL target](#the-install-target-is-broken) for why and what to do instead.

### What "no database" means

With `USE_SQL=OFF`, CMake drops the four connector-dependent sources from the build
(`WorldDatabaseConnection`, `SchemaIntrospector`, `SpawnQuery`, `DoctorConnectionChecks`) and
prints:

```
-- Connector/C++ absent - database connection layer excluded from the build
```

Everything else still compiles: the schema model, tile maths, spawn types, changeset emission,
the SQL builders and the spawn scene cache are all pure C++ and are always built and always
tested. What you lose is only the ability to *talk to a server* — the TrinityCore spawn loading,
editing and live changeset application. Every other editor feature, including all of upstream
Noggit Red and the terrain, erosion, ambient-occlusion, alpha-integrity and ground-effect work
this fork adds, is unaffected.

## What a fresh clone does not contain

A clone is not a buildable tree on its own. Verified by cloning this repository to a scratch
directory and listing what came out empty:

**Submodules — six, all required.** `cmake/` is itself a submodule, so *nothing configures at
all* without this step; the failure is `include(cmake/cmake_function.cmake)` on line 16 of the
top-level `CMakeLists.txt`.

| Path | Why it matters |
|---|---|
| `cmake/` | The entire build system: `Find*.cmake`, platform files, `windeployqt.cmake`. Configure fails immediately without it. |
| `src/external/blizzard-archive-library` | Compiled into a static library. Empty ⇒ no sources, and every `#include <ClientFile.hpp>` fails. |
| `src/external/blizzard-database-library` | Same. |
| `dist/definitions` | DBD field definitions, copied next to the exe at build time. Empty ⇒ the app logs "Unable to find database definitions". |
| `dist/themes` | UI themes, copied next to the exe at build time. |
| `dist/listfile` | `listfile.csv`. The app needs it at runtime and CMake does **not** copy it — see below. |

```bash
git submodule update --init --recursive
```

**FetchContent — seven, fetched at configure time.** These are not vendored, so your **first
configure needs a working network connection**. Subsequent configures reuse `build/_deps/`.

| Dependency | Source |
|---|---|
| StormLib | gitlab.com/prophecy-rp/dependencies (`dep-stormlib`) — prebuilt |
| CascLib | gitlab.com/prophecy-rp/dependencies (`dep-casclib`) — prebuilt |
| nlohmann/json | github.com/ArthurSonzogni/nlohmann_json_cmake_fetchcontent v3.9.1 |
| lodepng | github.com/lvandeve/lodepng |
| sol2 (+ Lua 5.4) | github.com/tswow/sol2 |
| FastNoise2 | github.com/Auburn/FastNoise2 v0.10.0-alpha |
| Catch2 v3.7.1 | github.com/catchorg/Catch2 — **test target only**, not the application |

Upstream's README claims the find scripts fall back to system-installed libraries when
FetchContent is unavailable. They do contain `find_path`/`find_library` calls that could do
that, but this has not been tested here and there is no packaged StormLib/CascLib for MSVC to
fall back *to*. Plan on having a network connection for the first configure.

**Not in a clone by design.** `docs/environment.md`, `tools/dev-db/db-policy.json` and
`tools/dev-db/dev-db.config.json` are gitignored because they describe one machine's
infrastructure. Each has a committed `.example` twin, and none of them is needed to build — only
to set up the optional dev database.

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

That completes in about 90 seconds. Run it **without `--archives`**: the default installs the
whole base package, which is what supplies every Qt component the tree's `find_package` calls
ask for. Collected from every `CMakeLists.txt` in the tree, that set is:

| Component | Wanted by |
|---|---|
| Core, Gui, Widgets | everywhere |
| Network, Xml, Multimedia | root `CMakeLists.txt` |
| OpenGLExtensions | root `CMakeLists.txt` |
| OpenGL | `src/external/NodeEditor` (configure time only — see the runtime note below) |
| **Quick, Qml, Svg** | `src/external/qtimgui`, `src/external/framelesshelper` |
| GuiPrivate | `src/external/framelesshelper` |

The `Quick`/`Qml`/`Svg` row is easy to miss because nothing in Noggit's own source uses QtQuick;
`src/external/qtimgui/CMakeLists.txt:7` does, and it is `REQUIRED`.

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
- `src/external/framelesshelper/CMakeLists.txt:83` — links `Qt::GuiPrivate`.

The failure surfaces at **generate**, not configure, so a configure that says
`Configuring done` has told you nothing about Qt yet. Symptom is
`Target "nodes" links to Qt5::OpenGL but the target was not found`.

## Optional: MySQL Connector/C++ for `USE_SQL`

Skip this entire section unless you want the TrinityCore database features. `USE_SQL` is `OFF`
by default and the editor is fully functional without it.

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

> [!note] The upstream `USE_SQL` path did not build, and ran DDL against arbitrary servers
> Two separate problems, both fixed here and both logged in `ATTRIBUTION.md`.
>
> **It did not compile.** `src/mysql/mysql.cpp` shipped with `#include <driver.h>` while CMake's
> `FIND_PATH(MYSQLCPPCONN_INCLUDE NAMES cppconn/driver.h)` resolves to the *parent* of `cppconn/`,
> and the file used `QMessageBox`, `std::stringstream`, `std::unique_ptr` and `std::string` without
> including any of them. Rather than re-point the include, the connector headers were removed from
> that file entirely — everything now goes through `WorldDatabaseConnection`, whose header
> forward-declares `sql::Connection`. Verified by a successful link against Connector/C++ 26.7.0.
>
> **It issued DDL wherever you pointed it.** `connect()` ran `CREATE DATABASE IF NOT EXISTS` and
> then a `CREATE TABLE IF NOT EXISTS` for the `UIDs` table on *every call*, against whatever host
> `project/mysql/server` named. `CREATE DATABASE` is gone; the `CREATE TABLE` is now reachable only
> through the `DEV_WRITE` guard. See [How the write protection is
> layered](#how-the-write-protection-is-layered).
>
> If you are diffing against upstream, that is why those lines differ.

## Build options

Everything below is `OFF`/default unless you ask for it. The full list is in the top-level
`CMakeLists.txt`; these are the ones worth knowing.

| Option | Default | Meaning |
|---|---|---|
| `USE_SQL` | `OFF` | Compile the MySQL UID storage and the database-editing connection layer. |
| `NOGGIT_DEV_BRIDGE` | `OFF` | Compiles a loopback TCP command socket into the binary for scripted testing. **Leave it off.** It is excluded from the source list entirely when off, and CMake prints a warning when on. See `docs/dev-bridge.md`. |
| `NOGGIT_BUILD_NODE_DATAMODELS` | `ON` | Turn off for a much faster build, at the cost of the Node Editor. Not for deployment. |
| `NOGGIT_LOGTOCONSOLE` | `OFF` | Log to the console instead of `log.txt`. |
| `NOGGIT_OPENGL_ERROR_CHECK` | `ON` | Per-call `glGetError`. Cheap to leave on while developing. |
| `FAST_BUILD_NOGGIT_JUMBO` / `_PCH` | `OFF` | Unity builds / precompiled headers. Faster, occasionally fragile. |
| `NOGGIT_ENABLE_TRACY_PROFILER` | `OFF` | Tracy instrumentation. |

## Running it

### 1. Copy the listfile

`listfile.csv` is the one runtime asset CMake does **not** place for you — the copy step in
`CMakeLists.txt` is commented out, while the app looks for `listfile.csv` in its working
directory (`NoggitApplicationConfigurationWriter.cpp:16`). Copy it once:

```bash
cp dist/listfile/listfile.csv build/bin/RelWithDebInfo/
```

`definitions/`, `noggit-definitions/` and `themes/` *are* copied automatically by POST_BUILD
steps, so those three need nothing from you — provided the submodules were initialised.

### 2. Qt DLLs and plugins

`windeployqt` is wired in as a POST_BUILD step (`cmake/windeployqt.cmake`), and with the Visual
Studio generator — where `CMAKE_BUILD_TYPE` is undefined — it runs for both debug and release.
Verified: a finished build directory contains `Qt5Core.dll`, `Qt5Cored.dll` and
`platforms/qwindows.dll` without anyone having run anything by hand.

If your generator or toolchain skips that step, or you copy the executable somewhere else, run it
yourself. Without the **platform plugin** specifically, the binary exits immediately with no
useful message:

```bash
<QtDir>/5.15.2/msvc2019_64/bin/windeployqt.exe --release --no-translations build/bin/RelWithDebInfo/noggit.exe
```

### 3. Connector runtime (`USE_SQL` builds only)

Copy these next to the executable by hand — nothing does it for you. Note the connector DLL
lives one level **above** the `vs14/` directory that holds the import library:

```
<Connector>/lib64/mysqlcppconn-<n>-vs14.dll     (e.g. mysqlcppconn-10-vs14.dll)
<MySQL Server>/lib/libmysql.dll
```

### 4. Optional: Font Awesome, for the icons

**Expect a plain-looking UI on the first run.** No Font Awesome file ships with this repository —
upstream's `resources/font_awesome.otf` was Font Awesome 5 **Pro**, which may not be redistributed
— so icon buttons fall back to a Qt standard icon where one fits the meaning, and otherwise to a
short text label in an outlined box. Nothing is broken; every control is still labelled and
clickable.

To get the glyphs, either install a Font Awesome desktop family system-wide (any family whose name
starts "Font Awesome" is found, Solid preferred), or put a font file in a `fonts` folder beside the
executable:

```bash
mkdir build/bin/RelWithDebInfo/fonts
cp <downloaded>/fa-solid-900.ttf build/bin/RelWithDebInfo/fonts/
```

`fa-solid-900.ttf` comes from the free *Font Awesome Free for Desktop* download (SIL OFL 1.1). A
licensed `Font Awesome 5 Pro-Regular-400.otf` works too, and is the only way to get the Pro-only
codepoints in the enum — those stay blank with Free installed. The full list of accepted file names
is the comment block at the top of `src/noggit/ui/FontAwesome.cpp`.

Segoe UI is likewise not bundled; the interface font is resolved by family name and falls back
through Noto Sans / DejaVu Sans / Liberation Sans / Arial to the Qt system font. Nothing to do.

### 5. First run

A healthy start writes `log.txt` beside the executable, reporting the build revision, the
OpenGL version and renderer, and whether the listfile and DBC definitions were found. If the
window never appears, read that file first — it names the missing piece.

You supply your own legally obtained 3.3.5a client. No game data ships with this repository and
none ever will.

### The INSTALL target is broken

`cmake --build … --target INSTALL` **fails on a fresh clone**, and has nothing to do with this
fork's changes. `cmake/win32_pack.cmake` installs files from a top-level `bin/` directory:

```cmake
install(DIRECTORY bin/shaders DESTINATION .)
install(DIRECTORY bin/fonts   DESTINATION .)
install(FILES bin/noggit_template.conf bin/freetype6.dll bin/StormLib.dll
              bin/glew32.dll bin/zlib1.dll DESTINATION .)
```

That file is reached through `includePlatform("pack")` at `CMakeLists.txt:503`. No `bin/` directory
is tracked in this repository (`git ls-files bin/` prints nothing) and none is generated, so the
first `install(DIRECTORY …)` is where it stops. Measured:

```
-- Installing: <prefix>/./noggit.exe
CMake Error at build/cmake_install.cmake:60 (file):
  file INSTALL cannot find "H:/NoggitUpdate/bin/shaders": File exists.
```

Note that the executable *is* copied before the failure, so a partially populated prefix is not
evidence that it worked. The listed DLLs are leftovers from a much older build system —
`freetype6`, `glew32` and `zlib1` are not linked by the current tree at all — and the shaders are
compiled into the Qt resource bundle (`resources/resources.qrc`, prefix `/shader`), not shipped as
files. There is no `CMAKE_INSTALL_PREFIX` worth setting.

Run the editor out of `build/bin/<config>/` instead. Apart from `listfile.csv`, that directory is
already complete: the executable, the Qt runtime, the platform and image plugins and the
`definitions/`, `noggit-definitions/` and `themes/` directories are all in it. To distribute a
build, zip that directory.

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

## Tests

The test suite is a **standalone CMake project** in `tests/`. It links neither Qt nor Noggit and
compiles only the dependency-free sources, which is what lets it build and run on a bare machine
long before the application does. It is not part of `ALL_BUILD`; you configure it separately.

```bash
cmake -S tests -B build-tests -G "Visual Studio 17 2022" -A x64
cmake --build build-tests --config Release
ctest --test-dir build-tests -C Release --output-on-failure
```

Catch2 v3.7.1 is fetched at configure time, so this first configure also needs a network
connection.

### Expected results without a database

**A lower test count with zero failures is the correct outcome on a machine with no MySQL. It is
not a regression.** Measured on this tree:

| Machine | Test cases | Assertions | ctest |
|---|---|---|---|
| No Connector/C++ at all | 376 | 276,784 | 33 tests, 0 failures |
| Connector present, no database reachable | 382 (377 pass, **5 skipped**) | 276,787 | 0 failures |
| Connector present, dev database reachable | 382, none skipped | more | 0 failures |

The first two rows are measured on this tree. The third is the expected state and the exact
figures will drift as cases are added — the point of the table is the *shape*: the count goes
down as capability goes away, and failures stay at zero throughout.

Three mechanisms produce that, and each is deliberate:

1. **Source gating.** Without Connector/C++, `tests/CMakeLists.txt` omits
   `DatabaseIntegrationTests.cpp` and the four connector-dependent sources. Six test cases
   simply do not exist in that binary. It prints
   `Connector/C++ not found - pure logic tests only.`
2. **Runtime skips.** With the connector but no reachable server, those cases call Catch2's
   `SKIP()` — `NOGGIT_RO_DB_PWD not set; skipping live database tests.` Catch2 exits **4** when
   every selected case skips, and every registered test carries `SKIP_RETURN_CODE 4`, so ctest
   records a skip rather than a failure.
3. **Label exclusion.** The database tests are labelled `needs-database`:

   ```bash
   ctest --test-dir build-tests -C Release -LE needs-database
   ```

   Measured: `100% tests passed, 0 tests failed out of 33`, in about 1.3 seconds.

### Adding a test

`tests/CMakeLists.txt` is **hand-maintained**, while the root `CMakeLists.txt` globs
`src/noggit` recursively. A new module therefore builds into the application automatically but
stays silently untested until you add it, in two places: its `.cpp` to `NOGGIT_DB_SOURCES`, your
test file to `NOGGIT_DB_TESTS`, and — if you introduce a new Catch2 tag — that tag to
`NOGGIT_TEST_TAGS`.

Tests are registered by *tag*, not with `catch_discover_tests`. That is not a style choice:
`catch_discover_tests` parses `--list-tests` output into a CMake list, so a test name containing
a comma collapses every case into one entry and reports failure even when the binary passes.
Several names here legitimately contain commas (`"[0, 2pi)"`, `"tile (49,31)"`).

Only add a tag to `NOGGIT_TEST_TAGS` if at least one *always-compiled* file carries it. A tag
carried only by connector-gated files matches zero cases on a machine without the connector, and
Catch2 exits **2** for "no tests ran" — which is not the configured `SKIP_RETURN_CODE` of 4, so
ctest would report a failure on a machine behaving exactly as intended.

## Dev database

Optional, and only relevant to `USE_SQL` builds. All world-data edits are emitted as reviewable
`.sql` changesets; the only schema the application will execute a statement against is the one you
nominate as writable, and it never creates a schema. The precise guarantee and its limits are in
[How the write protection is layered](#how-the-write-protection-is-layered). Setting that schema up:

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
   and confirming the server refuses it. This does not depend on our code being correct, which
   is exactly why it is first: it is the only layer that still holds when every line of code in
   this repository is wrong.
2. **In-script assertions.** `seed-dev-db.ps1` refuses to run if its configured dev schema is a
   protected name or equals the source schema. The names come from `protectedSchemas` in
   `db-policy.json`; if that list is missing the script falls back to the four MySQL system
   schemas. Note that an *incomplete* list costs you a clearer error message rather than your
   safety — the check that matters is that the target is not the source, and grants cover the
   rest.
3. **The application's own refusals**, described below. Narrower than the two above, because it
   only governs what `noggit.exe` sends.

The reason there are three and not one is that each covers a different actor. Grants cover
anything that connects with the read-only account, including a mistake made by hand at a `mysql`
prompt. The script assertions cover the tooling, which is the thing actually issuing DDL and
which no server-side grant can make safe once it is running as `noggit_rw`. The application
refusals cover the editor. None of the three is a substitute for either of the others.

### What the application itself guarantees

Layer 3, spelled out. Narrower than the two above it — this is only about the statements
`noggit.exe` sends.

- **No `CREATE DATABASE` statement exists in the source.** Verified by grepping the whole of
  `src/` for `CREATE|DROP (DATABASE|SCHEMA)` and `CREATE TABLE`: the only non-comment hit is
  `src/mysql/mysql.cpp:70`, the `UIDs` bookkeeping table.
- **Reads use `AccessMode::READ_ONLY`.** `WorldDatabaseConnection::execute` throws on any
  non-`DEV_WRITE` connection, and the constructor additionally asks the server for
  `SET SESSION TRANSACTION READ ONLY` where it is honoured.
- **Writes use `AccessMode::DEV_WRITE`**, whose constructor refuses unless
  `config.schema == writable_schema`, and which re-checks the same thing before every individual
  statement rather than trusting the construction-time result. There are exactly two write call
  sites: the spawn panel's unchecked "Also apply to the dev schema" box, and the `USE_SQL` UID
  storage in `src/mysql/mysql.cpp`.
- **The `USE_SQL` UID storage refuses outright** unless `project/mysql/db` equals
  `project/mysql/dev_schema`, before a socket is opened. The reason is written to `log.txt` and
  surfaced by **Noggit ▸ Settings ▸ MySQL ▸ Test Connection**, which is itself `READ_ONLY` now.
  *Behaviour change for upstream users:* if your `UIDs` table lives in a differently-named schema,
  UID persistence stops until you point `dev_schema` at it. There is no migration and no dialog at
  map-load time.

What it does **not** do: stop you nominating a live schema as the writable one, or stop you applying
an emitted changeset by hand. It is a guarantee about what Noggit sends, not a replacement for
grants. It has been established by code inspection, the DDL grep and a clean compile — the refusal
path has not been observed against a live server.

## Troubleshooting

| Symptom | Cause |
|---|---|
| `include could not find load file: cmake/cmake_function.cmake` | Submodules not initialised. `cmake/` is a submodule. |
| `Invalid CMAKE_POLICY_VERSION_MINIMUM value "3"` | The flag was not quoted; the shell ate the `.5`. |
| `Target "nodes" links to Qt5::OpenGL but the target was not found` | Qt below 5.10, or `CMAKE_PREFIX_PATH` points at the wrong kit. Appears at *generate*, not configure. |
| `MySQL lib or connector not found` | `-DUSE_SQL=ON` with bad or missing connector paths. Drop `USE_SQL` if you do not need the database features. |
| `Cannot open include file: 'cppconn/driver.h'` | An old build tree from before the `USE_SQL=OFF` source filter. Delete the build directory and reconfigure. |
| `file INSTALL cannot find …/bin/noggit_template.conf` | You ran the `INSTALL` target. Don't — see above. |
| Executable starts and exits with no window | Qt platform plugin missing. Run `windeployqt`, then read `log.txt`. |
| `Unable to find database definitions` in `log.txt` | `dist/definitions` submodule is empty. |
| Asset browser is empty / no models listed | `listfile.csv` was not copied next to the executable. |
| Toolbar buttons show `SAV`, `CFG`, `+`, `X` instead of icons | Expected. No Font Awesome file ships here — see [Font Awesome](#4-optional-font-awesome-for-the-icons). |
| Some icons stay blank with Font Awesome Free installed | Those codepoints are Pro-only. A licensed Pro file is the only fix. |
| `MySQL UID storage refused: …` in `log.txt` | `project/mysql/db` is not equal to `project/mysql/dev_schema`. Deliberate; see [What the application itself guarantees](#what-the-application-itself-guarantees). |
| ctest reports fewer tests than a colleague | Expected. See [Tests](#tests) — the database tests are gated and skip cleanly. |
