// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/ui/tools/MissingObjects/MissingObjectsPanel.hpp>

#include <noggit/application/NoggitApplication.hpp>
#include <noggit/AssetScan.hpp>
#include <noggit/AsyncObject.h>
#include <noggit/map_index.hpp>
#include <noggit/MapTile.h>
#include <noggit/MapView.h>
#include <noggit/Model.h>
#include <noggit/ModelInstance.h>
#include <noggit/SceneObject.hpp>
#include <noggit/TileIndex.hpp>
#include <noggit/ui/DesignTokens.hpp>
#include <noggit/WMO.h>
#include <noggit/WMOInstance.h>
#include <noggit/World.h>

#include <QtCore/QStringList>
#include <QtCore/QTimer>
#include <QtCore/QVariant>
#include <QtGui/QColor>
#include <QtGui/QHideEvent>
#include <QtGui/QShowEvent>
#include <QtWidgets/QAbstractItemView>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QTableWidget>
#include <QtWidgets/QTableWidgetItem>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <map>
#include <tuple>

namespace
{
  enum Column
  {
    COLUMN_TYPE = 0,
    COLUMN_STATE,
    COLUMN_FILE,
    COLUMN_UID,
    COLUMN_X,
    COLUMN_Y,
    COLUMN_Z,
    COLUMN_ADT_X,
    COLUMN_ADT_Z,
    COLUMN_COUNT
  };

  // QTableWidgetItem::operator< compares Qt::DisplayRole, which for these cells is a formatted
  // string -- so "10" sorts before "9" and a coordinate of -3.50 sorts beside "-30.00". The
  // value is therefore carried separately and compared numerically, while the display keeps the
  // formatting the mapper needs to read.
  constexpr int SORT_VALUE_ROLE = Qt::UserRole + 1;

  class NumericItem : public QTableWidgetItem
  {
    public:
      NumericItem(QString const& text, double sort_value)
        : QTableWidgetItem(text)
      {
        setData(SORT_VALUE_ROLE, sort_value);
      }

      bool operator< (QTableWidgetItem const& other) const override
      {
        QVariant const mine (data(SORT_VALUE_ROLE));
        QVariant const theirs (other.data(SORT_VALUE_ROLE));

        if (mine.isValid() && theirs.isValid())
        {
          return mine.toDouble() < theirs.toDouble();
        }

        return QTableWidgetItem::operator< (other);
      }
  };

  QColor stateColor(Noggit::MissingAssetState state)
  {
    switch (state)
    {
      // BAD #E86F62 measures 4.938:1 on BG_PANEL #292621 and WARN #E2803C measures 5.301:1,
      // both computed here as WCAG 2.1 sRGB relative luminance, (Lmax + 0.05) / (Lmin + 0.05).
      // Both clear the 4.5:1 body-text floor on the surface a dock body actually is.
      case Noggit::MissingAssetState::Missing:
        return Noggit::Ui::Design::color(Noggit::Ui::Design::BAD);
      case Noggit::MissingAssetState::Unreadable:
        return Noggit::Ui::Design::color(Noggit::Ui::Design::WARN);

      // TEXT_DIM #BFB7AA, 7.585:1 on BG_PANEL. "Not probed" is a real state, not a disabled
      // one, so it does not take TEXT_OFF -- that token is deliberately below the floor and
      // means "you cannot act on this".
      case Noggit::MissingAssetState::Unknown:
      default:
        return Noggit::Ui::Design::color(Noggit::Ui::Design::TEXT_DIM);
    }
  }
}

namespace Noggit::Ui::Tools::MissingObjects
{
  MissingObjectsPanel::MissingObjectsPanel(MapView* map_view, QWidget* parent)
    : QWidget(parent)
    , _map_view(map_view)
  {
    buildUi();
    refresh();
  }

  void MissingObjectsPanel::buildUi()
  {
    auto* layout (new QVBoxLayout(this));

    _header = new QLabel(this);
    _header->setWordWrap(true);
    layout->addWidget(_header);

    _empty_notice = new QLabel(this);
    _empty_notice->setWordWrap(true);

    // Top-aligned and given the stretch the table would have had. A hidden widget contributes
    // nothing to a QBoxLayout, stretch factor included, so without this the button row floats to
    // the middle of the dock whenever the table is hidden.
    _empty_notice->setAlignment(Qt::AlignTop);
    _empty_notice->setText
      ( "Nothing is missing.\n\n"
        "Every model and world model referenced by the tiles loaded so far was found and read.\n"
        "Rows appear here as tiles stream in, so fly over the area you want checked and this\n"
        "list follows. It never forgets a failure, so a tile that has since unloaded keeps its\n"
        "rows."
      );
    layout->addWidget(_empty_notice, 1);

    _table = new QTableWidget(this);
    _table->setColumnCount(COLUMN_COUNT);
    _table->setHorizontalHeaderLabels
      ( QStringList {"Type", "State", "Missing file", "Owner UID", "X", "Y", "Z", "ADT X", "ADT Z"} );
    _table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    _table->setSelectionBehavior(QAbstractItemView::SelectRows);
    _table->setSelectionMode(QAbstractItemView::SingleSelection);
    _table->setAlternatingRowColors(true);
    _table->verticalHeader()->setVisible(false);

    // Elide from the LEFT. An asset path is identified by its tail -- the mapper needs
    // "creature/murloc/murloc.m2", not "world/azeroth/elwynn/passivedoo..." -- and Qt's default
    // is ElideRight, which throws exactly the useful half away. This is a view-wide setting
    // rather than a per-column one, which is harmless: every other column is a short number or a
    // fixed word and never overflows.
    _table->setTextElideMode(Qt::ElideLeft);

    _table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    _table->horizontalHeader()->setSectionResizeMode(COLUMN_FILE, QHeaderView::Stretch);

    layout->addWidget(_table, 1);

    auto* buttons (new QHBoxLayout());

    _previous_button = new QPushButton("Previous", this);
    _go_to_button = new QPushButton("Go To", this);
    _next_button = new QPushButton("Next", this);
    _refresh_button = new QPushButton("Refresh", this);

    _previous_button->setStatusTip("Select the row above and move the camera to it.");
    _go_to_button->setStatusTip("Move the camera to the selected placement.");
    _next_button->setStatusTip("Select the row below and move the camera to it.");
    _refresh_button->setStatusTip
      ("Walk the loaded tiles again and re-probe the client data for every listed file.");

    buttons->addWidget(_previous_button);
    buttons->addWidget(_go_to_button);
    buttons->addWidget(_next_button);
    buttons->addStretch(1);
    buttons->addWidget(_refresh_button);

    layout->addLayout(buttons);

    connect(_table, &QTableWidget::itemDoubleClicked, this
           , [this] (QTableWidgetItem*) { goToSelectedRow(); });
    connect(_go_to_button, &QPushButton::clicked, this, [this] { goToSelectedRow(); });
    connect(_previous_button, &QPushButton::clicked, this, [this] { stepSelection(-1); });
    connect(_next_button, &QPushButton::clicked, this, [this] { stepSelection(1); });
    connect(_refresh_button, &QPushButton::clicked, this, [this] { refresh(); });

    _poll_timer = new QTimer(this);
    _poll_timer->setInterval(POLL_MS);
    connect(_poll_timer, &QTimer::timeout, this, [this] { onPoll(); });
  }

  void MissingObjectsPanel::showEvent(QShowEvent* event)
  {
    QWidget::showEvent(event);

    refresh();
    _poll_timer->start();
  }

  void MissingObjectsPanel::hideEvent(QHideEvent* event)
  {
    _poll_timer->stop();

    QWidget::hideEvent(event);
  }

  void MissingObjectsPanel::onPoll()
  {
    // The whole point of the poll: one relaxed atomic load, and nothing else happens unless a
    // loader thread has recorded something since the table was built.
    if (MissingPlacementLog::instance().generation() == _built_generation)
    {
      return;
    }

    refresh();
  }

  void MissingObjectsPanel::refresh()
  {
    rescanWorld();
    probeStates();
    rebuildTable();
    updateHeader();

    _built_generation = MissingPlacementLog::instance().generation();
  }

  void MissingObjectsPanel::rescanWorld()
  {
    if (!_map_view)
    {
      return;
    }

    World* world = _map_view->getWorld();

    if (!world)
    {
      return;
    }

    auto& log = MissingPlacementLog::instance();

    // Nothing has failed yet, so there is nothing for a walk to attach placements to. Worth the
    // early return: this runs on a timer and the healthy case is by far the common one.
    if (log.empty())
    {
      return;
    }

    // MapIndex::loaded_tiles() already filters to non-null and finishedLoading (map_index.cpp),
    // which is why there is no second check for either here.
    for (MapTile* tile : world->mapIndex.loaded_tiles())
    {
      for (auto const& pair : tile->getObjectInstances())
      {
        if (pair.second.empty() || !pair.first->finishedLoading())
        {
          continue;
        }

        bool const load_failed = pair.first->loading_failed();
        SceneObjectTypes const object_type = pair.second[0]->which();

        if (load_failed)
        {
          if (!pair.first->file_key().hasFilepath())
          {
            continue;
          }

          std::string const file_path (pair.first->file_key().filepath());

          for (SceneObject* instance : pair.second)
          {
            MissingPlacementRecord record;
            record.file_path = file_path;
            record.kind = object_type == eWMO ? MissingPlacementKind::WorldModel
                                              : MissingPlacementKind::Model;
            record.owner_uid = instance->uid;
            record.x = instance->pos.x;
            record.y = instance->pos.y;
            record.z = instance->pos.z;

            TileIndex const index (instance->pos);
            record.tile.known = index.is_valid();
            record.tile.x = static_cast<std::uint32_t>(index.x);
            record.tile.z = static_cast<std::uint32_t>(index.z);

            log.recordPlacement(std::move(record));
          }

          continue;
        }

        // The WMO loaded. Its MODD list may still name an M2 that did not, and that case is
        // invisible everywhere else in the program -- the building renders, one of its props is
        // simply absent.
        if (object_type != eWMO)
        {
          continue;
        }

        for (SceneObject* instance : pair.second)
        {
          auto* wmo_instance = static_cast<WMOInstance*>(instance);

          // true: include doodads of a WMO the mapper has hidden. Hiding a model is a viewing
          // choice and must not hide a defect report about it.
          auto* doodads = wmo_instance->get_doodads(true);

          if (!doodads)
          {
            continue;
          }

          std::string const owner_path
            ( wmo_instance->wmo->file_key().hasFilepath()
              ? wmo_instance->wmo->file_key().filepath()
              : std::string()
            );

          for (auto& group : *doodads)
          {
            for (auto& doodad : group.second)
            {
              if (!doodad.model->finishedLoading() || !doodad.model->loading_failed())
              {
                continue;
              }

              if (!doodad.model->file_key().hasFilepath())
              {
                continue;
              }

              MissingPlacementRecord record;
              record.file_path = doodad.model->file_key().filepath();
              record.owner_path = owner_path;
              record.kind = MissingPlacementKind::WorldModelDoodad;

              // The OWNING WMO's uid. A WMO's internal doodads are not MDDF entries and have no
              // uid of their own; the placement a mapper can actually act on is the building.
              record.owner_uid = wmo_instance->uid;

              // world_pos, not pos: a doodad's pos is in the WMO's local space. get_doodads
              // above has already run update_transform_matrix_wmo for anything that needed it,
              // and that function gates on finishedLoading() -- true after a failure -- so
              // world_pos is populated even for the doodads listed here.
              record.x = doodad.world_pos.x;
              record.y = doodad.world_pos.y;
              record.z = doodad.world_pos.z;

              TileIndex const index (doodad.world_pos);
              record.tile.known = index.is_valid();
              record.tile.x = static_cast<std::uint32_t>(index.x);
              record.tile.z = static_cast<std::uint32_t>(index.z);

              log.recordPlacement(std::move(record));
            }
          }
        }
      }
    }
  }

  void MissingObjectsPanel::probeStates()
  {
    auto& log = MissingPlacementLog::instance();

    auto const files = log.files();

    bool any_unknown = false;

    for (MissingFileRecord const& file : files)
    {
      if (file.state == MissingAssetState::Unknown)
      {
        any_unknown = true;
        break;
      }
    }

    if (!any_unknown)
    {
      return;
    }

    auto* client_data = Noggit::Application::NoggitApplication::instance()->clientData();

    if (!client_data)
    {
      return;
    }

    // The same probe the "Report missing assets" action already uses, rather than a second
    // spelling of ClientData::exists -- it carries the try/catch that a broken archive needs.
    auto const probe (Noggit::AssetScanCollector::makeClientDataProbe(client_data));

    for (MissingFileRecord const& file : files)
    {
      if (file.state != MissingAssetState::Unknown)
      {
        continue;
      }

      // The loader already told us it failed. So if the probe FINDS the file, the failure was in
      // reading it -- truncated, wrong version, wrong format -- and if the probe does not, the
      // path is simply wrong or the patch was never applied. Opposite fixes, hence two states.
      log.setFileState
        ( file.file_path
        , probe(file.file_path) ? MissingAssetState::Unreadable : MissingAssetState::Missing
        );
    }
  }

  void MissingObjectsPanel::rebuildTable()
  {
    auto& log = MissingPlacementLog::instance();

    auto const files = log.files();
    auto const placements = log.placements();

    std::map<std::string, MissingAssetState> states;
    std::map<std::string, bool> file_has_placement;

    for (MissingFileRecord const& file : files)
    {
      std::string const key (normaliseAssetPath(file.file_path));
      states[key] = file.state;
      file_has_placement[key] = false;
    }

    _rows.clear();
    _rows.reserve(placements.size() + files.size());

    for (MissingPlacementRecord const& placement : placements)
    {
      std::string const key (normaliseAssetPath(placement.file_path));
      file_has_placement[key] = true;

      Row row;
      row.file_path = placement.file_path;
      row.owner_path = placement.owner_path;
      row.kind = placement.kind;
      row.state = states.count(key) ? states.at(key) : MissingAssetState::Unknown;
      row.owner_uid = placement.owner_uid;
      row.x = placement.x;
      row.y = placement.y;
      row.z = placement.z;
      row.tile = placement.tile;
      row.has_position = true;

      _rows.push_back(std::move(row));
    }

    // A file can fail with no placement located: its tile has not been streamed in, or it is
    // named only from a WMO that is not loaded. Listing it anyway is the honest thing -- the
    // mapper still needs to know the file is broken -- and the row says so by having nothing in
    // its coordinate columns rather than by pretending the placement is at the origin.
    for (MissingFileRecord const& file : files)
    {
      std::string const key (normaliseAssetPath(file.file_path));

      if (file_has_placement[key])
      {
        continue;
      }

      Row row;
      row.file_path = file.file_path;
      row.kind = file.kind == MissingAssetKind::WorldModel ? MissingPlacementKind::WorldModel
                                                           : MissingPlacementKind::Model;
      row.state = file.state;
      row.has_position = false;

      _rows.push_back(std::move(row));
    }

    // Grouped by file first, so every placement of one broken model is adjacent and the mapper
    // can see at a glance whether they are fixing one path or a hundred; then by tile, so the
    // ones they can fix in a single flight are together; then by uid for a stable order.
    std::sort(_rows.begin(), _rows.end()
             , [] (Row const& lhs, Row const& rhs)
               {
                 return std::tie(lhs.file_path, lhs.tile.z, lhs.tile.x, lhs.owner_uid)
                      < std::tie(rhs.file_path, rhs.tile.z, rhs.tile.x, rhs.owner_uid);
               }
             );

    // Sorting is turned off for the fill and back on afterwards. With it left on, every
    // insertion re-sorts the model and the row a cell is written into stops being the row it was
    // created in -- the standard QTableWidget population trap.
    _table->setSortingEnabled(false);
    _table->clearContents();
    _table->setRowCount(static_cast<int>(_rows.size()));

    for (std::size_t i = 0; i < _rows.size(); ++i)
    {
      Row const& row = _rows[i];
      int const table_row = static_cast<int>(i);

      auto* type_item (new QTableWidgetItem(QString(missingPlacementKindLabel(row.kind))));

      // The index into _rows, carried by the row's first cell so it survives the user re-sorting
      // by any column.
      type_item->setData(Qt::UserRole, static_cast<qulonglong>(i));

      if (!row.owner_path.empty())
      {
        type_item->setToolTip
          (QString("Referenced from %1").arg(QString::fromStdString(row.owner_path)));
      }

      _table->setItem(table_row, COLUMN_TYPE, type_item);

      auto* state_item (new QTableWidgetItem(QString(missingAssetStateLabel(row.state))));
      state_item->setForeground(stateColor(row.state));
      _table->setItem(table_row, COLUMN_STATE, state_item);

      auto* file_item (new QTableWidgetItem(QString::fromStdString(row.file_path)));
      file_item->setToolTip(QString::fromStdString(row.file_path));
      _table->setItem(table_row, COLUMN_FILE, file_item);

      _table->setItem(table_row, COLUMN_UID
                     , new NumericItem(row.has_position ? QString::number(row.owner_uid)
                                                        : QString("-")
                                      , static_cast<double>(row.owner_uid)));

      auto coordinate = [&row] (float value) -> QString
        {
          return row.has_position ? QString::number(value, 'f', 2) : QString("-");
        };

      _table->setItem(table_row, COLUMN_X, new NumericItem(coordinate(row.x), row.x));
      _table->setItem(table_row, COLUMN_Y, new NumericItem(coordinate(row.y), row.y));
      _table->setItem(table_row, COLUMN_Z, new NumericItem(coordinate(row.z), row.z));

      QString const adt_x
        (row.has_position && row.tile.known ? QString::number(row.tile.x) : QString("-"));
      QString const adt_z
        (row.has_position && row.tile.known ? QString::number(row.tile.z) : QString("-"));

      _table->setItem(table_row, COLUMN_ADT_X
                     , new NumericItem(adt_x, static_cast<double>(row.tile.x)));
      _table->setItem(table_row, COLUMN_ADT_Z
                     , new NumericItem(adt_z, static_cast<double>(row.tile.z)));
    }

    _table->setSortingEnabled(true);

    bool const has_rows = !_rows.empty();

    _table->setVisible(has_rows);
    _empty_notice->setVisible(!has_rows);

    _previous_button->setEnabled(has_rows);
    _go_to_button->setEnabled(has_rows);
    _next_button->setEnabled(has_rows);

    if (has_rows && _table->currentRow() < 0)
    {
      _table->selectRow(0);
    }
  }

  void MissingObjectsPanel::updateHeader()
  {
    auto const& log = MissingPlacementLog::instance();

    std::size_t const placements = log.totalPlacementCount();
    std::size_t const files = log.fileCount();

    if (_rows.empty())
    {
      _header->setText("No missing placements detected this session.");
      return;
    }

    // The count the mapper acts on is the number of PLACEMENTS; the number of FILES is the
    // number of fixes. One missing model with two hundred placements is one path to repoint, and
    // saying only "200 missing placements" makes that look like two hundred jobs.
    QString text
      ( QString("%1 missing placement%2 detected this session. Double-click a row to go to it.")
          .arg(placements).arg(placements == 1 ? "" : "s"));

    text += QString("\n%1 distinct file%2 could not be loaded.")
              .arg(files).arg(files == 1 ? "" : "s");

    if (log.truncated())
    {
      text += QString("\nOnly the first %1 are listed; the rest were counted but not recorded.")
                .arg(log.recordedPlacementCount());
    }

    _header->setText(text);
  }

  int MissingObjectsPanel::selectedRowIndex() const
  {
    int const table_row = _table->currentRow();

    if (table_row < 0)
    {
      return -1;
    }

    QTableWidgetItem* item = _table->item(table_row, COLUMN_TYPE);

    if (!item)
    {
      return -1;
    }

    bool ok = false;
    qulonglong const index = item->data(Qt::UserRole).toULongLong(&ok);

    if (!ok || index >= _rows.size())
    {
      return -1;
    }

    return static_cast<int>(index);
  }

  void MissingObjectsPanel::goToSelectedRow()
  {
    int const index = selectedRowIndex();

    if (index < 0 || !_map_view)
    {
      return;
    }

    Row const& row = _rows[static_cast<std::size_t>(index)];

    if (!row.has_position)
    {
      // Nothing to fly to. Said in the header rather than in a modal, because a dialog raised
      // from a dock the mapper is clicking through repeatedly is worse than the missing answer.
      _header->setText
        ( QString("%1 has not been located in a loaded tile yet -- load the area it is used in "
                  "and press Refresh.").arg(QString::fromStdString(row.file_path)));
      return;
    }

    _map_view->focusOnPoint(glm::vec3(row.x, row.y, row.z));

    // Select the placement as well, not just fly to it. Without this the mapper arrives at the
    // placeholder and still has to click it, and the whole reason selection had to be restored
    // (ModelInstance::intersect, WMOInstance::intersect, World::select_objects_in_area) is that
    // selecting a broken placement is the only way to delete or repoint it. Arriving with it
    // already selected means Delete works immediately.
    //
    // For a WMO doodad row this selects the OWNING WMO, which is the correct handle: the doodad
    // is not an MDDF entry and cannot be selected or removed on its own.
    if (World* world = _map_view->getWorld())
    {
      if (SceneObject* object = world->getObjectInstance(row.owner_uid))
      {
        world->reset_selection();
        world->add_to_selection(object);
      }
    }
  }

  void MissingObjectsPanel::stepSelection(int delta)
  {
    if (_table->rowCount() == 0)
    {
      return;
    }

    int const current = _table->currentRow();
    int next = current + delta;

    // Clamped rather than wrapped. Previous/Next are how a mapper walks a list of defects to the
    // end, and a wrap turns "I have seen them all" into an unbounded loop with no signal.
    next = std::max(0, std::min(next, _table->rowCount() - 1));

    if (next == current)
    {
      return;
    }

    _table->selectRow(next);

    goToSelectedRow();
  }
}
