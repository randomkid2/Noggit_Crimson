// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_DATABASE_CHUNKTRANSFORM_HPP
#define NOGGIT_DATABASE_CHUNKTRANSFORM_HPP

#include <noggit/database/SpawnTypes.hpp>

#include <cstdint>
#include <vector>

namespace Noggit::Database
{
  class ChangesetBuilder;

  // The dependency-aware side of a chunk move.
  //
  // Moving a tile's terrain without moving what is anchored to it leaves a world that is worse
  // than the one before the edit: the ground is somewhere else and every creature, object and
  // patrol node is still standing where the ground used to be. So the whole dependent set is
  // transformed in one pass and handed back as a plan, which is what the caller reviews and then
  // commits alongside the terrain edit.
  //
  // Pure arithmetic -- no SQL, no Qt, no I/O -- so the transform is exhaustively testable with
  // nothing installed, and so the terrain side can be written against a maths layer that is
  // already known good.
  //
  // The discipline this file exists to enforce: every output coordinate is computed from the
  // corresponding INPUT coordinate, never from one this module has already written. The columns
  // are FLOAT, where the measured error at x = -9512 is 2.7e-4 yards; a transform fed its own
  // previous output accumulates that on every step, so a session of small nudges walks the
  // content away from where the user put it. Both planners are therefore pure functions of
  // (source, transform): they return moved COPIES and cannot touch the source. Adjusting a move
  // means re-planning from the same source with a different transform, and there is deliberately
  // no helper that turns a plan back into a source -- that would make accumulation the
  // convenient path.
  struct WorldDelta
  {
    // Yards, in world axes. Zero-initialised so a default-constructed delta is a no-op rather
    // than whatever was on the stack.
    double dx = 0.0;
    double dy = 0.0;
    double dz = 0.0;
  };

  // The moved copies, plus everything the caller needs to decide whether to commit them.
  //
  // Nothing here is a diff or a patch: these are complete entities ready to be fed to a
  // ChangesetBuilder. A diff would have to be reapplied to the original to be useful, which is
  // the accumulation this module refuses to do.
  struct TransformPlan
  {
    // Where the content came from. Carried so a stored plan explains itself in review without
    // the source it was built from.
    TileIndex source_tile;
    std::uint16_t map = 0;

    std::vector<CreatureSpawn> creatures;
    std::vector<GameObjectSpawn> gameobjects;
    std::vector<WaypointPath> paths;

    // Problems the TRANSFORM introduced, not a full validation of the entities: entity-level
    // checks belong to SpawnValidation and ChangesetBuilder runs them on everything appendTo
    // feeds it. Reporting a pre-existing MovementType mismatch here would bury the one thing
    // this module can actually tell the caller.
    std::vector<ValidationIssue> issues;

    // The ADT tiles the moved content lands in, as returned by occupiedTiles() at planning
    // time. Held as well as computable because a caller reviewing a stored plan should not have
    // to recompute it, and because a test can then assert the two agree.
    std::vector<TileIndex> occupied_tiles;

    bool empty() const
    {
      return creatures.empty() && gameobjects.empty() && paths.empty();
    }
  };

  // Applies `delta` to every creature, gameobject and waypoint node in `source`.
  //
  // Facings are untouched: a translation does not turn anything. That includes a gameobject's
  // rotation quaternion, which is copied verbatim so an object with a tilt in rotation0/1 keeps
  // it -- recomputing the quaternion from the orientation column would silently flatten it.
  //
  // A delta of exactly zero is reported as a WARNING. Exact comparison is right here: the
  // question is "did the caller ask for nothing", not "are two computed values equal".
  TransformPlan planTranslation(TileSpawns const& source, WorldDelta const& delta);

  // Rotates `source` by `radians` about the vertical axis through `pivot`, positive towards +y
  // -- the same sense in which `orientation` increases, so a spawn keeps facing the same
  // landmark.
  //
  // Heights are untouched: the axis is vertical, so position_z passes through.
  //
  // Each creature's orientation gains `radians`, normalised back into [0, 2pi) because the core
  // stores it in that range and a wrapped value round-trips as a different facing. Each
  // gameobject's quaternion is recomputed from the resulting yaw with rotation0/1 left at zero;
  // a source tilt cannot survive a yaw-only recomputation and is reported as a WARNING rather
  // than dropped in silence. A waypoint node's orientation follows the same rule when the node
  // carries one, and stays NULL when it does not.
  //
  // An angle that normalises to exactly zero -- 0, 2pi, any whole number of turns -- is an
  // identity, and is passed through bit-exactly rather than run through the arithmetic:
  // pivot + (p - pivot) is not guaranteed to reproduce p to the last bit, so a full-turn
  // rotation would otherwise rewrite every coordinate in the tile by an ulp and appear in review
  // as a change nobody made. It is also reported as a no-op WARNING.
  TransformPlan planRotation
    (TileSpawns const& source, WorldPosition const& pivot, double radians);

  // Which ADT tiles the moved content lands in, deduplicated and ordered by (x, y) so the
  // result is stable across runs. A move that pushes content across a tile boundary is legal,
  // but the caller has to know, because the terrain half of the edit has to load and save those
  // tiles too.
  //
  // Recomputed from the entities and ignores TransformPlan::occupied_tiles, so it can be used to
  // check that field rather than merely echo it. Content that landed off the grid contributes
  // nothing: there is no such ADT to load, and it is already reported as an error-severity
  // issue.
  std::vector<TileIndex> occupiedTiles(TransformPlan const& plan);

  // Feeds the moved entities into a changeset, so the database half of the move commits with the
  // terrain half instead of after it.
  //
  // Deliberately does not gate on plan.issues. Everything an issue reports that would corrupt a
  // row -- a non-finite coordinate, a position off the world extent -- is also caught by
  // SpawnValidation inside the builder, which refuses to build unless the caller explicitly
  // turned reject_invalid off. Adding a second, harder gate here would take that escape hatch
  // away for no extra safety. The tile-crossing and no-op warnings are advisory and have no
  // bearing on whether the SQL is correct.
  void appendTo(ChangesetBuilder& builder, TransformPlan const& plan);
}

#endif
