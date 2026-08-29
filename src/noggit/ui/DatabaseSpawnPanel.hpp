// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_UI_DATABASESPAWNPANEL_HPP
#define NOGGIT_UI_DATABASESPAWNPANEL_HPP

#include <QtWidgets/QWidget>

#include <cstddef>
#include <cstdint>

class MapView;

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QListWidget;
class QPushButton;
class QSpinBox;
class QTimer;

namespace Noggit::Database
{
  // Forward-declared rather than including SpawnSceneCache.hpp, which drags in ModelInstance and
  // with it most of the scene graph. Declaring functions that return it by value is legal with
  // the type incomplete; the one caller outside this pair (MapView.cpp) already includes the
  // full header.
  struct SpawnRef;
}

namespace Noggit::Ui
{
  class SpawnTilePicker;

  // The editing surface for TrinityCore world-database spawns.
  //
  // Everything it drives already existed and was proven before this panel was written --
  // SpawnSceneCache::moveTo, dirtyEntries and MapView::saveDatabaseChanges were exercised over
  // the dev bridge and verified against the database, coordinates read back exactly. This is the
  // buttons on top of that, not new machinery.
  //
  // > [!important] "Save to database" writes a file, and that is deliberate
  // > HARD RULE 2 requires every edit to become a reviewable DELETE-then-INSERT changeset before
  // > it touches anything, and HARD RULE 1 permits writes only against the configured dev schema.
  // > So the button emits .sql, and the apply checkbox runs it against the dev schema and nowhere
  // > else -- WorldDatabaseConnection refuses to construct write-capable against any other, so a
  // > live world database cannot be written to by this path even if the settings point at one.
  class DatabaseSpawnPanel : public QWidget
  {
    Q_OBJECT

    public:
      explicit DatabaseSpawnPanel(MapView* map_view, QWidget* parent = nullptr);

      // True while the user is placing spawns, so MapView knows a click means "move the selection
      // here" rather than whatever the active tool would normally do.
      bool moveMode() const;

      // True while a click should CREATE a spawn rather than move one.
      //
      // Kept mutually exclusive with moveMode in the panel rather than resolved by whichever
      // branch MapView happens to test first: two armed modes would make one click do something
      // that depends on the order of two `if` statements in a file the user cannot see.
      bool placeMode() const;

      // What placeMode would create: the kind, and the creature_template / gameobject_template
      // entry id.
      bool placeCreature() const;
      std::uint32_t placeEntry() const;

      // The highlighted spawn. Its guid is 0 when nothing is selected.
      //
      // Kind and guid, not a bare guid: creature.guid and gameobject.guid are separate primary
      // keys with overlapping ranges, so a number alone names up to two different rows. See
      // Noggit::Database::SpawnRef.
      Noggit::Database::SpawnRef selectedSpawn() const;

      // Called by MapView after a click has moved something, and after a load, so the list and the
      // pending count follow the world rather than drifting from it.
      void refresh();

      // Highlight this spawn in the list, for a viewport click. Does not re-enter the
      // viewport selection, so a click cannot bounce between the two.
      void selectSpawn(Noggit::Database::SpawnRef const& spawn);

    private:
      // How many tiles may be loaded in one go.
      //
      // The bound exists because the cost of a load is not the query, it is that every resolved
      // spawn builds a ModelInstance and queues an asynchronous M2 load, and the whole loop runs
      // synchronously on the GUI thread. Fifty populated tiles is tens of thousands of spawns and
      // presents as a hang followed by a very large resident set; see the tile status label, which
      // shows the count against this limit at all times rather than only complaining at the end.
      //
      // 64 -- an 8x8 block -- rather than something smaller because the spawn count, not the tile
      // count, is what actually hurts, and that is guarded separately by the pre-flight estimate.
      static constexpr std::size_t MAX_SELECTED_TILES = 64;

      // How often the drawable/not-drawable markers are recomputed while the panel is visible.
      //
      // They depend on which ADTs are streamed in, and that changes as the camera flies without
      // anything telling this panel. A marker that says "on screen" about a tile the world unloaded
      // two seconds ago is the same lie the three-state overlay exists to stop telling, so it is
      // refreshed on a timer rather than only when the user presses something. The work is 4096
      // cache probes plus 4096 array lookups -- it is off the render thread and does not rebuild
      // the list widget.
      static constexpr int LOAD_STATE_POLL_MS = 1500;

      void onLoad(bool all_tiles);
      void onSave();
      void onDiscard();

      // Places a spawn at the camera's ground position, for a user who would rather press a
      // button than aim a click. Goes through exactly the same MapView entry point as place mode.
      void onPlaceAtCamera();

      // Marks the selected spawn for deletion. Asks first when the spawn came from the database,
      // because that one emits a DELETE somebody will apply; a spawn the user placed a moment ago
      // does not, and a confirmation for it would be noise.
      void onDelete();

      // Keeps move mode and place mode from being armed at once, and keeps the Delete button in
      // step with the selection.
      void updateSpawnActions();

      // Loads database spawns for exactly the tiles picked on the grid.
      void onLoadSelectedTiles();

      // Counts spawns per tile for the whole map in two queries, for the picker's overlay.
      void onScanMapForSpawns();

      // Keeps the tile status label and the load button in step with the grid.
      void updateTileStatus();

      // Repaints the picker's "currently loaded" markers from the scene cache, splitting them into
      // drawable and not-drawable, and caches the two counts for the status line.
      void updateLoadedTileOverlay();

      // How many of the picked tiles the world has NOT streamed in, i.e. how many would load into
      // the cache and draw nothing. Zero when there is no world.
      std::size_t pickedTilesNotStreamed() const;

      // Applies the spin box to the selection, but only when it actually differs from what the
      // spawn already stores.
      //
      // QSpinBox::editingFinished fires on focus-out as well as on an edit, so without the
      // comparison merely tabbing through the box wrote a quantised whole-degree orientation over
      // exact stored radians and marked an untouched spawn dirty -- it then appeared in the
      // changeset with a facing nobody had changed.
      void applyOrientationIfChanged();

      // Facing of a loaded spawn in whole degrees, the way the spin box shows it. False when that
      // spawn is not loaded.
      bool spawnFacingDegrees(Noggit::Database::SpawnRef const& spawn, int& degrees) const;

      // Moves every loaded spawn standing on the picked tiles the same way the terrain under them
      // was moved, through Noggit::Database::moveSpawnsWithChunks.
      //
      // > [!important] This is the manual half of a feature whose automatic half needs a hook
      // > this panel cannot make
      // > The Chunk Manipulator can already copy, paste, rotate and mirror terrain, and
      // > ChunkTransform can already move the spawns that stand on it -- but the two have never
      // > been connected, because ChunkClipboard::pasted carries a ChunkPasteReport of four
      // > counters (chunks, objects added, objects removed, chunks skipped) and no geometry
      // > whatever. There is no way, from outside that class, to learn which chunks a paste came
      // > from, where they landed, or what grid ops were applied on the way: _cached_chunks,
      // > _clipboard_pivot and _clipboard_pivot_chunk are all private and no accessor exposes
      // > them. Guessing at it from the cursor position and signal ordering would move spawns to
      // > a place nobody chose, which is worse than not moving them.
      // >
      // > So the same transform is driven from the tile grid this panel already owns, and the
      // > module underneath takes the move in chunk-grid terms so the paste hook drops straight
      // > in once ChunkPasteReport carries the geometry. See the report accompanying this change.
      void onMoveSpawnsWithTerrain();

      // Points the camera at the selected spawn from a short distance.
      //
      // The outline alone is not enough when the spawn is off screen or behind terrain, which is
      // most of the time on a populated tile -- you cannot look for a highlight you cannot see.
      void onFocus();

      void onSelectionChanged();

      MapView* _map_view;

      // The tile grid. Owns its own selection buffer; see SpawnTilePicker on why it is a second
      // widget rather than MapView's minimap.
      SpawnTilePicker* _tile_picker;
      QLabel* _tile_status;
      QPushButton* _load_selected;
      QPushButton* _scan_button;

      // Drives updateLoadedTileOverlay while the panel is visible; see LOAD_STATE_POLL_MS.
      QTimer* _load_state_poll;

      // Tiles in the spawn cache as of the last updateLoadedTileOverlay, split by whether the
      // renderer can reach them.
      //
      // Cached rather than recomputed in updateTileStatus: both numbers fall out of the walk the
      // overlay already does, and updateTileStatus runs on every mouse move of a drag-paint.
      std::size_t _loaded_tiles_drawn = 0;
      std::size_t _loaded_tiles_not_drawn = 0;

      QListWidget* _spawn_list;

      // --- creating -------------------------------------------------------------------------
      QComboBox* _place_kind;
      QSpinBox* _place_entry;
      QCheckBox* _place_mode;
      QPushButton* _delete_button;

      // --- following a terrain move -----------------------------------------------------------
      //
      // Offsets in whole ADT tiles rather than in chunks, because that is the granularity the tile
      // picker above works at and offering a chunk offset the picker cannot express would be a
      // control with nothing to point at. The module underneath is chunk-granular; only this UI
      // rounds to tiles.
      QSpinBox* _chunk_move_dx;
      QSpinBox* _chunk_move_dz;
      QComboBox* _chunk_move_turn;
      QDoubleSpinBox* _chunk_move_height;
      QPushButton* _chunk_move_button;
      QLabel* _chunk_move_status;

      QCheckBox* _move_mode;
      QSpinBox* _orientation;
      QCheckBox* _apply_to_dev;
      QLabel* _pending;
      QPushButton* _save_button;
      QPushButton* _discard_button;
  };
}

#endif
