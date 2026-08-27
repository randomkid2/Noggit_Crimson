// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_UI_TOOLS_CHUNKMANIPULATOR_CHUNKDATA_HPP
#define NOGGIT_UI_TOOLS_CHUNKMANIPULATOR_CHUNKDATA_HPP

#include <noggit/Alphamap.hpp>
#include <noggit/MapHeaders.h>
#include <noggit/TileIndex.hpp>
#include <noggit/texture_set.hpp>

#include <blizzard-archive-library/include/Listfile.hpp>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

// THE ONE DESCRIPTION OF "A CHUNK, DETACHED FROM ITS MAP".
//
// Split out of ChunkClipboard.hpp so that the live clipboard (a QObject bound to a World) and
// the pack reader/writer (pure, bound to nothing) can share it without either including the
// other. Everything here is copyable and holds no pointer into the world -- that is the whole
// requirement, and it is what the previous revision of ChunkCache failed: it stored
// std::unique_ptr<Alphamap> and a std::vector<liquid_layer>, and liquid_layer carries a
// ChunkWater* back-pointer to the chunk it was read from (liquid_layer.hpp:224). A cache holding
// that cannot be copied, cannot be written to a file, and if pasted into a different chunk points
// its water at the source's chunk.
namespace Noggit::Ui::Tools::ChunkManipulator
{
  //! What a copy captures. Each value is one check box in the panel.
  enum class ChunkCopyFlags : std::uint32_t
  {
    NONE                    = 0,
    TERRAIN                 = 0x0001,
    LIQUID                  = 0x0002,
    WMOS                    = 0x0004,
    MODELS                  = 0x0008,
    SHADOWS                 = 0x0010,
    TEXTURES                = 0x0020,
    VERTEX_COLORS           = 0x0040,
    HOLES                   = 0x0080,
    FLAGS                   = 0x0100,
    AREA_ID                 = 0x0200,
    ALPHAMAPS               = 0x0400,
    GROUND_EFFECT_IDS       = 0x0800,
    GROUND_EFFECT_EXCLUSION = 0x1000,

    ALL                     = 0x1FFF
  };

  // Proper bitwise operators, because the alternative is what the previous revision did at every
  // one of its five test sites: static_cast<unsigned>(a) & static_cast<unsigned>(b), spelled out
  // by hand. Thirteen check boxes would have made that unreadable.
  constexpr ChunkCopyFlags operator| (ChunkCopyFlags lhs, ChunkCopyFlags rhs)
  {
    return static_cast<ChunkCopyFlags> (static_cast<std::uint32_t> (lhs) | static_cast<std::uint32_t> (rhs));
  }

  constexpr ChunkCopyFlags operator& (ChunkCopyFlags lhs, ChunkCopyFlags rhs)
  {
    return static_cast<ChunkCopyFlags> (static_cast<std::uint32_t> (lhs) & static_cast<std::uint32_t> (rhs));
  }

  constexpr ChunkCopyFlags operator~ (ChunkCopyFlags value)
  {
    return static_cast<ChunkCopyFlags> (~static_cast<std::uint32_t> (value) & static_cast<std::uint32_t> (ChunkCopyFlags::ALL));
  }

  constexpr ChunkCopyFlags& operator|= (ChunkCopyFlags& lhs, ChunkCopyFlags rhs)
  {
    lhs = lhs | rhs;
    return lhs;
  }

  constexpr ChunkCopyFlags& operator&= (ChunkCopyFlags& lhs, ChunkCopyFlags rhs)
  {
    lhs = lhs & rhs;
    return lhs;
  }

  //! True when every bit of `wanted` is set in `flags`.
  constexpr bool hasFlag (ChunkCopyFlags flags, ChunkCopyFlags wanted)
  {
    return (static_cast<std::uint32_t> (flags) & static_cast<std::uint32_t> (wanted))
        == static_cast<std::uint32_t> (wanted);
  }

  //! What a paste does with what it finds at the destination.
  enum class ChunkPasteFlags : std::uint32_t
  {
    NONE                = 0,
    //! Overwrite the destination outright. Without it a class of data is written only where the
    //! destination has none of its own -- see ChunkClipboard::pasteSelection.
    REPLACE_DESTINATION = 0x1,
    //! Ignore the cursor and put every chunk back at the map coordinates it was copied from.
    AT_SOURCE_LOCATION  = 0x2,
    //! Delete the destination footprint's existing M2s and WMOs before adding the copied ones.
    REPLACE_OBJECTS     = 0x4,
    //! Weld the pasted block's outer height edge to whatever it lands against.
    SEW_SEAMS           = 0x8
  };

  constexpr ChunkPasteFlags operator| (ChunkPasteFlags lhs, ChunkPasteFlags rhs)
  {
    return static_cast<ChunkPasteFlags> (static_cast<std::uint32_t> (lhs) | static_cast<std::uint32_t> (rhs));
  }

  constexpr ChunkPasteFlags& operator|= (ChunkPasteFlags& lhs, ChunkPasteFlags rhs)
  {
    lhs = lhs | rhs;
    return lhs;
  }

  constexpr bool hasFlag (ChunkPasteFlags flags, ChunkPasteFlags wanted)
  {
    return (static_cast<std::uint32_t> (flags) & static_cast<std::uint32_t> (wanted)) != 0;
  }

  //! How a pasted height relates to the terrain that was already there.
  //!
  //! The two enumerators are NOT called ABSOLUTE and RELATIVE, which is what they were first
  //! written as: both are object-like macros in the Windows SDK (wingdi.h defines ABSOLUTE as 1
  //! and RELATIVE as 2), windows.h is reachable from this header through the Qt and archive
  //! includes, and the preprocessor turns the enumerator list into `1 = 0, 2 = 1`. Measured:
  //! cl /Zs answered C2059 "syntax error: 'constant'" on the first of the two lines. The names
  //! below say what the mode does anyway, which is the better name regardless of the collision.
  enum class ChunkHeightMode : std::uint8_t
  {
    //! The pasted terrain keeps the elevation it had at the source, plus the offset. This is the
    //! mode that reproduces the source exactly, and the default.
    SOURCE_ELEVATION = 0,
    //! The pasted terrain keeps its SHAPE but is re-based onto the destination's elevation: a
    //! vertex `d` above the source pivot's ground ends up `d` above the destination pivot's
    //! ground. This is what makes a hillside copied from a valley usable on a plateau.
    DESTINATION_ELEVATION = 1
  };

  enum class ChunkSelectionMode
  {
    SELECT,
    DESELECT
  };

  enum class ChunkManipulatorObjectTypes : std::uint8_t
  {
    M2 = 0,
    WMO = 1
  };

  //! One chunk's address on the map: which ADT, and which of that ADT's 16x16 chunks.
  //!
  //! `x` is MapChunk::px and `z` is MapChunk::py, which is the order MapTile::getChunk takes
  //! (MapTile.cpp:477, `mChunks[z][x]`) and the order World::getChunkAt passes
  //! (World.cpp:1122, x from pos.x, z from pos.z). Both therefore grow with world X and world Z.
  struct SelectedChunkIndex
  {
    //! TileIndex has no default constructor, so the member initialiser is not decoration: without
    //! it neither this struct nor CachedChunk below is default-constructible, and the pack reader
    //! builds both before it has anything to put in them.
    TileIndex tile_index {std::size_t (0), std::size_t (0)};
    unsigned x = 0;
    unsigned z = 0;

    //! Chunk coordinates on the map-wide 1024x1024 grid, so that a difference between two
    //! selections is a plain subtraction even when they straddle an ADT border.
    [[nodiscard]]
    int globalX() const { return static_cast<int> (tile_index.x) * 16 + static_cast<int> (x); }

    [[nodiscard]]
    int globalZ() const { return static_cast<int> (tile_index.z) * 16 + static_cast<int> (z); }

    //! The inverse. A global coordinate off the 0..1023 map grid produces a TileIndex whose
    //! is_valid() is false rather than a wrapped one, which is what every caller tests.
    //!
    //! Floor division is spelled out instead of `>> 4`: a right shift of a negative signed value
    //! is implementation-defined before C++20, and a paste near the map edge genuinely produces
    //! negative global coordinates.
    [[nodiscard]]
    static SelectedChunkIndex fromGlobal (int global_x, int global_z)
    {
      auto const floor_div ([] (int value) { return value >= 0 ? value / 16 : -((-value + 15) / 16); });
      auto const floor_mod ([&] (int value) { return value - floor_div (value) * 16; });

      return { TileIndex (static_cast<std::size_t> (floor_div (global_x))
                         , static_cast<std::size_t> (floor_div (global_z)))
             , static_cast<unsigned> (floor_mod (global_x))
             , static_cast<unsigned> (floor_mod (global_z))
             };
    }

    friend bool operator< (SelectedChunkIndex const& lhs, SelectedChunkIndex const& rhs)
    {
      return std::tie (lhs.tile_index, lhs.x, lhs.z) < std::tie (rhs.tile_index, rhs.x, rhs.z);
    }

    friend bool operator== (SelectedChunkIndex const& lhs, SelectedChunkIndex const& rhs)
    {
      return std::tie (lhs.tile_index, lhs.x, lhs.z) == std::tie (rhs.tile_index, rhs.x, rhs.z);
    }
  };

  //! The layer LIST: how many texture layers the chunk has and what they are called.
  //!
  //! Deliberately separate from the alphamaps below, because the panel offers them as two check
  //! boxes and both halves are independently useful: names without weights re-textures a chunk
  //! while leaving its blend alone, weights without names copies a blend pattern onto whatever
  //! textures the destination already uses.
  struct ChunkTextureCache
  {
    std::size_t n_textures = 0;
    //! Texture FILE NAMES, never MTEX indices. A destination ADT has its own MTEX table and the
    //! same index there names a different texture, so indices are worthless off their own tile.
    //! Action::undo rebuilds its scoped_blp_texture_references from strings for the same reason
    //! (Action.cpp:94-97).
    std::vector<std::string> textures;
  };

  //! The alphamaps as bytes rather than as Alphamap objects.
  //!
  //! Alphamap's only state is `uint8_t amap[64 * 64]` (Alphamap.hpp:39) reachable through
  //! getAlpha()/setAlpha(), so nothing is lost, and the cache becomes copyable and writable to a
  //! file instead of a graph of unique_ptrs.
  struct ChunkAlphamapCache
  {
    std::array<bool, MAX_ALPHAMAPS> present {};
    std::array<std::array<std::uint8_t, 64 * 64>, MAX_ALPHAMAPS> maps {};

    //! The live float editing buffer, present only while a paint stroke has not been folded
    //! down. TextureSet::create_temporary_alphamaps_if_needed (texture_set.cpp:1684) allocates
    //! it lazily and apply_alpha_changes (:1679) resets it, so it is null for every chunk that
    //! has not been painted since the last save -- which is why carrying it costs almost nothing
    //! on a large selection, and why dropping it would silently paste stale alpha for exactly the
    //! handful of chunks where it is not null. It is 64 KiB per chunk when present, against
    //! roughly 14 KiB for everything else a chunk carries, so the pack writes it only when it is.
    std::optional<std::array<std::array<float, 64 * 64>, 4>> tmp_edit_values;
  };

  //! Vertex colours are three pieces of state, not one.
  //!
  //! This mirrors Noggit::VertexColorChangeCache (Action.hpp:91) and the reasoning there applies
  //! unchanged: MapChunk::save gates the whole MCCV block on the RUNTIME hasMCCV
  //! (MapChunk.cpp:1553), so pasting colours without the flag drops them at save, and setting the
  //! flag without colours writes an MCCV block into an ADT that never had one.
  struct ChunkVertexColorCache
  {
    std::array<float, 145 * 3> colors {};
    bool has_mccv_runtime = false;
    bool has_mccv_header = false;
  };

  //! One liquid layer, flattened away from liquid_layer.
  //!
  //! WHAT IS NOT HERE AND WHY. `_liquid_vertex_format` is not carried: liquid_layer's constructor
  //! derives it from the liquid id through changeLiquidID (liquid_layer.cpp:39), so carrying it
  //! could only ever disagree with the destination's own DB lookup. MH2O_Attributes (fishable and
  //! fatigue) is not carried: ChunkWater::update_attributes (ChunkWater.cpp:714) recomputes both
  //! from the layers on every save. Vertex X and Z are not carried: they are the destination
  //! chunk's own grid, and liquid_layer::create_vertices (liquid_layer.cpp:252) lays them out
  //! from the chunk base. Only the numbers that are genuinely per-layer data travel.
  struct ChunkLiquidLayerCache
  {
    int liquid_id = 0;
    //! Which of the 8x8 subchunks this layer covers. Bit index is row * 8 + column
    //! (liquid_layer.cpp:530).
    std::uint64_t subchunks = 0;
    std::array<float, 9 * 9> height {};
    std::array<float, 9 * 9> depth {};
    std::array<glm::vec2, 9 * 9> uv {};
  };

  //! A model or WMO carried along with the terrain under it.
  //!
  //! Identified by FILE KEY, never by uid: a uid is meaningless in another map and in another
  //! process. `pos` is relative to the pivot chunk's (xbase, zbase) corner in X and Z and is
  //! ABSOLUTE in Y, because Y is what the height mode and offset act on.
  struct ChunkObjectCacheEntry
  {
    BlizzardArchive::Listfile::FileKey file_key;
    ChunkManipulatorObjectTypes type = ChunkManipulatorObjectTypes::M2;
    glm::vec3 pos {0.0f, 0.0f, 0.0f};
    glm::vec3 dir {0.0f, 0.0f, 0.0f}; //!< Degrees, as SceneObject::dir stores it.
    float scale = 1.0f;
  };

  //! Everything one chunk can carry. Every member is optional and absent means "this class was
  //! not copied", which is what lets a paste tell "no liquid was copied" from "liquid was copied
  //! and the chunk had none".
  struct ChunkCache
  {
    //! Heights only. The X and Z of a terrain vertex are a function of the chunk's own
    //! xbase/zbase and its position in the 145-vertex grid (MapChunk.cpp:180-185), so carrying
    //! them would carry the source's map coordinates into the destination.
    std::optional<std::array<float, 145>> terrain_height;
    std::optional<ChunkVertexColorCache> vertex_colors;
    std::optional<std::array<std::uint8_t, 64 * 64>> shadows;
    std::optional<std::vector<ChunkLiquidLayerCache>> liquid_layers;
    std::optional<ChunkTextureCache> textures;
    std::optional<ChunkAlphamapCache> alphamaps;
    //! MCLY flags and ground-effect ids, four layers of them.
    //!
    //! Declared as layer_info and NOT as ENTRY_MCLY. The previous revision declared
    //! `ENTRY_MCLY layers_info[4]` and memcpy'd `sizeof(layer_info) * 4` bytes into it from
    //! TextureSet::getMCLYEntries(). layer_info is 8 bytes (flags, effectID -- texture_set.hpp:34)
    //! and ENTRY_MCLY is 16 (textureID, flags, ofsAlpha, effectID -- MapHeaders.h:172), so 32
    //! bytes of the former landed in the first two elements of the latter: the source's `flags`
    //! read back as `textureID`, its `effectID` as `flags`, and layers 2 and 3 were uninitialised.
    //! That destroyed the ground-effect ids specifically, which is the one field this whole
    //! member exists to move. Noggit::TextureChangeCache (Action.hpp:74) already had it right.
    std::optional<std::array<layer_info, 4>> layers_info;
    //! Ground-effect exclusion: one bit per 8x8 unit.
    //!
    //! The companion field, TextureSet::_doodadMapping ("predTex"), is deliberately NOT carried:
    //! MapChunk::save calls TextureSet::updateDoodadMapping (MapChunk.cpp:1516) which re-derives
    //! the whole 8x8 from the alphamap weights, so a copied mapping would be overwritten at the
    //! next save anyway. The paste calls updateDoodadMapping itself after applying alphamaps,
    //! which is the same derivation at the same point in the same order.
    std::optional<std::array<std::uint8_t, 8>> doodad_stencil;
    std::optional<std::vector<ChunkObjectCacheEntry>> objects;
    std::optional<int> holes;
    std::optional<mcnk_flags> flags;
    std::optional<unsigned> area_id;
  };

  //! One chunk plus where it sits relative to the copy pivot.
  struct CachedChunk
  {
    //! Offset from the pivot chunk, in chunks, on the map-wide grid. Rotation and mirroring
    //! rewrite these.
    int rel_x = 0;
    int rel_z = 0;
    //! Where it came from, for "Paste at source ADT location".
    SelectedChunkIndex source;
    ChunkCache data;
  };

  //! What a paste actually did, for the status line.
  struct ChunkPasteReport
  {
    unsigned chunks = 0;
    unsigned objects_added = 0;
    unsigned objects_removed = 0;
    unsigned chunks_skipped = 0; //!< Destination chunks whose ADT is not on the map.
  };
}

#endif // NOGGIT_UI_TOOLS_CHUNKMANIPULATOR_CHUNKDATA_HPP
