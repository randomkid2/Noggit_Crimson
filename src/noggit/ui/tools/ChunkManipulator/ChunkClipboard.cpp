// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include "ChunkClipboard.hpp"
#include "ChunkPack.hpp"

#include <noggit/ActionManager.hpp>
#include <noggit/Alphamap.hpp>
#include <noggit/AsyncObject.h>
#include <noggit/ChunkWater.hpp>
#include <noggit/MapChunk.h>
#include <noggit/MapTile.h>
#include <noggit/MapView.h>
#include <noggit/SceneObject.hpp>
#include <noggit/World.h>
#include <noggit/liquid_layer.hpp>
#include <noggit/scoped_blp_texture_reference.hpp>
#include <noggit/texture_set.hpp>
#include <noggit/World.inl>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <utility>

using namespace Noggit::Ui::Tools::ChunkManipulator;

namespace
{
  //! The 9x9 outer vertex at (row = Z index, column = X index) inside the 145-vertex array.
  constexpr int outerVertex (int row, int col)
  {
    return row * 17 + col;
  }

  //! True when `position` falls inside the chunk that starts at (xbase, zbase).
  //!
  //! Half-open on the far edge so that an object standing exactly on a chunk border belongs to
  //! precisely one chunk and can never be captured twice.
  bool insideChunk (glm::vec3 const& position, float xbase, float zbase)
  {
    return position.x >= xbase && position.x < xbase + CHUNKSIZE
        && position.z >= zbase && position.z < zbase + CHUNKSIZE;
  }

  //! Closes the paste action however the scope is left, including by exception.
  //!
  //! beginAction sets the NOGGIT_CUR_ACTION global, and every tool in the editor tests that
  //! global before starting work of its own -- pasteSelection returns early on it at the top of
  //! this file. So an exception escaping between beginAction and endAction does not merely lose
  //! one paste: the flag stays set for the REST OF THE SESSION, and paste then silently refuses
  //! every subsequent attempt with no message, until the editor is restarted.
  //!
  //! The realistic throw is std::bad_alloc. A paste allocates a fresh Alphamap and a 64 KiB
  //! float edit buffer PER CHUNK, so a several-thousand-chunk paste asks for hundreds of
  //! megabytes in small pieces -- exactly the shape of allocation that fails first. Roughly five
  //! hundred lines separate the two calls, which is far too much to keep exception-free by
  //! inspection.
  struct ScopedAction
  {
    ~ScopedAction()
    {
      NOGGIT_ACTION_MGR->endAction();
    }
  };
}

ChunkClipboard::ChunkClipboard (MapView* map_view, QObject* parent)
  : QObject (parent)
  , _map_view (map_view)
  , _world (map_view->getWorld())
{
  // PRIME THE SHARED CLIPBOARD'S TIMESTAMP WITHOUT READING IT. The cross-window pack outlives
  // the process that wrote it, so a pack left in the temp directory by a previous session is
  // still sitting there at startup. Recording its stamp now means adoptSharedClipboard treats it
  // as already seen: a new window starts with an EMPTY clipboard, which is what a user expects,
  // and picks up only copies made after it opened.
  std::error_code error;
  _shared_clipboard_stamp = std::filesystem::last_write_time (sharedClipboardPackPath(), error);
  _shared_clipboard_stamp_valid = !error;
}

// =================================================================================================
// Selection
// =================================================================================================

void ChunkClipboard::selectRange (glm::vec3 const& cursor_pos, float radius, ChunkSelectionMode mode)
{
  bool const select (mode == ChunkSelectionMode::SELECT);

  _world->for_all_chunks_in_range
    ( cursor_pos, radius
    , [this, select] (MapChunk* chunk) -> bool
      {
        SelectedChunkIndex const index
          { TileIndex (glm::vec3 {chunk->xbase, 0.0f, chunk->zbase})
          , static_cast<unsigned> (chunk->px)
          , static_cast<unsigned> (chunk->py)
          };

        if (select)
        {
          _selected_chunks.emplace (index);
        }
        else
        {
          _selected_chunks.erase (index);
        }

        // FALSE, and this is not a detail. World::for_all_chunks_in_range treats a true return
        // as "this chunk was edited" and answers it with mapIndex.setChanged(tile)
        // (World.inl:121-125). The previous revision of this function returned true, so merely
        // dragging a selection across the terrain marked every ADT it touched as having unsaved
        // changes. Selecting is not editing.
        return false;
      }
    );

  emit selectionChanged (_selected_chunks);
}

void ChunkClipboard::selectChunk (glm::vec3 const& pos, ChunkSelectionMode mode)
{
  // Deliberately NOT World::for_chunk_at, which calls mapIndex.setChanged unconditionally
  // (World.inl:53) whether or not the callback changes anything. getChunkAt has no such
  // side effect.
  MapChunk* const chunk (_world->getChunkAt (pos));

  if (!chunk)
  {
    return;
  }

  selectChunk ( SelectedChunkIndex { TileIndex (glm::vec3 {chunk->xbase, 0.0f, chunk->zbase})
                                   , static_cast<unsigned> (chunk->px)
                                   , static_cast<unsigned> (chunk->py)
                                   }
              , mode
              );
}

void ChunkClipboard::selectChunk (SelectedChunkIndex const& index, ChunkSelectionMode mode)
{
  if (!index.tile_index.is_valid() || !_world->mapIndex.hasTile (index.tile_index))
  {
    return;
  }

  if (mode == ChunkSelectionMode::SELECT)
  {
    _selected_chunks.emplace (index);
  }
  else
  {
    _selected_chunks.erase (index);
  }

  emit selectionChanged (_selected_chunks);
}

void ChunkClipboard::clearSelection()
{
  _selected_chunks.clear();

  // Both signals, and the second one is the fix. selectionCleared() was declared by the previous
  // revision and emitted from nowhere in the tree, so anything listening for it -- a status
  // label, a highlight -- would have gone on showing a selection that no longer existed.
  emit selectionChanged (_selected_chunks);
  emit selectionCleared();
}

std::set<SelectedChunkIndex> const& ChunkClipboard::selectedChunks() const
{
  return _selected_chunks;
}

// =================================================================================================
// Copy
// =================================================================================================

unsigned ChunkClipboard::copySelected (glm::vec3 const& pivot_pos)
{
  _cached_chunks.clear();
  _clipboard_from_another_window = false;

  MapChunk* const pivot_chunk (_world->getChunkAt (pivot_pos));

  if (!pivot_chunk || _selected_chunks.empty())
  {
    emit clipboardChanged();
    return 0;
  }

  _clipboard_pivot = pivot_pos;
  _clipboard_pivot_height = pivot_pos.y;
  _clipboard_pivot_chunk = { TileIndex (glm::vec3 {pivot_chunk->xbase, 0.0f, pivot_chunk->zbase})
                           , static_cast<unsigned> (pivot_chunk->px)
                           , static_cast<unsigned> (pivot_chunk->py)
                           };
  _clipboard_map_name = _world->basename;
  _clipboard_map_id = _world->getMapID();
  _clipboard_flags = _copy_flags;

  int const pivot_global_x (_clipboard_pivot_chunk.globalX());
  int const pivot_global_z (_clipboard_pivot_chunk.globalZ());

  ChunkCopyFlags const flags (_copy_flags);
  bool const want_objects (hasFlag (flags, ChunkCopyFlags::MODELS)
                        || hasFlag (flags, ChunkCopyFlags::WMOS));

  // WHICH CHUNK OWNS A MODEL. An object is carried by the chunk its ORIGIN stands in, not by
  // every chunk its bounding box overlaps. MapChunk::save's MCRF pass uses the overlap test
  // (MapChunk.cpp:1670-1687) because a reference list is allowed -- and required -- to name the
  // same doodad from several chunks; a clipboard is not, because every extra claim on the same
  // object is an extra copy of that tree at paste time. The origin test also settles the
  // tile-boundary case for free: an instance appears in the object list of every tile it touches
  // (MapTile::getObjectInstances), and only one of those tiles contains its origin.
  //
  // The cost of the rule is a WMO whose origin sits outside the selection but whose footprint
  // covers it: that building is not carried. That is the right answer for "copy the models
  // standing on this ground" and the wrong one for "copy everything I can see", and it is the
  // first of the two that this tool is for.
  std::unordered_map<MapTile*, std::vector<SceneObject*>> objects_by_tile;

  if (want_objects)
  {
    std::unordered_set<MapTile*> tiles;

    for (SelectedChunkIndex const& index : _selected_chunks)
    {
      if (MapTile* const tile = _world->mapIndex.loadTile (index.tile_index))
      {
        tile->wait_until_loaded();
        tiles.emplace (tile);
      }
    }

    for (MapTile* const tile : tiles)
    {
      std::vector<SceneObject*>& bucket (objects_by_tile[tile]);

      for (auto const& entry : tile->getObjectInstances())
      {
        for (SceneObject* const object : entry.second)
        {
          bool const is_wmo (object->which() == eWMO);

          if (is_wmo ? hasFlag (flags, ChunkCopyFlags::WMOS) : hasFlag (flags, ChunkCopyFlags::MODELS))
          {
            bucket.emplace_back (object);
          }
        }
      }
    }
  }

  for (SelectedChunkIndex const& index : _selected_chunks)
  {
    MapTile* const tile (_world->mapIndex.loadTile (index.tile_index));

    if (!tile)
    {
      continue;
    }

    tile->wait_until_loaded();
    MapChunk* const chunk (tile->getChunk (index.x, index.z));

    if (!chunk)
    {
      continue;
    }

    CachedChunk cached;
    cached.source = index;
    cached.rel_x = index.globalX() - pivot_global_x;
    cached.rel_z = index.globalZ() - pivot_global_z;

    ChunkCache& data (cached.data);

    if (hasFlag (flags, ChunkCopyFlags::TERRAIN))
    {
      std::array<float, 145> heights {};

      for (int i (0); i < mapbufsize; ++i)
      {
        heights[static_cast<std::size_t> (i)] = chunk->mVertices[i].y;
      }

      data.terrain_height = heights;
    }

    if (hasFlag (flags, ChunkCopyFlags::VERTEX_COLORS))
    {
      ChunkVertexColorCache colors;
      std::memcpy (colors.colors.data(), &chunk->mccv, 145 * 3 * sizeof (float));
      colors.has_mccv_runtime = chunk->hasColors();
      colors.has_mccv_header = chunk->header_flags.flags.has_mccv != 0;
      data.vertex_colors = colors;
    }

    if (hasFlag (flags, ChunkCopyFlags::SHADOWS))
    {
      std::array<std::uint8_t, 64 * 64> shadows {};
      std::memcpy (shadows.data(), &chunk->_shadow_map, 64 * 64 * sizeof (std::uint8_t));
      data.shadows = shadows;
    }

    if (hasFlag (flags, ChunkCopyFlags::HOLES))
    {
      data.holes = chunk->holes;
    }

    if (hasFlag (flags, ChunkCopyFlags::FLAGS))
    {
      data.flags = chunk->header_flags;
    }

    if (hasFlag (flags, ChunkCopyFlags::AREA_ID))
    {
      data.area_id = chunk->areaID;
    }

    if (hasFlag (flags, ChunkCopyFlags::LIQUID))
    {
      std::vector<ChunkLiquidLayerCache> layers;

      for (liquid_layer& layer : *chunk->liquid_chunk()->getLayers())
      {
        ChunkLiquidLayerCache cache;
        cache.liquid_id = layer.liquidID();
        cache.subchunks = layer.getSubchunks();

        auto& vertices (layer.getVertices());

        for (std::size_t i (0); i < 9 * 9; ++i)
        {
          cache.height[i] = vertices[i].position.y;
          cache.depth[i] = vertices[i].depth;
          cache.uv[i] = vertices[i].uv;
        }

        layers.emplace_back (cache);
      }

      data.liquid_layers = std::move (layers);
    }

    TextureSet* const texture_set (chunk->getTextureSet());

    if (texture_set)
    {
      if (hasFlag (flags, ChunkCopyFlags::TEXTURES))
      {
        ChunkTextureCache textures;
        textures.n_textures = texture_set->num();
        textures.textures.reserve (textures.n_textures);

        for (std::size_t i (0); i < textures.n_textures; ++i)
        {
          textures.textures.emplace_back (texture_set->filename (i));
        }

        data.textures = std::move (textures);
      }

      if (hasFlag (flags, ChunkCopyFlags::ALPHAMAPS))
      {
        ChunkAlphamapCache alphamaps;
        auto const& source (*texture_set->getAlphamaps());

        for (std::size_t i (0); i < MAX_ALPHAMAPS; ++i)
        {
          alphamaps.present[i] = source[i] != nullptr;

          if (source[i])
          {
            std::memcpy (alphamaps.maps[i].data(), source[i]->getAlpha(), 64 * 64);
          }
        }

        if (auto const& temporary = texture_set->getTempAlphamaps())
        {
          std::array<std::array<float, 64 * 64>, 4> values {};

          for (std::size_t layer (0); layer < 4; ++layer)
          {
            values[layer] = temporary->map[layer];
          }

          alphamaps.tmp_edit_values = values;
        }

        data.alphamaps = std::move (alphamaps);
      }

      if (hasFlag (flags, ChunkCopyFlags::GROUND_EFFECT_IDS))
      {
        std::array<layer_info, 4> infos {};
        std::memcpy (infos.data(), texture_set->getMCLYEntries(), sizeof (layer_info) * 4);
        data.layers_info = infos;
      }

      if (hasFlag (flags, ChunkCopyFlags::GROUND_EFFECT_EXCLUSION))
      {
        std::array<std::uint8_t, 8> stencil {};
        std::memcpy (stencil.data(), texture_set->getDoodadStencilBase(), 8);
        data.doodad_stencil = stencil;
      }
    }

    if (want_objects)
    {
      std::vector<ChunkObjectCacheEntry> objects;
      auto const bucket (objects_by_tile.find (tile));

      if (bucket != objects_by_tile.end())
      {
        for (SceneObject* const object : bucket->second)
        {
          if (!insideChunk (object->pos, chunk->xbase, chunk->zbase))
          {
            continue;
          }

          ChunkObjectCacheEntry entry;
          entry.file_key = object->instance_model()->file_key();
          entry.type = object->which() == eWMO ? ChunkManipulatorObjectTypes::WMO
                                               : ChunkManipulatorObjectTypes::M2;
          // X and Z relative to the PIVOT chunk's corner, Y absolute: Y is what the height mode
          // and the height offset act on, and everything else about the placement is a rigid
          // move in the horizontal plane.
          entry.pos = { object->pos.x - pivot_chunk->xbase
                      , object->pos.y
                      , object->pos.z - pivot_chunk->zbase
                      };
          entry.dir = object->dir;
          entry.scale = object->scale;

          objects.emplace_back (std::move (entry));
        }
      }

      data.objects = std::move (objects);
    }

    _cached_chunks.emplace_back (std::move (cached));
  }

  publishSharedClipboard();

  emit clipboardChanged();
  return static_cast<unsigned> (_cached_chunks.size());
}

void ChunkClipboard::clearClipboard()
{
  _cached_chunks.clear();
  _clipboard_map_name.clear();
  _clipboard_flags = ChunkCopyFlags::NONE;
  _clipboard_from_another_window = false;
  emit clipboardChanged();
}

bool ChunkClipboard::hasClipboard() const
{
  return !_cached_chunks.empty();
}

std::size_t ChunkClipboard::clipboardChunkCount() const
{
  return _cached_chunks.size();
}

std::string const& ChunkClipboard::clipboardMapName() const
{
  return _clipboard_map_name;
}

bool ChunkClipboard::clipboardFromAnotherWindow() const
{
  return _clipboard_from_another_window;
}

// =================================================================================================
// Transforms
// =================================================================================================

void ChunkClipboard::applyGridOp (ChunkGridOp op)
{
  if (op == ChunkGridOp::IDENTITY || _cached_chunks.empty())
  {
    return;
  }

  // THE BLOCK IS TRANSFORMED ABOUT ITS OWN BOUNDING BOX, NOT ABOUT THE PIVOT CHUNK. Rotating
  // about the pivot cell's corner would send the pivot chunk itself to a neighbouring cell,
  // which reads as the block jumping. Anchoring the box's minimum corner keeps the block where
  // the user can see it; for a square selection -- which is what a radius select produces -- the
  // two are the same thing anyway.
  int min_x (_cached_chunks.front().rel_x);
  int min_z (_cached_chunks.front().rel_z);
  int max_x (min_x);
  int max_z (min_z);

  for (CachedChunk const& chunk : _cached_chunks)
  {
    min_x = std::min (min_x, chunk.rel_x);
    min_z = std::min (min_z, chunk.rel_z);
    max_x = std::max (max_x, chunk.rel_x);
    max_z = std::max (max_z, chunk.rel_z);
  }

  int const width (max_x - min_x + 1);
  int const height (max_z - min_z + 1);
  float const width_units (static_cast<float> (width) * CHUNKSIZE);
  float const height_units (static_cast<float> (height) * CHUNKSIZE);

  for (CachedChunk& chunk : _cached_chunks)
  {
    // ---- where the chunk goes -------------------------------------------------------------
    int const block_x (chunk.rel_x - min_x);
    int const block_z (chunk.rel_z - min_z);
    int new_block_x (block_x);
    int new_block_z (block_z);

    switch (op)
    {
      case ChunkGridOp::ROTATE_90:
        new_block_x = block_z;
        new_block_z = width - 1 - block_x;
        break;
      case ChunkGridOp::MIRROR_X:
        new_block_x = width - 1 - block_x;
        break;
      case ChunkGridOp::MIRROR_Z:
        new_block_z = height - 1 - block_z;
        break;
      case ChunkGridOp::IDENTITY:
        break;
    }

    chunk.rel_x = new_block_x + min_x;
    chunk.rel_z = new_block_z + min_z;

    // ---- and what happens inside it --------------------------------------------------------
    ChunkCache& data (chunk.data);

    if (data.terrain_height)
    {
      permuteTerrainGrid<float> (op, data.terrain_height->data(), 1);
    }

    if (data.vertex_colors)
    {
      permuteTerrainGrid<float> (op, data.vertex_colors->colors.data(), 3);
    }

    if (data.shadows)
    {
      permuteSquare<std::uint8_t> (op, 64, data.shadows->data());
    }

    if (data.alphamaps)
    {
      for (std::size_t i (0); i < MAX_ALPHAMAPS; ++i)
      {
        if (data.alphamaps->present[i])
        {
          permuteSquare<std::uint8_t> (op, 64, data.alphamaps->maps[i].data());
        }
      }

      if (data.alphamaps->tmp_edit_values)
      {
        for (std::array<float, 64 * 64>& layer : *data.alphamaps->tmp_edit_values)
        {
          permuteSquare<float> (op, 64, layer.data());
        }
      }
    }

    if (data.holes)
    {
      data.holes = permuteHoles (op, *data.holes);
    }

    if (data.doodad_stencil)
    {
      data.doodad_stencil = permuteDoodadStencil (op, *data.doodad_stencil);
    }

    if (data.liquid_layers)
    {
      for (ChunkLiquidLayerCache& layer : *data.liquid_layers)
      {
        // The 9x9 liquid vertex grid is a point grid on the same footprint as the terrain's
        // outer grid, so it takes the plain N = 9 permutation; the 8x8 coverage mask is a cell
        // grid and takes the bit version.
        permuteSquare<float> (op, 9, layer.height.data());
        permuteSquare<float> (op, 9, layer.depth.data());
        permuteSquare<glm::vec2> (op, 9, layer.uv.data());
        layer.subchunks = permuteSubchunkMask (op, layer.subchunks);
      }
    }

    // The MCLY entries are per LAYER, not per texel: rotating a chunk does not renumber its
    // texture layers, so layers_info, the texture name list and the area id are all invariant.

    if (data.objects)
    {
      for (ChunkObjectCacheEntry& object : *data.objects)
      {
        // Object positions are relative to the pivot chunk's corner; the block's own corner is
        // min_x / min_z chunks away from that, and the transform is about the block.
        float const local_x (object.pos.x - static_cast<float> (min_x) * CHUNKSIZE);
        float const local_z (object.pos.z - static_cast<float> (min_z) * CHUNKSIZE);
        float new_local_x (local_x);
        float new_local_z (local_z);

        switch (op)
        {
          case ChunkGridOp::ROTATE_90:
            new_local_x = local_z;
            new_local_z = width_units - local_x;
            break;
          case ChunkGridOp::MIRROR_X:
            new_local_x = width_units - local_x;
            break;
          case ChunkGridOp::MIRROR_Z:
            new_local_z = height_units - local_z;
            break;
          case ChunkGridOp::IDENTITY:
            break;
        }

        object.pos.x = new_local_x + static_cast<float> (min_x) * CHUNKSIZE;
        object.pos.z = new_local_z + static_cast<float> (min_z) * CHUNKSIZE;
        object.dir.y = transformedYaw (op, object.dir.y);
      }
    }
  }

  emit clipboardChanged();
}

// =================================================================================================
// Paste
// =================================================================================================

bool ChunkClipboard::resolvePasteOrigin (glm::vec3 const& pos, int& base_global_x, int& base_global_z) const
{
  if (hasFlag (_paste_flags, ChunkPasteFlags::AT_SOURCE_LOCATION))
  {
    base_global_x = _clipboard_pivot_chunk.globalX();
    base_global_z = _clipboard_pivot_chunk.globalZ();
    return true;
  }

  MapChunk* const chunk (_world->getChunkAt (pos));

  if (!chunk)
  {
    return false;
  }

  TileIndex const tile (glm::vec3 {chunk->xbase, 0.0f, chunk->zbase});
  base_global_x = static_cast<int> (tile.x) * 16 + chunk->px;
  base_global_z = static_cast<int> (tile.z) * 16 + chunk->py;
  return true;
}

ChunkPasteReport ChunkClipboard::pasteSelection (glm::vec3 const& pos)
{
  ChunkPasteReport report;

  if (_cached_chunks.empty())
  {
    return report;
  }

  // Joining somebody else's running action would put this paste's undo data on their stroke and
  // then close it under them. beginAction returns the running action rather than starting a new
  // one (ActionManager.cpp:64), so this has to be checked here and not hoped for.
  if (NOGGIT_CUR_ACTION)
  {
    return report;
  }

  int base_global_x (0);
  int base_global_z (0);

  if (!resolvePasteOrigin (pos, base_global_x, base_global_z))
  {
    return report;
  }

  // Same idiom as Action::undo (Action.cpp:55-56): every path below can reach a texture upload
  // or a chunk update, and a paste is reachable from a panel button where no context is current.
  _map_view->context()->makeCurrent (_map_view->context()->surface());
  OpenGL::context::scoped_setter const _ (::gl, _map_view->context());

  ChunkPasteFlags const flags (_paste_flags);
  bool const replace (hasFlag (flags, ChunkPasteFlags::REPLACE_DESTINATION));

  float const height_shift
    ( _height_offset
    + (_height_mode == ChunkHeightMode::DESTINATION_ELEVATION ? pos.y - _clipboard_pivot_height : 0.0f)
    );

  // ---- resolve every destination before writing anything ------------------------------------
  //
  // A paste that discovers half way through that the next ADT is not on the map has already
  // changed the map, and the user's undo is one step behind their eyes. Resolving first turns
  // that into a count in the status line.
  struct Destination
  {
    CachedChunk const* source;
    MapChunk* chunk;
    int global_x;
    int global_z;
  };

  std::vector<Destination> destinations;
  destinations.reserve (_cached_chunks.size());

  std::unordered_set<MapTile*> touched_tiles;

  for (CachedChunk const& cached : _cached_chunks)
  {
    int const global_x (base_global_x + cached.rel_x);
    int const global_z (base_global_z + cached.rel_z);

    SelectedChunkIndex const target (SelectedChunkIndex::fromGlobal (global_x, global_z));

    if (!target.tile_index.is_valid() || !_world->mapIndex.hasTile (target.tile_index))
    {
      ++report.chunks_skipped;
      continue;
    }

    MapTile* const tile (_world->mapIndex.loadTile (target.tile_index));

    if (!tile)
    {
      ++report.chunks_skipped;
      continue;
    }

    tile->wait_until_loaded();
    MapChunk* const chunk (tile->getChunk (target.x, target.z));

    if (!chunk)
    {
      ++report.chunks_skipped;
      continue;
    }

    destinations.push_back ({&cached, chunk, global_x, global_z});
    touched_tiles.emplace (tile);
  }

  if (destinations.empty())
  {
    return report;
  }

  std::unordered_set<MapChunk*> pasted_chunks;
  std::map<std::pair<int, int>, MapChunk*> chunk_by_global;

  for (Destination const& destination : destinations)
  {
    pasted_chunks.emplace (destination.chunk);
    chunk_by_global.emplace (std::make_pair (destination.global_x, destination.global_z), destination.chunk);
  }

  // ---- one action for the whole paste --------------------------------------------------------
  NOGGIT_ACTION_MGR->beginAction (_map_view, Noggit::ActionFlags::eNO_FLAG);

  // Armed on the line after beginAction so that no early return or thrown exception below can
  // leave the action open. See ScopedAction for why an open action is a session-long failure
  // rather than a lost paste.
  ScopedAction const close_action_on_leaving_scope;

  Noggit::Action* const action (NOGGIT_CUR_ACTION);

  // registerAllChunkChanges snapshots terrain, textures, vertex colours, holes, area id, flags,
  // liquid, the vertex selection, shadows, layer info and doodad exclusion in one call
  // (Action.cpp:913-926), and every registrar sets its own ActionFlags bit, so the single
  // beginAction above does not have to guess the flag set in advance.
  for (Destination const& destination : destinations)
  {
    action->registerAllChunkChanges (destination.chunk);
  }

  bool terrain_written (false);

  for (Destination const& destination : destinations)
  {
    ChunkCache const& data (destination.source->data);
    MapChunk* const chunk (destination.chunk);

    if (data.terrain_height)
    {
      for (int i (0); i < mapbufsize; ++i)
      {
        chunk->mVertices[i].y = (*data.terrain_height)[static_cast<std::size_t> (i)] + height_shift;
      }

      chunk->registerChunkUpdate (ChunkUpdateFlags::VERTEX);
      terrain_written = true;
    }

    if (data.vertex_colors)
    {
      std::memcpy (&chunk->mccv, data.vertex_colors->colors.data(), 145 * 3 * sizeof (float));

      // All three pieces, in this order, exactly as Action::undo does it (Action.cpp:104-124).
      // setHasMccv derives the header bit from the runtime flag; the header bit is then written
      // from the snapshot so that a source where the two disagreed is reproduced rather than
      // normalised.
      chunk->setHasMccv (data.vertex_colors->has_mccv_runtime);
      chunk->header_flags.flags.has_mccv = data.vertex_colors->has_mccv_header ? 1 : 0;
      chunk->registerChunkUpdate (ChunkUpdateFlags::MCCV);
    }

    if (data.shadows)
    {
      std::memcpy (&chunk->_shadow_map, data.shadows->data(), 64 * 64 * sizeof (std::uint8_t));
      chunk->registerChunkUpdate (ChunkUpdateFlags::SHADOW);
    }

    if (data.holes)
    {
      chunk->holes = *data.holes;
      chunk->registerChunkUpdate (ChunkUpdateFlags::HOLES);
    }

    if (data.area_id)
    {
      chunk->areaID = *data.area_id;
      chunk->registerChunkUpdate (ChunkUpdateFlags::AREA_ID);
    }

    if (data.flags)
    {
      bool const had_colors (chunk->hasColors());

      chunk->header_flags = *data.flags;

      // has_mccv IS NOT A COPYABLE FLAG ON ITS OWN. MapChunk::save gates the whole MCCV block on
      // the RUNTIME hasMCCV (MapChunk.cpp:1553), which this assignment cannot reach, so copying
      // the header bit without the colours produces a chunk whose two halves disagree -- and
      // the disagreement is invisible until save time. When the colours came along the block
      // above has already set both consistently; when they did not, the destination's own state
      // is the only correct answer.
      if (!data.vertex_colors)
      {
        chunk->header_flags.flags.has_mccv = had_colors ? 1 : 0;
      }

      chunk->registerChunkUpdate (ChunkUpdateFlags::FLAGS);
    }

    TextureSet* const texture_set (chunk->getTextureSet());

    if (texture_set && (data.textures || data.alphamaps || data.layers_info || data.doodad_stencil))
    {
      // THE LAYER COUNT AND THE ALPHAMAP COUNT ARE NOT INDEPENDENT, and getting that wrong is a
      // null dereference rather than a wrong picture. TextureSet::addTexture allocates
      // alphamaps[k - 1] when it creates layer k (texture_set.cpp:86-89), so a set with n layers
      // has exactly n - 1 alphamaps; TextureSet::apply_alpha_changes then walks
      // `alpha_layer < nTextures - 1` and dereferences alphamaps[alpha_layer] unconditionally
      // (:1656-1673). Raising nTextures without supplying the matching alphamaps crashes there.
      //
      // So the three ways the two check boxes can be combined are handled as three different
      // operations rather than as one with holes in it:
      //
      //   both      full replacement of the texture set, the same thing Action::undo restores.
      //   names     re-texture in place: swap file names for the layers the destination already
      //             has, keep its count and its blend.
      //   weights   re-blend in place: swap the alphamaps the destination already has room for,
      //             keep its names and its count.
      std::size_t const destination_layers (texture_set->num());
      bool const full_replacement (data.textures && data.alphamaps);

      if (data.alphamaps)
      {
        std::array<std::unique_ptr<Alphamap>, MAX_ALPHAMAPS> maps;

        // Without a full replacement the destination's layer count is staying as it is, so only
        // the alphamaps that count has room for may be written.
        std::size_t const writable
          ( full_replacement
          ? std::size_t (MAX_ALPHAMAPS)
          : (destination_layers ? destination_layers - 1 : 0)
          );

        for (std::size_t i (0); i < MAX_ALPHAMAPS; ++i)
        {
          if (i < writable && data.alphamaps->present[i])
          {
            maps[i] = std::make_unique<Alphamap>();
            // Alphamap::setAlpha takes a non-const pointer although it only reads through it, so
            // the copy is what stands in for a const_cast.
            std::array<std::uint8_t, 64 * 64> bytes (data.alphamaps->maps[i]);
            maps[i]->setAlpha (bytes.data());
          }
          else if (!full_replacement)
          {
            // Keep whatever the destination had in the slots this paste is not entitled to.
            auto const& existing (*texture_set->getAlphamaps());

            if (existing[i])
            {
              maps[i] = std::make_unique<Alphamap>(*existing[i]);
            }
          }
        }

        texture_set->setAlphamaps (maps);

        if (data.alphamaps->tmp_edit_values)
        {
          auto temporary (std::make_unique<tmp_edit_alpha_values>());

          for (std::size_t layer (0); layer < 4; ++layer)
          {
            temporary->map[layer] = (*data.alphamaps->tmp_edit_values)[layer];
          }

          texture_set->getTempAlphamaps() = std::move (temporary);
        }
        else
        {
          texture_set->getTempAlphamaps().reset();
        }
      }

      if (data.textures)
      {
        // Rebuilt FROM THE PATH STRINGS, exactly as Action::undo does (Action.cpp:88-97). A
        // texture id is an index into the source ADT's MTEX table and means something else in
        // the destination's.
        auto* const textures (texture_set->getTextures());

        // A pack could name a different number of layers than it lists files for; the reader
        // bounds both at four but cannot make them agree, so the smaller of the two wins.
        std::size_t source_layers
          (std::min (data.textures->n_textures, data.textures->textures.size()));

        if (!full_replacement)
        {
          source_layers = std::min (source_layers, destination_layers);
        }

        if (full_replacement)
        {
          textures->clear();
          textures->reserve (source_layers);

          for (std::size_t i (0); i < source_layers; ++i)
          {
            textures->emplace_back (data.textures->textures[i], _world->getRenderContext());
          }

          texture_set->setNTextures (source_layers);
        }
        else
        {
          for (std::size_t i (0); i < source_layers && i < textures->size(); ++i)
          {
            (*textures)[i] = scoped_blp_texture_reference (data.textures->textures[i]
                                                          , _world->getRenderContext());
          }
        }
      }

      if (data.layers_info)
      {
        std::memcpy (texture_set->getMCLYEntries(), data.layers_info->data(), sizeof (layer_info) * 4);
        chunk->registerChunkUpdate (ChunkUpdateFlags::GROUND_EFFECT);
      }

      if (data.doodad_stencil)
      {
        std::memcpy (texture_set->getDoodadStencilBase(), data.doodad_stencil->data(), 8);
        chunk->registerChunkUpdate (ChunkUpdateFlags::DETAILDOODADS_EXCLUSION);
      }

      // LAST-DITCH INVARIANT GUARD, and it is what keeps apply_alpha_changes below from
      // dereferencing a null Alphamap.
      //
      // Everything above is careful, but a pack is a file and a file can say anything: a
      // hand-edited or bit-rotted one can declare four layers and carry one alphamap, or carry
      // alphamaps 0 and 2 with a hole at 1. TextureSet's invariant is that a set with n layers
      // has alphamaps 0 .. n-2 all non-null (addTexture, texture_set.cpp:86-89), and
      // apply_alpha_changes walks `alpha_layer < nTextures - 1` and dereferences each one
      // unconditionally (:1656-1673). So rather than reason about which combination of check
      // boxes could break it, the layer count is measured back off the state that actually
      // exists and lowered if it disagrees. This costs three loads on a path that already
      // rebuilds a whole texture set.
      {
        std::size_t contiguous_alphamaps (0);
        auto const& final_maps (*texture_set->getAlphamaps());

        while (contiguous_alphamaps < MAX_ALPHAMAPS && final_maps[contiguous_alphamaps])
        {
          ++contiguous_alphamaps;
        }

        std::size_t const safe_layers
          ( std::min ({ std::size_t (texture_set->num())
                      , texture_set->getTextures()->size()
                      , contiguous_alphamaps + 1
                      })
          );

        if (safe_layers != texture_set->num())
        {
          texture_set->setNTextures (safe_layers);
        }
      }

      texture_set->markDirty();
      texture_set->apply_alpha_changes();
      // Re-derived rather than carried; MapChunk::save does exactly this call at exactly this
      // point in the sequence (MapChunk.cpp:1515-1516), and its own note says the temporary
      // alphamaps have to be applied first -- which the line above has just done.
      texture_set->updateDoodadMapping();
      chunk->registerChunkUpdate (ChunkUpdateFlags::FLAGS); // texture animation bits
    }

    if (data.liquid_layers)
    {
      ChunkWater* const water (chunk->liquid_chunk());
      std::vector<liquid_layer>* const layers (water->getLayers());

      if (replace)
      {
        layers->clear();
      }

      for (ChunkLiquidLayerCache const& cache : *data.liquid_layers)
      {
        // Rebuilt against the DESTINATION's ChunkWater rather than copied. A copied liquid_layer
        // carries the source's ChunkWater* back-pointer (liquid_layer.hpp:224) and would leave
        // the destination's water pointing at another chunk; the constructor here is also what
        // derives the vertex format from the liquid id (liquid_layer.cpp:39), which is why the
        // cache does not carry one.
        float const base_height (cache.height[0] + height_shift);
        liquid_layer layer (water, glm::vec3 (chunk->xbase, base_height, chunk->zbase)
                           , base_height, cache.liquid_id);

        auto& vertices (layer.getVertices());

        for (std::size_t i (0); i < 9 * 9; ++i)
        {
          vertices[i].position.y = cache.height[i] + height_shift;
          vertices[i].depth = cache.depth[i];
          vertices[i].uv = cache.uv[i];
        }

        for (int row (0); row < 8; ++row)
        {
          for (int col (0); col < 8; ++col)
          {
            layer.setSubchunk (col, row, ((cache.subchunks >> (row * 8 + col)) & 1) != 0);
          }
        }

        layer.updateMinMax();
        layers->emplace_back (std::move (layer));
      }

      water->update_layers();
      water->tagUpdate();
    }
    else if (replace && hasFlag (_clipboard_flags, ChunkCopyFlags::LIQUID))
    {
      // Liquid WAS copied and this chunk had none. With "Replace destination" on, that absence
      // is data: the destination's water goes.
      ChunkWater* const water (chunk->liquid_chunk());

      if (!water->getLayers()->empty())
      {
        water->getLayers()->clear();
        water->update_layers();
        water->tagUpdate();
      }
    }

    ++report.chunks;
  }

  // ---- objects -------------------------------------------------------------------------------
  //
  // After the terrain, so that the height a model is dropped at is measured against ground that
  // has already moved.
  if (hasFlag (flags, ChunkPasteFlags::REPLACE_OBJECTS))
  {
    std::vector<unsigned> doomed;

    for (Destination const& destination : destinations)
    {
      MapChunk* const chunk (destination.chunk);

      for (auto const& entry : chunk->mt->getObjectInstances())
      {
        for (SceneObject* const object : entry.second)
        {
          if (insideChunk (object->pos, chunk->xbase, chunk->zbase))
          {
            doomed.emplace_back (object->uid);
          }
        }
      }
    }

    // Collected first and deleted after: World::deleteInstance mutates the storage the loop
    // above is walking and calls reset_selection (World.cpp:2028-2030).
    std::sort (doomed.begin(), doomed.end());
    doomed.erase (std::unique (doomed.begin(), doomed.end()), doomed.end());

    for (unsigned const uid : doomed)
    {
      _world->deleteInstance (static_cast<int> (uid), true);
      ++report.objects_removed;
    }
  }

  {
    MapChunk* const anchor (destinations.front().chunk);
    // The destination pivot chunk's corner, which is what every cached object position is
    // measured from. Reconstructed from the first destination and its own relative offset so
    // that it is right whether the paste landed under the cursor or back at the source.
    float const anchor_x (anchor->xbase - static_cast<float> (destinations.front().source->rel_x) * CHUNKSIZE);
    float const anchor_z (anchor->zbase - static_cast<float> (destinations.front().source->rel_z) * CHUNKSIZE);

    for (Destination const& destination : destinations)
    {
      ChunkCache const& data (destination.source->data);

      if (!data.objects)
      {
        continue;
      }

      for (ChunkObjectCacheEntry const& object : *data.objects)
      {
        glm::vec3 const position { anchor_x + object.pos.x
                                 , object.pos.y + height_shift
                                 , anchor_z + object.pos.z
                                 };

        if (object.type == ChunkManipulatorObjectTypes::WMO)
        {
          _world->addWMOAndGetInstance (object.file_key, position, object.dir, object.scale, true);
        }
        else
        {
          // ignore_params = true: the object/paste randomisation settings exist for placing a NEW
          // doodad by hand (World.cpp:2126-2149). A paste is reproducing a placement that already
          // had a rotation and a scale, and randomising them would make a copy that is not one.
          _world->addM2AndGetInstance (object.file_key, position, object.scale, object.dir
                                      , nullptr, true, true);
        }

        ++report.objects_added;
      }
    }
  }

  // ---- seams ---------------------------------------------------------------------------------
  std::unordered_set<MapChunk*> renormalise (pasted_chunks);

  if (terrain_written && hasFlag (flags, ChunkPasteFlags::SEW_SEAMS))
  {
    // A HEIGHT WELD, SYMMETRIC AND ORDER-INDEPENDENT.
    //
    // MapChunk::fixGapLeft / fixGapAbove (MapChunk.cpp:1837, :1862) exist, but they OVERWRITE
    // this chunk's edge with the neighbour's, one direction only. Run over a pasted block that
    // means whichever chunk is visited first wins: the paste would be clipped to the old terrain
    // along two of its sides and would bleed into it along the other two, purely by iteration
    // order. So this gathers every copy of every shared border vertex first, keyed by its
    // position on the map-wide vertex grid, and writes the mean back to all of them afterwards.
    // A corner vertex has up to four copies and they all end up equal, which an edge-at-a-time
    // fixGap pass cannot manage.
    //
    // Only the 9x9 OUTER grid can be shared. The 8x8 inner vertices sit half a unit inside the
    // chunk and have no counterpart in the neighbour, so there is no seam there to weld.
    //
    // WHAT THIS DOES NOT DO: it snaps heights, it does not blend them over a band, and it does
    // nothing at all for alphamap, liquid, shadow or vertex-colour seams -- none of which exist
    // anywhere in this codebase today. A pasted block's texture blend still terminates hard at
    // its border.
    std::map<std::pair<int, int>, std::vector<std::pair<MapChunk*, int>>> shared;

    auto const contribute
      ( [&shared] (MapChunk* chunk, int global_x, int global_z, int row, int col)
        {
          // Global vertex coordinates: 8 units of terrain grid per chunk, so the vertex a chunk
          // calls (row, 8) and its right-hand neighbour calls (row, 0) get the same key.
          shared[{global_x * 8 + col, global_z * 8 + row}]
            .emplace_back (chunk, outerVertex (row, col));
        }
      );

    static constexpr std::array<std::pair<int, int>, 4> DIRECTIONS
      {{ {-1, 0}, {1, 0}, {0, -1}, {0, 1} }};

    for (Destination const& destination : destinations)
    {
      for (auto const& direction : DIRECTIONS)
      {
        int const neighbour_x (destination.global_x + direction.first);
        int const neighbour_z (destination.global_z + direction.second);

        // An interior edge of the pasted block needs no weld: both sides came from chunks that
        // were adjacent at the source, and every transform in applyGridOp is rigid.
        if (chunk_by_global.count ({neighbour_x, neighbour_z}))
        {
          continue;
        }

        SelectedChunkIndex const index (SelectedChunkIndex::fromGlobal (neighbour_x, neighbour_z));

        if (!index.tile_index.is_valid() || !_world->mapIndex.tileLoaded (index.tile_index))
        {
          continue;
        }

        MapTile* const tile (_world->mapIndex.getTile (index.tile_index));
        MapChunk* const neighbour (tile ? tile->getChunk (index.x, index.z) : nullptr);

        if (!neighbour)
        {
          continue;
        }

        action->registerChunkTerrainChange (neighbour);
        renormalise.emplace (neighbour);
        touched_tiles.emplace (tile);

        for (int i (0); i < 9; ++i)
        {
          if (direction.first == -1)      { contribute (destination.chunk, destination.global_x, destination.global_z, i, 0);
                                            contribute (neighbour, neighbour_x, neighbour_z, i, 8); }
          else if (direction.first == 1)  { contribute (destination.chunk, destination.global_x, destination.global_z, i, 8);
                                            contribute (neighbour, neighbour_x, neighbour_z, i, 0); }
          else if (direction.second == -1){ contribute (destination.chunk, destination.global_x, destination.global_z, 0, i);
                                            contribute (neighbour, neighbour_x, neighbour_z, 8, i); }
          else                            { contribute (destination.chunk, destination.global_x, destination.global_z, 8, i);
                                            contribute (neighbour, neighbour_x, neighbour_z, 0, i); }
        }
      }
    }

    for (auto& entry : shared)
    {
      // De-duplicated because a corner vertex is contributed once per welded direction and the
      // same (chunk, index) pair can therefore turn up twice; counting it twice would weight
      // that chunk's height double in the mean.
      std::vector<std::pair<MapChunk*, int>>& copies (entry.second);
      std::sort (copies.begin(), copies.end());
      copies.erase (std::unique (copies.begin(), copies.end()), copies.end());

      if (copies.size() < 2)
      {
        continue;
      }

      double sum (0.0);

      for (auto const& copy : copies)
      {
        sum += copy.first->mVertices[copy.second].y;
      }

      float const mean (static_cast<float> (sum / static_cast<double> (copies.size())));

      for (auto const& copy : copies)
      {
        copy.first->mVertices[copy.second].y = mean;
        copy.first->registerChunkUpdate (ChunkUpdateFlags::VERTEX);
      }
    }
  }

  // ---- normals, last ---------------------------------------------------------------------------
  //
  // A second pass after every height in the operation is final, because MapChunk::recalcNorms
  // samples the four neighbouring vertices (MapChunk.cpp:828-831) and those can live in another
  // chunk or another ADT. Action::undo splits its terrain restore for the same reason
  // (Action.cpp:60-71). recalcNorms registers ChunkUpdateFlags::NORMALS itself (MapChunk.cpp:854).
  if (terrain_written)
  {
    for (MapChunk* const chunk : renormalise)
    {
      _world->recalc_norms (chunk);
    }

    _world->updateVertexCenter();
  }

  for (MapTile* const tile : touched_tiles)
  {
    _world->mapIndex.setChanged (tile);
  }

  // No endAction() here: ScopedAction above closes it as this scope unwinds, on every path.

  emit pasted (report);
  return report;
}

// =================================================================================================
// Parameters
// =================================================================================================

ChunkCopyFlags ChunkClipboard::copyFlags() const
{
  return _copy_flags;
}

void ChunkClipboard::setCopyFlags (ChunkCopyFlags flags)
{
  _copy_flags = flags;
}

ChunkPasteFlags ChunkClipboard::pasteFlags() const
{
  return _paste_flags;
}

void ChunkClipboard::setPasteFlags (ChunkPasteFlags flags)
{
  _paste_flags = flags;
}

ChunkHeightMode ChunkClipboard::heightMode() const
{
  return _height_mode;
}

void ChunkClipboard::setHeightMode (ChunkHeightMode mode)
{
  _height_mode = mode;
}

float ChunkClipboard::heightOffset() const
{
  return _height_offset;
}

void ChunkClipboard::setHeightOffset (float offset)
{
  _height_offset = offset;
}

// =================================================================================================
// Packs
// =================================================================================================

void ChunkClipboard::exportPack (std::filesystem::path const& path) const
{
  ChunkPack pack;
  pack.copy_flags = _clipboard_flags;
  pack.source_map_id = _clipboard_map_id;
  pack.source_map_name = _clipboard_map_name;
  pack.pivot = _clipboard_pivot;
  pack.chunks = _cached_chunks;

  writeChunkPackFile (path, pack);
}

void ChunkClipboard::importPack (std::filesystem::path const& path)
{
  // The decode completes into a local before a single member here is touched, so a throw leaves
  // the clipboard exactly as it was. That is the whole reason readChunkPackFile returns by value
  // instead of filling something in place.
  ChunkPack pack (readChunkPackFile (path));

  _cached_chunks = std::move (pack.chunks);
  _clipboard_flags = pack.copy_flags;
  _clipboard_map_id = pack.source_map_id;
  _clipboard_map_name = std::move (pack.source_map_name);
  _clipboard_pivot = pack.pivot;
  _clipboard_pivot_height = pack.pivot.y;
  _clipboard_from_another_window = true;

  // The pivot's own chunk, so "Paste at source ADT location" works on an imported pack too. It
  // is the chunk with relative offset (0, 0); a pack whose pivot chunk was not itself selected
  // has none, and the first chunk's own recorded source, walked back by its offset, is the same
  // answer.
  _clipboard_pivot_chunk = SelectedChunkIndex::fromGlobal (0, 0);

  if (!_cached_chunks.empty())
  {
    CachedChunk const& first (_cached_chunks.front());
    _clipboard_pivot_chunk = SelectedChunkIndex::fromGlobal (first.source.globalX() - first.rel_x
                                                            , first.source.globalZ() - first.rel_z);
  }

  emit clipboardChanged();
}

void ChunkClipboard::publishSharedClipboard()
{
  // BEST EFFORT AND SILENT ON FAILURE. This is what makes "copy here, paste in the other editor
  // window" work, and it must never be able to make copying itself fail: a read-only temp
  // directory, a full disk or an antivirus lock are all reasons to have no cross-window
  // clipboard, and none of them is a reason to lose the in-memory one.
  //
  // THE COST, STATED. Every copy writes the whole pack. A chunk with all thirteen classes ticked,
  // one liquid layer, two objects and no unapplied paint stroke encodes to 12,077 bytes -- 11.8
  // KiB, dominated by the 4096-byte alphamap, the 4096-byte shadow map, 1740 bytes of vertex
  // colour and 1296 bytes of liquid vertices. That figure is arithmetic on the field list in
  // ChunkPack.cpp AND it is confirmed against the encoder: three such chunks, one of them also
  // carrying a 64 KiB temporary alphamap buffer, plus the 48-byte header and an 11-byte map
  // name, came out at exactly 3 x 12077 + 65536 + 48 + 11 = 101,826 bytes.
  //
  // So a 3000-chunk selection is about 36 MB of sequential write per Copy. That is the price of
  // the feature, and it is paid on Copy rather than on Paste, which is the right way round: a
  // copy is a deliberate act and a paste wants to be immediate.
  try
  {
    ChunkPack pack;
    pack.copy_flags = _clipboard_flags;
    pack.source_map_id = _clipboard_map_id;
    pack.source_map_name = _clipboard_map_name;
    pack.pivot = _clipboard_pivot;
    pack.chunks = _cached_chunks;

    std::filesystem::path const path (sharedClipboardPackPath());
    writeChunkPackFile (path, pack);

    std::error_code error;
    _shared_clipboard_stamp = std::filesystem::last_write_time (path, error);
    _shared_clipboard_stamp_valid = !error;
  }
  catch (...)
  {
    _shared_clipboard_stamp_valid = false;
  }
}

bool ChunkClipboard::adoptSharedClipboard()
{
  try
  {
    std::filesystem::path const path (sharedClipboardPackPath());

    std::error_code error;
    std::filesystem::file_time_type const stamp (std::filesystem::last_write_time (path, error));

    if (error)
    {
      return false;
    }

    // Our own copy wrote this file a moment ago; adopting it would replace a clipboard with a
    // byte-identical one and tell the user it came from another window.
    if (_shared_clipboard_stamp_valid && stamp <= _shared_clipboard_stamp)
    {
      return false;
    }

    ChunkPack pack (readChunkPackFile (path));

    _cached_chunks = std::move (pack.chunks);
    _clipboard_flags = pack.copy_flags;
    _clipboard_map_id = pack.source_map_id;
    _clipboard_map_name = std::move (pack.source_map_name);
    _clipboard_pivot = pack.pivot;
    _clipboard_pivot_height = pack.pivot.y;
    _clipboard_from_another_window = true;
    _shared_clipboard_stamp = stamp;
    _shared_clipboard_stamp_valid = true;

    _clipboard_pivot_chunk = SelectedChunkIndex::fromGlobal (0, 0);

    if (!_cached_chunks.empty())
    {
      CachedChunk const& first (_cached_chunks.front());
      _clipboard_pivot_chunk = SelectedChunkIndex::fromGlobal (first.source.globalX() - first.rel_x
                                                              , first.source.globalZ() - first.rel_z);
    }

    emit clipboardChanged();
    return true;
  }
  catch (...)
  {
    // A pack still being written, or written by a build with a different format version. Both
    // are ordinary and neither is worth interrupting the user over -- the file will be complete,
    // or it will not, and the watcher will call again.
    return false;
  }
}
