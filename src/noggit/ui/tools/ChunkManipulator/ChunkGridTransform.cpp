// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include "ChunkGridTransform.hpp"

#include <noggit/MapChunk.h>

#include <cmath>

using namespace Noggit::Ui::Tools::ChunkManipulator;

// WHICH WAY ROUND THE 90 DEGREE STEP GOES, AND THE YAW THAT HAS TO GO WITH IT.
//
// gridSourceIndex's ROTATE_90 implements the world map (X, Z) -> (Z, -X) about the block centre,
// as derived in the header. Written as a 2x2 acting on a vector relative to the centre, that is
// (u, v) -> (v, -u).
//
// SceneObject::updateTransformMatrix (SceneObject.cpp:74-78) builds a model's orientation as
// glm::eulerAngleYZX(radians(dir.y - 90), ...), and glm's eulerAngleY(theta) sends
// (x, z) to (x cos + z sin, -x sin + z cos). Setting that equal to (v, -u) for (u, v) = (x, z):
//
//     x' = v  requires  cos(theta) = 0 and sin(theta) = 1, i.e. theta = +90
//     z' = -u then follows: -x sin + z cos = -x
//
// The matrix uses dir.y - 90, so adding 90 to dir.y adds exactly eulerAngleY(+90). Hence
// ROTATE_90 pairs with dir.y += 90. That pairing is the whole point of this file: a rotation
// that moves terrain and leaves its trees behind is a bug that looks like a feature until
// somebody uses it.
//
// THE MIRRORS BOTH NEGATE THE YAW, and that is not a typo. Conjugating a rotation by any
// reflection inverts it -- M R(theta) M = R(-theta) for every reflection M in the plane -- so
// MIRROR_X and MIRROR_Z produce the same formula. Checked concretely for theta = 90 in both
// cases: mirror-X sends (1,0) -> (-1,0) -> R(90) -> (0,1) -> mirror-X -> (0,1), and mirror-Z
// sends (1,0) -> (1,0) -> R(90) -> (0,-1) -> mirror-Z -> (0,1); R(-90) sends (1,0) to (0,1).
// With theta = dir.y - 90 and theta' = -theta, dir.y' - 90 = 90 - dir.y, so dir.y' = 180 - dir.y.
//
// WHAT A MIRROR CANNOT DO. A reflection is orientation-reversing and no rotation of an
// unreflected mesh reproduces it. The formula above is exact for the placement's FACING and for
// any model symmetric about the mirrored plane; a model with a non-zero pitch or roll
// (dir.x or dir.z) comes out with that tilt un-mirrored, because expressing it would require
// negative scale on one axis and ModelInstance carries a single uniform scale
// (SceneObject.hpp:84). dir.x and dir.z are therefore left alone rather than guessed at. Almost
// every doodad placement in a 3.3.5 ADT is upright, so this is a corner rather than a wall, but
// it is a real limit and it is not hidden.

namespace Noggit::Ui::Tools::ChunkManipulator
{
  template<typename T>
  void permuteTerrainGrid (ChunkGridOp op, T* data, int stride)
  {
    if (op == ChunkGridOp::IDENTITY)
    {
      return;
    }

    std::vector<T> const source (data, data + static_cast<std::size_t> (mapbufsize) * stride);

    auto const copy_vertex
      ( [&] (int destination, int origin)
        {
          for (int component (0); component < stride; ++component)
          {
            data[destination * stride + component]
              = source[static_cast<std::size_t> (origin) * stride + component];
          }
        }
      );

    // The 9x9 outer grid. gridSourceIndex answers on a dense 9x9; MapChunk::indexNoLoD turns a
    // (row, column) back into the interleaved 145-vertex layout.
    for (int row (0); row < 9; ++row)
    {
      for (int col (0); col < 9; ++col)
      {
        int const flat (gridSourceIndex (op, 9, row, col));
        copy_vertex (MapChunk::indexNoLoD (row, col), MapChunk::indexNoLoD (flat / 9, flat % 9));
      }
    }

    // The 8x8 inner grid, half a unit offset in both axes, which is why it rotates with N = 8
    // and not with N = 9.
    for (int row (0); row < 8; ++row)
    {
      for (int col (0); col < 8; ++col)
      {
        int const flat (gridSourceIndex (op, 8, row, col));
        copy_vertex (MapChunk::indexLoD (row, col), MapChunk::indexLoD (flat / 8, flat % 8));
      }
    }
  }

  // The two instantiations this feature needs: a bare height array and a vec3 array read as
  // floats. Explicit rather than exported, so the template body can stay next to the derivation.
  template void permuteTerrainGrid<float> (ChunkGridOp, float*, int);

  int permuteHoles (ChunkGridOp op, int holes)
  {
    if (op == ChunkGridOp::IDENTITY)
    {
      return holes;
    }

    unsigned const original (static_cast<unsigned> (holes));
    unsigned result (original & ~0xFFFFu);

    for (int row (0); row < 4; ++row)
    {
      for (int col (0); col < 4; ++col)
      {
        if (original & (1u << gridSourceIndex (op, 4, row, col)))
        {
          result |= 1u << (row * 4 + col);
        }
      }
    }

    return static_cast<int> (result);
  }

  std::uint64_t permuteSubchunkMask (ChunkGridOp op, std::uint64_t mask)
  {
    if (op == ChunkGridOp::IDENTITY)
    {
      return mask;
    }

    std::uint64_t result (0);

    for (int row (0); row < 8; ++row)
    {
      for (int col (0); col < 8; ++col)
      {
        if (mask & (std::uint64_t (1) << gridSourceIndex (op, 8, row, col)))
        {
          result |= std::uint64_t (1) << (row * 8 + col);
        }
      }
    }

    return result;
  }

  std::array<std::uint8_t, 8> permuteDoodadStencil (ChunkGridOp op, std::array<std::uint8_t, 8> const& stencil)
  {
    if (op == ChunkGridOp::IDENTITY)
    {
      return stencil;
    }

    std::array<std::uint8_t, 8> result {};

    for (int row (0); row < 8; ++row)
    {
      for (int col (0); col < 8; ++col)
      {
        int const flat (gridSourceIndex (op, 8, row, col));

        if (stencil[static_cast<std::size_t> (flat / 8)] & (1u << (flat % 8)))
        {
          result[static_cast<std::size_t> (row)] |= static_cast<std::uint8_t> (1u << col);
        }
      }
    }

    return result;
  }

  std::array<std::uint16_t, 8> permuteDoodadMapping (ChunkGridOp op, std::array<std::uint16_t, 8> const& mapping)
  {
    if (op == ChunkGridOp::IDENTITY)
    {
      return mapping;
    }

    std::array<std::uint16_t, 8> result {};

    for (int row (0); row < 8; ++row)
    {
      for (int col (0); col < 8; ++col)
      {
        int const flat (gridSourceIndex (op, 8, row, col));
        unsigned const layer
          ((mapping[static_cast<std::size_t> (flat / 8)] >> ((flat % 8) * 2)) & 0x03u);

        result[static_cast<std::size_t> (row)]
          |= static_cast<std::uint16_t> (layer << (col * 2));
      }
    }

    return result;
  }

  float transformedYaw (ChunkGridOp op, float yaw_degrees)
  {
    float result (yaw_degrees);

    switch (op)
    {
      case ChunkGridOp::ROTATE_90: result = yaw_degrees + 90.0f; break;
      case ChunkGridOp::MIRROR_X:
      case ChunkGridOp::MIRROR_Z:  result = 180.0f - yaw_degrees; break;
      case ChunkGridOp::IDENTITY:
      default:                     return yaw_degrees;
    }

    // Wrapped into (-180, 180] the same way math::normalize_degrees does it (trig.hpp:19-22),
    // because SceneObject::dir is a bare glm::vec3 of degrees rather than a math::degrees and
    // nothing downstream normalises it. Four rotations would otherwise leave a doodad reading
    // 360 in the object editor, and a mirror of a doodad facing 300 would read -120.
    // The rotation matrix does not care -- sin and cos are periodic -- but the number a user
    // reads does.
    return result - std::floor ((result + 180.0f) / 360.0f) * 360.0f;
  }
}
