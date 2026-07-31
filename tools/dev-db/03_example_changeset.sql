-- Reference changeset. This is the exact shape the M2 emitter must produce, and the
-- known-good fixture to check new changeset output against.
--
--   Apply:  mysql noggit_dev_world -e "source tools/dev-db/03_example_changeset.sql"
--
-- Three properties every emitted changeset must have, all demonstrated here:
--
--   1. Idempotent. DELETE precedes INSERT for every table touched, so applying twice
--      leaves identical rows and raises no error.
--   2. Variable-driven. GUIDs and entries are declared once at the top, TDB style, so a
--      reviewer can retarget the whole changeset by editing the header.
--   3. Column-explicit. Never positional. The column list below is the core's own
--      WORLD_INS_CREATURE shape, which is what the server writes when a GM spawns a
--      creature, plus the columns Noggit needs on top.
--
-- zoneId and areaId are deliberately omitted: ObjectMgr::LoadCreatures does not read them
-- and the core's own INSERT does not write them. The server derives them.

SET @CGUID     := 9000004;
SET @CTEMPLATE := 990001;
SET @PATH      := 90000040;

-- Deliberately awkward coordinates. `position_x` is a FLOAT, so -9512.345 is NOT exactly
-- representable and will not compare equal to the literal on read-back. An emitter that
-- round-trips by exact equality will report false failures; compare within tolerance
-- instead. See the verification block at the bottom.
SET @POSX := -9512.345;
SET @POSY :=    83.117;
SET @POSZ :=    58.271;
SET @ORI  :=     4.7124;   -- 3*pi/2

-- creature ------------------------------------------------------------------------------
DELETE FROM `creature` WHERE `guid` = @CGUID;
INSERT INTO `creature`
  (`guid`, `id`, `map`, `spawnMask`, `phaseMask`, `modelid`, `equipment_id`,
   `position_x`, `position_y`, `position_z`, `orientation`,
   `spawntimesecs`, `wander_distance`, `currentwaypoint`, `curhealth`, `curmana`,
   `MovementType`, `npcflag`, `unit_flags`, `dynamicflags`) VALUES
  (@CGUID, @CTEMPLATE, 0, 1, 1, 0, 0,
   @POSX, @POSY, @POSZ, @ORI,
   120, 0.0, 0, 1, 0,
   2, 0, 0, 0);

-- creature_addon ------------------------------------------------------------------------
-- MovementType 2 requires a path, and the path is bound here, not on the template.
-- Note the column names: bytes1/bytes2 do not exist on this schema.
DELETE FROM `creature_addon` WHERE `guid` = @CGUID;
INSERT INTO `creature_addon`
  (`guid`, `path_id`, `mount`, `MountCreatureID`, `StandState`, `AnimTier`,
   `VisFlags`, `SheathState`, `PvPFlags`, `emote`, `visibilityDistanceType`, `auras`) VALUES
  (@CGUID, @PATH, 0, 0, 0, 0, 0, 1, 0, 0, 0, NULL);

-- waypoint_data -------------------------------------------------------------------------
-- `point` starts at 1 and must stay contiguous. `wpguid` is core-managed and never authored.
DELETE FROM `waypoint_data` WHERE `id` = @PATH;
INSERT INTO `waypoint_data`
  (`id`, `point`, `position_x`, `position_y`, `position_z`, `orientation`,
   `delay`, `move_type`, `action`, `action_chance`) VALUES
  (@PATH, 1, @POSX,        @POSY,        @POSZ, NULL,    0, 0, 0, 100),
  (@PATH, 2, @POSX + 8.0,  @POSY,        @POSZ, NULL,    0, 0, 0, 100),
  (@PATH, 3, @POSX + 8.0,  @POSY + 8.0,  @POSZ, NULL, 1500, 0, 0, 100),
  (@PATH, 4, @POSX,        @POSY + 8.0,  @POSZ, NULL,    0, 1, 0, 100);

-- Verification --------------------------------------------------------------------------
-- Round-trip within float tolerance, not by equality. 1e-2 is generous for world
-- coordinates at this magnitude; a FLOAT holds ~7 significant digits, so at ~9500 the
-- representable step is roughly 1e-3.
SELECT
  `guid`,
  `position_x`,
  ABS(`position_x` - @POSX) AS dx,
  ABS(`position_y` - @POSY) AS dy,
  ABS(`position_z` - @POSZ) AS dz,
  IF(ABS(`position_x` - @POSX) < 0.01
     AND ABS(`position_y` - @POSY) < 0.01
     AND ABS(`position_z` - @POSZ) < 0.01, 'ROUNDTRIP OK', '** ROUNDTRIP FAILED **') AS verdict
FROM `creature` WHERE `guid` = @CGUID;

-- Row counts must be stable across repeated application.
SELECT 'creature' AS tbl, COUNT(*) AS n FROM `creature`       WHERE `guid` = @CGUID
UNION ALL SELECT 'creature_addon', COUNT(*) FROM `creature_addon` WHERE `guid` = @CGUID
UNION ALL SELECT 'waypoint_data',  COUNT(*) FROM `waypoint_data`  WHERE `id`   = @PATH;

-- Waypoint points must be contiguous from 1.
SELECT MIN(`point`) AS first_, MAX(`point`) AS last_, COUNT(*) AS n,
       IF(COUNT(*) = MAX(`point`) - MIN(`point`) + 1 AND MIN(`point`) = 1,
          'CONTIGUOUS OK', '** GAPS IN point **') AS verdict
FROM `waypoint_data` WHERE `id` = @PATH;
