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

    // One full turn, in radians.
    //
    // Written out rather than derived from std::acos(-1.0) so the value is a constant expression
    // and identical in every translation unit. It rounds to 0x1.921fb54442d18p+2, which is the
    // same double as 2 * M_PI.
    //
    // Shared rather than copied per module. The emitter, the transform planner and this file all
    // fold angles against it; a second copy written to fewer digits is a silent invitation for
    // one of them to wrap at a different place from the others.
    constexpr double TWO_PI = 6.283185307179586476925286766559;

    // Largest yaw disagreement between a gameobject's `orientation` column and its rotation
    // quaternion that is written off as float noise rather than reported. A tenth of a degree is
    // far below anything a reviewer could see in game.
    //
    // Shared for the same reason: ChangesetBuilder and ChunkTransform are answering the same
    // question about the same row, and two thresholds would mean one warns where the other is
    // silent.
    constexpr double ORIENTATION_AGREEMENT_TOLERANCE = 1.0e-3;

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

    // True when `rotation` still holds the default-constructed identity, which is
    // indistinguishable from "nobody authored this" -- the caller then has nothing to go on but
    // the `orientation` column.
    //
    // Exact comparison is deliberate and is not the float-equality mistake docs/schema-335.md
    // warns about: the question is whether the field was touched, not whether two computed
    // rotations agree. It lives here because every module that chooses between the quaternion and
    // the orientation column has to ask it the same way -- ChangesetBuilder when it emits the
    // row, ChunkTransform when it rotates one, GmCommands when it points a GM at one. A copy that
    // drifted would make one of them read a yaw out of a field nobody set.
    bool isDefaultRotation(Quaternion const& rotation);

    // Normalises any angle into [0, 2*pi). The core stores orientations in this range and a
    // negative or wrapped value round-trips differently, which shows up as spawns facing the
    // wrong way rather than as an error.
    double normaliseOrientation(double orientation);

    // Shortest angular distance between two yaws, so 0.001 and 2*pi - 0.001 come out close rather
    // than a full turn apart. Both arguments are expected to be normalised already;
    // normaliseOrientation is how a caller gets there.
    double yawSeparation(double a, double b);
  }
}

#endif
