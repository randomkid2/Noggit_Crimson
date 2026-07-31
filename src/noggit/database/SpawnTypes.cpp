// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/database/SpawnTypes.hpp>
#include <noggit/database/TileCoordinates.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <locale>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using namespace Noggit::Database;

namespace
{
  // rotation0..3 are FLOAT columns, so a quaternion that was exactly unit length in double
  // loses a little on the way through the database. The measured float step at magnitude 1 is
  // about 6e-8, so 1e-3 is several orders of magnitude wider than any legitimate loss while
  // still catching what actually goes wrong here: an all-zero rotation, or Euler angles
  // written into the quaternion columns by mistake.
  constexpr double QUATERNION_LENGTH_TOLERANCE = 1e-3;

  // waypoint_data.action_chance is a percentage.
  constexpr std::uint32_t MAX_ACTION_CHANCE = 100;

  // creature.equipment_id: -1 random, 0 none, >0 a creature_equip_template ID.
  constexpr int MIN_EQUIPMENT_ID = -1;

  // GAMEOBJECT_TYPE values with nothing the editor can draw, from docs/schema-335.md, which
  // measured them rather than copying a published table: 6 TRAP and 18 RITUAL are invisible in
  // the client, 11 TRANSPORT and 15 MO_TRANSPORT move (and the core refuses manual creation of
  // the latter outright), and 12 AREADAMAGE and 13 CAMERA have no model at all.
  //
  // Six named values rather than a full GAMEOBJECT_TYPE enum, on purpose. docs/schema-335.md
  // records that the numbering above 26 is unverified, so a complete enum written from a
  // reference would be precisely the hardcode-from-documentation this project has already been
  // bitten by four times. All six of these sit inside the verified 0..26 range.
  constexpr std::uint32_t GAMEOBJECT_TYPE_TRAP = 6;
  constexpr std::uint32_t GAMEOBJECT_TYPE_TRANSPORT = 11;
  constexpr std::uint32_t GAMEOBJECT_TYPE_AREADAMAGE = 12;
  constexpr std::uint32_t GAMEOBJECT_TYPE_CAMERA = 13;
  constexpr std::uint32_t GAMEOBJECT_TYPE_MO_TRANSPORT = 15;
  constexpr std::uint32_t GAMEOBJECT_TYPE_RITUAL = 18;

  // std::to_string always emits six decimals, which makes every message about a whole number
  // read as "5.000000". Trim to something a reviewer can scan.
  std::string number(double value)
  {
    if (std::isnan(value))
    {
      return "NaN";
    }

    if (!std::isfinite(value))
    {
      return value > 0.0 ? "inf" : "-inf";
    }

    // Not std::to_string: it is specified as sprintf("%f"), so it reads LC_NUMERIC. Qt calls
    // setlocale(LC_ALL, "") when a QCoreApplication is constructed, and nothing here resets
    // LC_NUMERIC, so under a comma-decimal locale every number in a validation message would
    // come out as "4,712400" -- and the '.' guard below would then never fire, leaving the
    // trailing zeros in place too. Every other formatter in this layer imbues the classic
    // locale for exactly this reason.
    std::ostringstream stream;
    stream.imbue(std::locale::classic());

    // Enough significant digits to distinguish any float the caller could be complaining about.
    // The previous fixed six DECIMALS collapsed anything below 5e-7 to "0", so a spawn with
    // wander_distance = 1e-7 produced "requires wander_distance = 0" while reporting the value
    // as 0 -- a message asserting the value is already what it demands.
    stream << std::setprecision(9) << value;

    std::string text (stream.str());

    // Canonicalise negative zero. fmod and unary minus both preserve the sign, so -0.0 reaches
    // here and renders as "-0", which reads as a defect in review and differs textually from a
    // stored 0.
    if (text == "-0")
    {
      return "0";
    }

    return text;
  }

  std::string positionText(WorldPosition const& position)
  {
    return "(" + number(position.x) + ", " + number(position.y) + ", " + number(position.z)
      + ")";
  }

  std::string extentText()
  {
    return "+/-" + number(MAP_HALF_EXTENT);
  }

  // TileCoordinates::isValidPosition is the authority on the horizontal extent; it is not
  // duplicated here so the two can never disagree. It says nothing about z, and it cannot
  // speak for a NaN that would be emitted into SQL as a literal the server rejects, so both
  // are checked explicitly.
  bool positionIsUsable(WorldPosition const& position)
  {
    return std::isfinite(position.x)
      && std::isfinite(position.y)
      && std::isfinite(position.z)
      && TileCoordinates::isValidPosition(position.x, position.y);
  }
}

void SpawnValidation::addIssue
  ( std::vector<ValidationIssue>& issues
  , ValidationIssue::Severity severity
  , std::string message
  )
{
  ValidationIssue issue;
  issue.severity = severity;
  issue.message = std::move(message);
  issues.push_back(std::move(issue));
}

void SpawnValidation::addError(std::vector<ValidationIssue>& issues, std::string message)
{
  addIssue(issues, ValidationIssue::Severity::BLOCKING, std::move(message));
}

void SpawnValidation::addWarning(std::vector<ValidationIssue>& issues, std::string message)
{
  addIssue(issues, ValidationIssue::Severity::WARNING, std::move(message));
}

bool WaypointPath::isContiguous() const
{
  // An empty path is reported as non-contiguous rather than vacuously contiguous: the question
  // this answers is "can the core walk these nodes in order", and there is nothing to walk.
  // validate() reports the emptiness on its own so the caller still gets the real reason.
  if (nodes.empty())
  {
    return false;
  }

  // Deliberately index-based rather than set-based. Vector order is the authoritative order
  // everywhere else -- SpawnQuery::loadPath returns nodes ordered by point, the editor renders
  // them in vector order and renumber() assigns by vector order -- so a path whose point values
  // are the right set in the wrong sequence is exactly as broken as one with a gap, and
  // renumber() would silently reroute it. For a path read back from the database the two
  // definitions coincide.
  for (std::size_t i = 0; i < nodes.size(); ++i)
  {
    if (nodes[i].point != static_cast<std::uint32_t>(i + 1))
    {
      return false;
    }
  }

  return true;
}

void WaypointPath::renumber()
{
  std::uint32_t point = 0;

  for (WaypointNode& node : nodes)
  {
    node.point = ++point;
  }
}

CreatureTemplateInfo SpawnDisplay::resolveCreatureTemplateInfo
  ( std::uint32_t spawn_model_id
  , std::vector<std::uint32_t> const& template_candidates
  , std::string template_name
  )
{
  CreatureTemplateInfo info;
  info.name = std::move(template_name);

  // Distinct non-zero candidates, kept in the template's own order.
  //
  // Distinct rather than a raw count because modelid1 == modelid2 is one model listed twice: the
  // core would roll the same display id either way, so there is no ambiguity to warn about, and
  // reporting one would train the user to ignore the warning. Linear search rather than a set:
  // there are at most four of these and the order is the whole point.
  std::vector<std::uint32_t> distinct;
  distinct.reserve(template_candidates.size());

  for (std::uint32_t candidate : template_candidates)
  {
    if (candidate == 0)
    {
      continue;
    }

    if (std::find(distinct.begin(), distinct.end(), candidate) == distinct.end())
    {
      distinct.push_back(candidate);
    }
  }

  info.template_model_count = distinct.size();

  // creature.modelid wins whenever it is set. That is what the core does, and it is also the only
  // one of the two values the editor can write, so showing the template's model for a spawn that
  // overrides it would misrepresent what a changeset would actually change.
  if (spawn_model_id != 0)
  {
    info.display_id = spawn_model_id;
    info.display_id_origin = DisplayIdOrigin::SPAWN;
    return info;
  }

  if (!distinct.empty())
  {
    // First non-zero, always. The core rolls one of the template's models per spawn at runtime;
    // the editor has to be deterministic, so it takes the first and leaves
    // templateOffersAlternatives() to say that the rest exist.
    info.display_id = distinct.front();
    info.display_id_origin = DisplayIdOrigin::TEMPLATE;
    return info;
  }

  // Neither side named a model. display_id stays 0 and the origin stays UNRESOLVED, which is what
  // tells the renderer to draw a marker instead of dropping the spawn.
  return info;
}

bool SpawnDisplay::typeHasRenderableModel(std::uint32_t gameobject_type)
{
  switch (gameobject_type)
  {
    case GAMEOBJECT_TYPE_TRAP:
    case GAMEOBJECT_TYPE_TRANSPORT:
    case GAMEOBJECT_TYPE_AREADAMAGE:
    case GAMEOBJECT_TYPE_CAMERA:
    case GAMEOBJECT_TYPE_MO_TRANSPORT:
    case GAMEOBJECT_TYPE_RITUAL:
      return false;

    default:
      // Anything else is assumed renderable. An unknown type is far more likely to be a normal
      // object on a schema generation this build has not seen than one of the six above, and a
      // displayId of 0 already excludes the entries that genuinely have no model.
      return true;
  }
}

bool SpawnDisplay::isRenderable(CreatureSpawn const& spawn)
{
  return spawn.template_info.display_id != 0;
}

bool SpawnDisplay::isRenderable(GameObjectSpawn const& spawn)
{
  return spawn.template_info.display_id != 0
    && typeHasRenderableModel(spawn.template_info.type);
}

std::vector<ValidationIssue> SpawnValidation::validate(CreatureSpawn const& spawn)
{
  std::vector<ValidationIssue> issues;

  if (spawn.guid == 0)
  {
    addError
      ( issues
      , "creature guid is 0. Every spawn needs a unique non-zero guid; 0 is the core's"
        " 'no spawn' sentinel and a row carrying it is unreachable."
      );
  }

  if (spawn.id == 0)
  {
    addError
      ( issues
      , "creature guid " + std::to_string(spawn.guid) + " has id 0. It must name a row in"
        " creature_template."
      );
  }

  if (!positionIsUsable(spawn.position))
  {
    addError
      ( issues
      , "creature guid " + std::to_string(spawn.guid) + " sits at "
        + positionText(spawn.position) + ", outside the representable world extent of "
        + extentText() + "."
      );
  }

  if (!std::isfinite(spawn.orientation))
  {
    addError
      ( issues
      , "creature guid " + std::to_string(spawn.guid) + " has orientation "
        + number(spawn.orientation) + ", which is not a finite angle."
      );
  }

  if (static_cast<int>(spawn.equipment_id) < MIN_EQUIPMENT_ID)
  {
    addError
      ( issues
      , "creature guid " + std::to_string(spawn.guid) + " has equipment_id "
        + std::to_string(static_cast<int>(spawn.equipment_id))
        + ". Only -1 (random), 0 (none) and a creature_equip_template ID are meaningful."
      );
  }

  if (!std::isfinite(spawn.wander_distance))
  {
    addError
      ( issues
      , "creature guid " + std::to_string(spawn.guid) + " has wander_distance "
        + number(spawn.wander_distance) + ", which is not a finite distance."
      );
  }
  else if (spawn.wander_distance < 0.0)
  {
    addError
      ( issues
      , "creature guid " + std::to_string(spawn.guid) + " has a negative wander_distance of "
        + number(spawn.wander_distance) + "."
      );
  }

  // The core rejects these combinations at load time. MySQL accepts the row, which makes this
  // the difference between a loud refusal here and a spawn that quietly does not move.
  switch (spawn.movement_type)
  {
    case MovementType::IDLE:
      if (spawn.wander_distance != 0.0)
      {
        addError
          ( issues
          , "creature guid " + std::to_string(spawn.guid) + " has MovementType 0 (IDLE) with"
            " wander_distance " + number(spawn.wander_distance)
            + ". IDLE requires wander_distance = 0."
          );
      }
      break;

    case MovementType::RANDOM:
      if (!(spawn.wander_distance > 0.0))
      {
        addError
          ( issues
          , "creature guid " + std::to_string(spawn.guid) + " has MovementType 1 (RANDOM) with"
            " wander_distance " + number(spawn.wander_distance)
            + ". RANDOM requires wander_distance > 0, otherwise the creature never moves."
          );
      }
      break;

    case MovementType::WAYPOINT:
      if (!spawn.has_addon || spawn.path_id == 0)
      {
        addError
          ( issues
          , "creature guid " + std::to_string(spawn.guid) + " has MovementType 2 (WAYPOINT)"
            " without a creature_addon path_id. The path binds on the addon row, not on the"
            " creature row."
          );
      }
      break;

    default:
      addError
        ( issues
        , "creature guid " + std::to_string(spawn.guid) + " has MovementType "
          + std::to_string(static_cast<unsigned>(spawn.movement_type))
          + ", which is not one of 0 (IDLE), 1 (RANDOM) or 2 (WAYPOINT)."
        );
      break;
  }

  if (spawn.spawn_time_secs == 0)
  {
    addWarning
      ( issues
      , "creature guid " + std::to_string(spawn.guid) + " has spawntimesecs 0. That is a legal"
        " value meaning the creature never respawns once killed, which is rarely intended."
      );
  }

  return issues;
}

std::vector<ValidationIssue> SpawnValidation::validate(GameObjectSpawn const& spawn)
{
  std::vector<ValidationIssue> issues;

  if (spawn.guid == 0)
  {
    addError
      ( issues
      , "gameobject guid is 0. Every spawn needs a unique non-zero guid; 0 is the core's"
        " 'no spawn' sentinel and a row carrying it is unreachable."
      );
  }

  if (spawn.id == 0)
  {
    addError
      ( issues
      , "gameobject guid " + std::to_string(spawn.guid) + " has id 0. It must name a row in"
        " gameobject_template."
      );
  }

  if (!positionIsUsable(spawn.position))
  {
    addError
      ( issues
      , "gameobject guid " + std::to_string(spawn.guid) + " sits at "
        + positionText(spawn.position) + ", outside the representable world extent of "
        + extentText() + "."
      );
  }

  if (!std::isfinite(spawn.orientation))
  {
    addError
      ( issues
      , "gameobject guid " + std::to_string(spawn.guid) + " has orientation "
        + number(spawn.orientation) + ", which is not a finite angle."
      );
  }

  // rotation0..3 is a quaternion, not Euler angles. A non-unit one is the signature of Euler
  // values having been written into these columns, and the client renders the result as a
  // sheared or invisible object rather than reporting anything.
  double const length_squared
    ( spawn.rotation.r0 * spawn.rotation.r0
    + spawn.rotation.r1 * spawn.rotation.r1
    + spawn.rotation.r2 * spawn.rotation.r2
    + spawn.rotation.r3 * spawn.rotation.r3
    );

  double const length (std::sqrt(length_squared));

  // The NaN case has to be tested for rather than fall out of the comparison: every comparison
  // against NaN is false, so a NaN length would pass a bare tolerance check.
  if (!std::isfinite(length) || std::abs(length - 1.0) > QUATERNION_LENGTH_TOLERANCE)
  {
    addError
      ( issues
      , "gameobject guid " + std::to_string(spawn.guid) + " has rotation ("
        + number(spawn.rotation.r0) + ", " + number(spawn.rotation.r1) + ", "
        + number(spawn.rotation.r2) + ", " + number(spawn.rotation.r3) + ") of length "
        + number(length) + ". rotation0..3 is a quaternion and must be unit length; for an"
        " orientation o about the vertical axis it is (0, 0, sin(o/2), cos(o/2))."
      );
  }

  return issues;
}

std::vector<ValidationIssue> SpawnValidation::validate(WaypointPath const& path)
{
  std::vector<ValidationIssue> issues;

  if (path.nodes.empty())
  {
    addError
      ( issues
      , "waypoint path " + std::to_string(path.id) + " has no nodes. An empty path leaves any"
        " creature bound to it with MovementType 2 and nowhere to go."
      );

    // Everything below describes the nodes, and reporting "not contiguous" as well would bury
    // the one issue that matters.
    return issues;
  }

  if (!path.isContiguous())
  {
    addError
      ( issues
      , "waypoint path " + std::to_string(path.id) + " does not run 1.."
        + std::to_string(path.nodes.size()) + " in order. The core walks nodes by point and a"
        " gap silently truncates the path rather than erroring. Call renumber()."
      );
  }

  for (WaypointNode const& node : path.nodes)
  {
    std::string const where
      ("waypoint path " + std::to_string(path.id) + " point " + std::to_string(node.point));

    if (node.action_chance > MAX_ACTION_CHANCE)
    {
      addError
        ( issues
        , where + " has action_chance " + std::to_string(node.action_chance)
          + ". It is a percentage and must be 0..100."
        );
    }

    if (!positionIsUsable(node.position))
    {
      addError
        ( issues
        , where + " sits at " + positionText(node.position) + ", outside the representable"
          " world extent of " + extentText() + "."
        );
    }

    if (node.has_orientation && !std::isfinite(node.orientation))
    {
      addError
        ( issues
        , where + " has orientation " + number(node.orientation) + ", which is not a finite"
          " angle. Leave has_orientation false to write NULL instead."
        );
    }
  }

  if (path.nodes.size() == 1)
  {
    addWarning
      ( issues
      , "waypoint path " + std::to_string(path.id) + " has a single node, so it describes a"
        " position rather than a route. A creature bound to it will not patrol."
      );
  }

  return issues;
}

bool SpawnValidation::hasErrors(std::vector<ValidationIssue> const& issues)
{
  for (ValidationIssue const& issue : issues)
  {
    if (issue.severity == ValidationIssue::Severity::BLOCKING)
    {
      return true;
    }
  }

  return false;
}
