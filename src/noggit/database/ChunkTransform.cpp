// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/database/ChangesetBuilder.hpp>
#include <noggit/database/ChunkTransform.hpp>
#include <noggit/database/SpawnTypes.hpp>
#include <noggit/database/TileCoordinates.hpp>

#include <algorithm>
#include <cmath>
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
  constexpr double TWO_PI = 6.283185307179586476925286766559;

  // Largest yaw disagreement between a gameobject's `orientation` column and its rotation
  // quaternion that is written off as float noise. Same value ChangesetBuilder uses, because the
  // two are answering the same question about the same row and disagreeing about the threshold
  // would mean one warns where the other does not.
  constexpr double ORIENTATION_AGREEMENT_TOLERANCE = 1.0e-3;

  void addIssue
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

  void addError(std::vector<ValidationIssue>& issues, std::string message)
  {
    addIssue(issues, ValidationIssue::Severity::ERROR, std::move(message));
  }

  void addWarning(std::vector<ValidationIssue>& issues, std::string message)
  {
    addIssue(issues, ValidationIssue::Severity::WARNING, std::move(message));
  }

  // Not std::to_string: that is specified as sprintf("%f"), so it reads LC_NUMERIC and produces
  // "4,712400" under a comma-decimal locale -- and Qt calls setlocale(LC_ALL, "") when a
  // QCoreApplication is constructed. Every formatter in this layer imbues the classic locale for
  // that reason. SqlFormat::coordinate is not reused because it throws on a non-finite value and
  // a non-finite value is precisely what some of these messages are about.
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

    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::setprecision(9) << value;

    std::string text (stream.str());

    // Negative zero survives subtraction and unary minus, renders as "-0", and reads in review
    // as a defect rather than as zero.
    if (text == "-0")
    {
      return "0";
    }

    return text;
  }

  std::string positionText(WorldPosition const& position)
  {
    return "(" + number(position.x) + ", " + number(position.y) + ", " + number(position.z) + ")";
  }

  std::string tileText(TileIndex const& tile)
  {
    return "(" + std::to_string(tile.x) + ", " + std::to_string(tile.y) + ")";
  }

  // TileCoordinates::isValidPosition is the authority on the horizontal extent and is not
  // duplicated here, so the two cannot disagree. It says nothing about z, and it cannot speak for
  // a NaN that would reach an INSERT as a literal the server rejects, so both are checked.
  bool positionIsUsable(WorldPosition const& position)
  {
    return std::isfinite(position.x)
      && std::isfinite(position.y)
      && std::isfinite(position.z)
      && TileCoordinates::isValidPosition(position.x, position.y);
  }

  bool deltaIsFinite(WorldDelta const& delta)
  {
    return std::isfinite(delta.dx) && std::isfinite(delta.dy) && std::isfinite(delta.dz);
  }

  bool pivotIsFinite(WorldPosition const& pivot)
  {
    return std::isfinite(pivot.x) && std::isfinite(pivot.y) && std::isfinite(pivot.z);
  }

  WorldPosition translated(WorldPosition const& position, WorldDelta const& delta)
  {
    WorldPosition out;
    out.x = position.x + delta.dx;
    out.y = position.y + delta.dy;
    out.z = position.z + delta.dz;

    return out;
  }

  // sin and cos are evaluated once for the whole plan rather than per entity. Not for speed: a
  // per-entity call could in principle return a differently-rounded result for the same argument
  // on a platform with a vectorised libm, and two spawns rotated by "the same" angle landing on
  // subtly different rotations is exactly the class of defect this module is built to avoid.
  struct PlanarRotation
  {
    WorldPosition pivot;
    double sin_yaw = 0.0;
    double cos_yaw = 1.0;
  };

  WorldPosition rotated(WorldPosition const& position, PlanarRotation const& rotation)
  {
    double const dx (position.x - rotation.pivot.x);
    double const dy (position.y - rotation.pivot.y);

    WorldPosition out;
    out.x = rotation.pivot.x + dx * rotation.cos_yaw - dy * rotation.sin_yaw;
    out.y = rotation.pivot.y + dx * rotation.sin_yaw + dy * rotation.cos_yaw;

    // The axis is vertical, so height is not part of the rotation at all.
    out.z = position.z;

    return out;
  }

  // Exact comparison against the identity, matching the test ChangesetBuilder applies to decide
  // whether a caller authored the quaternion or left it alone. This is not the float-equality
  // mistake the schema notes warn about: the question is "was this field touched", not "are these
  // two computed rotations the same". The two must agree, or an object the emitter treats as
  // unauthored would be treated as authored here and its yaw read out of a field nobody set.
  bool isDefaultRotation(TileCoordinates::Quaternion const& rotation)
  {
    return rotation.r0 == 0.0 && rotation.r1 == 0.0 && rotation.r2 == 0.0 && rotation.r3 == 1.0;
  }

  // Shortest angular distance between two normalised yaws, so 0.001 and 2pi - 0.001 are close
  // rather than a full turn apart.
  double yawSeparation(double a, double b)
  {
    double const separation (std::fabs(a - b));

    return std::min(separation, TWO_PI - separation);
  }

  // Reports what the move did to one position: whether it left the world, and whether it changed
  // tile. `what` names the entity, because an issue the caller cannot attribute to a row is not
  // actionable.
  void reportMove
    ( std::vector<ValidationIssue>& issues
    , std::string const& what
    , WorldPosition const& before
    , WorldPosition const& after
    )
  {
    if (!positionIsUsable(after))
    {
      addError
        ( issues
        , what + " lands at " + positionText(after) + ", outside the world extent of +/-"
          + number(MAP_HALF_EXTENT) + ". The move is refused rather than clamped: a clamped"
            " spawn is somewhere nobody chose, and the terrain half of this edit would still"
            " have moved."
        );

      // No tile comparison below. An off-grid position saturates to an index outside 0..63, and
      // "crossed into tile (64, 31)" on top of the error above is noise.
      return;
    }

    TileIndex const from (TileCoordinates::tileForPosition(before));
    TileIndex const to (TileCoordinates::tileForPosition(after));

    // A source position that was already off the grid has no tile to have crossed from, and
    // saying it crossed from tile 64 would be a fiction. The move fixed it; nothing to report.
    if (!TileCoordinates::isValidTile(from) || from == to)
    {
      return;
    }

    addWarning
      ( issues
      , what + " crossed a tile boundary: " + tileText(from) + " -> " + tileText(to)
        + ". That is legal, but the destination ADT has to be loaded and saved by the terrain"
          " half of this edit too."
      );
  }

  // The single walk over the dependent set, shared by both planners so neither can grow a rule
  // the other lacks.
  //
  // `map_point` is applied to the SOURCE position every time. That is the whole point of the
  // module: nothing here reads a value it has already written, so the transform cannot
  // accumulate rounding into the FLOAT columns.
  //
  // `yaw_delta` and `adjust_facings` are separate because a translation must leave facings
  // strictly alone -- including a gameobject's rotation quaternion, whose tilt a yaw-only
  // recomputation would silently discard.
  template<typename MapPoint>
  TransformPlan buildPlan
    ( TileSpawns const& source
    , MapPoint map_point
    , double yaw_delta
    , bool adjust_facings
    , std::vector<ValidationIssue> issues
    )
  {
    TransformPlan plan;
    plan.source_tile = source.tile;
    plan.map = source.map;
    plan.issues = std::move(issues);

    plan.creatures.reserve(source.creatures.size());
    plan.gameobjects.reserve(source.gameobjects.size());
    plan.paths.reserve(source.paths.size());

    for (CreatureSpawn const& spawn : source.creatures)
    {
      CreatureSpawn moved (spawn);
      moved.position = map_point(spawn.position);

      if (adjust_facings)
      {
        moved.orientation
          = TileCoordinates::normaliseOrientation(spawn.orientation + yaw_delta);
      }

      reportMove
        ( plan.issues, "creature guid " + std::to_string(spawn.guid)
        , spawn.position, moved.position
        );

      plan.creatures.push_back(moved);
    }

    for (GameObjectSpawn const& spawn : source.gameobjects)
    {
      std::string const what ("gameobject guid " + std::to_string(spawn.guid));

      GameObjectSpawn moved (spawn);
      moved.position = map_point(spawn.position);

      if (adjust_facings)
      {
        // The core reads rotation0..3, not the orientation column, so an authored quaternion is
        // the authority on the current yaw. Taking the orientation column instead would rotate a
        // correctly-stored object away from where it was pointing.
        bool const authored (!isDefaultRotation(spawn.rotation));

        double const from_column (TileCoordinates::normaliseOrientation(spawn.orientation));
        double const base
          (authored ? TileCoordinates::orientationForQuaternion(spawn.rotation) : from_column);

        if (authored)
        {
          if (spawn.rotation.r0 != 0.0 || spawn.rotation.r1 != 0.0)
          {
            addWarning
              ( plan.issues
              , what + " carries a tilt in rotation0/rotation1 (" + number(spawn.rotation.r0)
                + ", " + number(spawn.rotation.r1) + "). A rotation about the vertical axis is"
                  " re-emitted as a yaw-only quaternion, so the tilt is lost. Translate it"
                  " instead, or set the rotation by hand afterwards."
              );
          }

          if (yawSeparation(base, from_column) > ORIENTATION_AGREEMENT_TOLERANCE)
          {
            addWarning
              ( plan.issues
              , what + " had a rotation quaternion yielding yaw " + number(base)
                + " while its orientation column said " + number(from_column)
                + ". The quaternion is what the core uses, so it was taken as the starting"
                  " angle and the orientation column is rewritten to agree."
              );
          }
        }

        double const yaw (TileCoordinates::normaliseOrientation(base + yaw_delta));

        // Written to both representations from the same angle, so they agree by construction and
        // the disagreement above cannot be reintroduced by this module.
        moved.orientation = yaw;
        moved.rotation = TileCoordinates::quaternionForOrientation(yaw);
      }

      reportMove(plan.issues, what, spawn.position, moved.position);

      plan.gameobjects.push_back(moved);
    }

    for (WaypointPath const& path : source.paths)
    {
      WaypointPath moved;
      moved.id = path.id;
      moved.nodes.reserve(path.nodes.size());

      for (WaypointNode const& node : path.nodes)
      {
        WaypointNode moved_node (node);
        moved_node.position = map_point(node.position);

        // A node orientation is a world-frame facing like any other, so it turns with the
        // content. A node without one keeps NULL: inventing a facing for it would change how the
        // creature behaves at that node, which is not what a move was asked to do.
        if (adjust_facings && node.has_orientation)
        {
          moved_node.orientation
            = TileCoordinates::normaliseOrientation(node.orientation + yaw_delta);
        }

        reportMove
          ( plan.issues
          , "waypoint path " + std::to_string(path.id) + " point " + std::to_string(node.point)
          , node.position, moved_node.position
          );

        moved.nodes.push_back(moved_node);
      }

      plan.paths.push_back(std::move(moved));
    }

    plan.occupied_tiles = Noggit::Database::occupiedTiles(plan);

    return plan;
  }

  // Everything is dropped and one error is reported when the transform itself cannot be
  // computed. The alternatives are both worse: NaN coordinates reach the FLOAT columns, or the
  // spawns stay exactly where they were while the terrain moves out from under them.
  TransformPlan refusedPlan(TileSpawns const& source, std::string message)
  {
    TransformPlan plan;
    plan.source_tile = source.tile;
    plan.map = source.map;

    addError(plan.issues, std::move(message));

    return plan;
  }

  WorldPosition unchanged(WorldPosition const& position)
  {
    return position;
  }
}

namespace Noggit::Database
{
  TransformPlan planTranslation(TileSpawns const& source, WorldDelta const& delta)
  {
    if (!deltaIsFinite(delta))
    {
      return refusedPlan
        ( source
        , "the translation delta (" + number(delta.dx) + ", " + number(delta.dy) + ", "
          + number(delta.dz) + ") is not finite, so no destination can be computed. Nothing was"
            " moved."
        );
    }

    std::vector<ValidationIssue> issues;

    if (delta.dx == 0.0 && delta.dy == 0.0 && delta.dz == 0.0)
    {
      addWarning
        ( issues
        , "the translation delta is exactly zero, so this plan moves nothing. Committing it"
          " rewrites every affected row with the values it already holds."
        );
    }

    // No special case for the zero delta: adding 0.0 to a finite double is exact, so the
    // pass-through and the arithmetic produce the same numbers.
    return buildPlan
      ( source
      , [&delta](WorldPosition const& position) { return translated(position, delta); }
      , 0.0
      , false
      , std::move(issues)
      );
  }

  TransformPlan planRotation
    (TileSpawns const& source, WorldPosition const& pivot, double radians)
  {
    if (!std::isfinite(radians))
    {
      return refusedPlan
        ( source
        , "the rotation angle " + number(radians) + " is not a finite number of radians, so no"
          " destination can be computed. Nothing was moved."
        );
    }

    if (!pivotIsFinite(pivot))
    {
      return refusedPlan
        ( source
        , "the rotation pivot " + positionText(pivot) + " is not finite, so no destination can be"
          " computed. Nothing was moved."
        );
    }

    // Normalised once, up front. A whole number of turns then collapses to exactly zero and is
    // recognised as the identity it is, and an absurd argument never reaches sin/cos where the
    // library's own argument reduction would decide how much precision to keep.
    double const yaw (TileCoordinates::normaliseOrientation(radians));

    std::vector<ValidationIssue> issues;

    if (yaw == 0.0)
    {
      addWarning
        ( issues
        , "the rotation of " + number(radians) + " radians is a whole number of turns, so this"
          " plan moves nothing. Content is passed through unchanged rather than run through the"
          " arithmetic, which would shift every coordinate by an ulp for no reason."
        );

      return buildPlan(source, unchanged, 0.0, false, std::move(issues));
    }

    PlanarRotation rotation;
    rotation.pivot = pivot;
    rotation.sin_yaw = std::sin(yaw);
    rotation.cos_yaw = std::cos(yaw);

    return buildPlan
      ( source
      , [&rotation](WorldPosition const& position) { return rotated(position, rotation); }
      , yaw
      , true
      , std::move(issues)
      );
  }

  std::vector<TileIndex> occupiedTiles(TransformPlan const& plan)
  {
    std::vector<TileIndex> tiles;

    auto const record
      ( [&tiles](WorldPosition const& position)
        {
          TileIndex const tile (TileCoordinates::tileForPosition(position));

          // Off-grid content contributes nothing. There is no such ADT for the caller to load,
          // and it is already reported as an error-severity issue.
          if (TileCoordinates::isValidTile(tile))
          {
            tiles.push_back(tile);
          }
        }
      );

    for (CreatureSpawn const& spawn : plan.creatures)
    {
      record(spawn.position);
    }

    for (GameObjectSpawn const& spawn : plan.gameobjects)
    {
      record(spawn.position);
    }

    for (WaypointPath const& path : plan.paths)
    {
      for (WaypointNode const& node : path.nodes)
      {
        record(node.position);
      }
    }

    // Ordered before deduplicating, and ordered at all so the list is identical on every run:
    // this ends up in review output next to the terrain edit, and a set that reshuffles between
    // runs makes a diff of two plans unreadable.
    std::sort
      ( tiles.begin(), tiles.end()
      , [](TileIndex const& a, TileIndex const& b)
        {
          return a.x != b.x ? a.x < b.x : a.y < b.y;
        }
      );

    tiles.erase(std::unique(tiles.begin(), tiles.end()), tiles.end());

    return tiles;
  }

  void appendTo(ChangesetBuilder& builder, TransformPlan const& plan)
  {
    // Creatures, then gameobjects, then paths -- the order the builder emits its sections in, so
    // the changeset reads in the same order it was assembled.
    for (CreatureSpawn const& spawn : plan.creatures)
    {
      builder.addCreature(spawn);
    }

    for (GameObjectSpawn const& spawn : plan.gameobjects)
    {
      builder.addGameObject(spawn);
    }

    for (WaypointPath const& path : plan.paths)
    {
      builder.addWaypointPath(path);
    }
  }
}
