// This file is part of Noggit3, licensed under GNU General Public License (version 3).

// The dock that lists every placement in this session whose model or WMO could not be loaded.
//
// WHAT IT REPLACES. Until this panel existed the entire user-facing account of a missing model
// was a modal at save time (MapView::save) that said "check the log file for the list of model
// errors". It named nothing, listed nothing, and appeared only when the mapper pressed Save --
// by which point the damage is done, because saving an ADT whose models failed to load writes
// wrong culling and collision data for them. There was no way to see, count, locate, or reach a
// broken placement from inside the editor.
//
// THREADING, AND WHY THIS POLLS. The failures are recorded on AsyncLoader's worker threads
// (AsyncObject::error_on_loading). A queued signal from those threads into this widget was the
// obvious design and was rejected: AsyncLoader is a process-global set up at application entry
// and its threads outlive MapView, which destroys its docks, so a worker holding any pointer
// into this widget has a lifetime this class cannot guarantee -- and a stale
// QMetaObject::invokeMethod target from a loader thread is exactly the kind of intermittent
// crash that is impossible to attribute afterwards. Instead MissingPlacementLog is STL-only and
// Qt-free, the worker thread touches nothing but a mutex, and this panel polls a single relaxed
// atomic (MissingPlacementLog::generation) on a timer. The upper bound on staleness is one poll
// interval; the lower bound on crash risk is zero.
//
// The placement walk itself -- which UID, which tile, which coordinates -- also runs here, on
// the GUI thread, and not at the point of failure. It has to: AsyncObject knows a file key and
// nothing else, and the placements that reference a file live in MapTile::getObjectInstances and
// in WMOInstance's doodad lists, neither of which a loader thread may safely walk.

#ifndef NOGGIT_UI_TOOLS_MISSINGOBJECTS_MISSINGOBJECTSPANEL_HPP
#define NOGGIT_UI_TOOLS_MISSINGOBJECTS_MISSINGOBJECTSPANEL_HPP

#include <noggit/reports/MissingPlacementLog.hpp>

#include <QtWidgets/QWidget>

#include <cstdint>
#include <string>
#include <vector>

class MapView;

class QLabel;
class QPushButton;
class QTableWidget;
class QTimer;

namespace Noggit::Ui::Tools::MissingObjects
{
  class MissingObjectsPanel : public QWidget
  {
    Q_OBJECT

    public:
      explicit MissingObjectsPanel(MapView* map_view, QWidget* parent = nullptr);

      // Re-walks the loaded tiles, probes the client data for any file whose state is still
      // unknown, and rebuilds the table. Safe to call at any time; the log accumulates, so a
      // tile that has since been streamed out keeps the rows it contributed.
      void refresh();

    protected:
      // The poll runs only while the dock is on screen. A hidden panel that keeps walking every
      // loaded tile every 1.5 seconds is a cost the mapper cannot see and did not ask for.
      void showEvent(QShowEvent* event) override;
      void hideEvent(QHideEvent* event) override;

    private:
      // One table row. Kept beside the table rather than encoded into the cells because the user
      // can re-sort by any column and the row index then stops meaning anything -- the index into
      // this vector is stored in the row's first item under Qt::UserRole and moves with it.
      struct Row
      {
        std::string file_path;
        std::string owner_path;
        MissingPlacementKind kind = MissingPlacementKind::Model;
        MissingAssetState state = MissingAssetState::Unknown;
        std::uint32_t owner_uid = 0;
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        MissingPlacementTile tile;

        // False for a file that failed to load but whose placements have not been located --
        // its tile was never streamed in, or it is referenced only from a WMO that is not
        // loaded. The row still appears, because "this file is broken" is worth knowing even
        // when "and it is used here" cannot yet be answered, but Go To has nothing to aim at.
        bool has_position = true;
      };

      // How often the log's generation counter is checked while the panel is visible.
      //
      // Models stream in asynchronously and failures therefore arrive seconds after the tile
      // does. A panel that refreshed only on a button press would show a mapper an empty table
      // for a map that is visibly full of holes, which is the same lie the save-time modal
      // tells. 1500 ms matches DatabaseSpawnPanel::LOAD_STATE_POLL_MS; the poll itself is one
      // relaxed atomic load and does nothing further unless the counter moved.
      static constexpr int POLL_MS = 1500;

      void buildUi();
      void rescanWorld();
      void probeStates();
      void rebuildTable();
      void updateHeader();
      void goToSelectedRow();
      void stepSelection(int delta);
      void onPoll();

      [[nodiscard]]
      int selectedRowIndex() const;

      MapView* _map_view = nullptr;

      QLabel* _header = nullptr;
      QLabel* _empty_notice = nullptr;
      QTableWidget* _table = nullptr;
      QPushButton* _previous_button = nullptr;
      QPushButton* _go_to_button = nullptr;
      QPushButton* _next_button = nullptr;
      QPushButton* _refresh_button = nullptr;
      QTimer* _poll_timer = nullptr;

      std::vector<Row> _rows;

      // Last generation the table was built from. Starts at a value the log can never report, so
      // the first poll always rebuilds.
      std::uint64_t _built_generation = static_cast<std::uint64_t>(-1);
  };
}

#endif // NOGGIT_UI_TOOLS_MISSINGOBJECTS_MISSINGOBJECTSPANEL_HPP
