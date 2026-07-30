-- Synthetic fixtures for noggit_dev_world. Safe to re-run: every block deletes first.
--
-- No Blizzard or client data is copied here. The only real values are display IDs, which are
-- plain integer keys into the client's DBC files -- they have to be real or M1's
-- displayId -> CreatureDisplayInfo -> CreatureModelData -> model-path chain cannot be tested
-- end to end. Bounding/reach figures below are invented, not copied.
--
-- All fixtures sit in map 0, ADT tile 49_31 (Elwynn Forest). That tile was chosen because the
-- coordinate formula was verified against real spawns there:
--   blockX = FLOOR(32 - (-9500 / 533.33333)) = 49
--   blockY = FLOOR(32 - (   70 / 533.33333)) = 31
--
-- ID ranges are deliberately far above anything TDB uses:
--   creature_template.entry     990001-990003
--   gameobject_template.entry   990101
--   creature.guid               9000001-9000003
--   gameobject.guid             9000101
--   waypoint_data.id            90000030   (= creature guid 9000003 * 10, the TDB convention)

SET @T1 := 990001;   -- idle sentinel
SET @T2 := 990002;   -- random wanderer
SET @T3 := 990003;   -- waypoint follower, SmartAI
SET @GT := 990101;   -- gameobject template

SET @C1 := 9000001;
SET @C2 := 9000002;
SET @C3 := 9000003;
SET @G1 := 9000101;
SET @PATH := 90000030;

-- Provenance marker -------------------------------------------------------------------
DELETE FROM `version`;
INSERT INTO `version` (`core_version`, `core_revision`, `db_version`, `cache_id`)
VALUES ('noggit-red dev fixture (structure derived from TDB 335.25101)', 'dev', 'TDB 335.25101 (dev copy)', 25101);

-- Model info --------------------------------------------------------------------------
DELETE FROM `creature_model_info` WHERE `DisplayID` IN (1126, 11686, 16480);
INSERT INTO `creature_model_info`
  (`DisplayID`, `BoundingRadius`, `CombatReach`, `Gender`, `DisplayID_Other_Gender`) VALUES
  ( 1126, 0.75, 1.5, 2, 0),
  (11686, 0.50, 1.0, 2, 0),
  (16480, 1.00, 1.5, 0, 0);

-- Creature templates ------------------------------------------------------------------
DELETE FROM `creature_template` WHERE `entry` IN (@T1, @T2, @T3);
INSERT INTO `creature_template`
  (`entry`, `modelid1`, `name`, `subname`, `minlevel`, `maxlevel`, `faction`,
   `unit_class`, `rank`, `type`, `AIName`, `MovementType`, `RegenHealth`) VALUES
  (@T1, 1126,  'NOGGIT Test Sentinel',   'idle',     10, 10, 35, 1, 0, 7, '',        0, 1),
  (@T2, 11686, 'NOGGIT Test Wanderer',   'random',   10, 10, 35, 1, 0, 7, '',        1, 1),
  (@T3, 16480, 'NOGGIT Test Pathwalker', 'waypoint', 10, 10, 35, 1, 0, 7, 'SmartAI', 2, 1);

-- Creature spawns ---------------------------------------------------------------------
-- MovementType here must agree with wander_distance or the core rejects the row:
--   type 0 (idle)   requires wander_distance = 0
--   type 1 (random) requires wander_distance > 0
-- See ObjectMgr::LoadCreatures validation in the core.
DELETE FROM `creature` WHERE `guid` IN (@C1, @C2, @C3);
INSERT INTO `creature`
  (`guid`, `id`, `map`, `zoneId`, `areaId`, `spawnMask`, `phaseMask`, `modelid`, `equipment_id`,
   `position_x`, `position_y`, `position_z`, `orientation`, `spawntimesecs`, `wander_distance`,
   `currentwaypoint`, `curhealth`, `curmana`, `MovementType`) VALUES
  (@C1, @T1, 0, 0, 0, 1, 1, 0, 0, -9500.0, 70.0, 58.0, 0.0000, 120, 0.0, 0, 1, 0, 0),
  (@C2, @T2, 0, 0, 0, 1, 1, 0, 0, -9505.0, 75.0, 58.0, 1.5708, 120, 5.0, 0, 1, 0, 1),
  (@C3, @T3, 0, 0, 0, 1, 1, 0, 0, -9510.0, 80.0, 58.0, 3.1416, 120, 0.0, 0, 1, 0, 2);

-- zoneId/areaId are left 0 on purpose: the core derives them and its own INSERT statement
-- omits them entirely. Do not compute them in the editor.

-- Bind the pathwalker to a waypoint path ----------------------------------------------
DELETE FROM `creature_addon` WHERE `guid` = @C3;
INSERT INTO `creature_addon` (`guid`, `path_id`) VALUES (@C3, @PATH);

-- Waypoint path: a 5-node loop, ~10 yards square ---------------------------------------
DELETE FROM `waypoint_data` WHERE `id` = @PATH;
INSERT INTO `waypoint_data`
  (`id`, `point`, `position_x`, `position_y`, `position_z`, `orientation`, `delay`,
   `move_type`, `action`, `action_chance`) VALUES
  (@PATH, 1, -9510.0, 80.0, 58.0, NULL,    0, 0, 0, 100),
  (@PATH, 2, -9500.0, 80.0, 58.0, NULL,    0, 0, 0, 100),
  (@PATH, 3, -9500.0, 90.0, 58.0, NULL, 2000, 0, 0, 100),
  (@PATH, 4, -9510.0, 90.0, 58.0, NULL,    0, 1, 0, 100),
  (@PATH, 5, -9510.0, 80.0, 58.0, NULL,    0, 0, 0, 100);

-- Gameobject --------------------------------------------------------------------------
DELETE FROM `gameobject_template` WHERE `entry` = @GT;
INSERT INTO `gameobject_template`
  (`entry`, `type`, `displayId`, `name`, `size`, `AIName`, `ScriptName`) VALUES
  (@GT, 5, 26, 'NOGGIT Test Marker', 1, '', '');

-- rotation0..3 is a quaternion, not Euler. For orientation o:
--   rotation2 = sin(o/2), rotation3 = cos(o/2); o = 0 gives (0, 0, 0, 1).
DELETE FROM `gameobject` WHERE `guid` = @G1;
INSERT INTO `gameobject`
  (`guid`, `id`, `map`, `zoneId`, `areaId`, `spawnMask`, `phaseMask`,
   `position_x`, `position_y`, `position_z`, `orientation`,
   `rotation0`, `rotation1`, `rotation2`, `rotation3`,
   `spawntimesecs`, `animprogress`, `state`) VALUES
  (@G1, @GT, 0, 0, 0, 1, 1, -9515.0, 85.0, 58.0, 0.0, 0, 0, 0, 1, 300, 100, 1);

-- Report --------------------------------------------------------------------------------
SELECT 'creature'          AS fixture, COUNT(*) AS rows_ FROM `creature`           WHERE `guid` BETWEEN 9000001 AND 9000003
UNION ALL SELECT 'creature_template',  COUNT(*) FROM `creature_template`  WHERE `entry` BETWEEN 990001 AND 990003
UNION ALL SELECT 'creature_model_info',COUNT(*) FROM `creature_model_info`WHERE `DisplayID` IN (1126, 11686, 16480)
UNION ALL SELECT 'creature_addon',     COUNT(*) FROM `creature_addon`     WHERE `guid` = 9000003
UNION ALL SELECT 'waypoint_data',      COUNT(*) FROM `waypoint_data`      WHERE `id` = 90000030
UNION ALL SELECT 'gameobject_template',COUNT(*) FROM `gameobject_template`WHERE `entry` = 990101
UNION ALL SELECT 'gameobject',         COUNT(*) FROM `gameobject`         WHERE `guid` = 9000101;
