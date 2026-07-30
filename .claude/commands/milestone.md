---
description: Load a milestone brief and plan it before writing code
argument-hint: "M0|M1|M2|M3|M4|M5"
allowed-tools: Read, Grep, Glob, PowerShell
---

Load milestone **$ARGUMENTS** and plan it.

1. Read `docs/milestones.md` and extract the brief and definition of done for $ARGUMENTS.
2. Read `docs/schema-335.md` for every table the milestone touches. Note which assertions
   protect it.
3. Read the existing code the milestone extends. For anything DB-related that means
   `src/mysql/mysql.{h,cpp}` and its call sites in `MapView.cpp`, `map_index.cpp`,
   `NoggitWindow.cpp`, `SettingsPanel.cpp`.
4. Run `/schema-check source` and confirm it passes before planning any DB work.
5. **Enter Plan Mode.** Produce a written plan covering:
   - The specific files added or changed, and the namespace/directory each belongs in.
   - Which schema facts are read at runtime versus baked in. Anything baked in needs a
     justification, because HARD RULE 3 forbids it by default.
   - How the change is verified — the actual command that produces evidence.
   - What is explicitly out of scope for this milestone.
6. **Stop for approval.** Do not Edit or Write implementation code until the plan is approved.

Think hard about the adapter seam on M0. Getting the `information_schema` layer wrong is the
one mistake that silently corrupts every emitted changeset downstream, and it is the hardest
to retrofit.
