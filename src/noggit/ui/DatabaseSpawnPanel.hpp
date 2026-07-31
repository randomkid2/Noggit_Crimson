// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_UI_DATABASESPAWNPANEL_HPP
#define NOGGIT_UI_DATABASESPAWNPANEL_HPP

#include <QtWidgets/QWidget>

#include <cstddef>
#include <cstdint>

class MapView;

class QCheckBox;
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
      QCheckBox* _move_mode;
      QSpinBox* _orientation;
      QCheckBox* _apply_to_dev;
      QLabel* _pending;
      QPushButton* _save_button;
      QPushButton* _discard_button;
  };
}

#endif
