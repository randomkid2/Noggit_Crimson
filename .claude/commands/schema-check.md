---
description: Verify a target DB against the assertions in docs/schema-335.md
argument-hint: "[source|dev|both]"
allowed-tools: PowerShell, Read, Edit
---

Run the schema assertions against `${ARGUMENTS:-source}`:

```
powershell -NoProfile -ExecutionPolicy Bypass -File tools/dev-db/schema-check.ps1 -Target ${ARGUMENTS:-source}
```

Then:

1. **Paste the full output.** Do not summarise it away — the assertion list is the evidence.
2. If everything passes, say so and stop.
3. If anything fails, do **not** modify the database. A failure means one of:
   - `docs/schema-335.md` is stale for this target → update the doc and the assertion's `why`.
   - The target is a different core or branch (AzerothCore keeps `spawndist`; TC master differs
     on `creature_model_info` and the addon tables) → the schema map needs a branch, not a fix.
   - The target is a partially-seeded dev schema → re-run `/seed`.

   Say which of the three it is and why, citing the specific failing assertions.
4. If the version line reads `NO VERSION TABLE`, that is a hard stop — report it and do not
   proceed with any DB work.

Never "fix" a live schema to satisfy an assertion. The assertions describe reality; if they
disagree with reality, the assertions are what changes.
