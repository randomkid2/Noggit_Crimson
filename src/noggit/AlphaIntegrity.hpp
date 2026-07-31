// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_ALPHAINTEGRITY_HPP
#define NOGGIT_ALPHAINTEGRITY_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Alpha map integrity: states that an ADT can legally store but that no renderer can display
// correctly, and how to repair them.
//
// WHAT THIS IS NOT. Two fixes already exist in this tree and are deliberately not repeated here:
//
//   - Alphamap::readNotCompressed (Alphamap.cpp:109-133) unpacks the 4-bit MCAL format and then,
//     unless the MCNK do_not_fix_alpha_map bit is set, duplicates row 62 into row 63 and column
//     62 into column 63. That format stores 64x64 nibbles but only 63x63 of them are meaningful,
//     so the last row and column are whatever the exporter left there.
//   - TextureSet::convertToBigAlpha (texture_set.cpp:1221) turns the stored RELATIVE weights of
//     the old format into ABSOLUTE ones, and the constructor runs it on load (texture_set.cpp:65)
//     so that everything downstream sees one representation.
//
// This module runs AFTER both, over the resulting weights, and answers a question neither of them
// asks: are the weights themselves coherent?
//
// THE WEIGHTS ARE ABSOLUTE. Every function here assumes big-alpha semantics: a texel's stored
// layer weights are independent shares of 255 and the implicit layer 0 gets whatever is left,
// which is how TextureSet reads them (`255 - sum_alpha(i)`, texture_set.cpp:187, and
// get_textures_weight_for_unit, texture_set.cpp:1475-1490). That is true of the in-memory data of
// every chunk except one case: a tile opened in uid_fix_all mode passes do_not_convert_alphamaps
// (MapChunk.cpp:163), skipping the conversion, so its weights are still relative and the
// over-opacity check would be meaningless on them. Those tiles are loaded to renumber UIDs and
// are never edited or rendered, so the collector at the bottom is not expected to meet one.
//
// THE SPLIT. Everything above AlphaIntegrityCollector is plain STL over plain byte arrays: no Qt,
// no OpenGL, no MapChunk, no TextureSet. That is what lets tests/AlphaIntegrityTests.cpp build it
// on a bare machine and drive every defect class from hand-built input. The collector is a set of
// templates NOT because anything is generic -- there is one World, one MapTile, one MapChunk --
// but because a template body is only looked up when instantiated, which keeps MapChunk.h and
// texture_set.hpp out of this header and out of AlphaIntegrity.cpp. Same construction as
// AssetScan.hpp, for the same reason.
namespace Noggit
{
  // One alpha map is 64x64 bytes. Fixed by the format, not a tunable.
  constexpr std::size_t ALPHA_DIM = 64;
  constexpr std::size_t ALPHA_TEXELS = ALPHA_DIM * ALPHA_DIM;

  // Explicit (stored) layers per chunk. Mirrors MAX_ALPHAMAPS (Alphamap.hpp:13); a chunk has up
  // to 4 texture layers and layer 0 has no alpha map because its weight is derived.
  constexpr std::size_t MAX_ALPHA_LAYERS = 3;

  constexpr int ALPHA_OPAQUE = 255;

  using AlphaLayer = std::array<std::uint8_t, ALPHA_TEXELS>;

  // Row-major, matching every indexing site in the tree: Alphamap.cpp:117 writes `x * 64 + y` and
  // TextureSet::get_textures_weight_for_unit reads `(unit_y * 8 + y) * 64 + (unit_x * 8 + x)`
  // (texture_set.cpp:1479). Which axis is world x and which is world z does not matter to
  // anything here -- the border fix is symmetric in the two -- so they are named row and column.
  constexpr std::size_t alphaTexel(std::size_t row, std::size_t column)
  {
    return row * ALPHA_DIM + column;
  }

  constexpr std::size_t alphaRow(std::size_t texel) { return texel / ALPHA_DIM; }
  constexpr std::size_t alphaColumn(std::size_t texel) { return texel % ALPHA_DIM; }

  // Clamp-and-round used when weights arrive as floats, which they do while a paint stroke is in
  // flight (tmp_edit_alpha_values, texture_set.hpp:22). Matches float_alpha_to_uint8
  // (texture_set.cpp:1638) except that it is NaN-safe: `!(value > 0)` is true for NaN, where
  // std::min/std::max would propagate it into an undefined narrowing conversion.
  std::uint8_t alphaFromFloat(float value);

  // The explicit alpha layers of one chunk.
  //
  // Index 0 HERE is chunk texture layer 1. There is no entry for layer 0: its weight is derived,
  // and making that explicit in the type is the whole reason this is not a bare vector.
  class AlphaLayerStack
  {
  public:
    AlphaLayerStack() = default;

    // All three return false, and add nothing, once MAX_ALPHA_LAYERS layers are held.
    bool addLayer(AlphaLayer const& layer);
    // From Alphamap::getAlpha(); reads ALPHA_TEXELS bytes. False on a null pointer.
    bool addLayer(std::uint8_t const* data);
    // From one plane of tmp_edit_alpha_values; reads ALPHA_TEXELS floats through alphaFromFloat.
    bool addLayerFromFloats(float const* data);
    // A layer that is present but has no stored alpha. TextureSet leaves alphamaps[k] null when
    // the MCLY entry lacks FLAG_USE_ALPHA (texture_set.cpp:57) and sum_alpha skips those
    // (texture_set.cpp:1627), which is arithmetically the same as a zero layer. Adding the zero
    // layer rather than skipping the slot keeps this stack's indices equal to the chunk's.
    bool addZeroLayer();

    std::size_t size() const;
    bool empty() const;

    // Throw std::out_of_range past the end, like std::vector::at. A silent 0 would turn an
    // indexing mistake into a plausible-looking clean report.
    AlphaLayer const& layer(std::size_t layer_index) const;
    AlphaLayer& layer(std::size_t layer_index);

    std::uint8_t at(std::size_t layer_index, std::size_t texel) const;
    void set(std::size_t layer_index, std::size_t texel, std::uint8_t value);

    // Combined stored weight at one texel, 0..765.
    //
    // int, not uint8_t, and that is the point of the whole module: TextureSet::sum_alpha
    // (texture_set.cpp:1621) accumulates into a uint8_t, so a chunk whose layers sum past 255
    // wraps there and `255 - sum_alpha(i)` (texture_set.cpp:187) hands layer 0 a large weight
    // instead of none. The defect is invisible to the code that consumes it.
    int sumAt(std::size_t texel) const;

    // Weight of the implicit layer 0: 255 - sumAt. NEGATIVE exactly where the stack is
    // over-opaque, which is the state repair removes.
    int baseWeightAt(std::size_t texel) const;

    bool layerIsEmpty(std::size_t layer_index) const;

    bool operator==(AlphaLayerStack const& other) const;
    bool operator!=(AlphaLayerStack const& other) const;

  private:
    std::vector<AlphaLayer> _layers;
  };

  enum class AlphaDefect
  {
    // Stored weights sum past 255 at one or more texels, so layer 0 cannot show there however the
    // renderer resolves it.
    OverOpaque,
    // A stored layer is zero everywhere: it costs an MCLY entry, a texture reference and 2 or 4
    // KiB of MCAL, and paints nothing.
    EmptyLayer,
    // Row 63 or column 63 disagrees with row 62 / column 62, i.e. the read-time duplication was
    // skipped. Only a defect for data that came from the 4-bit format; see
    // AlphaIntegrityOptions::check_border.
    BorderMismatch
  };

  std::string_view alphaDefectName(AlphaDefect defect);

  struct AlphaIntegrityOptions
  {
    bool check_overflow = true;
    bool check_empty_layers = true;

    // Whether the last row and column of this chunk are supposed to be duplicates.
    //
    // They are, and only are, for data unpacked by Alphamap::readNotCompressed with the fix
    // enabled. readBigAlpha and readCompressed (Alphamap.cpp:93 and :54) both deliver a full 4096
    // real bytes, where row 63 is genuine data and equality with row 62 is coincidence -- running
    // the check there reports legitimate maps as broken. Use forChunk rather than setting this by
    // hand.
    //
    // Duplication survives the big-alpha conversion: alphas_to_big_alpha (texture_set.cpp:1190)
    // is a per-texel function of that texel's layer values, so two texels that entered equal
    // leave equal. The check is therefore valid on the converted in-memory data.
    bool check_border = true;

    // uses_big_alphamaps is the chunk's FILE format (MapChunk::use_big_alphamap, MapChunk.h:90),
    // not the in-memory representation -- in memory everything is big alpha after the load-time
    // conversion.
    //
    // Note what the second argument means for round-tripping: MapChunk::save sets
    // do_not_fix_alpha_map to 1 whenever the chunk is not written with MCLQ liquids
    // (MapChunk.cpp:1477), so an ADT that Noggit has saved normally comes back with the border
    // check off. That is correct -- the border was written out verbatim and is nobody's to
    // second-guess -- but it does mean this check mostly has something to say about ADTs from
    // other tools.
    static AlphaIntegrityOptions forChunk(bool uses_big_alphamaps, bool do_not_fix_alpha_map);
  };

  // Border disagreement for one layer. The three counters are disjoint: the corner is the single
  // texel (63,63), which the read-time fix takes from (62,62) rather than from either edge.
  struct AlphaBorderMismatch
  {
    std::size_t layer = 0;
    // Rows 0..62 where texel(row,63) != texel(row,62).
    std::size_t last_column_texels = 0;
    // Columns 0..62 where texel(63,column) != texel(62,column).
    std::size_t last_row_texels = 0;
    bool corner = false;
    // Lowest offending texel index, ALPHA_TEXELS when there is none.
    std::size_t first_texel = ALPHA_TEXELS;
    // Largest absolute disagreement, so a caller can tell 1-off rounding noise from a border that
    // was never fixed at all.
    int max_delta = 0;

    std::size_t total() const;
  };

  // What one chunk's layers are guilty of. A value type: build it with checkAlphaIntegrity.
  struct AlphaIntegrityReport
  {
    std::size_t layer_count = 0;

    std::size_t over_opaque_texels = 0;
    // max(sum - 255) over all texels, 0 when none. A stack that is 1 over everywhere is rounding
    // drift; one that is 300 over has had layers pasted into it.
    int max_excess = 0;
    std::size_t first_over_opaque_texel = ALPHA_TEXELS;

    std::vector<std::size_t> empty_layers;
    std::vector<AlphaBorderMismatch> border_mismatches;

    bool clean() const;
    bool has(AlphaDefect defect) const;
    std::size_t borderMismatchTexels() const;

    // One line, for a status bar.
    std::string summary() const;
    // Multi-line, deterministic, diffable.
    std::string toReport() const;
  };

  AlphaIntegrityReport checkAlphaIntegrity( AlphaLayerStack const& layers
                                          , AlphaIntegrityOptions const& options = {}
                                          );

  enum class OverOpacityRepair
  {
    // Take the excess out of the highest-index layers first, leaving the lower ones byte for
    // byte as they were. Default because the common case is drift of one or two points from
    // rounding, and this fixes it by changing one byte of one layer; scaling would rewrite every
    // layer of every over-opaque texel to say the same thing.
    ClipUpperLayers,
    // Scale every layer by 255/sum, preserving the ratios between them -- the visible blend --
    // and apportioning the rounding residual by largest remainder so the result sums to exactly
    // 255. Right when a stack is grossly over, where clipping would delete an upper layer
    // outright.
    //
    // Order between layers survives, with one unavoidable exception: two layers that were exactly
    // equal can end up one apart, because the leftover unit has to go to one of them. It goes to
    // the lower index, so the result is deterministic.
    ScaleProportionally
  };

  struct AlphaRepairOptions
  {
    bool repair_over_opacity = true;
    // Honoured only when AlphaIntegrityOptions::check_border also says the border is a duplicate;
    // repairing a border that is real data would destroy it.
    bool repair_border = true;
    OverOpacityRepair over_opacity_strategy = OverOpacityRepair::ClipUpperLayers;
  };

  struct AlphaRepairResult
  {
    AlphaLayerStack layers;

    std::size_t over_opaque_texels_repaired = 0;
    // Counted per (layer, texel) write that actually changed a byte, so a clean input reports 0
    // and a second pass over a repaired stack reports 0.
    std::size_t border_texels_repaired = 0;

    // Layers that are zero everywhere in the REPAIRED stack, including any that clipping emptied.
    //
    // Not removed here, and that is a scope boundary rather than an omission: dropping a layer
    // means dropping its MCLY entry, its texture reference and renumbering the ones above it,
    // which is TextureSet::eraseTexture's job (texture_set.cpp) and needs the chunk. Repair on
    // plain arrays cannot change how many layers there are without lying about which layer is
    // which.
    std::vector<std::size_t> layers_to_drop;

    bool changed() const;
    std::string summary() const;
  };

  // Repairs in the order over-opacity then border, which is the order that terminates: clipping
  // or scaling leaves every texel summing to at most 255, and the border pass then only copies
  // already-valid texels onto other texels, so it cannot reintroduce over-opacity. The reverse
  // order also happens to work, but only by an argument about the two edges being clipped
  // identically, which is a worse thing to depend on.
  //
  // Postcondition, and what the tests assert: checkAlphaIntegrity(repair(x).layers, options) has
  // no OverOpaque and no BorderMismatch whenever the corresponding repair flag was set. EmptyLayer
  // can survive by design -- see layers_to_drop.
  AlphaRepairResult repairAlphaIntegrity( AlphaLayerStack const& layers
                                        , AlphaIntegrityOptions const& options = {}
                                        , AlphaRepairOptions const& repair = {}
                                        );

  // Where an offending chunk is, in the terms the user navigates by.
  struct AlphaChunkAddress
  {
    int tile_x = 0;
    int tile_z = 0;
    int chunk_x = 0;
    int chunk_z = 0;

    std::string label() const;
    bool operator==(AlphaChunkAddress const& other) const;
  };

  // Roll-up over many chunks. The per-chunk reports are not kept: a fully broken map is 256 tiles
  // x 256 chunks and the reporter needs counts plus a handful of places to look, not 65536
  // structs.
  class AlphaScanResult
  {
  public:
    static constexpr std::size_t DEFAULT_MAX_OFFENDERS = 16;

    void addChunk( AlphaChunkAddress const& address
                 , AlphaIntegrityReport const& report
                 , std::size_t max_offenders = DEFAULT_MAX_OFFENDERS
                 );

    std::size_t chunksScanned() const;
    std::size_t chunksWithDefects() const;
    std::size_t chunksWith(AlphaDefect defect) const;

    std::size_t overOpaqueTexels() const;
    std::size_t borderMismatchTexels() const;
    std::size_t emptyLayerCount() const;
    int maxExcess() const;

    // Bounded by max_offenders, in the order chunks were added.
    std::vector<AlphaChunkAddress> const& offenders() const;
    std::size_t offendersOmitted() const;

    bool clean() const;

    // Folds in another result, for a scan split across threads or run tile by tile.
    void merge(AlphaScanResult const& other, std::size_t max_offenders = DEFAULT_MAX_OFFENDERS);

    std::string summary() const;
    std::string toReport() const;

  private:
    std::size_t _chunks_scanned = 0;
    std::size_t _chunks_with_defects = 0;
    std::size_t _chunks_over_opaque = 0;
    std::size_t _chunks_with_empty_layers = 0;
    std::size_t _chunks_with_border_mismatch = 0;
    std::size_t _over_opaque_texels = 0;
    std::size_t _border_mismatch_texels = 0;
    std::size_t _empty_layers = 0;
    int _max_excess = 0;
    std::vector<AlphaChunkAddress> _offenders;
    std::size_t _offenders_omitted = 0;
  };

  // The world walk. Templates only, so this header pulls in no Noggit headers; see the note at
  // the top of the file. Type parameters are named for the one type each is ever given.
  namespace AlphaIntegrityCollector
  {
    // Absolute per-layer weights of one TextureSet, WITHOUT mutating it.
    //
    // Reads the in-flight float values when a paint stroke has not been committed. The obvious
    // alternative -- call apply_alpha_changes() first -- commits the stroke, marks the chunk
    // dirty and puts an unsaved-change prompt in front of a user who only asked a question. The
    // float planes are offset by one because plane 0 holds the derived layer-0 weight
    // (create_temporary_alphamaps_if_needed, texture_set.cpp:1703-1707).
    //
    // Layer count comes from num() rather than from how many alphamap slots are non-null: slots
    // past nTextures-1 are stale, and a null slot inside the range is a real layer with no stored
    // alpha (texture_set.cpp:57), which addZeroLayer records at the right index.
    template <typename TextureSetT>
    AlphaLayerStack readTextureSet(TextureSetT* texture_set)
    {
      AlphaLayerStack stack;

      if (!texture_set)
      {
        return stack;
      }

      std::size_t const texture_count = texture_set->num();

      if (texture_count < 2)
      {
        // One texture covers the chunk and stores no alpha; zero textures store nothing at all.
        return stack;
      }

      std::size_t const explicit_layers = texture_count - 1;
      auto& temporary = texture_set->getTempAlphamaps();

      if (temporary)
      {
        for (std::size_t layer = 0; layer < explicit_layers; ++layer)
        {
          stack.addLayerFromFloats((*temporary)[layer + 1].data());
        }

        return stack;
      }

      auto* alphamaps = texture_set->getAlphamaps();

      for (std::size_t layer = 0; layer < explicit_layers; ++layer)
      {
        auto const& alphamap = (*alphamaps)[layer];

        if (alphamap)
        {
          stack.addLayer(alphamap->getAlpha());
        }
        else
        {
          stack.addZeroLayer();
        }
      }

      return stack;
    }

    // Reads the two flags that decide whether the border is a duplicate straight off the chunk.
    // Both are public members of MapChunk (MapChunk.h:89-90).
    template <typename ChunkT>
    AlphaIntegrityOptions optionsForChunk(ChunkT* chunk)
    {
      return AlphaIntegrityOptions::forChunk( chunk->use_big_alphamap
                                            , chunk->header_flags.flags.do_not_fix_alpha_map != 0
                                            );
    }

    template <typename ChunkT>
    AlphaIntegrityReport checkChunk(ChunkT* chunk)
    {
      if (!chunk)
      {
        return {};
      }

      return checkAlphaIntegrity(readTextureSet(chunk->getTextureSet()), optionsForChunk(chunk));
    }

    // Writes repaired weights back. Returns false and writes nothing if the layer counts
    // disagree, which means the chunk changed under the caller between check and apply.
    //
    // When a stroke is in flight the floats are written instead of the byte maps: writing the
    // byte maps would be undone by the next apply_alpha_changes, which overwrites them from the
    // floats (texture_set.cpp:1673).
    template <typename TextureSetT>
    bool applyToTextureSet(TextureSetT* texture_set, AlphaLayerStack const& layers)
    {
      if (!texture_set)
      {
        return false;
      }

      std::size_t const texture_count = texture_set->num();

      if (texture_count < 2 || texture_count - 1 != layers.size())
      {
        return false;
      }

      auto& temporary = texture_set->getTempAlphamaps();

      if (temporary)
      {
        for (std::size_t texel = 0; texel < ALPHA_TEXELS; ++texel)
        {
          for (std::size_t layer = 0; layer < layers.size(); ++layer)
          {
            (*temporary)[layer + 1][texel] = static_cast<float>(layers.at(layer, texel));
          }

          // Plane 0 is derived, and repair has just made the sum fit, so this is never negative.
          (*temporary)[0][texel] = static_cast<float>(layers.baseWeightAt(texel));
        }
      }
      else
      {
        auto* alphamaps = texture_set->getAlphamaps();

        for (std::size_t layer = 0; layer < layers.size(); ++layer)
        {
          auto& alphamap = (*alphamaps)[layer];

          if (!alphamap)
          {
            // A layer with no stored alpha map stays that way when the repair left it empty;
            // materialising one would add 4 KiB of zeroes to the file for no change in what is
            // drawn. If repair produced values for it, the caller has to create the map, which
            // needs Alphamap and therefore the caller's TU.
            if (!layers.layerIsEmpty(layer))
            {
              return false;
            }

            continue;
          }

          // setAlpha takes a mutable pointer (Alphamap.hpp:22) and copies, hence the buffer.
          AlphaLayer buffer = layers.layer(layer);
          alphamap->setAlpha(buffer.data());
        }
      }

      texture_set->markDirty();
      return true;
    }

    // Checks one chunk and writes the repair back. Returns true when something changed.
    template <typename ChunkT>
    bool repairChunk(ChunkT* chunk, AlphaRepairOptions const& repair = {})
    {
      if (!chunk)
      {
        return false;
      }

      auto* texture_set = chunk->getTextureSet();
      AlphaLayerStack const before = readTextureSet(texture_set);

      if (before.empty())
      {
        return false;
      }

      AlphaRepairResult const result = repairAlphaIntegrity(before, optionsForChunk(chunk), repair);

      if (!result.changed())
      {
        return false;
      }

      return applyToTextureSet(texture_set, result.layers);
    }

    // Every chunk of one loaded tile.
    //
    // 16x16 by hand because MapTile has no chunk iterator (MapTile.h:84 carries a todo for it),
    // and NOT via World::for_all_chunks_on_tile, which calls mapIndex.setChanged(tile) on entry
    // (World.inl:16) and would mark every visited tile dirty during a read-only scan.
    template <typename TileT>
    void collectTile( TileT* tile
                    , AlphaScanResult& result
                    , std::size_t max_offenders = AlphaScanResult::DEFAULT_MAX_OFFENDERS
                    )
    {
      if (!tile || !tile->finishedLoading())
      {
        return;
      }

      for (unsigned int chunk_z = 0; chunk_z < 16; ++chunk_z)
      {
        for (unsigned int chunk_x = 0; chunk_x < 16; ++chunk_x)
        {
          auto* chunk = tile->getChunk(chunk_x, chunk_z);

          if (!chunk)
          {
            continue;
          }

          AlphaChunkAddress address;
          address.tile_x = static_cast<int>(tile->index.x);
          address.tile_z = static_cast<int>(tile->index.z);
          address.chunk_x = static_cast<int>(chunk_x);
          address.chunk_z = static_cast<int>(chunk_z);

          result.addChunk(address, checkChunk(chunk), max_offenders);
        }
      }
    }

    // Repairs every chunk of one loaded tile; returns how many chunks changed.
    template <typename TileT>
    std::size_t repairTile(TileT* tile, AlphaRepairOptions const& repair = {})
    {
      if (!tile || !tile->finishedLoading())
      {
        return 0;
      }

      std::size_t repaired = 0;

      for (unsigned int chunk_z = 0; chunk_z < 16; ++chunk_z)
      {
        for (unsigned int chunk_x = 0; chunk_x < 16; ++chunk_x)
        {
          if (repairChunk(tile->getChunk(chunk_x, chunk_z), repair))
          {
            ++repaired;
          }
        }
      }

      return repaired;
    }

    // Every LOADED tile of `world`. Call as:
    //
    //   Noggit::AlphaScanResult result;
    //   Noggit::AlphaIntegrityCollector::collectWorld<World, MapTile>(*world, result);
    //
    // from a TU that has included World.h, MapTile.h, MapChunk.h and texture_set.hpp. Neither
    // type parameter is deducible from `world`, so both are always written out.
    //
    // Tiles the user has never visited are not scanned. Loading the whole map to scan it is a
    // different operation with a different cost and belongs behind its own action.
    // MapIndex::loaded_tiles already filters to non-null and finishedLoading (map_index.cpp:29).
    template <typename WorldT, typename TileT>
    void collectWorld( WorldT& world
                     , AlphaScanResult& result
                     , std::size_t max_offenders = AlphaScanResult::DEFAULT_MAX_OFFENDERS
                     )
    {
      for (TileT* tile : world.mapIndex.loaded_tiles())
      {
        collectTile(tile, result, max_offenders);
      }
    }
  }
}

#endif // NOGGIT_ALPHAINTEGRITY_HPP
