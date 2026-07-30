---
description: Configure and build, reporting compiler errors only
argument-hint: "[Debug|RelWithDebInfo]"
allowed-tools: PowerShell, Bash, Read, Edit, Grep
---

Build the project. Config: `${ARGUMENTS:-RelWithDebInfo}`.

**Read `docs/environment.md` first.** As of the last update this repo could not be built on
this machine: Qt 5.9.9 is installed but Noggit Red needs 5.15.2, MySQL Connector/C++ headers
are absent, and submodules are uninitialised. If those are still unresolved, say so and stop —
do not attempt a build you know will fail, and do not report a configure failure as if it were
a code defect.

Once the toolchain is ready:

```
git submodule update --init --recursive
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH="<Qt>/msvc2019_64"
cmake --build build --config ${ARGUMENTS:-RelWithDebInfo}
```

Reporting rules:

- **Paste the actual compiler output** for every error and warning introduced by the current
  change set. Never summarise a build as "successful" without output showing it.
- Report errors only — do not paste thousands of lines of successful compilation.
- Attribute each error to a file and line. If an error is pre-existing on the upstream base
  (`6f0776d4`), say so explicitly instead of trying to fix it as part of unrelated work.
- If the `USE_SQL` path is enabled, expect trouble: `src/mysql/mysql.cpp` includes
  `<driver.h>` while CMake and the README both resolve `MYSQLCPPCONN_INCLUDE` to the parent of
  `cppconn/`, and the file uses `QMessageBox` and `std::stringstream` without including them.
  These are upstream defects, documented in CLAUDE.md. Fixing them is legitimate work; being
  surprised by them is not.
