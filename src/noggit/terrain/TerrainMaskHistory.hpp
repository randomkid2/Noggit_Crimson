// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_TERRAINMASKHISTORY_HPP
#define NOGGIT_TERRAINMASKHISTORY_HPP

#include <noggit/terrain/TerrainMask.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <set>
#include <string>
#include <vector>

// UNDO FOR MASK STROKES, AND WHY IT IS NOT THE EDITOR'S UNDO STACK.
//
// This was the one design question the mask brush could not dodge, so the argument is recorded
// here rather than in a commit message.
//
// THE EDITOR'S UNDO STACK CANNOT CARRY A MASK, and not because it is full. Noggit::ActionFlags
// (Action.hpp:24-43) has fifteen enumerators and every single one of them names either a field of
// MCNK -- terrain, area id, holes, vertex colour, water, texture, chunk flags, shadows, doodad
// exclusion, layer info -- or a scene object. That is not a coincidence of naming: every registrar
// on Action takes a MapChunk* and keys the before-image by it (Action::registerChunkTerrainChange,
// Action.cpp:689), and Action::undo restores through that pointer and then repairs the chunk's GPU
// state -- World::recalc_norms and MapChunk::registerChunkUpdate at Action.cpp:64-72. A mask has no
// MapChunk. There is nothing to register and nothing for undo to repair.
//
// Forcing one in would cost more than it bought, because the only way to register a mask stroke is
// to attach it to some chunk it did not edit -- and an edited chunk's tile is an unsaved tile.
// MapIndex::unloadTiles refuses to release a tile whose `changed` flag is set, so a session of mask
// painting would (a) pin every visited tile in memory for as long as the map stayed open and (b)
// offer to save ADTs in which not one byte had changed. A mask writes nothing into any client file;
// making its undo step pretend otherwise would give away the exact property that lets the feature
// exist at all.
//
// THE TWO HISTORIES ALSO HAVE DIFFERENT LIFETIMES. A mask is a project sidecar under
// <project>/noggit_masks/ (TerrainMaskStore::save) and survives closing the map, closing Noggit
// and opening a different map in the same project. The Action stack is per-session and is purged
// when the map view goes away. One stack cannot have two lifetimes.
//
// AND INTERLEAVING WOULD BE THE WORSE FAILURE EVEN IF THE ABOVE WERE FREE. Sharing a stack means
// a user who paints a mask, sculpts a hill and then presses Ctrl+Z three times undoes the hill
// and then silently moves the region the NEXT brush is allowed to touch -- with nothing on screen
// to say so, because a mask has no appearance in the viewport of its own. "The shape of the
// ground" and "where the next edit is allowed to apply" are not one history to the person doing
// them, and a single Ctrl+Z that walks between them is a worse tool than two explicit ones.
//
// SO: a private, bounded, mask-local history, reached from the mask tool's own panel and its own
// hotkeys, and never from Ctrl+Z. Ctrl+Z keeps meaning exactly what it meant before this file
// existed, which is the other half of "do not change what any existing tool does".
//
// THE STORAGE, computed rather than guessed. A mask chunk is MASK_CHUNK_TEXELS = 4096 bytes, and
// a stroke has to snapshot BOTH fields TerrainMaskPainter always writes together -- the paint
// layer, which persists, and the composited field, which is what the brushes read -- so one
// touched chunk costs 2 x 4096 = 8192 bytes.
//
// Undo and redo SWAP rather than storing a before and an after: an entry holds one image set,
// and restoring it exchanges it with what is live. That halves the cost of a stroke and removes
// the second pass over the touched chunks that capturing an after-image would need.
//
//   one touched chunk                      8'192 B
//   a drag sweeping a whole tile   256 chunks   =  2'097'152 B  =  2.000 MiB
//   the 64 MiB default budget                     8192 chunk images  =  32 full-tile strokes
//
// The budget is enforced oldest-first, which is the only defensible direction: the stroke the
// user is most likely to want back is the one they just made.
namespace Noggit
{
  struct NamedTerrainMask;
  class TerrainMaskStore;

  class TerrainMaskHistory
  {
    public:
      static TerrainMaskHistory* instance();

      // --- Recording ---

      // Opens a stroke against one named mask. Calling it again for the same name while a stroke
      // is open does nothing, which is what lets the tool call it on every tick of a held button
      // the way the terrain tools call ActionManager::beginAction.
      //
      // Opening against a DIFFERENT name closes the stroke in flight first. That can only happen
      // if the active mask changed mid-drag, which the panel does not allow, but a history that
      // silently mixed two masks into one entry would be unrecoverable rather than merely wrong.
      void beginStroke(std::string const& mask_name);

      bool strokeOpen() const;

      // Snapshots every chunk the circle at (world_x, world_z, radius) can touch, once per stroke.
      // CALL BEFORE PAINTING.
      //
      // The chunks are those of the circle's BOUNDING SQUARE, which is a superset -- at most a
      // ring of chunks the stroke did not actually reach. Those are restored to the values they
      // already hold, so the only cost is their 8 KiB each. Reproducing paintCircle's exact texel
      // walk to shave it would mean two copies of that walk which could disagree.
      //
      // Returns false when nothing could be recorded: no stroke open, an invalid radius, or an
      // allocation that failed. A false does NOT mean the caller should stop painting -- it means
      // this stroke is not undoable, which is a much smaller loss than refusing the edit. See the
      // note on the definition for why bad_alloc is swallowed here specifically.
      bool capture(NamedTerrainMask const& mask, float world_x, float world_z, float radius);

      // Closes the open stroke and pushes it. A stroke that captured nothing is discarded rather
      // than pushed as an empty entry, so tapping the button with no movement does not fill the
      // history with no-ops.
      void endStroke();

      // --- Replay ---

      bool canUndo() const;
      bool canRedo() const;

      // Exchanges the top entry's images with what is currently in the mask. Returns false when
      // the stack is empty or the mask the entry names no longer exists -- a mask that was
      // deleted or renamed takes its history with it, and the entry is dropped rather than
      // resurrecting a mask under its old name.
      bool undo(TerrainMaskStore& store);
      bool redo(TerrainMaskStore& store);

      // Drops everything. CALL THIS WHENEVER THE SET OF MASKS IS REPLACED WHOLESALE -- a project
      // load does exactly that (TerrainMaskStore::load replaces _masks), and an entry naming a
      // mask from the previous project would restore bytes into a mask that merely shares a name.
      void clear();

      std::size_t undoDepth() const;
      std::size_t redoDepth() const;

      // Payload bytes across both stacks and the stroke in flight. Texels only; the per-entry
      // bookkeeping is a few dozen bytes and is not counted, the same convention
      // TerrainMask::bytes uses for its own entry overhead.
      std::size_t bytes() const;

      std::size_t budgetBytes() const;
      void setBudgetBytes(std::size_t bytes);

    private:
      TerrainMaskHistory() = default;

      // One chunk of one mask, both fields. Held as vectors rather than fixed arrays because an
      // entry is moved between the two stacks on every undo, and a 8 KiB member array would be
      // copied by that move.
      struct ChunkImage
      {
        std::uint32_t packed = 0;
        std::vector<std::uint8_t> paint;
        std::vector<std::uint8_t> composited;
      };

      struct Entry
      {
        std::string mask_name;
        std::vector<ChunkImage> images;
      };

      // Exchanges every image in `entry` with the mask's current contents. This is both undo and
      // redo; the direction is which stack the entry came from and which it goes to.
      static bool swapEntry(Entry& entry, NamedTerrainMask& mask);

      // Drops oldest undo entries until the payload fits. Never touches the redo stack, which is
      // by construction smaller and is discarded on the next stroke anyway.
      void enforceBudget();

      static std::size_t entryBytes(Entry const& entry);

      std::deque<Entry> _undo;
      std::deque<Entry> _redo;

      Entry _open;
      bool _stroke_open = false;
      // Which chunks the open stroke has already snapshotted, so a drag that crosses the same
      // chunk fifty times stores it once.
      std::set<std::uint32_t> _open_captured;

      std::size_t _budget_bytes = 64u * 1024u * 1024u;
  };
}

#endif // NOGGIT_TERRAINMASKHISTORY_HPP
