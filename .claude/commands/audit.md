---
description: Audit a milestone's diff against its definition of done
argument-hint: "M0|M1|M2|M3|M4|M5"
allowed-tools: Read, Grep, Glob, Bash, PowerShell, Agent
---

Audit the work for milestone **$ARGUMENTS** against its stated definition of done.

Gather the diff:

```
git diff feature/tc335-db-editing --stat
git diff feature/tc335-db-editing
```

Dispatch the `code-reviewer` subagent with the diff and the milestone's definition of done
from `docs/milestones.md`.

Report **gaps only** — things that fail the definition of done, violate a HARD RULE in
CLAUDE.md, or are outright incorrect. Specifically check:

- Any hardcoded table or column name that should have come from the schema layer (HARD RULE 3).
  Grep the diff for the names in `docs/schema-335.md` appearing as string literals.
- Any code path that can write to a schema other than `noggit_dev_world` (HARD RULE 1).
- Emitted SQL that names `bytes1`/`bytes2`, `spawndist`, `creature_template_model`,
  `waypoints`, `script_waypoint`, `pool_creature`, `pool_gameobject`, or `version_db_world`
  unconditionally. Each of these is wrong on the reference DB and each is a silent corruption.
- Emitted `INSERT INTO creature` that writes non-zero `zoneId`/`areaId` — the core derives them.
- `#pragma once` in new headers, and naming that departs from the conventions in CLAUDE.md.
- Claims of success unsupported by pasted evidence.

Do not report style preferences, speculative refactors, or anything the milestone explicitly
placed out of scope. If the work meets the definition of done, say so plainly and list nothing.
