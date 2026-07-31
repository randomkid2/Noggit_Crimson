# TrinityCore 3.3.5 world schema — measured reference

**Every column list below was read from a live `information_schema`, not from documentation.**

| Provenance | Value |
|---|---|
| Measured | 2026-07-30 |
| Server | MySQL 8.4 |
| Schema | `world` (the stock TrinityCore name) |
| `version.db_version` | `TDB 335.25101` (`cache_id` 25101) |
| Core source cross-check | a TrinityCore 3.3.5a (WotLK-line) source checkout |

Re-measure with `/schema-check` before trusting any of this. Column *order* matters for
positional `INSERT`, so it is preserved verbatim throughout.

> [!warning] The measured `world` is not a pristine TDB install
> It carries a small number of non-stock tables. None of them are spawn, template, scripting
> or waypoint tables, none are in scope for this fork, and none are documented here — every
> table described below is stock TrinityCore. What this does mean is that the table *list* and
> table *count* of the measured database are not a statement about stock TDB 335.25101; only
> the column-level facts below are.
>
> So treat this document as one measurement, not as the definition of the schema. Where it
> disagrees with a published reference, the resolution is to re-measure your own target with
> `/schema-check` and trust `information_schema`, not either document.
>
> One genuine trap: `playercreateinfo_spell_custom` and `spell_custom_attr` read as
> customisations but *are* stock TrinityCore table names.

## Corrections to the source brief

The research brief this project started from is wrong or stale in the following places. Items
1–4 will hard-fail SQL emission if implemented as written.

| # | Brief claimed | Measured reality | Severity |
|---|---|---|---|
| 1 | `creature_addon` / `creature_template_addon` carry `bytes1`, `bytes2` | Both are **split into discrete columns**: `MountCreatureID`, `StandState`, `AnimTier`, `VisFlags`, `SheathState`, `PvPFlags`. `mount` is retained alongside `MountCreatureID`. | **HIGH** — `INSERT` naming `bytes1`/`bytes2` fails outright |
| 2 | `smart_scripts` has 4 event params on 3.3.5; master added a 5th. 3 target params. | **5** event params (`event_param1..5`) and **4** target params (`target_param1..4`). Still 30 columns total. | **HIGH** |
| 3 | SmartAI waypoints live in a `waypoints` table, distinct from `waypoint_data` | **`waypoints` does not exist.** | **HIGH** — see "Waypoints" below |
| 4 | Escort C++ scripts read `script_waypoint` | **`script_waypoint` does not exist.** | **HIGH** |
| 5 | `creature_template_model` coexists with `modelid1..4` on 3.3.5 | **`creature_template_model` does not exist.** `creature_template.modelid1..4` are present and are the only model source. | MEDIUM |
| 6 | `pool_creature` and `pool_gameobject` | Both replaced by one unified **`pool_members`** (`type`, `spawnId`, `poolSpawnId`, `chance`, `description`). | MEDIUM |
| 7 | Read `version_db_world` to detect DB version | **`version_db_world` does not exist.** Only `version` (`core_version`, `core_revision`, `db_version`, `cache_id`). Probe both. | MEDIUM |
| 8 | `creature_template_locale` = `entry, locale, Name, NameAlt/Title, SubName` | `entry, locale, Name, Title, VerifiedBuild`. **No `SubName`, no `NameAlt`.** | MEDIUM |
| 9 | `gameobject_template_locale` = `entry, locale, name, castBarCaption` | Adds `VerifiedBuild`. | LOW |
| 10 | `waypoint_scripts` has no comment field | Has **`Comment`**. (The brief is right that `creature` has none.) | LOW |
| 11 | `conditions` column list | Adds **`ConditionStringValue1`** between `ConditionValue3` and `NegativeCondition`. | LOW |

Confirmed correct in the brief, for the record: the full 25-column `creature` shape;
`wander_distance` (not `spawndist`); `zoneId`/`areaId` present; no `Comment` on `creature`;
`curhealth`/`curmana` retained; `creature_model_info` having `Gender` +
`DisplayID_Other_Gender` and **no** `VerifiedBuild` (the brief flagged this as its own biggest
uncertainty — resolved in its favour); `gameobject_template_addon` ending at `artkit3`;
`creature_template` and `gameobject_template` full column lists; `waypoint_data` including
`wpguid`.

## Spawn tables

### `creature` — 25 columns, with types

| # | Column | Type | Null | Default |
|---|---|---|---|---|
| 1 | `guid` | int unsigned | NO | — (PK, auto_inc) |
| 2 | `id` | int unsigned | NO | 0 |
| 3 | `map` | smallint unsigned | NO | 0 |
| 4 | `zoneId` | smallint unsigned | NO | 0 |
| 5 | `areaId` | smallint unsigned | NO | 0 |
| 6 | `spawnMask` | tinyint unsigned | NO | 1 |
| 7 | `phaseMask` | int unsigned | NO | 1 |
| 8 | `modelid` | int unsigned | NO | 0 |
| 9 | `equipment_id` | tinyint | NO | 0 |
| 10 | `position_x` | float | NO | 0 |
| 11 | `position_y` | float | NO | 0 |
| 12 | `position_z` | float | NO | 0 |
| 13 | `orientation` | float | NO | 0 |
| 14 | `spawntimesecs` | int unsigned | NO | 120 |
| 15 | `wander_distance` | float | NO | 0 |
| 16 | `currentwaypoint` | int unsigned | NO | 0 |
| 17 | `curhealth` | int unsigned | NO | 1 |
| 18 | `curmana` | int unsigned | NO | 0 |
| 19 | `MovementType` | tinyint unsigned | NO | 0 |
| 20 | `npcflag` | int unsigned | NO | 0 |
| 21 | `unit_flags` | int unsigned | NO | 0 |
| 22 | `dynamicflags` | int unsigned | NO | 0 |
| 23 | `ScriptName` | char(64) | YES | `''` |
| 24 | `StringId` | varchar(64) | YES | NULL |
| 25 | `VerifiedBuild` | int | YES | 0 |

> [!important] `zoneId` / `areaId` are derived, not authored
> `ObjectMgr::LoadCreatures` never selects them, and the core's own `WORLD_INS_CREATURE`
> prepared statement omits them. Emit `0` and let the core populate them. Do not compute them
> in the editor. Verify in a TrinityCore 3.3.5 checkout at
> `src/server/game/Globals/ObjectMgr.cpp:2161` and
> `src/server/database/Database/Implementation/WorldDatabase.cpp:86`.

The core's own insert shape — the safest template for emitted SQL, because it is exactly what
the server writes when a GM spawns a creature:

```sql
INSERT INTO creature
  (guid, id, map, spawnMask, phaseMask, modelid, equipment_id,
   position_x, position_y, position_z, orientation, spawntimesecs, wander_distance,
   currentwaypoint, curhealth, curmana, MovementType, npcflag, unit_flags, dynamicflags)
VALUES (...);
```

### `gameobject` — 21 columns

`guid`, `id`, `map`, `zoneId`, `areaId`, `spawnMask`, `phaseMask`, `position_x`,
`position_y`, `position_z`, `orientation`, `rotation0`, `rotation1`, `rotation2`,
`rotation3`, `spawntimesecs`, `animprogress`, `state`, `ScriptName`, `StringId`,
`VerifiedBuild`.

`rotation0..3` is a quaternion, not Euler angles.

### `creature_addon` / `creature_template_addon` — 12 columns each

```
creature_addon:          guid,  path_id, mount, MountCreatureID, StandState, AnimTier,
                         VisFlags, SheathState, PvPFlags, emote, visibilityDistanceType, auras
creature_template_addon: entry, path_id, mount, MountCreatureID, StandState, AnimTier,
                         VisFlags, SheathState, PvPFlags, emote, visibilityDistanceType, auras
```

Per-spawn (`creature_addon`) overrides per-entry (`creature_template_addon`). `path_id` here
is what binds a spawn to a waypoint path.

### `creature_equip_template`

`CreatureID`, `ID` (tinyint, default 1), `ItemID1`, `ItemID2`, `ItemID3`, `VerifiedBuild`.
PK (`CreatureID`, `ID`). `creature.equipment_id`: `-1` random, `0` none, `>0` this `ID`.

### `creature_model_info`

`DisplayID`, `BoundingRadius`, `CombatReach`, `Gender`, `DisplayID_Other_Gender`.
No `VerifiedBuild` on 3.3.5 (master is the inverse).

## Templates

### `creature_template` — 58 columns

`entry`, `difficulty_entry_1..3`, `KillCredit1..2`, `modelid1..4`, `name`, `subname`,
`IconName`, `gossip_menu_id`, `minlevel`, `maxlevel`, `exp`, `faction`, `npcflag`,
`speed_walk`, `speed_run`, `scale`, `rank`, `dmgschool`, `BaseAttackTime`, `RangeAttackTime`,
`BaseVariance`, `RangeVariance`, `unit_class`, `unit_flags`, `unit_flags2`, `dynamicflags`,
`family`, `type`, `type_flags`, `lootid`, `pickpocketloot`, `skinloot`, `PetSpellDataId`,
`VehicleId`, `mingold`, `maxgold`, `AIName`, `MovementType`, `HoverHeight`, `HealthModifier`,
`ManaModifier`, `ArmorModifier`, `DamageModifier`, `ExperienceModifier`, `RacialLeader`,
`movementId`, `RegenHealth`, `mechanic_immune_mask`, `spell_school_immune_mask`,
`flags_extra`, `ScriptName`, `StringId`, `VerifiedBuild`.

`AIName` decides which scripting system owns the creature — `'SmartAI'`, `''`, or a C++
script name. The waypoint editor must read this.

### `gameobject_template` — 36 columns

`entry`, `type`, `displayId`, `name`, `IconName`, `castBarCaption`, `unk1`, `size`,
`Data0`..`Data23`, `AIName`, `ScriptName`, `StringId`, `VerifiedBuild`.

Not renderable / needs special handling: type `15` MO_TRANSPORT (the core refuses manual
creation), `11` TRANSPORT (moving), `6` TRAP, `12` AREADAMAGE, `13` CAMERA, `18` RITUAL
(commonly invisible), and anything with `displayId = 0`.

## Waypoints — one system here, not three

The brief describes three parallel waypoint systems. On TDB 335.25101 **only one of the three
tables exists**:

| Table | Status |
|---|---|
| `waypoint_data` | **present** |
| `waypoints` | absent |
| `script_waypoint` | absent |
| `waypoint_scripts` | present (action targets, not paths) |
| `script_spline_chain_waypoints` | present (spline chains, a separate newer feature) |

So M3 targets `waypoint_data` only, and the "pick a table based on `AIName`" branching the
brief calls for is unnecessary on this DB generation. **Still verify at runtime** — a
different TDB or an AzerothCore target may reintroduce `waypoints`.

```
waypoint_data:                  id, point, position_x, position_y, position_z, orientation,
                                delay, move_type, action, action_chance, wpguid
waypoint_scripts:               id, delay, command, datalong, datalong2, dataint,
                                x, y, z, o, guid, Comment
script_spline_chain_waypoints:  entry, chainId, splineId, wpId, x, y, z
```

- `waypoint_data.point` starts at 1. `move_type`: 0 WALK, 1 RUN, 2 LAND, 3 TAKEOFF.
- `wpguid` is core-managed — never author it.
- `action` references `waypoint_scripts.id`; `action_chance` is 0–100.
- A creature follows a path when `creature.MovementType = 2` and
  `creature_addon.path_id` (or `creature_template_addon.path_id`) matches `waypoint_data.id`.
- `path_id = guid * 10` is a **TDB data convention only**, not enforced by the core. Any
  unique int works. Make it configurable.

## Scripting and conditions

```
smart_scripts: entryorguid, source_type, id, link, event_type, event_phase_mask,
               event_chance, event_flags, event_param1..5, action_type, action_param1..6,
               target_type, target_param1..4, target_x, target_y, target_z, target_o, comment
conditions:    SourceTypeOrReferenceId, SourceGroup, SourceEntry, SourceId, ElseGroup,
               ConditionTypeOrReference, ConditionTarget, ConditionValue1..3,
               ConditionStringValue1, NegativeCondition, ErrorType, ErrorTextId,
               ScriptName, Comment
```

`smart_scripts.entryorguid` > 0 is a template entry; < 0 is a negated spawn guid. Requires
`creature_template.AIName = 'SmartAI'`.

## Grouping, pooling, events, formations

```
spawn_group_template: groupId, groupName, groupFlags
spawn_group:          groupId, spawnType, spawnId          -- spawnType 0 creature, 1 gameobject
linked_respawn:       guid, linkedGuid, linkType
pool_template:        entry, max_limit, description
pool_members:         type, spawnId, poolSpawnId, chance, description
creature_formations:  leaderGUID, memberGUID, dist, angle, groupAI, point_1, point_2
game_event:           eventEntry, start_time, end_time, occurence, length, holiday,
                      holidayStage, description, world_event, announce
game_event_creature:   eventEntry, guid
game_event_gameobject: eventEntry, guid
```

## Locales, quests, transport

```
creature_template_locale:   entry, locale, Name, Title, VerifiedBuild
gameobject_template_locale: entry, locale, name, castBarCaption, VerifiedBuild
creature_queststarter:      id, quest
creature_questender:        id, quest
game_tele:                  id, position_x, position_y, position_z, orientation, map, name
transports:                 guid, entry, name, ScriptName
vehicle_template_accessory: entry, accessory_entry, seat_id, minion, description,
                            summontype, summontimer
```

`locale` values: `koKR`, `frFR`, `deDE`, `zhCN`, `zhTW`, `esES`, `esMX`, `ruRU`. enUS/enGB
is the base table row and is not stored as a locale.

`TaxiPath.dbc` / `TaxiPathNode.dbc` are client-side DBC and **not** editable through the world
DB. The UI must distinguish DBC-driven from DB-driven data.

## Enums for display

- `CreatureType`: 0 none, 1 Beast, 2 Dragonkin, 3 Demon, 4 Elemental, 5 Giant, 6 Undead,
  7 Humanoid, 8 Critter, 9 Mechanical, 10 Not specified, 11 Totem, 12 Non-combat Pet,
  13 Gas Cloud.
- `rank`: 0 Normal, 1 Elite, 2 Rare Elite, 3 Boss, 4 Rare, 5 Trivial.
- `unit_class`: 1 Warrior, 2 Paladin, 4 Rogue, 8 Mage.
- `MovementType`: 0 Idle, 1 Random (bounded by `wander_distance`), 2 Waypoint.
- `phaseMask`: bitmask, default 1. Visible when the player's phase bit intersects.
- `GAMEOBJECT_TYPE` 0–26: DOOR, BUTTON, QUESTGIVER, CHEST, BINDER, GENERIC, TRAP, CHAIR,
  SPELL_FOCUS, TEXT, GOOBER, TRANSPORT, AREADAMAGE, CAMERA, MAP_OBJECT, MO_TRANSPORT,
  DUEL_ARBITER, FISHINGNODE, RITUAL, MAILBOX, (20: `DO_NOT_USE` in current TC docs,
  `AUCTIONHOUSE` in older references — unresolved), GUARDPOST, SPELLCASTER, MEETINGSTONE,
  FLAGSTAND, FISHINGHOLE, FLAGDROP. Values past 26 exist (CAPTURE_POINT, AURA_GENERATOR,
  DUNGEON_DIFFICULTY, BARBER_CHAIR, DESTRUCTIBLE_BUILDING, GUILD_BANK, TRAPDOOR) but the
  exact numbering is **unverified** — read it from the core enum, don't hardcode.

## Coordinates

ADT tile is 533.33333 yards; the map is 64×64 tiles; origin at map centre, so X/Y span
±17066.66656.

```
blockX = floor(32 - (x / 533.33333))
blockY = floor(32 - (y / 533.33333))
```

ADT placement matrices use `posx = 32*533.33333 - position[0]` and
`posz = 32*533.33333 - position[2]`. 3.3.5a is ADT v18. MDDF stores `.mdx`/`.mdl` names that
must be rewritten to `.m2` when read.

`displayId` resolution: creatures via `CreatureDisplayInfo.dbc` → `CreatureModelData.dbc`;
gameobjects via `GameObjectDisplayInfo.dbc`.

### Float precision — measured, not theoretical

`position_x/y/z` and `orientation` are `float`, not `double`. Storing `-9512.345` and reading
it back gives a delta of **0.00027** yards; at `y = 83.117` the delta is `0.0000032`. The error
scales with magnitude, and world coordinates reach ±17066.

Consequences for the emitter:

- **Never round-trip-verify by exact equality.** It will report false failures on correct
  output. Compare within tolerance; `0.01` is safe at world magnitudes, and the representable
  step near ±9500 is roughly `1e-3`.
- Do not accumulate transforms in `float`. Compute in `double`, narrow once on write.
- A moved chunk (M4) that applies a delta repeatedly will drift. Apply the delta to the
  original value, not to the previously-written one.

Verified by `tools/dev-db/03_example_changeset.sql`, which is also the reference for changeset
shape and idempotency.

## Verification gaps

Not yet measured — do so before relying on them:

- `quest_template`, `quest_template_addon`, `quest_poi`, `quest_poi_points`
- `gameobject_queststarter` / `gameobject_questender`
- Index and foreign-key definitions (only columns were read)
- `GAMEOBJECT_TYPE` numbering above 26
- Whether any AzerothCore target reintroduces `waypoints` / `spawndist`
