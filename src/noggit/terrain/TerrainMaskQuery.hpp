// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_TERRAINMASKQUERY_HPP
#define NOGGIT_TERRAINMASKQUERY_HPP

// THE ONE CALL EVERY MASKED TOOL MAKES.
//
// This header exists to be included by the brush code, and its entire job is to be cheap to
// include: two free functions, no Qt, no templates, no Noggit types, nothing that drags a
// dependency into MapChunk.cpp or texture_set.cpp. Everything the answer depends on -- the named
// masks, the filter stacks, the project sidecar, the Qt dialog -- lives behind TerrainMaskStore and
// is reached only from this file's .cpp.
//
// HOW A TOOL USES IT. Multiply the per-sample strength you were about to apply:
//
//     dt *= Noggit::TerrainMaskQuery::factorAt(vertex.x, vertex.z);
//
// That is the whole contract. A masked-out point gets a factor of 0 and the edit does nothing
// there; a fully masked-in point gets 1 and the edit is exactly what it was before the feature
// existed. There is no second call to make, no state to set up, and no way for a tool to get the
// masking wrong other than by not calling this.
//
// WHY MULTIPLY RATHER THAN SKIP. A boolean "is this point in the mask" would give the mask a hard
// edge at the texel boundary and would make a feathered mask -- which is most of them, since every
// derived filter has a feather and every painted stroke has a falloff -- behave as though it were
// binary. Scaling the strength is what makes a soft mask produce a soft edit.
//
// FAIL OPEN, ALWAYS. Every path that cannot produce an answer returns 1.0, never 0.0:
//
//   - no mask selected, or clipping switched off
//   - a non-finite coordinate, which reaches a brush from an unprojection that missed the terrain
//   - a tile the active mask has not been baked over yet
//
// The reasoning is one-directional. Returning 0 on an unanswerable query makes the brush the user
// is holding down silently stop working, which is indistinguishable from the tool being broken and
// sends people to the bug tracker. Returning 1 makes the mask silently not apply, which the user
// sees immediately -- the paint went where they did not want it -- and can undo. Between a failure
// mode that looks like a broken editor and one that looks like a missed click, the missed click is
// the correct one to choose.
namespace Noggit
{
  namespace TerrainMaskQuery
  {
    namespace Detail
    {
      // Mirrors "a mask is selected AND clipping is on", maintained by TerrainMaskStore. Read
      // directly by active() so a hot loop can skip the call entirely; never written from here.
      extern bool g_clipping_active;
    }

    // Whether factorAt can currently return anything but 1.0.
    //
    // Inline and reading one bool, so a brush that wants to hoist the test out of its inner loop
    // pays nothing for the mask feature when no mask is active. Tools are NOT required to call
    // this -- factorAt is correct on its own -- it is here for the loops that run 4096 times per
    // chunk.
    inline bool active()
    {
      return Detail::g_clipping_active;
    }

    // Multiplier in [0, 1] for an edit at this world position. Editor space, the same coordinates
    // MapChunk::mVertices and TextureSet::paintTexture already work in -- NOT server space.
    //
    // Bilinear across the mask's texel grid, which sits at 0.52 yards. Safe to call at any rate
    // from the render or tool threads; it takes no lock and mutates nothing.
    float factorAt(float world_x, float world_z);
  }
}

#endif // NOGGIT_TERRAINMASKQUERY_HPP
