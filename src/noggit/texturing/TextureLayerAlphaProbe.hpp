// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_TEXTURELAYERALPHAPROBE_HPP
#define NOGGIT_TEXTURELAYERALPHAPROBE_HPP

#include <noggit/terrain/TerrainMask.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

// Reading one named texture layer's coverage out of a chunk, as a 64x64 byte field.
//
// This is what makes the mask feature's LayerAlpha filter work: "wherever the road texture already
// is" is a mask, and it is the one derived filter whose input the terrain itself cannot supply.
//
// TEMPLATES, so this header includes neither MapChunk.h nor texture_set.hpp. A template body is not
// looked up until instantiation, which is the same device TerrainRuleCollector uses
// (TerrainRules.hpp:322) and it is what keeps TerrainMaskFilters.cpp linkable on a bare machine
// while still letting the editor pass real chunks in.
//
// THE CONTRACT THAT COMES WITH THAT. The instantiating translation unit must have already included
// everything these bodies dereference, and it is more than the chunk:
//
//     #include <noggit/MapChunk.h>       // ChunkT itself
//     #include <noggit/texture_set.hpp>  // getTextureSet() returns TextureSet*
//     #include <noggit/Alphamap.hpp>     // the alpha planes inside it
//
// MapChunk.h only FORWARD-DECLARES TextureSet -- it holds one by unique_ptr (MapChunk.h:93) -- so a
// TU that includes the chunk and stops there compiles everything above this line and then fails
// inside these function bodies with "use of undefined type". The three includes are listed here
// because that error names this file and gives no hint which header is missing.
namespace Noggit
{
  namespace TextureLayerAlphaProbe
  {
    // Index of `texture` among the chunk's layers, or -1.
    //
    // Compared by the exact string TextureSet::filename returns, which is how every other caller in
    // the tree identifies a layer. No normalisation is attempted here: a caller that wants
    // case-insensitive or slash-insensitive matching has to do it before it gets here, because
    // doing it silently would make a filter match a texture the user did not name.
    template <typename ChunkT>
    int layerIndex(ChunkT* chunk, std::string const& texture)
    {
      if (!chunk || texture.empty())
      {
        return -1;
      }

      auto* const texture_set = chunk->getTextureSet();

      if (!texture_set)
      {
        return -1;
      }

      std::size_t const count = texture_set->num();

      for (std::size_t i = 0; i < count; ++i)
      {
        if (texture_set->filename(i) == texture)
        {
          return static_cast<int>(i);
        }
      }

      return -1;
    }

    // Writes MASK_CHUNK_TEXELS bytes of coverage for `texture` into `out`. False, writing nothing,
    // when the chunk does not carry that texture.
    //
    // LAYER 0 IS NOT AN ALPHAMAP, and getting this wrong is the whole difficulty of the function.
    // An MCNK stores n textures and only n-1 alpha planes: layer 0 is the base and is whatever is
    // left after the others have been drawn over it. So its coverage is 255 minus the sum of the
    // rest, clamped at 0 -- not 255, which is what reading "the alphamap of layer 0" would suggest,
    // and not 0, which is what an off-by-one into the alphamap array would give. A mask derived
    // from the base layer of a chunk is a completely ordinary thing to want ("everywhere the ground
    // is still plain dirt"), so this case is the normal one rather than an edge.
    template <typename ChunkT>
    bool readLayerAlpha(ChunkT* chunk, std::string const& texture, std::uint8_t* out)
    {
      if (!out)
      {
        return false;
      }

      int const index = layerIndex(chunk, texture);

      if (index < 0)
      {
        return false;
      }

      auto* const texture_set = chunk->getTextureSet();
      auto* const alphamaps = texture_set->getAlphamaps();

      if (!alphamaps)
      {
        return false;
      }

      std::size_t const count = texture_set->num();

      if (index > 0)
      {
        // Layers 1..n-1 map to alpha planes 0..n-2.
        auto const& plane = (*alphamaps)[static_cast<std::size_t>(index) - 1];

        if (!plane)
        {
          return false;
        }

        for (int i = 0; i < MASK_CHUNK_TEXELS; ++i)
        {
          out[i] = plane->getAlpha(static_cast<std::size_t>(i));
        }

        return true;
      }

      // The base layer. Start full and subtract everything painted over it.
      std::memset(out, 255, MASK_CHUNK_TEXELS);

      for (std::size_t layer = 1; layer < count; ++layer)
      {
        auto const& plane = (*alphamaps)[layer - 1];

        if (!plane)
        {
          continue;
        }

        for (int i = 0; i < MASK_CHUNK_TEXELS; ++i)
        {
          int const remaining = static_cast<int>(out[i])
                              - static_cast<int>(plane->getAlpha(static_cast<std::size_t>(i)));

          // Clamped at 0 rather than allowed to wrap. The alpha planes of a hand-painted chunk do
          // not always sum to 255 -- nothing in the format enforces it and the editor's own
          // blending can leave a texel a few units over -- so an unclamped subtraction would wrap a
          // slightly-oversubscribed texel to near 255 and report the base layer as fully covering
          // the one place it covers least.
          out[i] = static_cast<std::uint8_t>(remaining < 0 ? 0 : remaining);
        }
      }

      return true;
    }
  }
}

#endif // NOGGIT_TEXTURELAYERALPHAPROBE_HPP
