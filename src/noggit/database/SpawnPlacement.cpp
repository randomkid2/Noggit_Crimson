// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/database/SpawnPlacement.hpp>

#include <cmath>

using namespace Noggit::Database;

namespace
{
  constexpr double DEGREES_PER_RADIAN = 57.29577951308232;   // 180 / pi
  constexpr double FULL_TURN_DEGREES = 360.0;

  // Widened once, here, so every conversion below uses the same value as the glue seam's
  // static_assert against Noggit's ZEROPOINT.
  constexpr double ZEROPOINT = static_cast<double>(SpawnPlacement::ZEROPOINT_F);
}

NoggitPlacement SpawnPlacement::positionFor(WorldPosition const& world)
{
  NoggitPlacement placement;

  // math::to_client(x, y, z) -> { -y + ZEROPOINT, z, -x + ZEROPOINT }
  placement.x = ZEROPOINT - world.y;
  placement.y = world.z;
  placement.z = ZEROPOINT - world.x;

  return placement;
}

WorldPosition SpawnPlacement::serverPositionFor(NoggitPlacement const& placement)
{
  WorldPosition world;

  // math::to_server(x, y, z) -> { ZEROPOINT - z, ZEROPOINT - x, y }
  world.x = ZEROPOINT - placement.z;
  world.y = ZEROPOINT - placement.x;
  world.z = placement.y;

  return world;
}

double SpawnPlacement::normaliseDegrees(double degrees)
{
  double wrapped (std::fmod(degrees, FULL_TURN_DEGREES));

  if (wrapped >= 180.0)
  {
    wrapped -= FULL_TURN_DEGREES;
  }
  else if (wrapped < -180.0)
  {
    wrapped += FULL_TURN_DEGREES;
  }

  // fmod preserves its argument's sign, so -360 arrives here as -0.0. It compares equal to zero
  // but renders as "-0" and reads as a difference against a stored 0.
  return wrapped == 0.0 ? 0.0 : wrapped;
}

namespace
{
  double yawDegreesFor(double orientation_radians)
  {
    return SpawnPlacement::normaliseDegrees
      (orientation_radians * DEGREES_PER_RADIAN + SpawnPlacement::YAW_OFFSET_DEGREES);
  }

  // A model scale of zero collapses the model to nothing, which looks identical to "failed to
  // load" and would send someone hunting the wrong bug. An unresolved or nonsense scale becomes
  // 1.0 so the spawn is visible and obviously the wrong size rather than invisible.
  double usableScale(double model_scale)
  {
    return (std::isfinite(model_scale) && model_scale > 0.0) ? model_scale : 1.0;
  }
}

NoggitPlacement SpawnPlacement::placementFor(CreatureSpawn const& spawn, double model_scale)
{
  NoggitPlacement placement (positionFor(spawn.position));

  placement.dir_y_degrees = yawDegreesFor(spawn.orientation);
  placement.scale = usableScale(model_scale);

  return placement;
}

NoggitPlacement SpawnPlacement::placementFor(GameObjectSpawn const& spawn, double model_scale)
{
  NoggitPlacement placement (positionFor(spawn.position));

  placement.dir_y_degrees = yawDegreesFor(spawn.orientation);
  placement.scale = usableScale(model_scale);

  return placement;
}

AdtFileIndex SpawnPlacement::adtIndexFor(WorldPosition const& world)
{
  return toAdtFileIndex(TileCoordinates::tileForPosition(world.x, world.y));
}
