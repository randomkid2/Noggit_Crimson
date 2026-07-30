// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_DATABASE_SPAWNTYPES_HPP
#define NOGGIT_DATABASE_SPAWNTYPES_HPP

#include <noggit/database/TileCoordinates.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace Noggit::Database
{
  // Movement generator selection, from creature.MovementType.
  enum class MovementType : std::uint8_t
  {
    IDLE = 0,
    RANDOM = 1,     // bounded by the wander distance
    WAYPOINT = 2
  };

  // waypoint_data.move_type
  enum class WaypointMoveType : std::uint8_t
  {
    WALK = 0,
    RUN = 1,
    LAND = 2,
    TAKEOFF = 3
  };

  // A row of `creature`.
  //
  // zoneId and areaId are deliberately absent. ObjectMgr::LoadCreatures does not read them and
  // the core's own INSERT omits them -- the server derives both. Carrying them here would
  // invite writing them back.
  struct CreatureSpawn
  {
    std::uint32_t guid = 0;
    std::uint32_t id = 0;            // creature_template.entry
    std::uint16_t map = 0;
    std::uint8_t spawn_mask = 1;
    std::uint32_t phase_mask = 1;
    std::uint32_t model_id = 0;      // 0 means "use the template's model"
    std::int8_t equipment_id = 0;    // -1 random, 0 none, >0 a creature_equip_template ID

    WorldPosition position;
    double orientation = 0.0;

    std::uint32_t spawn_time_secs = 120;
    double wander_distance = 0.0;
    std::uint32_t current_waypoint = 0;
    std::uint32_t cur_health = 1;
    std::uint32_t cur_mana = 0;
    MovementType movement_type = MovementType::IDLE;
    std::uint32_t npc_flag = 0;
    std::uint32_t unit_flags = 0;
    std::uint32_t dynamic_flags = 0;

    // From creature_addon, when present. path_id binds this spawn to a waypoint path.
    bool has_addon = false;
    std::uint32_t path_id = 0;
  };

  // A row of `gameobject`. Rotation is a quaternion, not Euler angles.
  struct GameObjectSpawn
  {
    std::uint32_t guid = 0;
    std::uint32_t id = 0;            // gameobject_template.entry
    std::uint16_t map = 0;
    std::uint8_t spawn_mask = 1;
    std::uint32_t phase_mask = 1;

    WorldPosition position;
    double orientation = 0.0;
    TileCoordinates::Quaternion rotation;

    std::int32_t spawn_time_secs = 300;
    std::uint32_t anim_progress = 100;
    std::uint32_t state = 1;
  };

  // One node of a waypoint path. `point` is 1-based and must stay contiguous.
  //
  // wpguid is absent on purpose: it is core-managed and authoring it corrupts the path.
  struct WaypointNode
  {
    std::uint32_t point = 0;
    WorldPosition position;

    bool has_orientation = false;    // the column is nullable and usually NULL
    double orientation = 0.0;

    std::uint32_t delay_ms = 0;
    WaypointMoveType move_type = WaypointMoveType::WALK;
    std::uint32_t action = 0;
    std::uint32_t action_chance = 100;
  };

  // A complete path from waypoint_data, keyed by id.
  struct WaypointPath
  {
    std::uint32_t id = 0;
    std::vector<WaypointNode> nodes;

    // True when points run 1..n with no gaps or duplicates. The core walks them in order and
    // a gap silently truncates the path rather than erroring.
    bool isContiguous() const;

    // Renumbers points to 1..n in their current vector order.
    void renumber();
  };

  // Everything anchored to one ADT tile.
  struct TileSpawns
  {
    TileIndex tile;
    std::uint16_t map = 0;
    std::vector<CreatureSpawn> creatures;
    std::vector<GameObjectSpawn> gameobjects;
    std::vector<WaypointPath> paths;

    bool empty() const
    {
      return creatures.empty() && gameobjects.empty() && paths.empty();
    }
  };

  // Validation outcome for a spawn about to be written.
  //
  // These mirror checks the core performs at load time. A row that fails them is accepted by
  // MySQL and then rejected or silently corrected by the server, which is a far worse failure
  // mode than refusing to emit it.
  struct ValidationIssue
  {
    enum class Severity { WARNING, ERROR };

    Severity severity = Severity::ERROR;
    std::string message;
  };

  namespace SpawnValidation
  {
    // MovementType 0 requires wander_distance == 0; MovementType 1 requires > 0.
    // MovementType 2 requires an addon path_id.
    std::vector<ValidationIssue> validate(CreatureSpawn const& spawn);
    std::vector<ValidationIssue> validate(GameObjectSpawn const& spawn);
    std::vector<ValidationIssue> validate(WaypointPath const& path);

    bool hasErrors(std::vector<ValidationIssue> const& issues);
  }
}

#endif
