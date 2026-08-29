// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/terrain/TerrainMaskHistory.hpp>
#include <noggit/terrain/TerrainMaskStore.hpp>

#include <cmath>
#include <new>
#include <utility>

namespace
{
  // Global chunk index on one axis: the tile index times 16 plus the chunk inside it. Painting
  // works in this space because a stroke does not care about tile borders, and MaskChunkAddress
  // is reconstructed from it below.
  int globalChunkIndex(float world_coordinate)
  {
    return static_cast<int>(std::floor(world_coordinate / Noggit::MASK_CHUNK_SIZE));
  }
}

namespace Noggit
{
  TerrainMaskHistory* TerrainMaskHistory::instance()
  {
    static TerrainMaskHistory history;
    return &history;
  }

  void TerrainMaskHistory::beginStroke(std::string const& mask_name)
  {
    if (_stroke_open)
    {
      if (_open.mask_name == mask_name)
      {
        return;
      }

      endStroke();
    }

    _open = Entry();
    _open.mask_name = mask_name;
    _open_captured.clear();
    _stroke_open = true;
  }

  bool TerrainMaskHistory::strokeOpen() const
  {
    return _stroke_open;
  }

  bool TerrainMaskHistory::capture( NamedTerrainMask const& mask
                                  , float world_x
                                  , float world_z
                                  , float radius
                                  )
  {
    if (!_stroke_open || !(radius > 0.0f))
    {
      return false;
    }

    // Non-finite guard before any cast to int, the same rule TerrainMaskStore::factorAt states:
    // a cursor position that missed the terrain arrives here as NaN and casting it is undefined
    // rather than merely wrong.
    if (!(world_x > -1.0e9f) || !(world_x < 1.0e9f) || !(world_z > -1.0e9f) || !(world_z < 1.0e9f))
    {
      return false;
    }

    int const first_chunk_x = globalChunkIndex(world_x - radius);
    int const last_chunk_x = globalChunkIndex(world_x + radius);
    int const first_chunk_z = globalChunkIndex(world_z - radius);
    int const last_chunk_z = globalChunkIndex(world_z + radius);

    bool captured_anything = false;

    for (int chunk_z = first_chunk_z; chunk_z <= last_chunk_z; ++chunk_z)
    {
      if (chunk_z < 0)
      {
        continue;
      }

      for (int chunk_x = first_chunk_x; chunk_x <= last_chunk_x; ++chunk_x)
      {
        if (chunk_x < 0)
        {
          continue;
        }

        MaskChunkAddress address;
        address.tile_x = chunk_x / MASK_TILE_CHUNK_SIDE;
        address.chunk_x = chunk_x % MASK_TILE_CHUNK_SIDE;
        address.tile_z = chunk_z / MASK_TILE_CHUNK_SIDE;
        address.chunk_z = chunk_z % MASK_TILE_CHUNK_SIDE;

        if (!address.valid())
        {
          continue;
        }

        std::uint32_t const packed = address.packed();

        if (!_open_captured.insert(packed).second)
        {
          continue;
        }

        // ALLOCATION IS SWALLOWED HERE AND NOWHERE ELSE. Every caller of this function is a mask
        // tool tick, and a tool tick runs inside paintGL (MapView.cpp:5109 calls tick from
        // paintGL). An exception leaving paintGL unwinds through OpenGL::Scoped destructors with
        // no current context, and a throw from a destructor terminates -- so the two 4 KiB
        // vectors below are the one place in this file that can throw and the one place that
        // catches. Losing undo for a stroke is a far smaller failure than losing the editor.
        try
        {
          ChunkImage image;
          image.packed = packed;
          image.paint.resize(MASK_CHUNK_TEXELS);
          image.composited.resize(MASK_CHUNK_TEXELS);

          mask.paint.readChunk(address, image.paint.data());
          mask.composited.readChunk(address, image.composited.data());

          _open.images.push_back(std::move(image));
          captured_anything = true;
        }
        catch (std::bad_alloc const&)
        {
          // Undo the bookkeeping so a later, smaller stroke over the same chunk can still try.
          _open_captured.erase(packed);
          return false;
        }
      }
    }

    return captured_anything;
  }

  void TerrainMaskHistory::endStroke()
  {
    if (!_stroke_open)
    {
      return;
    }

    _stroke_open = false;
    _open_captured.clear();

    if (_open.images.empty())
    {
      _open = Entry();
      return;
    }

    _undo.push_back(std::move(_open));
    _open = Entry();

    // A new stroke invalidates redo, which is the universal rule and is what stops a redo from
    // writing an image that was captured against a field that has since moved underneath it.
    _redo.clear();

    enforceBudget();
  }

  bool TerrainMaskHistory::canUndo() const
  {
    return !_undo.empty();
  }

  bool TerrainMaskHistory::canRedo() const
  {
    return !_redo.empty();
  }

  bool TerrainMaskHistory::swapEntry(Entry& entry, NamedTerrainMask& mask)
  {
    std::vector<std::uint8_t> scratch(MASK_CHUNK_TEXELS);

    for (ChunkImage& image : entry.images)
    {
      MaskChunkAddress const address = MaskChunkAddress::fromPacked(image.packed);

      if (!address.valid()
       || image.paint.size() != static_cast<std::size_t>(MASK_CHUNK_TEXELS)
       || image.composited.size() != static_cast<std::size_t>(MASK_CHUNK_TEXELS))
      {
        continue;
      }

      // Read the live values into the entry as the stored ones go out. That exchange is what
      // makes one image set serve as both undo and redo -- see the storage note in the header.
      //
      // Restoring an all-zero image writes a UNIFORM chunk where there may previously have been
      // no chunk at all. Those are the same thing to every reader (TerrainMask documents absence
      // as zero) and a uniform block carries no payload, so the difference is one hash entry.
      mask.paint.readChunk(address, scratch.data());
      mask.paint.writeChunk(address, image.paint.data());
      std::swap(scratch, image.paint);

      mask.composited.readChunk(address, scratch.data());
      mask.composited.writeChunk(address, image.composited.data());
      std::swap(scratch, image.composited);
    }

    return true;
  }

  bool TerrainMaskHistory::undo(TerrainMaskStore& store)
  {
    // A stroke still in flight has to be closed before its own before-image can be replayed;
    // otherwise the entry the user is asking for is the one still sitting in _open.
    endStroke();

    while (!_undo.empty())
    {
      Entry entry = std::move(_undo.back());
      _undo.pop_back();

      NamedTerrainMask* const mask = store.find(entry.mask_name);

      if (!mask)
      {
        // The mask was deleted or renamed. Drop the entry and try the one below it rather than
        // reporting failure for a stack that may still hold usable steps.
        continue;
      }

      swapEntry(entry, *mask);
      _redo.push_back(std::move(entry));
      return true;
    }

    return false;
  }

  bool TerrainMaskHistory::redo(TerrainMaskStore& store)
  {
    endStroke();

    while (!_redo.empty())
    {
      Entry entry = std::move(_redo.back());
      _redo.pop_back();

      NamedTerrainMask* const mask = store.find(entry.mask_name);

      if (!mask)
      {
        continue;
      }

      swapEntry(entry, *mask);
      _undo.push_back(std::move(entry));
      return true;
    }

    return false;
  }

  void TerrainMaskHistory::clear()
  {
    _stroke_open = false;
    _open = Entry();
    _open_captured.clear();
    _undo.clear();
    _redo.clear();
  }

  std::size_t TerrainMaskHistory::undoDepth() const
  {
    return _undo.size();
  }

  std::size_t TerrainMaskHistory::redoDepth() const
  {
    return _redo.size();
  }

  std::size_t TerrainMaskHistory::entryBytes(Entry const& entry)
  {
    // 8192 bytes per image, which is the two MASK_CHUNK_TEXELS fields. Computed from the vectors
    // rather than assumed, so a partially built entry reports what it actually holds.
    std::size_t bytes = 0;

    for (ChunkImage const& image : entry.images)
    {
      bytes += image.paint.size() + image.composited.size();
    }

    return bytes;
  }

  std::size_t TerrainMaskHistory::bytes() const
  {
    std::size_t bytes = entryBytes(_open);

    for (Entry const& entry : _undo)
    {
      bytes += entryBytes(entry);
    }

    for (Entry const& entry : _redo)
    {
      bytes += entryBytes(entry);
    }

    return bytes;
  }

  std::size_t TerrainMaskHistory::budgetBytes() const
  {
    return _budget_bytes;
  }

  void TerrainMaskHistory::setBudgetBytes(std::size_t bytes)
  {
    _budget_bytes = bytes;
    enforceBudget();
  }

  void TerrainMaskHistory::enforceBudget()
  {
    std::size_t held = bytes();

    // The newest entry is never dropped even when it alone exceeds the budget: a single stroke
    // that swept sixteen tiles is 32 MiB, and refusing to hold it would mean the one operation a
    // user most wants back is the one operation that is never undoable.
    while (held > _budget_bytes && _undo.size() > 1)
    {
      held -= entryBytes(_undo.front());
      _undo.pop_front();
    }
  }
}
