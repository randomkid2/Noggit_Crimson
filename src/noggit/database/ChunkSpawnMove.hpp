// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_DATABASE_CHUNKSPAWNMOVE_HPP
#define NOGGIT_DATABASE_CHUNKSPAWNMOVE_HPP

#include <noggit/database/ChunkTransform.hpp>
#include <noggit/database/SpawnTypes.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Noggit::Database
{
  class SpawnSceneCache;

  // The bridge between a terrain chunk move and the spawns standing on it.
  //
  // ChunkTransform has been in this tree since the first audit with exactly one caller, and that
  // caller is its own test file. It knows how to move creature, gameobject and waypoint rows to
  // follow terrain; nothing ever asked it to. This file is what asks. It turns a move expressed
  // the way the editor expresses one -- a set of chunks, where they went, and how far they turned
  // -- into the (source, transform) pair ChunkTransform takes, and feeds the answer back into the
  // loaded spawn scene so the existing "Save to database" changeset carries it.
  //
  // > [!important] Nothing here writes to a database, and nothing here emits SQL
  // > HARD RULE 1 and 2 are not negotiated by this feature. A chunk move marks the affected
  // > spawns dirty in SpawnSceneCache, exactly as dragging one with the mouse does, and they
  // > reach a file only through MapView::saveDatabaseChanges -- the one reviewable
  // > DELETE-then-INSERT emitter. Adding a second emission path for chunk moves would mean two
  // > places deciding what a spawn edit looks like on disk.

  // ---- the Noggit chunk grid ---------------------------------------------------------------

  // Noggit's TILESIZE, as a float, because that is the number the terrain actually moved by.
  //
  // > [!warning] Do not substitute TileCoordinates::TILE_SIZE here
  // > That one is the double 533.33333 and this one is the float 533.33333f, which is
  // > 533.33331298828125. They differ by 1.7e-5 per tile. The database layer measures TILES with
  // > the double because that is what TrinityCore's own formula uses; a chunk MOVE has to measure
  // > with the float because the ground moved by whatever MapChunk::xbase arithmetic produced,
  // > and a spawn shifted by the other constant lands a fraction off the terrain it was standing
  // > on. Both constants are right; mixing them is not. SpawnPlacement.hpp carries the same
  // > warning about ZEROPOINT_F for the same reason.
  //
  // Dividing a float by 16 and multiplying it by 32 are both exact in binary, so
  // ZEROPOINT_F / CHUNK_SIZE is exactly 512 -- computed, not assumed: the two constants promote
  // to 17066.666015625 and 33.333332061767578, whose quotient is 512 with no residue. That is
  // what makes the chunk grid below line up with the world origin instead of drifting off it.
  constexpr double CHUNK_SIZE = static_cast<double>(533.33333f) / 16.0;

  // A chunk's address on Noggit's map-wide 1024 x 1024 grid, in Noggit's axis order, with the
  // world origin on the boundary at 512.
  //
  // > [!warning] This is the SAME frame as ChunkManipulator's SelectedChunkIndex::globalX/globalZ
  // > and the TRANSPOSE of everything else in this directory. `x` grows with NOGGIT x, which is
  // > the direction server y DECREASES in; `z` grows with Noggit z, which is where server x
  // > decreases. Database::TileIndex is the other way round. The conversions below are the only
  // > place the two frames meet, and they are named so that doing it by hand is obviously wrong.
  struct ChunkAddress
  {
    int x = 0;
    int z = 0;
  };

  // Which chunk a server-frame position stands on.
  //
  // An off-grid position answers with an off-grid address rather than a clamped one, so a caller
  // can tell "outside the map" from "on the edge" -- but only out to +/- 1000000 chunks, past
  // which the answer saturates. That bound is not tidiness: position_x and position_y are FLOAT
  // and a corrupt row holds 1e38 as readily as -9512, and converting a double outside int's range
  // to int is undefined behaviour rather than a large number. Nothing on the 0..1023 grid is
  // anywhere near the saturation point, so a real spawn is never affected.
  ChunkAddress chunkForServerPosition(WorldPosition const& position);

  // The centre of a chunk, in server coordinates. Height is left at zero: the vertical axis plays
  // no part in the horizontal transform and inventing a ground height here would be a lie the
  // rotation pivot does not need.
  WorldPosition chunkCentreServerPosition(ChunkAddress const& chunk);

  // ---- the move ----------------------------------------------------------------------------

  // One rigid move of a block of chunks, described in the terms the editor has to hand.
  //
  // A reference PAIR rather than a pivot, and that is the whole reason this struct is usable from
  // the Chunk Manipulator. Its paste rotates the block about the block's own bounding box and
  // then translates it onto the cursor (ChunkClipboard::applyGridOp, then pasteSelection), so
  // neither the rotation centre nor the translation is separately available at the call site --
  // but the correspondence between ONE source chunk and where it landed always is, and a
  // correspondence plus the angle determines the map completely. See planChunkMove for the
  // derivation of the fixed point that turns the pair back into a single rotation.
  struct ChunkMove
  {
    // Every chunk whose content should follow. Not a rectangle: a radius selection is not one.
    std::vector<ChunkAddress> source_chunks;

    // Any one chunk of the block, and the chunk it ended up as.
    ChunkAddress reference_source;
    ChunkAddress reference_destination;

    // Applications of ChunkGridOp::ROTATE_90, which is the Noggit-frame map (X, Z) -> (Z, -X).
    // Normalised into 0..3 by planChunkMove, so a caller may pass 4 or -1.
    int quarter_turns = 0;

    // True when ChunkGridOp::MIRROR_X or MIRROR_Z was applied. Refused rather than approximated;
    // see planChunkMove.
    bool mirrored = false;

    // Yards added to every height, matching the paste's own height offset and elevation mode.
    // Server z, Noggit y.
    double height_offset = 0.0;
  };

  // What a move did, in the terms a status line and a review dialog need.
  struct ChunkSpawnMoveReport
  {
    // False when no spawns are loaded at all, which is the ordinary case: the spawn layer only
    // exists in a USE_SQL build after the user has loaded tiles in the database panel. Told apart
    // from "loaded, and nothing was standing there" because they call for different words.
    bool spawn_layer_loaded = false;

    std::size_t creatures = 0;
    std::size_t gameobjects = 0;

    // Rows the plan produced that the scene cache would not accept. Non-zero means the cache
    // changed under the plan -- a reload between planning and applying -- and is reported rather
    // than swallowed, because the terrain has moved and those spawns have not.
    std::size_t unmatched = 0;

    // What the transform had to say. A BLOCKING issue means nothing was moved.
    std::vector<ValidationIssue> issues;

    // The ADT tiles the moved spawns landed in, from ChunkTransform::occupiedTiles. A move that
    // pushes spawns into a tile the user has not loaded is legal and worth saying out loud.
    std::vector<TileIndex> occupied_tiles;

    std::string summary;

    std::size_t moved() const { return creatures + gameobjects; }
  };

  // ---- the three steps ---------------------------------------------------------------------

  // The loaded creatures and gameobjects standing on `chunks`, as the TileSpawns ChunkTransform
  // takes.
  //
  // Waypoint paths are NOT included, and cannot be: SpawnSceneCache::setTile takes a TileSpawns
  // and keeps only its entries (TileSpawnScene holds `std::vector<SpawnSceneEntry>` and nothing
  // else), so a loaded tile's paths are dropped at load time and there is nothing here to move.
  // ChunkTransform moves paths perfectly well; reaching them needs the query layer, not this
  // function. A creature whose MovementType is WAYPOINT is therefore moved while its patrol route
  // stays where it was, which planChunkMove reports as a warning rather than leaving to be
  // discovered in game.
  TileSpawns spawnsOnChunks(SpawnSceneCache const& cache, std::vector<ChunkAddress> const& chunks);

  // The move as a ChunkTransform plan: one planTranslation or one planRotation, never both.
  //
  // Composing two planners would mean feeding the output of the first into the second as a
  // source, which is precisely the accumulation ChunkTransform.hpp refuses to make convenient. A
  // rotation combined with a height change is therefore not expressible and is refused with a
  // BLOCKING issue rather than silently losing the height.
  TransformPlan planChunkMove(TileSpawns const& source, ChunkMove const& move);

  // Plan the move and push the result into the scene cache, marking every moved spawn dirty.
  //
  // The spawns move in the viewport immediately and reach SQL only when the user saves, through
  // the same emitter every other spawn edit uses. "Discard changes" undoes a chunk move like any
  // other.
  ChunkSpawnMoveReport moveSpawnsWithChunks(SpawnSceneCache& cache, ChunkMove const& move);
}

#endif
