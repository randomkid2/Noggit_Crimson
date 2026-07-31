// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_UI_SPAWNTILEPICKER_HPP
#define NOGGIT_UI_SPAWNTILEPICKER_HPP

#include <noggit/TileIndex.hpp>
#include <noggit/ui/minimap_widget.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace Noggit::Ui
{
  // The tile grid the spawn panel loads from: pick several ADTs, then load the database spawns
  // for exactly those.
  //
  // A subclass of minimap_widget rather than a second grid, because minimap_widget already does
  // all of the hard part -- it paints the map's own minimap image, knows which tiles exist, which
  // are loaded and which have unsaved ADT changes, and already implements plain-click select,
  // shift 3x3 brush, ctrl deselect, drag-paint and right-click clear against a caller-owned
  // buffer. None of that needed rewriting. What it does NOT do is own its selection buffer, emit
  // anything when the selection changes, or know anything about database spawns; those three gaps
  // are the whole of this class.
  //
  // > [!important] This instance is deliberately NOT MapView's minimap
  // > MapView::set_editing_mode calls `_minimap->use_selection(nullptr)` unconditionally on every
  // > mode change (MapView.cpp:306), so a selection bound to the shared minimap silently detaches
  // > the first time the user switches tool. Owning a second widget, bound to a buffer this object
  // > owns, is immune to that -- and it also means the spawn selection cannot be confused with
  // > MinimapTool's render selection, which is painted in the same red by the base class.
  //
  // ---
  //
  // COORDINATE FRAME. Everything in this class is in ADT-filename order, i.e. the global
  // `::TileIndex` (x, z) frame, because that is the frame minimap_widget paints and indexes in:
  // its buffer is `64 * adt_x + adt_z` (minimap_widget.cpp:49, :60, :259) and `QPoint::y()` from
  // locateTile is the ADT **z**, not a server y. Nothing here is in Noggit::Database::TileIndex
  // (x, y) server order. Callers converting to the server frame must go through
  // Noggit::Database::fromAdtFileIndex; assigning field by field compiles, stays in range, and
  // reads a tile about 9.6 km away.
  class SpawnTilePicker : public minimap_widget
  {
    Q_OBJECT

    public:
      // Number of `char`s minimap_widget indexes, and therefore the exact size the selection
      // buffer must have: paintEvent calls `_selected_tiles->at(64 * i + j)` for every i and j
      // below 64, so anything shorter throws from inside a paint event.
      static constexpr std::size_t TILE_BUFFER_SIZE = 64 * 64;

      // What the overlay is allowed to claim about a tile.
      //
      // > [!important] "Loaded" and "on screen" are two different facts and the grid must not
      // > merge them
      // > WorldRender only ever asks the spawn cache for tiles it is ALREADY walking: the tile
      // > loop is built from `mapIndex.loaded_tiles()` (WorldRender.cpp:122) and the spawn block
      // > calls `db_spawns->tile(tile->index)` from inside it (WorldRender.cpp:658-660). Noggit
      // > streams roughly a 5x5 block around the camera, so of the 64 tiles this picker will
      // > happily load at most about 25 can ever be drawn. A single "loaded" marker therefore
      // > painted 64 green cells for a viewport that showed nothing, which is the one thing an
      // > overlay must never do. CACHED_ONLY is that case, drawn in its own colour.
      enum class TileLoadState : char
      {
        // Nothing in the spawn cache for this tile.
        NONE = 0,

        // Spawns are in the cache -- they are in the list, they can be moved, they will be in the
        // changeset -- but the ADT is not streamed in, so the renderer never reaches them.
        CACHED_ONLY = 1,

        // In the cache and the ADT is streamed, so the spawns are actually rendered.
        DRAWN = 2
      };

      explicit SpawnTilePicker(QWidget* parent = nullptr);

      // The picked tiles, in ADT-filename (x, z) order. See the frame note above.
      std::vector<::TileIndex> selectedAdtTiles() const;

      std::size_t selectedCount() const;

      void clearSelection();

      // Picks every tile the world currently has streamed in, so "the tiles I can see" is one
      // click rather than a drag across the grid.
      void selectLoadedTiles();

      // --- what the overlay knows -------------------------------------------------------------
      // Both of these are pushed in by the panel and then only read while painting. Nothing here
      // queries a database or walks the spawn cache from paintEvent: at 64x64 tiles a per-tile
      // lookup inside a repaint would run 4096 times per frame, and a per-tile *query* would run
      // 8192 statements per frame.

      // Spawns per tile from the panel's map scan, indexed `64 * adt_x + adt_z`. Empty means the
      // map has not been scanned, which is drawn as "unknown", not as "empty".
      void setSpawnCounts(std::vector<std::uint32_t> counts);

      bool hasSpawnCounts() const { return !_spawn_counts.empty(); }

      // Total spawns across the current selection, or 0 when the map has not been scanned. Used
      // for the pre-flight estimate, so the panel can say how big a load is about to be without
      // issuing two COUNT queries per selected tile.
      std::uint64_t scannedSpawnsInSelection() const;

      // What the spawn cache holds for each tile right now, indexed `64 * adt_x + adt_z`.
      //
      // Pushed in rather than derived here: deciding DRAWN from CACHED_ONLY needs the world's
      // streaming state, and the panel already walks the cache to build this. See TileLoadState.
      void setLoadedTiles(std::vector<TileLoadState> states);

    signals:
      // minimap_widget has no such signal: it mutates the caller's buffer from a lambda and calls
      // update() on itself, leaving a consumer to connect to tile_clicked and reset_selection and
      // diff. This is that connection, made once, here.
      void selectionChanged();

    protected:
      void paintEvent(QPaintEvent* paint_event) override;

    private:
      // Owned here, and bound to the base with use_selection() in the constructor. minimap_widget
      // never allocates it -- it holds a bare pointer and dereferences it unconditionally once
      // bound.
      //
      // std::vector<char>, not std::vector<bool>: the base memsets it on reset_selection
      // (minimap_widget.cpp:75), which a bit-packed vector would not survive.
      std::vector<char> _selection;

      std::vector<std::uint32_t> _spawn_counts;

      // Not std::vector<char> like _selection: this one is never touched by the base class, so it
      // can carry the three-state type instead of a number the reader has to decode.
      std::vector<TileLoadState> _loaded;
  };
}

#endif
