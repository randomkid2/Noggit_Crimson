// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/terrain/TerrainMaskPainter.hpp>
#include <noggit/terrain/TerrainMaskStore.hpp>

#include <cstddef>

namespace Noggit
{
  namespace TerrainMaskPainter
  {
    std::size_t strokeInto( TerrainMask& paint
                          , TerrainMask& composited
                          , float world_x
                          , float world_z
                          , float radius
                          , float hardness
                          , float strength
                          , StrokeMode mode
                          )
    {
      // Add raises toward full with Max, Erase lowers toward empty with Min against the stroke's
      // complement. Using Max and Min rather than Add and Subtract is what makes a stroke held in
      // one place converge on the target value instead of marching past it: dragging back and forth
      // over the same spot with Add would saturate the mask to 255 regardless of the strength the
      // user set, which is the behaviour of a paint tool nobody wants.
      bool const erasing = (mode == StrokeMode::Erase);

      MaskCombine const op = erasing ? MaskCombine::Min : MaskCombine::Max;

      // Erase writes the COMPLEMENT of the falloff. Without it the rim of the brush -- where the
      // falloff tends to zero -- would be driven to zero by Min, so an erase stroke would bite
      // hardest exactly where it should touch least. See TerrainMask::paintCircle's `complement`.
      bool const complement = erasing;

      // Both fields, always. The paint layer is what survives a rebake and a restart; the composite
      // is what the brushes actually read. Writing one without the other is the single mistake this
      // function exists to make impossible -- see the note at the top of the header.
      std::size_t const touched
        = paint.paintCircle(world_x, world_z, radius, hardness, strength, op, complement);

      composited.paintCircle(world_x, world_z, radius, hardness, strength, op, complement);

      return touched;
    }

    std::size_t stroke( float world_x
                      , float world_z
                      , float radius
                      , float hardness
                      , float strength
                      , StrokeMode mode
                      )
    {
      NamedTerrainMask* const mask = TerrainMaskStore::instance()->active();

      if (!mask)
      {
        return 0;
      }

      return strokeInto( mask->paint
                       , mask->composited
                       , world_x
                       , world_z
                       , radius
                       , hardness
                       , strength
                       , mode
                       );
    }
  }
}
