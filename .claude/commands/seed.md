---
description: Rebuild noggit_dev_world from live structure plus synthetic fixtures
argument-hint: "[--fixtures-only]"
allowed-tools: PowerShell, Read
---

Rebuild the disposable dev schema.

**Preconditions — check both before running:**

1. `tools/dev-db/01_bootstrap_root.sql` has been applied as root. Verify:
   ```
   powershell -NoProfile -File tools/dev-db/schema-check.ps1 -Target dev
   ```
   If that cannot connect, stop and tell the user to run the bootstrap. Do not attempt to
   create the schema or users yourself — the project's DB account holds only `USAGE` globally
   and cannot, by design.
2. `$env:NOGGIT_DEV_DB_PWD` is set. If not, stop and say so. Never prompt for or embed a
   password.

Then:

```
powershell -NoProfile -ExecutionPolicy Bypass -File tools/dev-db/seed-dev-db.ps1 $ARGUMENTS
```

Paste the verification tables the script prints. The tile-lookup check must return exactly the
three fixture creatures in tile 49_31 — if it returns fewer, the fixtures did not apply and
you must say so rather than proceeding.

Follow with `/schema-check dev` to confirm the copied structure still satisfies every
assertion. A structure copy that drifts from the source is a real finding: report it.

This script only ever writes to `noggit_dev_world`, and refuses to run if its configured dev
schema is a protected name or matches the source schema.
