// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/database/TileCoordinates.hpp>

#include <cmath>

using namespace Noggit::Database;

namespace
{
  // Written out rather than derived from std::acos(-1.0) so the value is a constant expression
  // and identical in every translation unit. It rounds to the same double as 2 * M_PI.
  constexpr double TWO_PI = 6.283185307179586476925286766559;

  // One tile outside the grid on either side. tileForPosition has to answer for any double a
  // caller can hand it -- a coordinate that failed to parse, an infinity out of a bad transform,
  // a NaN out of a degenerate matrix -- and converting such a value to int is undefined
  // behaviour rather than merely a wrong answer. Saturating here keeps the conversion defined
  // while still landing outside the grid, so isValidTile rejects it exactly as it should.
  constexpr int INDEX_UNDERFLOW = -1;
  constexpr int INDEX_OVERFLOW = TILES_PER_SIDE;

  // Both axes use the same formula; only the argument differs. Keeping it in one place is the
  // point -- the sign inversion is easy to get right once and easy to get wrong twice.
  int tileIndexForAxis(double v)
  {
    double const raw (std::floor(static_cast<double>(TILE_ORIGIN) - v / TILE_SIZE));

    // Phrased as a positive test so a NaN, which compares false against everything including
    // itself, falls out here instead of reaching the conversion below.
    if (!(raw >= static_cast<double>(INDEX_UNDERFLOW)))
    {
      return INDEX_UNDERFLOW;
    }

    if (raw > static_cast<double>(INDEX_OVERFLOW))
    {
      return INDEX_OVERFLOW;
    }

    return static_cast<int>(raw);
  }

  // Inverting floor(32 - x / TILE_SIZE) == i gives
  //
  //   (31 - i) * TILE_SIZE  <  x  <=  (32 - i) * TILE_SIZE
  //
  // so the tile's LOWER world edge comes from the HIGHER index. That is the sign inversion
  // showing up a second time, and it is why these two helpers are not a copy of each other with
  // a +1 moved around.
  //
  // The subtraction is done in double so an absurd tile index cannot overflow int on the way in.
  // Every value involved is a small whole number, which double represents exactly, so the
  // products are bit-identical to the ones tileIndexForAxis divides back out -- a tile's upper
  // edge and its neighbour's lower edge are the same double, not merely close.
  double axisLowerEdge(int index)
  {
    return (static_cast<double>(TILE_ORIGIN) - static_cast<double>(index) - 1.0) * TILE_SIZE;
  }

  double axisUpperEdge(int index)
  {
    return (static_cast<double>(TILE_ORIGIN) - static_cast<double>(index)) * TILE_SIZE;
  }
}

TileIndex TileCoordinates::tileForPosition(double x, double y)
{
  TileIndex tile;
  tile.x = tileIndexForAxis(x);
  tile.y = tileIndexForAxis(y);

  return tile;
}

TileIndex TileCoordinates::tileForPosition(WorldPosition const& position)
{
  // z plays no part: ADT tiles are columns, and a spawn underground belongs to the same tile as
  // one on the surface above it.
  return tileForPosition(position.x, position.y);
}

TileBounds TileCoordinates::boundsForTile(TileIndex const& tile)
{
  // Not validated against the grid on purpose. Bounds for an off-grid index are still
  // meaningful arithmetic, and the caller that wants the grid check has isValidTile.
  TileBounds bounds;
  bounds.min_x = axisLowerEdge(tile.x);
  bounds.max_x = axisUpperEdge(tile.x);
  bounds.min_y = axisLowerEdge(tile.y);
  bounds.max_y = axisUpperEdge(tile.y);

  return bounds;
}

bool TileCoordinates::isValidTile(TileIndex const& tile)
{
  return tile.x >= 0 && tile.x < TILES_PER_SIDE
      && tile.y >= 0 && tile.y < TILES_PER_SIDE;
}

bool TileCoordinates::isValidPosition(double x, double y)
{
  // Deliberately expressed as "does this land on the grid" rather than as
  // std::abs(x) <= MAP_HALF_EXTENT, because the two disagree at one point and the grid is the
  // one that matters. The tiles cover (-MAP_HALF_EXTENT, +MAP_HALF_EXTENT] on each axis: the
  // positive extent is the upper edge of tile 0, but the negative extent is one step past the
  // lower edge of tile 63 and floors to index 64, which is off the map. Answering "valid" for a
  // position whose tile is invalid would hand the caller a tile it cannot load.
  //
  // This also inherits the saturation in tileIndexForAxis, so infinities and NaN answer false
  // rather than trapping.
  return isValidTile(tileForPosition(x, y));
}

TileCoordinates::Quaternion TileCoordinates::quaternionForOrientation(double orientation)
{
  // Normalised first. sin and cos accept any argument, but a negative or wrapped input produces
  // the NEGATED quaternion -- the same rotation, different numbers in the row. That reads back
  // as a changed gameobject when nothing has moved, which is a diff the user has to disprove by
  // hand.
  double const half_angle (normaliseOrientation(orientation) * 0.5);

  Quaternion rotation;
  rotation.r0 = 0.0;
  rotation.r1 = 0.0;
  rotation.r2 = std::sin(half_angle);
  rotation.r3 = std::cos(half_angle);

  return rotation;
}

double TileCoordinates::orientationForQuaternion(Quaternion const& rotation)
{
  // std::atan2 recovers the half-angle over the full circle, which matters because a quaternion
  // and its negation describe the same rotation: (0, 0, -sin(o/2), -cos(o/2)) has to decode to
  // the same orientation as (0, 0, sin(o/2), cos(o/2)). atan2 puts the negated branch half a
  // turn away and normaliseOrientation folds it back.
  //
  // r0 and r1 are ignored. For a rotation about the vertical axis they are zero, and a row
  // where they are not is not describing a yaw -- reporting a yaw for it anyway would be worse
  // than ignoring them.
  return normaliseOrientation(2.0 * std::atan2(rotation.r2, rotation.r3));
}

double TileCoordinates::normaliseOrientation(double orientation)
{
  double normalised (std::fmod(orientation, TWO_PI));

  if (normalised < 0.0)
  {
    normalised += TWO_PI;
  }

  // The addition above is where the range is actually lost. A tiny negative input -- -1e-18,
  // which fmod passes through untouched -- is far below half an ulp of TWO_PI, so adding TWO_PI
  // back rounds to exactly TWO_PI. That is outside the half-open range the core stores and it
  // round-trips as a full turn, so fold it to zero.
  //
  // The test is negated so a NaN argument lands here too and leaves with an angle instead of a
  // NaN that would otherwise flow straight into a quaternion and out into an INSERT.
  if (!(normalised < TWO_PI))
  {
    normalised = 0.0;
  }

  // fmod preserves the sign of the argument, so an exact multiple of TWO_PI on the negative side
  // arrives here as -0.0. It compares equal to zero and is harmless arithmetically, but it
  // renders as "-0" in emitted SQL and shows up as a spurious diff against a stored 0.
  if (normalised == 0.0)
  {
    return 0.0;
  }

  return normalised;
}
