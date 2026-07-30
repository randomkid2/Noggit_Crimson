// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_DATABASE_TILECOORDINATES_HPP
#define NOGGIT_DATABASE_TILECOORDINATES_HPP

#include <cstdint>

namespace Noggit::Database
{
  // ADT tile arithmetic for TrinityCore world coordinates.
  //
  // Verified against real spawn data: creatures at x ~ -9500, y ~ 70 on map 0 resolve to tile
  // (49, 31), which is Elwynn Forest. The formula works directly on TrinityCore coordinates
  // with no intermediate transform.
  //
  // Pure arithmetic, no dependencies. All computation is done in double even though the
  // database columns are float, because errors accumulate: a FLOAT holds about 7 significant
  // digits, so at the ~17000 yard edge of the map the representable step is roughly 0.002
  // yards. Narrow once, on write, never in the middle of a calculation.

  constexpr double TILE_SIZE = 533.33333;
  constexpr int TILES_PER_SIDE = 64;
  constexpr int TILE_ORIGIN = 32;              // tile index of the world origin
  constexpr double MAP_HALF_EXTENT = TILE_ORIGIN * TILE_SIZE;   // 17066.66656

  // A tile index in WORLD-AXIS order: x is derived from world x, y from world y.
  //
  // > [!warning] This is the TRANSPOSE of Noggit's own ::TileIndex
  // > The pre-existing global `::TileIndex` (src/noggit/TileIndex.hpp) works in Noggit's
  // > internal frame, where `pos.x = ZEROPOINT - world_y` and `pos.z = ZEROPOINT - world_x`.
  // > So `::TileIndex::x` is derived from world **y**, and `::TileIndex::z` from world **x** --
  // > the opposite of the fields here, despite `x` naming both.
  // >
  // > The Elwynn tile that Noggit loads as `Azeroth_31_49.adt` is `::TileIndex{x=31, z=49}`
  // > but `Database::TileIndex{x=49, y=31}`.
  // >
  // > Passing one straight into the other compiles, keeps both indices inside 0..63, passes
  // > isValidTile, and returns a plausible non-empty result set from a tile roughly 9.6 km
  // > away on both axes. Only the diagonal x == y is immune. Use the conversions below rather
  // > than assigning field by field.
  struct TileIndex
  {
    int x = 0;
    int y = 0;

    friend bool operator==(TileIndex const& a, TileIndex const& b)
    {
      return a.x == b.x && a.y == b.y;
    }

    friend bool operator!=(TileIndex const& a, TileIndex const& b) { return !(a == b); }
  };

  // Tile index in ADT-filename order, `<map>_<x>_<z>.adt`, which is also the order Noggit's
  // own ::TileIndex and map_index use.
  struct AdtFileIndex
  {
    int x = 0;
    int z = 0;
  };

  // Defined inline, and named so that the transposition is impossible to perform by accident.
  inline AdtFileIndex toAdtFileIndex(TileIndex const& tile)
  {
    return AdtFileIndex{tile.y, tile.x};
  }

  inline TileIndex fromAdtFileIndex(AdtFileIndex const& adt)
  {
    return TileIndex{adt.z, adt.x};
  }

  // A world position. Field names match the database columns to keep the mapping obvious.
  struct WorldPosition
  {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
  };

  // Axis-aligned world-space bounds of a tile, used to build the spawn query.
  struct TileBounds
  {
    double min_x = 0.0;
    double max_x = 0.0;
    double min_y = 0.0;
    double max_y = 0.0;
  };

  namespace TileCoordinates
  {
    // blockX = floor(32 - x / 533.33333), blockY = floor(32 - y / 533.33333).
    //
    // Note the inversion: increasing world x yields a DECREASING tile index. Getting the sign
    // wrong still produces plausible-looking indices, which is why this is tested against
    // known real spawns rather than only against synthetic values.
    TileIndex tileForPosition(double x, double y);
    TileIndex tileForPosition(WorldPosition const& position);

    // World-space bounds of a tile, as the half-open interval **(min, max]** on each axis:
    // the lower edge is EXCLUSIVE and the upper edge is INCLUSIVE.
    //
    // That is the opposite way round from the usual convention, because the axis runs
    // backwards. Inverting floor(32 - x / TILE_SIZE) == i gives
    // (31-i)*TILE_SIZE < x <= (32-i)*TILE_SIZE, so min_x belongs to the neighbour with the
    // HIGHER index. Any consumer building a SQL predicate wants `> min AND <= max`; writing
    // `>= min AND < max` silently attributes every tile-edge spawn to the wrong tile.
    //
    // tileForPosition is the authority on ownership; this function agrees with it by
    // construction.
    TileBounds boundsForTile(TileIndex const& tile);

    // True when the tile index is inside the 64x64 map grid.
    bool isValidTile(TileIndex const& tile);

    // True when the position lies inside the representable world extent.
    bool isValidPosition(double x, double y);

    // Gameobject orientation is stored as a quaternion in rotation0..3, not as Euler angles.
    // For an orientation o about the vertical axis: rotation2 = sin(o/2), rotation3 = cos(o/2),
    // with rotation0 and rotation1 zero.
    struct Quaternion
    {
      double r0 = 0.0;
      double r1 = 0.0;
      double r2 = 0.0;
      double r3 = 1.0;
    };

    Quaternion quaternionForOrientation(double orientation);

    // Inverse of the above. Returns a value in [0, 2*pi).
    double orientationForQuaternion(Quaternion const& rotation);

    // Normalises any angle into [0, 2*pi). The core stores orientations in this range and a
    // negative or wrapped value round-trips differently, which shows up as spawns facing the
    // wrong way rather than as an error.
    double normaliseOrientation(double orientation);
  }
}

#endif
