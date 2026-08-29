// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_TERRAINMASKPAINTER_HPP
#define NOGGIT_TERRAINMASKPAINTER_HPP

#include <noggit/terrain/TerrainMask.hpp>

#include <cstddef>

// Hand-adjusting a mask after it has been derived.
//
// A derived mask gets the mapper ninety per cent of the way -- "every slope above 40 degrees" is
// almost the cliffs, but it also catches the quarry wall that should stay flagstone. The last ten
// per cent is a human with a brush, and this is that brush's back half.
//
// WHAT MAKES THIS ITS OWN MODULE RATHER THAN A METHOD ON THE STORE. A stroke has to touch TWO
// fields to be both correct and visible:
//
//   - the PAINT layer, which is what persists and what a rebake folds back on top, and
//   - the COMPOSITED field, which is what every brush query actually reads.
//
// Writing only the paint layer would leave the stroke invisible until the next bake. Writing only
// the composite would make it vanish at the next bake. Doing both in one call is the entire
// content of this file, and putting it here means the MapView-side tool that owns the mouse gesture
// is a single call with no opportunity to get that pairing wrong.
//
// NO WORLD, NO GL, NO QT. The stroke is pure geometry against a byte field, so the tool that drives
// it needs to supply nothing but a position and a radius.
namespace Noggit
{
  namespace TerrainMaskPainter
  {
    // What a stroke does to the mask under it.
    enum class StrokeMode
    {
      // Raise the mask toward full. The default gesture.
      Add,
      // Lower it toward empty.
      Erase
    };

    // Applies one stroke to the ACTIVE mask, or does nothing when none is selected.
    //
    // `radius` is in yards, matching every other brush in the editor. `hardness` in [0, 1] is the
    // fraction of the radius that is fully weighted, and `strength` in [0, 1] scales the whole
    // stroke -- the same three numbers, meaning the same three things, as the texture brush, so a
    // mask painted at radius 30 hardness 0.5 has the same profile as a texture stroke with those
    // settings.
    //
    // Returns the number of chunks touched, which is what a caller registers for undo. Zero when
    // nothing was selected or the stroke fell off the map.
    //
    // NOT UNDOABLE BY ITSELF. Masks are editor-side state and are not part of the ActionManager's
    // chunk-snapshot model, which records ADT data. A mask stroke therefore does NOT land in the
    // undo stack, and that is a deliberate limitation rather than an oversight -- see the note in
    // the dialog. The safety net is that a mask changes nothing in any file: the worst a bad stroke
    // can do is clip the next edit somewhere unintended, which is visible immediately and repaired
    // by painting over it or rebaking.
    std::size_t stroke( float world_x
                      , float world_z
                      , float radius
                      , float hardness
                      , float strength
                      , StrokeMode mode
                      );

    // The same stroke against a specific mask rather than the active one. Exists for the dialog,
    // which can edit a mask that is not the current clip.
    std::size_t strokeInto( TerrainMask& paint
                          , TerrainMask& composited
                          , float world_x
                          , float world_z
                          , float radius
                          , float hardness
                          , float strength
                          , StrokeMode mode
                          );
  }
}

#endif // NOGGIT_TERRAINMASKPAINTER_HPP
