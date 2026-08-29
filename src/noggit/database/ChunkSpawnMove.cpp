// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/database/ChunkSpawnMove.hpp>
#include <noggit/database/ChunkTransform.hpp>
#include <noggit/database/SpawnPlacement.hpp>
#include <noggit/database/SpawnSceneCache.hpp>
#include <noggit/database/SpawnTypes.hpp>
#include <noggit/database/TileCoordinates.hpp>

#include <glm/vec3.hpp>

#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace Noggit::Database;

namespace
{
  // Bound on a chunk index this file will produce, either sign. Far outside the 0..1023 grid, so
  // clamping to it can never turn an off-grid spawn into an on-grid one, and small enough that
  // the packing below stays injective in 64 bits.
  constexpr int CHUNK_INDEX_LIMIT = 1000000;

  // Multiplier for packing (x, z) into one key. 2^22 = 4194304, which is more than twice
  // CHUNK_INDEX_LIMIT, so x * CHUNK_PACK_STRIDE + z is injective over the whole clamped range
  // including negatives -- the naive "x * grid_width + z" is not, and a chunk one step west of the
  // map would collide with a real one on the row above.
  constexpr std::int64_t CHUNK_PACK_STRIDE = 4194304;

  // A chunk address packed into one integer, so the source set can be probed in O(1) per spawn
  // rather than scanned. A radius selection is routinely a few hundred chunks and a populated tile
  // a few thousand spawns; the linear version is a million comparisons for one button press.
  std::int64_t packedChunk(ChunkAddress const& chunk)
  {
    return static_cast<std::int64_t>(chunk.x) * CHUNK_PACK_STRIDE
         + static_cast<std::int64_t>(chunk.z);
  }

  // floor division, spelled out, and clamped before the cast.
  //
  // Spelled out because a block near the map edge genuinely produces negative chunk coordinates
  // and C++ integer division truncates towards zero -- -1 / 16 is 0, so the chunk one step off the
  // west edge would report as chunk 0, i.e. inside the map. SelectedChunkIndex::fromGlobal spells
  // the same thing out for the same reason.
  //
  // Clamped because the columns these coordinates arrive from are FLOAT and a corrupt row holds
  // 1e38 as readily as -9512: converting a double outside int's range is undefined behaviour, not
  // a large number. The comparisons are written as negations so that a NaN, which compares false
  // against everything, is caught by the first of them rather than falling through to the cast.
  int floorDivide(double value, double divisor)
  {
    double const quotient (std::floor(value / divisor));

    if (!(quotient > -static_cast<double>(CHUNK_INDEX_LIMIT)))
    {
      return -CHUNK_INDEX_LIMIT;
    }

    if (!(quotient < static_cast<double>(CHUNK_INDEX_LIMIT)))
    {
      return CHUNK_INDEX_LIMIT;
    }

    return static_cast<int>(quotient);
  }

  // A plan that moves nothing and says why, for the two moves this file refuses to express.
  TransformPlan refusal(TileSpawns const& source, std::string message)
  {
    TransformPlan plan;
    plan.source_tile = source.tile;
    plan.map = source.map;

    SpawnValidation::addError(plan.issues, std::move(message));

    return plan;
  }

  // The fixed point of "rotate the block by k quarter turns, then put it where it landed".
  //
  // DERIVATION. planRotation computes f(v) = P + R(v - P) about a pivot P. The move is a rigid
  // map whose linear part is R -- verified separately: the Chunk Manipulator's ROTATE_90 is the
  // Noggit-frame map (X, Z) -> (Z, -X), and carrying that through math::to_server gives the
  // server-frame map (x, y) -> (-y, x), which is a rotation of +90 degrees in the sense
  // creature.orientation increases -- and which is pinned by the one correspondence the caller
  // supplies, A -> B. So the move is g(v) = R(v - A) + B. Equating the two for all v:
  //
  //     P + Rv - RP = Rv - RA + B   =>   (I - R) P = B - RA
  //
  // I - R is [[1-c, s], [-s, 1-c]] with determinant 2(1 - c), which is non-zero for every angle
  // that is not a whole number of turns -- so this is only ever called with k in 1..3, and k == 0
  // goes to planTranslation instead.
  //
  // Why bother rather than rotating about the block centre and translating afterwards: the
  // second step would have to take the first step's OUTPUT as its input, and ChunkTransform.hpp
  // refuses to make that convenient on purpose. Every coordinate this file produces is computed
  // once, from the row as it was read.
  //
  // Checked numerically against a direct simulation of the composite map at 10000 random points
  // spread over the whole 1024 x 1024 chunk grid and all three rotations: worst disagreement
  // 1.46e-11 yards, against a FLOAT column resolution of 2.7e-4 yards at x = -9512.
  WorldPosition rotationFixedPoint(WorldPosition const& a, WorldPosition const& b, double radians)
  {
    double const c (std::cos(radians));
    double const s (std::sin(radians));

    // B - RA
    double const dx (b.x - (a.x * c - a.y * s));
    double const dy (b.y - (a.x * s + a.y * c));

    double const determinant (2.0 * (1.0 - c));

    WorldPosition pivot;
    pivot.x = ((1.0 - c) * dx - s * dy) / determinant;
    pivot.y = (s * dx + (1.0 - c) * dy) / determinant;
    pivot.z = 0.0;

    return pivot;
  }
}

namespace Noggit::Database
{
  ChunkAddress chunkForServerPosition(WorldPosition const& position)
  {
    // Both halves of this are Noggit's float constants promoted to double, never the database
    // layer's double TILE_SIZE. See the warning on CHUNK_SIZE: the two disagree by 1.7e-5 per
    // tile, which is a whole chunk of drift at the far edge of a 64-tile map.
    double const zeropoint (static_cast<double>(SpawnPlacement::ZEROPOINT_F));

    ChunkAddress chunk;
    chunk.x = floorDivide(zeropoint - position.y, CHUNK_SIZE);
    chunk.z = floorDivide(zeropoint - position.x, CHUNK_SIZE);

    return chunk;
  }

  WorldPosition chunkCentreServerPosition(ChunkAddress const& chunk)
  {
    double const zeropoint (static_cast<double>(SpawnPlacement::ZEROPOINT_F));

    double const noggit_x ((static_cast<double>(chunk.x) + 0.5) * CHUNK_SIZE);
    double const noggit_z ((static_cast<double>(chunk.z) + 0.5) * CHUNK_SIZE);

    WorldPosition centre;
    centre.x = zeropoint - noggit_z;
    centre.y = zeropoint - noggit_x;
    centre.z = 0.0;

    return centre;
  }

  TileSpawns spawnsOnChunks
    (SpawnSceneCache const& cache, std::vector<ChunkAddress> const& chunks)
  {
    TileSpawns source;

    std::unordered_set<std::int64_t> wanted;
    wanted.reserve(chunks.size() * 2);

    for (ChunkAddress const& chunk : chunks)
    {
      wanted.insert(packedChunk(chunk));
    }

    bool tile_set (false);

    for (SpawnSceneEntry const* entry : cache.allEntries())
    {
      // The stored ROW, not the ModelInstance. The row is what the changeset is written from and
      // what SpawnSceneCache::moveTo converts back into; taking the instance's position would
      // read the display copy, which carries the frame conversion and YAW_OFFSET_DEGREES with it.
      WorldPosition const& position
        (entry->kind == SpawnKind::CREATURE ? entry->creature.position
                                            : entry->gameobject.position);

      if (!wanted.count(packedChunk(chunkForServerPosition(position))))
      {
        continue;
      }

      if (!tile_set)
      {
        // Carried so a stored plan explains where it came from. A block move can span tiles, so
        // this is the tile of the FIRST spawn found and nothing more -- occupied_tiles is the
        // field that answers the question properly.
        source.tile = TileCoordinates::tileForPosition(position);
        source.map = entry->kind == SpawnKind::CREATURE ? entry->creature.map
                                                        : entry->gameobject.map;
        tile_set = true;
      }

      if (entry->kind == SpawnKind::CREATURE)
      {
        source.creatures.push_back(entry->creature);
      }
      else
      {
        source.gameobjects.push_back(entry->gameobject);
      }
    }

    return source;
  }

  TransformPlan planChunkMove(TileSpawns const& source, ChunkMove const& move)
  {
    if (move.mirrored)
    {
      return refusal
        ( source
        , "the chunk block was mirrored, and a mirror cannot be applied to a spawn. A reflection"
          " reverses orientation and no rotation reproduces it: a creature's facing could be"
          " negated, but a gameobject's rotation quaternion could not, and a doodad set placed"
          " inside a mirrored WMO would still be the unmirrored one. Nothing was moved -- move the"
          " terrain and place the spawns by hand."
        );
    }

    // Normalised into 0..3 first, so a caller may hand over 4 (a full turn, which is the
    // identity) or -1 (three quarter turns the other way) without either being mistaken for a
    // rotation this file cannot express.
    int const quarter_turns (((move.quarter_turns % 4) + 4) % 4);

    WorldPosition const from (chunkCentreServerPosition(move.reference_source));
    WorldPosition const to (chunkCentreServerPosition(move.reference_destination));

    if (quarter_turns == 0)
    {
      WorldDelta delta;
      delta.dx = to.x - from.x;
      delta.dy = to.y - from.y;
      delta.dz = move.height_offset;

      return planTranslation(source, delta);
    }

    if (move.height_offset != 0.0)
    {
      // Exact comparison, and it is the right one: the question is whether the caller asked for a
      // height change at all, not whether two computed heights agree.
      return refusal
        ( source
        , "the chunk block was both turned and moved in height, and that is one transform too"
          " many for a single pass. planRotation turns about a vertical axis and leaves height"
          " alone; applying a translation afterwards would mean feeding a plan back in as a"
          " source, which is the error accumulation ChunkTransform exists to prevent. Nothing was"
          " moved -- turn the block with no height offset, or move it flat."
        );
    }

    // A quarter turn towards +y, matching the sense creature.orientation increases in and the
    // sense the terrain turned in. TWO_PI is shared with the emitter and the transform rather
    // than a private pi/2, so all three fold angles against the same constant.
    double const radians (TileCoordinates::TWO_PI * 0.25 * static_cast<double>(quarter_turns));

    return planRotation(source, rotationFixedPoint(from, to, radians), radians);
  }

  ChunkSpawnMoveReport moveSpawnsWithChunks(SpawnSceneCache& cache, ChunkMove const& move)
  {
    ChunkSpawnMoveReport report;
    report.spawn_layer_loaded = !cache.empty();

    if (!report.spawn_layer_loaded)
    {
      // Said plainly rather than reported as success with a zero count. A terrain move that
      // silently left the database alone is the exact failure this feature exists to stop, and
      // "0 spawns moved" reads identically whether there were none or whether the layer was never
      // there.
      report.summary = "No database spawns are loaded, so nothing followed the terrain. Load the"
                       " affected tiles in the database panel before moving chunks, or move the"
                       " spawns yourself afterwards.";

      return report;
    }

    TileSpawns const source (spawnsOnChunks(cache, move.source_chunks));

    if (source.empty())
    {
      report.summary = "No loaded spawn stands on the moved chunks, so there was nothing to move.";

      return report;
    }

    TransformPlan const plan (planChunkMove(source, move));

    report.issues = plan.issues;
    report.occupied_tiles = plan.occupied_tiles;

    if (SpawnValidation::hasErrors(report.issues))
    {
      report.summary = "The move was refused and no spawn was touched.";

      return report;
    }

    bool const turning ((((move.quarter_turns % 4) + 4) % 4) != 0);

    // Positions go back in through SpawnSceneCache::moveTo, which converts to the server frame
    // through SpawnPlacement::serverPositionFor -- the exact inverse of the positionFor below. The
    // round trip is deliberate rather than wasteful: it is the one tested seam between the two
    // frames, and going straight to the stored row would put a second copy of that conversion in
    // a file that has no business knowing it.
    for (CreatureSpawn const& moved : plan.creatures)
    {
      SpawnRef const spawn {SpawnKind::CREATURE, moved.guid};

      NoggitPlacement const placement (SpawnPlacement::positionFor(moved.position));

      if (!cache.moveTo(spawn, glm::vec3( static_cast<float>(placement.x)
                                        , static_cast<float>(placement.y)
                                        , static_cast<float>(placement.z)
                                        )))
      {
        ++report.unmatched;
        continue;
      }

      if (turning)
      {
        // Only when the block turned. rotateTo marks the entry dirty on its own, so calling it
        // for a pure translation would put every spawn in the block into the changeset with a
        // facing nobody changed -- the same defect the orientation spin box was fixed for.
        cache.rotateTo(spawn, moved.orientation);
      }

      ++report.creatures;
    }

    for (GameObjectSpawn const& moved : plan.gameobjects)
    {
      SpawnRef const spawn {SpawnKind::GAMEOBJECT, moved.guid};

      NoggitPlacement const placement (SpawnPlacement::positionFor(moved.position));

      if (!cache.moveTo(spawn, glm::vec3( static_cast<float>(placement.x)
                                        , static_cast<float>(placement.y)
                                        , static_cast<float>(placement.z)
                                        )))
      {
        ++report.unmatched;
        continue;
      }

      if (turning)
      {
        cache.rotateTo(spawn, moved.orientation);
      }

      ++report.gameobjects;
    }

    // Waypoint paths are not in the scene cache at all -- see spawnsOnChunks -- so a patrolling
    // creature has just been moved away from its route. Counted rather than assumed absent,
    // because on a populated tile it is the common case and it is invisible in the viewport: the
    // creature stands in the new place and walks back to the old one the moment the server loads
    // it.
    std::size_t patrolling (0);

    for (CreatureSpawn const& moved : plan.creatures)
    {
      if (moved.movement_type == MovementType::WAYPOINT)
      {
        ++patrolling;
      }
    }

    report.summary = std::to_string(report.creatures) + " creature(s) and "
      + std::to_string(report.gameobjects) + " gameobject(s) followed the terrain, across "
      + std::to_string(report.occupied_tiles.size()) + " tile(s). Nothing has been written: they"
        " are pending edits like any other, and reach SQL through Save to database.";

    if (patrolling > 0)
    {
      report.summary += " " + std::to_string(patrolling) + " of them use MovementType 2 (waypoint)"
        " and their paths did NOT move -- waypoint_data is not loaded into the spawn scene, so"
        " those routes still run over the old ground.";
    }

    if (report.unmatched > 0)
    {
      report.summary += " " + std::to_string(report.unmatched) + " planned row(s) were no longer in"
        " the scene and were left behind; reload the tiles and check them.";
    }

    return report;
  }
}
