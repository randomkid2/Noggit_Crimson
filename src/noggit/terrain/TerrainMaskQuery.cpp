// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/terrain/TerrainMaskQuery.hpp>
#include <noggit/terrain/TerrainMaskStore.hpp>

namespace Noggit
{
  namespace TerrainMaskQuery
  {
    float factorAt(float world_x, float world_z)
    {
      // The flag is tested before the singleton is touched, and that ordering is the entire reason
      // this function is a separate translation unit rather than an inline in the header.
      //
      // TerrainMaskStore::instance() is a function-local static, so every call to it goes through
      // the thread-safe-initialisation guard the compiler emits -- an atomic load on the happy
      // path. This function runs once per terrain vertex and once per alphamap texel, which is
      // 4096 calls per chunk in TextureSet::paintTexture alone (texture_set.cpp:902-910). Reading
      // one plain bool first means a build with no mask selected -- which is every build until
      // somebody makes one -- pays a predictable branch and nothing else.
      if (!Detail::g_clipping_active)
      {
        return 1.0f;
      }

      return TerrainMaskStore::instance()->factorAt(world_x, world_z);
    }
  }
}
