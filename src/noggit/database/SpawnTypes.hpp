// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_DATABASE_SPAWNTYPES_HPP
#define NOGGIT_DATABASE_SPAWNTYPES_HPP

#include <noggit/database/TileCoordinates.hpp>

#include <cstddef>
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

  // Where the display id a spawn renders with came from.
  //
  // Not cosmetic. A creature drawn with its template's model is showing something no row of
  // `creature` states, so an editor offering to change that model has to know whether the change
  // means writing creature.modelid or leaving it at 0. And a spawn that resolved to nothing has
  // to be drawn as a marker rather than quietly omitted, or a tile whose template data is broken
  // reads as an empty tile -- the same failure mode the query bounds are so careful about.
  enum class DisplayIdOrigin : std::uint8_t
  {
    UNRESOLVED = 0,   // neither the spawn nor its template named a model
    SPAWN,            // creature.modelid, non-zero -- overrides the template
    TEMPLATE          // creature_template.modelid1..4, or creature_template_model
  };

  // What creature_template contributes to one spawn: the display id to draw it with, and the
  // name to label it with.
  //
  // A companion struct nested inside CreatureSpawn rather than fields flattened into it, for the
  // same reason zoneId and areaId are absent from CreatureSpawn entirely: that struct mirrors a
  // row of `creature` and is the shape the changeset emitter writes back from. Nothing here is a
  // column of `creature` -- it all arrives from a joined template table -- and flattening it
  // would put derived values in the same namespace as authored ones, where the emitter will
  // eventually pick one up and name a column that does not exist.
  struct CreatureTemplateInfo
  {
    // The id to render with: creature.modelid when non-zero, otherwise the template's first
    // non-zero model. 0 means nothing resolved, so there is no model to draw.
    std::uint32_t display_id = 0;
    DisplayIdOrigin display_id_origin = DisplayIdOrigin::UNRESOLVED;

    // Distinct non-zero models the template offered. TrinityCore rolls one of them per spawn at
    // runtime; the editor takes the first and records here that there were others, because a
    // viewport showing a different model each time a tile is opened would be unusable, and a
    // changeset diff that moved with a dice roll would be unreviewable.
    std::size_t template_model_count = 0;

    // creature_template.name, for UI labels only. Empty when the template row or the column is
    // absent, which is worth surfacing on its own: it means this spawn names no template.
    std::string name;

    bool templateOffersAlternatives() const { return template_model_count > 1; }
  };

  // What gameobject_template contributes to one spawn.
  //
  // `gameobject` has no per-spawn display column at all, so without this join a gameobject can
  // never be resolved to a model no matter what else is read. Nested for the same reason as
  // CreatureTemplateInfo.
  struct GameObjectTemplateInfo
  {
    // gameobject_template.displayId. 0 is legal and common, and means there is nothing to draw.
    std::uint32_t display_id = 0;

    // GAMEOBJECT_TYPE. Carried because several types have no renderable model whatever their
    // displayId claims -- see SpawnDisplay::typeHasRenderableModel.
    std::uint32_t type = 0;

    std::string name;
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

    // Joined from creature_template. Derived, never written back -- see CreatureTemplateInfo.
    CreatureTemplateInfo template_info;
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

    // Joined from gameobject_template. The only source of a display id there is for a
    // gameobject -- see GameObjectTemplateInfo.
    GameObjectTemplateInfo template_info;
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

  // Model resolution: how a spawn plus its template becomes a display id, and whether that
  // display id is something the editor can actually draw.
  //
  // Pure functions over values a spawn already carries, so the rules are testable with no
  // database and live in exactly one place. The reader calls them while decoding a row; the
  // renderer calls them again when deciding what to skip.
  namespace SpawnDisplay
  {
    // The whole resolution rule: creature.modelid when non-zero, otherwise the first non-zero
    // entry of `template_candidates`, otherwise nothing.
    //
    // Candidates arrive in the template's own order -- modelid1..4, or creature_template_model
    // ordered by Idx. Zeroes and repeats are ignored, so a caller can hand over a fixed-width row
    // of columns without filtering it first, which is what lets the select list keep one shape
    // across both schema variants.
    CreatureTemplateInfo resolveCreatureTemplateInfo
      ( std::uint32_t spawn_model_id
      , std::vector<std::uint32_t> const& template_candidates
      , std::string template_name
      );

    // False for the GAMEOBJECT_TYPE values that have no model the editor can draw: 6 TRAP,
    // 11 TRANSPORT, 12 AREADAMAGE, 13 CAMERA, 15 MO_TRANSPORT and 18 RITUAL. Everything else is
    // treated as renderable, since displayId 0 already covers "this entry has no model".
    bool typeHasRenderableModel(std::uint32_t gameobject_type);

    // Whether there is anything to draw: a resolved display id, and for a gameobject a type that
    // can carry a model at all.
    bool isRenderable(CreatureSpawn const& spawn);
    bool isRenderable(GameObjectSpawn const& spawn);
  }

  // Validation outcome for a spawn about to be written.
  //
  // These mirror checks the core performs at load time. A row that fails them is accepted by
  // MySQL and then rejected or silently corrected by the server, which is a far worse failure
  // mode than refusing to emit it.
  struct ValidationIssue
  {
    // BLOCKING, not ERROR.
    //
    // <wingdi.h> does `#define ERROR 0`, and Qt pulls in windows.h, so an enumerator named ERROR
    // expands to `0` and turns this line into `enum class Severity { WARNING, 0 }` -- a syntax
    // error in every translation unit that reaches a Windows header before this one. That is most
    // of the application. It went unnoticed only because this header began life included solely
    // by the database layer and its Qt-free test target; the first UI file to include it broke
    // the build. Do not "restore" the shorter name.
    enum class Severity { WARNING, BLOCKING };

    Severity severity = Severity::BLOCKING;
    std::string message;
  };

  namespace SpawnValidation
  {
    // Appends one issue to a list under construction.
    //
    // Exposed rather than kept file-local because more than one module builds these lists: the
    // validators below, and ChunkTransform reporting what a move did to each entity. Two copies
    // of a three-line constructor is not a maintenance cost worth worrying about, but two places
    // that decide what a "warning" is are -- a copy that defaulted the severity differently would
    // make hasErrors() answer differently for the same complaint depending on which module raised
    // it, and that gates whether a changeset is allowed to build at all.
    void addIssue
      ( std::vector<ValidationIssue>& issues
      , ValidationIssue::Severity severity
      , std::string message
      );

    void addError(std::vector<ValidationIssue>& issues, std::string message);
    void addWarning(std::vector<ValidationIssue>& issues, std::string message);

    // MovementType 0 requires wander_distance == 0; MovementType 1 requires > 0.
    // MovementType 2 requires an addon path_id.
    std::vector<ValidationIssue> validate(CreatureSpawn const& spawn);
    std::vector<ValidationIssue> validate(GameObjectSpawn const& spawn);
    std::vector<ValidationIssue> validate(WaypointPath const& path);

    bool hasErrors(std::vector<ValidationIssue> const& issues);
  }
}

#endif
