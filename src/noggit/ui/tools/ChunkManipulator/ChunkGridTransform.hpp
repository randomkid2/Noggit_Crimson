// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_UI_TOOLS_CHUNKMANIPULATOR_CHUNKGRIDTRANSFORM_HPP
#define NOGGIT_UI_TOOLS_CHUNKMANIPULATOR_CHUNKGRIDTRANSFORM_HPP

#include <array>
#include <cstdint>
#include <vector>

// ONE PERMUTATION KERNEL FOR EVERY GRID A CHUNK CARRIES.
//
// A chunk stores its data on six differently-shaped square grids, and a rotation that moves one
// of them without moving the others is worse than no rotation at all: the terrain turns and the
// texture blend, the water and the trees stay where they were. So all six go through the single
// index map below, parameterised only by the grid's edge length.
//
// EVERY ONE OF THEM IS INDEXED (row = Z, column = X), which is what makes one kernel enough.
// Verified against the source rather than assumed, because the six are written by five different
// files and nothing forces them to agree:
//
//   terrain heights     MapChunk.cpp:176-186  MCVT reader: j counts 17 rows, zpos = j * 0.5 *
//                                             UNITSIZE, i counts columns, xpos = i * UNITSIZE.
//                                             So the 145 vertices are TWO interleaved square
//                                             grids: a 9x9 outer at index r*17+c, and an 8x8
//                                             inner at r*17+9+c offset half a unit in both axes.
//                                             MapChunk::indexNoLoD (MapChunk.cpp:356) and
//                                             indexLoD (:351) are exactly those two expressions.
//   alphamaps 64x64     texture_set.cpp:1052  offset = j * 64 + i, with z_pos advancing per j
//                                             and x_pos per i.
//   shadow map 64x64    MapChunk.cpp:256      the edge fix writes [i*64+63] from [i*64+62] and
//                                             [63*64+i] from [62*64+i] -- row-major, row = Z.
//   holes 4x4           MapChunk.cpp:1402     isHole(i, j) tests bit j*4+i, so bit = row*4+col.
//   doodad stencil 8x8  texture_set.cpp:456   _doodadStencil[y] bit x -- one byte per Z row.
//   doodad mapping 8x8  texture_set.cpp:438   _doodadMapping[y] >> (x*2) -- two bits per column.
//   liquid vertices 9x9 liquid_layer.cpp:255  create_vertices: index = z*9+x.
//   liquid subchunks    liquid_layer.cpp:530  bit = pz*8+px.
//
// WHY THE SAME FORMULA WORKS FOR POINT GRIDS AND CELL GRIDS. Take a 90 degree step to be the
// world map (X, Z) -> (Z, -X) about the block's corner, followed by the translation that brings
// the result back into the positive quadrant. For an N-point grid whose points sit at
// (c*U, r*U), U = spacing, the extent is (N-1)*U, so the translated image of point (r, c) is
// x' = r*U, z' = ((N-1)-c)*U -- integral, and on the grid. For an N-cell grid whose cell (r, c)
// covers [c*s, (c+1)*s] x [r*s, (r+1)*s], the image spans [r*s, (r+1)*s] x [(N-1-c)*s, (N-c)*s]
// -- again exactly one cell. Both give destination (N-1-c, r) for source (r, c), i.e. the
// destination reads from source (col, N-1-row). The 8x8 inner terrain grid is a point grid
// offset by half a unit; its points at ((c+.5)U, (r+.5)U) map to ((r+.5)U, (7.5-c)U) =
// ((r+.5)U, ((7-c)+.5)U), so it uses the same rule with N = 8.
//
// The rotation direction this produces is stated once, in ChunkGridTransform.cpp, together with
// the yaw change that has to accompany it on every model carried along.
namespace Noggit::Ui::Tools::ChunkManipulator
{
  enum class ChunkGridOp : std::uint8_t
  {
    IDENTITY = 0,
    ROTATE_90 = 1, //!< One 90 degree step. Applied three times for the panel's 270 entry.
    MIRROR_X = 2,  //!< "Horizontal mirror" (F): world X negated about the block centre.
    MIRROR_Z = 3   //!< "Vertical mirror" (Alt+F): world Z negated about the block centre.
  };

  //! The source (row, column) a destination (row, column) reads from, on an n x n grid.
  //!
  //! Returned as a linear row-major index so the callers below stay one line each. `n` is the
  //! edge length, not the element count.
  [[nodiscard]]
  constexpr int gridSourceIndex (ChunkGridOp op, int n, int row, int col)
  {
    switch (op)
    {
      case ChunkGridOp::ROTATE_90: return col * n + (n - 1 - row);
      case ChunkGridOp::MIRROR_X:  return row * n + (n - 1 - col);
      case ChunkGridOp::MIRROR_Z:  return (n - 1 - row) * n + col;
      case ChunkGridOp::IDENTITY:
      default:                     return row * n + col;
    }
  }

  //! Permute a dense row-major n x n block of `T` in place.
  //!
  //! The whole block is copied out first because the permutation is not decomposable into
  //! disjoint swaps for the rotation case -- writing in place without the copy would read
  //! destinations that have already been overwritten. The largest grid here is 64 x 64 floats
  //! (16 KiB for the temporary alphamap layers), which is why this is a plain vector and not a
  //! cycle-following in-place permutation.
  template<typename T>
  void permuteSquare (ChunkGridOp op, int n, T* data)
  {
    if (op == ChunkGridOp::IDENTITY)
    {
      return;
    }

    std::vector<T> const source (data, data + static_cast<std::size_t> (n) * n);

    for (int row (0); row < n; ++row)
    {
      for (int col (0); col < n; ++col)
      {
        data[row * n + col] = source[static_cast<std::size_t> (gridSourceIndex (op, n, row, col))];
      }
    }
  }

  //! The 145-vertex terrain layout: the 9x9 outer grid and the 8x8 inner grid, permuted
  //! separately because they are two grids that happen to share one array.
  //!
  //! `stride` is the number of `T` per vertex -- 1 for a height array, 3 for a vec3 array read
  //! as floats.
  template<typename T>
  void permuteTerrainGrid (ChunkGridOp op, T* data, int stride);

  //! Holes: a 4x4 bitmask, bit = row * 4 + col (MapChunk::isHole, MapChunk.cpp:1402).
  //!
  //! Only the low 16 bits carry the 4x4 mask. MapChunk::setHole writes 0xFFFFFFFF for "fill the
  //! chunk" (MapChunk.cpp:1409), so the high bits can be set; they are preserved untouched
  //! rather than dropped, because nothing here knows what the high-resolution hole variant
  //! (mcnk_flags::high_res_holes) would want done with them and silently clearing them would
  //! turn a filled chunk into a partly filled one.
  [[nodiscard]]
  int permuteHoles (ChunkGridOp op, int holes);

  //! Liquid subchunk coverage: an 8x8 bitmask, bit = row * 8 + col (liquid_layer.cpp:530).
  [[nodiscard]]
  std::uint64_t permuteSubchunkMask (ChunkGridOp op, std::uint64_t mask);

  //! Detail-doodad exclusion: 8 bytes, one Z row each, bit `col` per column
  //! (TextureSet::getDoodadDisabledAt, texture_set.cpp:456).
  [[nodiscard]]
  std::array<std::uint8_t, 8> permuteDoodadStencil (ChunkGridOp op, std::array<std::uint8_t, 8> const& stencil);

  //! Detail-doodad layer mapping ("predTex"): 8 uint16, one Z row each, two bits per column
  //! (TextureSet::getDoodadActiveLayerIdAt, texture_set.cpp:438).
  [[nodiscard]]
  std::array<std::uint16_t, 8> permuteDoodadMapping (ChunkGridOp op, std::array<std::uint16_t, 8> const& mapping);

  //! The yaw a model carried through `op` must be given, in degrees, from the yaw it had.
  //!
  //! Derivation and its limit are in ChunkGridTransform.cpp; the short version is that a
  //! rotation adds and a mirror negates, and that a mirror is exact only for a model whose
  //! pitch and roll are zero.
  [[nodiscard]]
  float transformedYaw (ChunkGridOp op, float yaw_degrees);
}

#endif // NOGGIT_UI_TOOLS_CHUNKMANIPULATOR_CHUNKGRIDTRANSFORM_HPP
