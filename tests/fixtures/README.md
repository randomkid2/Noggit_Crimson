# Schema fixtures

Captured `information_schema` output, used to test the M0 capability model without needing a
database. Tab-separated, one row per column:

```
table_name  ordinal_position  column_name  column_type  is_nullable  column_default  extra
```

`column_default` uses `\N` for SQL NULL, which is what the MySQL client emits in batch mode.

| File | What it is |
|---|---|
| `schema-tdb335-25101.tsv` | **Real.** Captured 2026-07-30 from a TDB 335.25101 `world` database. 427 columns across 44 tables. This is the primary target and the golden reference for `docs/schema-335.md`. |
| `schema-alt-drifted.tsv` | **Synthetic, hand-authored.** Not a dump of any real database. |

## Why the second fixture exists

A capability model tested against one schema is not a capability model — it is a hardcode with
extra steps. Every branch has to be exercised, and the only way to do that without standing up a
second server is a fixture that takes the opposite path at every decision point.

`schema-alt-drifted.tsv` deliberately combines drift from **both** AzerothCore and TrinityCore
master. No single real database looks like this. That is intentional: it forces every branch in
one pass, and a model that handles it will handle either real variant.

| Decision | `schema-tdb335-25101` (real) | `schema-alt-drifted` (synthetic) |
|---|---|---|
| Wander distance column | `wander_distance` | `spawndist` |
| Addon pose columns | `StandState`, `MountCreatureID`, `AnimTier`, `VisFlags`, `SheathState`, `PvPFlags` | `bytes1`, `bytes2` |
| `creature.Comment` | absent | present |
| `creature_model_info` gender | `Gender`, no `VerifiedBuild` | `VerifiedBuild`, no `Gender` |
| Creature model source | `creature_template.modelid1..4` | `creature_template_model` table |
| `gameobject_template_addon` tail | ends at `artkit3` | `artkit4`, `WorldEffectID`, `AIAnimKitID` |
| `smart_scripts` event params | 5 | 4 |
| `smart_scripts` target params | 4 | 3 |
| Waypoint tables | `waypoint_data` only | `waypoint_data` + `waypoints` + `script_waypoint` |
| Pool membership | `pool_members` | `pool_creature` + `pool_gameobject` |
| Version table | `version` | `version_db_world` |

## Rules

- **Never edit `schema-tdb335-25101.tsv` by hand.** It is measured data. Re-capture it if the
  reference database changes, and update `docs/schema-335.md`'s provenance block in the same
  commit.
- A test that passes against only one fixture proves nothing about drift handling. Assert
  against both, and assert they produce *different* answers where the table above says they
  should — a model that returns the same column name for both is broken even if one answer is
  right.
- Neither file contains data rows, only column metadata. No game content, no server content.

## Re-capturing the real fixture

```
powershell -NoProfile -File tools/dev-db/schema-check.ps1 -Target source
```

confirms the assertions still hold first. Then re-run the capture query documented in
`docs/schema-335.md`.
