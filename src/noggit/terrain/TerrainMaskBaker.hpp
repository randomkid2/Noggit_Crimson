// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_TERRAINMASKBAKER_HPP
#define NOGGIT_TERRAINMASKBAKER_HPP

#include <noggit/terrain/TerrainMaskFilters.hpp>
#include <noggit/terrain/TerrainMaskStore.hpp>
#include <noggit/texturing/TextureLayerAlphaProbe.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// THE WORLD WALK: turning a filter stack into a composited field over real terrain.
//
// Templates only, so this header pulls in no Noggit headers of its own -- the same arrangement, and
// the same reason, as TerrainRuleCollector at the bottom of TerrainRules.hpp. Instantiate from a
// translation unit that has already included World.h, MapTile.h and MapChunk.h.
namespace Noggit
{
  namespace TerrainMaskBaker
  {
    struct BakeResult
    {
      std::size_t tiles_baked = 0;
      std::size_t chunks_written = 0;

      // Set when the stack could not be run at all. `reason` is one line, suitable for a status
      // label.
      bool refused = false;
      std::string reason;
    };

    // Bakes one tile of `mask`, folding the paint layer on top of the derived result.
    //
    // ORDER MATTERS AND IT IS DERIVED-THEN-PAINT, never the reverse. The paint layer is the record
    // of decisions a human made about a mask they could already see, so it has to be applied to the
    // thing they were looking at. Baking paint first and deriving over it would let a rebake --
    // after, say, reshaping the valley -- overwrite hand corrections with the output of the very
    // filters those corrections were made to fix.
    //
    // Returns the number of chunks written, which is 256 on any tile that baked at all.
    template <typename TileT, typename ChunkT>
    std::size_t bakeTile(NamedTerrainMask& mask, TileT* tile, int tile_x, int tile_z)
    {
      if (!tile || !tile->finishedLoading())
      {
        return 0;
      }

      MaskFilterStack const& stack = mask.stack;

      // 195 KiB of scratch at most, and it dies with this function. See the cost note on
      // MaskTileHeightField.
      MaskTileHeightField field;

      if (stack.needsHeightField()
       && !MaskFieldCollector::fillTileHeightField<TileT, ChunkT>(tile, field))
      {
        return 0;
      }

      bool const wants_alpha = stack.needsLayerAlpha();
      bool const wants_area = stack.needsAreaId();

      std::vector<std::string> const textures = stack.requiredTextures();

      // ONE texture, deliberately. bakeMaskChunk carries a single layer_alpha input, so a stack with
      // two LayerAlpha layers naming different textures would need two fields per texel. Supporting
      // that means widening MaskFilterSample and threading a per-layer lookup through the fold, for
      // a case -- "wherever the road OR the gravel already is" -- that is expressible today by
      // baking two masks and combining them. The first named texture wins and validate() is where
      // the user is told, rather than here where nothing can report anything.
      std::string const texture = textures.empty() ? std::string() : textures.front();

      std::uint8_t derived[MASK_CHUNK_TEXELS];
      std::uint8_t painted[MASK_CHUNK_TEXELS];
      std::uint8_t alpha[MASK_CHUNK_TEXELS];

      std::size_t written = 0;

      for (int chunk_z = 0; chunk_z < MASK_TILE_CHUNK_SIDE; ++chunk_z)
      {
        for (int chunk_x = 0; chunk_x < MASK_TILE_CHUNK_SIDE; ++chunk_x)
        {
          ChunkT* const chunk = tile->getChunk( static_cast<unsigned int>(chunk_x)
                                              , static_cast<unsigned int>(chunk_z)
                                              );

          if (!chunk)
          {
            continue;
          }

          MaskChunkAddress address;
          address.tile_x = tile_x;
          address.tile_z = tile_z;
          address.chunk_x = chunk_x;
          address.chunk_z = chunk_z;

          MaskChunkInputs inputs;

          if (wants_area)
          {
            inputs.area_id = chunk->getAreaID();
          }

          if (wants_alpha && !texture.empty()
           && TextureLayerAlphaProbe::readLayerAlpha<ChunkT>(chunk, texture, alpha))
          {
            inputs.layer_alpha = alpha;
          }

          if (!bakeMaskChunk(stack, field, inputs, address, derived))
          {
            continue;
          }

          // The paint fold. Only chunks the user actually brushed are stored, so this is a hash
          // lookup that misses on almost every chunk of almost every tile.
          if (mask.paint.chunkIsDense(address) || mask.paint.chunkUniformValue(address) != 0)
          {
            mask.paint.readChunk(address, painted);

            for (int i = 0; i < MASK_CHUNK_TEXELS; ++i)
            {
              derived[i] = maskCombine(mask.paint_combine, derived[i], painted[i]);
            }
          }

          mask.composited.writeChunk(address, derived);
          ++written;
        }
      }

      mask.markTileBaked(tile_x, tile_z);

      return written;
    }

    // Bakes every LOADED tile.
    //
    // Loaded tiles only, matching TerrainRuleCollector::collectLoadedTileUnits: pulling unvisited
    // tiles in means loading the whole map, which is a different operation with a different cost and
    // belongs behind its own explicit button.
    //
    // Call as:
    //
    //   Noggit::TerrainMaskBaker::bakeLoadedTiles<World, MapTile, MapChunk>(*world, mask);
    //
    // from a TU that has included World.h, MapTile.h and MapChunk.h. None of the three type
    // parameters can be deduced from `world` alone, so all three are always written out.
    template <typename WorldT, typename TileT, typename ChunkT>
    BakeResult bakeLoadedTiles(WorldT& world, NamedTerrainMask& mask)
    {
      BakeResult result;

      if (mask.stack.enabledCount() == 0)
      {
        result.refused = true;
        result.reason = "the filter stack has no enabled layers";
        return result;
      }

      // The one validate() complaint that must block a bake rather than merely warn. Everything
      // else validate() reports -- an inverted range, an unnamed texture, a first-layer combinator
      // being ignored -- produces a mask that is defensible and simply not what the user meant. A
      // mixed curvature scale produces a bake that takes minutes and looks like a hang, so it is
      // stopped here where it can still be explained.
      for (std::string const& problem : mask.stack.validate())
      {
        if (problem.find("different scales") != std::string::npos)
        {
          result.refused = true;
          result.reason = problem;
          return result;
        }
      }

      for (TileT* tile : world.mapIndex.loaded_tiles())
      {
        if (!tile)
        {
          continue;
        }

        std::size_t const written = bakeTile<TileT, ChunkT>
          (mask, tile, static_cast<int>(tile->index.x), static_cast<int>(tile->index.z));

        if (written > 0)
        {
          ++result.tiles_baked;
          result.chunks_written += written;
        }
      }

      return result;
    }
  }
}

#endif // NOGGIT_TERRAINMASKBAKER_HPP
