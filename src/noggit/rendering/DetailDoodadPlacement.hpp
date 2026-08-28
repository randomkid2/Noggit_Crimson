// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_DETAILDOODADPLACEMENT_HPP
#define NOGGIT_DETAILDOODADPLACEMENT_HPP

#include <external/glm/glm.hpp>

#include <cstdint>
#include <vector>

class MapChunk;

namespace Noggit::Rendering
{
  // Where the detail doodads of one chunk go, and which GroundEffectDoodad row each one is.
  //
  // This module is a deterministic reimplementation of the client's CMapChunk::CreateDetailDoodads
  // -- the routine that turns a chunk's ground effect ids into the grass, flowers and scrub you
  // see standing on the terrain in game. It exists because Noggit Crimson ships a Ground Effect
  // Sets editor and, until now, no way to look at its output short of launching the game.
  //
  // PROVENANCE. The transcription of the client routine in this repository is
  // src/noggit/ui/tools/NodeEditor/Nodes/World/Chunk/ChunkAddDetailDoodads.cpp, which comes from
  // upstream noggit-red. The same routine was carried much further by Natsirt867 on his
  // `ground_effects_improvements` branch (offered upstream as merge request !49 and closed
  // unmerged); the ALGORITHM below -- the noise table, the seeded shuffle, the 8x8 cell picking,
  // the weighted 16-slot doodad table, the fan-triangle slope filter -- follows that line of work.
  // The renderer that consumes it does not: see DetailDoodadRender.hpp.
  //
  // It deliberately touches no OpenGL and no Qt. It reads a MapChunk, its TextureSet and two DBCs,
  // and returns plain data. That keeps the expensive part testable and keeps it off the hot path
  // of the frame -- the caller caches the result and only rebuilds when the chunk actually
  // changes.
  //
  // WHERE IT KNOWINGLY DEPARTS FROM THE IN-REPO TRANSCRIPTION, and why. Every one of these is a
  // case where the transcription is provably wrong against this codebase's own data layout, not a
  // case of preferring a different style:
  //
  //   - Heightmap fan offsets. ChunkAddDetailDoodads uses {11, 0, 0, 1, 12, 11, 1, 12} to reach
  //     the corners of a cell. MapChunk's heightmap is 9*9 + 8*8 = 145 vertices in alternating
  //     rows of 9 and 8, so from the outer vertex at (x, y) the four corners are at +0, +1, +17
  //     and +18 and the centre is at +9 (MapChunk.cpp:176-186). 11 and 12 address neither; they
  //     are 17 and 18 short by exactly 6. This file uses 17 and 18.
  //   - Plane normalisation. The transcription computes dist = sqrt(a^2+b^2+c^2) and then
  //     MULTIPLIES the normal by it. That is the length, not its reciprocal, so the "unit normal"
  //     it then slope-tests against 0.4 is scaled by the square of the triangle's size. Here the
  //     cross product is normalised properly, which is what makes the 0.4 threshold mean the
  //     ~66 degree slope limit it is meant to mean.
  //   - Height evaluation. The transcription's `a*x + b*y + fabs(d)/c` is not the height of any
  //     plane through the cell. Here the height is evaluated on the same fan triangle by the plane
  //     equation written correctly, so a doodad sits on the surface the viewport is drawing.
  //   - Fan triangle selection. The transcription picks between two of the four triangles with the
  //     single test (u < v). Four triangles need two tests; both diagonals are compared here.
  //   - Hole test. `holeMask[splat[1] / 2 * 4 + splat[0]]` indexes a 16-element array with a value
  //     that reaches 19, which is out of bounds. Holes are 4x4 over the chunk while these cells are
  //     8x8, so the x index needs halving too: bit (cz/2)*4 + (cx/2), matching MapChunk::isHole
  //     (MapChunk.cpp:1400-1403).
  //   - Doodad fallback index. `Doodads + accumWeight & 3u` parses as `(Doodads + accumWeight) & 3`
  //     because + binds tighter than &, so with Doodads == 1 it reads column (1 + accumWeight) & 3.
  //     Written here as `Doodads + (accumWeight & 3u)`, which is the only reading that names a
  //     doodad column.
  //   - Cell-local spread. The transcription places a doodad within +/-1 unit of a cell CORNER,
  //     leaving three quarters of every 4.1667-unit cell permanently bare. The two random numbers
  //     are in [-1, 1], so they are scaled by half a cell here and applied about the cell centre,
  //     which covers the cell exactly once.
  //   - `1u << n` with n up to 63 is undefined behaviour. The visited-cell set is uint64_t and the
  //     shift is 1ull.
  //
  // WHAT IS NOT VERIFIED. Nobody here has diffed this against the client binary. The one constant
  // that is genuinely uncertain is how the shuffle's four sub-seed bytes wrap; see the note on
  // DetailDoodadRandom in the implementation. Everything downstream of the shuffle is settled by
  // the data layout above and can be checked by reading this file against MapChunk.

  // One placement. `transform` is ready to hand to ModelRender::draw as an instance matrix.
  struct DetailDoodadPlacement
  {
    glm::mat4x4 transform;
  };

  // Every placement in a chunk that uses one GroundEffectDoodad row, so the caller can resolve the
  // model once and issue one instanced draw for the whole group.
  struct DetailDoodadGroup
  {
    std::uint32_t doodad_id = 0;
    std::vector<glm::mat4x4> transforms;
  };

  // The client's groundEffectDensity: how many 8x8 cell picks to make in a chunk. The client
  // clamps it to this range and so does buildChunkDetailDoodads.
  inline constexpr unsigned DETAIL_DOODAD_MIN_DENSITY = 16;
  inline constexpr unsigned DETAIL_DOODAD_MAX_DENSITY = 256;

  // Builds every detail doodad of one chunk, grouped by doodad row.
  //
  // Returns empty for a chunk with no TextureSet, no ground effect ids, or no doodads that pass
  // the exclusion, hole and slope filters. Deterministic: the same chunk data and the same density
  // always produce the same result, because the seed is the chunk's global (x, z) index and
  // nothing else feeds the generator.
  std::vector<DetailDoodadGroup> buildChunkDetailDoodads(MapChunk* chunk, unsigned density);
}

#endif // NOGGIT_DETAILDOODADPLACEMENT_HPP
