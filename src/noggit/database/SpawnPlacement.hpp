// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_DATABASE_SPAWNPLACEMENT_HPP
#define NOGGIT_DATABASE_SPAWNPLACEMENT_HPP

#include <noggit/database/SpawnTypes.hpp>
#include <noggit/database/TileCoordinates.hpp>

namespace Noggit::Database
{
  // Converts a database spawn into the position and rotation Noggit's scene graph wants.
  //
  // This is the seam between two coordinate frames, and it is the single place in the project
  // where getting it wrong produces no error at all -- just spawns kilometres from where they
  // belong, or facing the wrong way. So the arithmetic is here, isolated, and tested, rather
  // than inline at a call site.
  //
  // Deliberately free of glm, Qt and every Noggit header other than this layer's own, so it
  // stays in the test target that runs without a toolchain. The glue that hands these values to
  // a SceneObject static_asserts that ZEROPOINT_F below equals Noggit's own ZEROPOINT, so the
  // duplication cannot drift.

  // Where and how to place one spawn in Noggit's frame.
  //
  // `dir` is Euler angles in DEGREES, matching SceneObject::dir, which is a bare glm::vec3 with
  // no unit type safety on it at all -- so the units are stated here instead.
  struct NoggitPlacement
  {
    double x = 0.0;              // Noggit x: east
    double y = 0.0;              // Noggit y: UP. This is the server's z.
    double z = 0.0;              // Noggit z: south
    double dir_x_degrees = 0.0;
    double dir_y_degrees = 0.0;  // yaw
    double dir_z_degrees = 0.0;
    double scale = 1.0;
  };

  namespace SpawnPlacement
  {
    // Noggit's ZEROPOINT, computed exactly as MapHeaders.h:58,63 does.
    //
    // The float arithmetic is not incidental. Noggit computes 32.0f * 533.33333f in float, which
    // is 17066.666015625. This layer's TILE_SIZE is a double, and 32.0 * 533.33333 is
    // 17066.66656 -- a different number by 5.4e-4. Small, but it would put the static_assert at
    // the glue seam in the wrong place and quietly shift every converted position by half a
    // millimetre. Computed the way Noggit does it, the two agree bit for bit.
    //
    // > [!warning] Do not mix this float with the database layer's double TILE_SIZE
    // > ZEROPOINT_F is 17066.666015625; 32 * TILE_SIZE in double is 17066.66656. Dividing the
    // > float value by a double TILE_SIZE yields 31.999998 rather than 32, which is a real
    // > off-by-one tile at exactly world_x == 0 or world_y == 0 -- values that occur in real
    // > data. Derive a tile index either entirely in this layer's doubles
    // > (TileCoordinates::tileForPosition) or entirely in Noggit's floats, never across the two.
    inline constexpr float ZEROPOINT_F = 32.0f * 533.33333f;

    // Position only. Mirrors math::to_client(x, y, z) from src/math/coordinates.hpp:25-28:
    //
    //     noggit.x = ZEROPOINT - world_y
    //     noggit.y =             world_z      (height, no offset either way)
    //     noggit.z = ZEROPOINT - world_x
    //
    // Note which axis feeds which. The x/z swap here is the same transposition that separates
    // Database::TileIndex from Noggit's, and it is why field-by-field assignment between the two
    // silently loads a tile ~9.6 km away.
    NoggitPlacement positionFor(WorldPosition const& world);

    // The inverse, matching math::to_server. Exposed so a round trip can be asserted, and
    // because the editor has to turn a dragged model back into a server coordinate to emit SQL.
    WorldPosition serverPositionFor(NoggitPlacement const& placement);

    // Yaw offset applied on top of the TrinityCore orientation, in degrees.
    //
    // Derivation. A TrinityCore orientation `o` means facing (cos o, sin o, 0) in server
    // coordinates. Carrying that direction through to_client's linear part
    // (nx = -sy, ny = sz, nz = -sx) gives a Noggit facing of (-sin o, 0, -cos o).
    //
    // SceneObject::updateTransformMatrix (SceneObject.cpp:74-78) applies
    // glm::eulerAngleYZX(radians(dir.y - 90), ...), so the yaw actually applied is dir.y - 90
    // degrees. A right-handed yaw of theta about +Y takes a local +X forward to
    // (cos theta, 0, -sin theta). Setting that equal to the required facing:
    //
    //     cos theta = -sin o  and  sin theta = cos o   =>   theta = o + 90 degrees
    //
    // and since Noggit applies dir.y - 90, that means dir.y = degrees(o) + 180.
    //
    // > [!warning] This constant assumes M2 local forward is +X, which is NOT verified
    // > Everything above is derived from source. The one link that is not is the model-space
    // > forward convention. If spawns render at the right place facing consistently but
    // > uniformly wrong, this constant is why -- it will be off by exactly 90, 180 or 270, and
    // > nothing else needs to change. A single look at a fixture in the viewport settles it,
    // > which is cheaper than deriving it from the M2 format.
    inline constexpr double YAW_OFFSET_DEGREES = 180.0;

    // Full placement for a creature. `model_scale` comes from CreatureDisplayInfo's
    // CreatureModelScale (field 4) multiplied by CreatureModelData's ModelScale (field 4);
    // pass 1.0 when unresolved rather than 0, which would make the model vanish.
    NoggitPlacement placementFor(CreatureSpawn const& spawn, double model_scale = 1.0);

    // Full placement for a gameobject.
    //
    // Uses `orientation`, not the rotation0..3 quaternion, when the two disagree: the core reads
    // orientation for facing and the quaternion is what the client renders, but a spawn authored
    // by a GM tool sets both consistently and a hand-edited row often updates only orientation.
    // Preferring orientation matches what the server believes.
    NoggitPlacement placementFor(GameObjectSpawn const& spawn, double model_scale = 1.0);

    // The ADT tile a world position belongs to, in Noggit's axis order and ADT filename order --
    // `<map>_<x>_<z>.adt`. Convenience so a caller never has to remember the transposition.
    AdtFileIndex adtIndexFor(WorldPosition const& world);

    // Normalises a Euler angle into [-180, 180), matching SceneObject::normalizeDirection
    // (SceneObject.cpp:96-113). Noggit only normalises on explicit request, so a value handed in
    // from here should already be in range.
    double normaliseDegrees(double degrees);
  }
}

#endif
