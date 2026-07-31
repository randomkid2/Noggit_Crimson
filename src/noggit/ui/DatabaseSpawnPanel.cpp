// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/ui/DatabaseSpawnPanel.hpp>

#include <noggit/MapView.h>
#include <noggit/database/SpawnSceneCache.hpp>

#include <QtWidgets/QCheckBox>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QMessageBox>
#include <QtCore/QSignalBlocker>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QVBoxLayout>

#include <cmath>

using namespace Noggit::Ui;

namespace
{
  constexpr double PI = 3.14159265358979323846;

  // Where a list item keeps the kind half of its spawn's identity. Qt::UserRole holds the guid.
  //
  // Both halves are needed: creature.guid and gameobject.guid are independent primary keys, so
  // the guid alone selects up to two different rows. See Noggit::Database::SpawnRef.
  constexpr int SPAWN_KIND_ROLE = Qt::UserRole + 1;

  // A facing in whole degrees, the way the spin box shows it.
  //
  // Rounded and wrapped, not truncated. 90 degrees written out and read back is 1.5707963...
  // radians, which comes back as 89.99999...; static_cast<int> of that is 89, so the box would
  // walk one degree backwards every time the same spawn was reselected -- and the focus-out guard
  // in applyOrientationIfChanged, which compares the box against this, would see a difference the
  // user never made and write it. The double wrap keeps a hand-edited negative row out of a
  // spin box whose range starts at 0.
  int degreesFor(double radians)
  {
    long const degrees (std::lround(radians * 180.0 / PI));

    return static_cast<int>(((degrees % 360) + 360) % 360);
  }

  // The spawn a list item stands for. A default SpawnRef (guid 0) for no item.
  Noggit::Database::SpawnRef refOf(QListWidgetItem const* item)
  {
    if (!item)
    {
      return {};
    }

    // Carried as item data rather than parsed back out of the label, so the label stays free to
    // change without silently breaking selection.
    return Noggit::Database::SpawnRef
      { static_cast<Noggit::Database::SpawnKind>(item->data(SPAWN_KIND_ROLE).toUInt())
      , item->data(Qt::UserRole).toUInt()
      };
  }
}

DatabaseSpawnPanel::DatabaseSpawnPanel(MapView* map_view, QWidget* parent)
  : QWidget(parent)
  , _map_view(map_view)
{
  setWindowTitle("Database Spawns");

  auto layout (new QVBoxLayout(this));

  // --- load ------------------------------------------------------------------------------
  auto load_row (new QHBoxLayout());
  auto load_tile (new QPushButton("Load this tile", this));
  auto load_all (new QPushButton("Load all loaded tiles", this));
  load_all->setToolTip("Counts the spawns first and asks before loading a large set.");
  load_row->addWidget(load_tile);
  load_row->addWidget(load_all);
  layout->addLayout(load_row);

  // --- the spawns ------------------------------------------------------------------------
  _spawn_list = new QListWidget(this);
  _spawn_list->setToolTip("Select a spawn, then tick Move mode and click in the world to place it.");
  layout->addWidget(_spawn_list, 1);

  // --- moving ----------------------------------------------------------------------------
  auto move_box (new QGroupBox("Selected spawn", this));
  auto move_layout (new QVBoxLayout(move_box));

  auto focus_button (new QPushButton("Focus camera on it", this));
  focus_button->setToolTip
    ("Flies the camera to the selected spawn. The outline only helps once you can see it.");
  move_layout->addWidget(focus_button);

  _move_mode = new QCheckBox("Move mode - click in the world to place it", this);
  move_layout->addWidget(_move_mode);

  auto rotation_row (new QHBoxLayout());
  rotation_row->addWidget(new QLabel("Facing", this));

  _orientation = new QSpinBox(this);
  // Degrees in the UI, radians in the database. Degrees because nobody thinks in radians while
  // aiming a guard at a door; the conversion happens once, on the way out.
  _orientation->setRange(0, 359);
  _orientation->setSuffix("°");
  _orientation->setWrapping(true);
  _orientation->setToolTip("0 faces north (+x on the server). Wraps at 360.");
  rotation_row->addWidget(_orientation, 1);
  move_layout->addLayout(rotation_row);

  move_layout->addWidget
    ( new QLabel("Nothing is written to the database until you save.", this));

  layout->addWidget(move_box);

  // --- saving ----------------------------------------------------------------------------
  auto save_box (new QGroupBox("Save", this));
  auto save_layout (new QVBoxLayout(save_box));

  _pending = new QLabel("No changes.", this);
  save_layout->addWidget(_pending);

  _apply_to_dev = new QCheckBox("Also apply to the dev schema", this);
  _apply_to_dev->setToolTip
    ("Runs the changeset against the configured dev schema. Never against a live world database:\n"
     "the connection layer refuses to open write-capable against anything else.");
  save_layout->addWidget(_apply_to_dev);

  auto save_row (new QHBoxLayout());
  _save_button = new QPushButton("Save to database", this);
  _save_button->setToolTip
    ("Writes a reviewable .sql changeset into the project's changesets folder.");
  _discard_button = new QPushButton("Discard", this);
  save_row->addWidget(_save_button, 1);
  save_row->addWidget(_discard_button);
  save_layout->addLayout(save_row);

  layout->addWidget(save_box);

  connect(load_tile, &QPushButton::clicked, this, [this] { onLoad(false); });
  connect(load_all, &QPushButton::clicked, this, [this] { onLoad(true); });
  connect(_save_button, &QPushButton::clicked, this, &DatabaseSpawnPanel::onSave);
  connect(_discard_button, &QPushButton::clicked, this, &DatabaseSpawnPanel::onDiscard);
  connect(focus_button, &QPushButton::clicked, this, &DatabaseSpawnPanel::onFocus);

  connect(_spawn_list, &QListWidget::currentRowChanged, this
         , [this] (int) { onSelectionChanged(); });

  // editingFinished, not valueChanged: valueChanged fires on every step of a spin or a drag, so
  // holding the arrow would mark the spawn dirty dozens of times and rebuild the list under the
  // cursor while the user was still choosing an angle.
  //
  // But editingFinished is not "the user edited something" either -- QAbstractSpinBox emits it
  // from focusOutEvent as well, so it fires when the box is merely clicked into and away from.
  // That is why the slot compares before it writes; see applyOrientationIfChanged.
  connect(_orientation, &QSpinBox::editingFinished, this
         , &DatabaseSpawnPanel::applyOrientationIfChanged);

  refresh();
}

void DatabaseSpawnPanel::applyOrientationIfChanged()
{
  Noggit::Database::SpawnRef const spawn (selectedSpawn());

  auto* cache = _map_view->databaseSpawns();

  if (!spawn.valid() || !cache)
  {
    return;
  }

  int stored_degrees = 0;

  // Not loaded. Nothing to write, and writing anyway would mean writing to whichever spawn the
  // scan happened to reach first.
  if (!spawnFacingDegrees(spawn, stored_degrees))
  {
    return;
  }

  // The guard this function exists for.
  //
  // An equal value means the box was focused and left without an edit -- the focus-out case
  // above. Writing then would replace the row's exact stored radians with a whole-degree
  // approximation of themselves and mark the spawn dirty, putting a facing in the changeset that
  // nobody changed.
  if (_orientation->value() == stored_degrees)
  {
    return;
  }

  constexpr double DEGREES_TO_RADIANS = PI / 180.0;

  cache->rotateTo(spawn, _orientation->value() * DEGREES_TO_RADIANS);
  _map_view->markSpawnOverlayDirty();
  refresh();
}

bool DatabaseSpawnPanel::spawnFacingDegrees
  (Noggit::Database::SpawnRef const& spawn, int& degrees) const
{
  auto const* cache = _map_view->databaseSpawns();

  if (!cache || !spawn.valid())
  {
    return false;
  }

  for (auto const* entry : cache->allEntries())
  {
    if (entry->ref() != spawn)
    {
      continue;
    }

    degrees = degreesFor
      ( entry->kind == Noggit::Database::SpawnKind::CREATURE
          ? entry->creature.orientation
          : entry->gameobject.orientation
      );

    return true;
  }

  return false;
}

void DatabaseSpawnPanel::onSelectionChanged()
{
  auto* cache = _map_view->databaseSpawns();

  if (!cache)
  {
    return;
  }

  Noggit::Database::SpawnRef const spawn (selectedSpawn());

  cache->setSelected(spawn);

  // Show the spawn's current facing without marking it dirty -- reading a value must never look
  // like an edit, or every click in the list would queue a change to save.
  int degrees = 0;

  if (spawnFacingDegrees(spawn, degrees))
  {
    QSignalBlocker const blocker (_orientation);
    _orientation->setValue(degrees);
  }

  _map_view->markSpawnOverlayDirty();
}

void DatabaseSpawnPanel::selectSpawn(Noggit::Database::SpawnRef const& spawn)
{
  for (int row = 0; row < _spawn_list->count(); ++row)
  {
    auto* item = _spawn_list->item(row);

    if (refOf(item) != spawn)
    {
      continue;
    }

    // currentRowChanged fires onSelectionChanged, which is what loads the facing into the spin
    // box and re-marks the overlay. Letting it run is the point -- clicking a spawn in the world
    // should leave the panel in exactly the state it would be in had you clicked the row.
    _spawn_list->setCurrentItem(item);
    _spawn_list->scrollToItem(item);
    return;
  }
}

void DatabaseSpawnPanel::onFocus()
{
  Noggit::Database::SpawnRef const spawn (selectedSpawn());

  if (!spawn.valid())
  {
    return;
  }

  _map_view->focusOnSpawn(spawn);
}

bool DatabaseSpawnPanel::moveMode() const
{
  return _move_mode->isChecked();
}

Noggit::Database::SpawnRef DatabaseSpawnPanel::selectedSpawn() const
{
  return refOf(_spawn_list->currentItem());
}

void DatabaseSpawnPanel::refresh()
{
  auto const* cache = _map_view->databaseSpawns();

  // Preserved across the rebuild, otherwise every move would drop the selection and the next
  // click would do nothing.
  Noggit::Database::SpawnRef const previously_selected (selectedSpawn());

  _spawn_list->clear();

  std::size_t pending = 0;

  if (cache)
  {
    for (auto const* entry : cache->allEntries())
    {
      QString const label
        ( QString("%1%2  %3  %4")
            .arg(entry->dirty ? "* " : "  ")
            .arg(entry->guid)
            .arg(entry->kind == Noggit::Database::SpawnKind::CREATURE ? "creature" : "gameobject")
            .arg(entry->instance
                   ? QString::fromStdString(entry->instance->model->file_key().stringRepr())
                       .section('/', -1)
                   : QString("(no model)"))
        );

      auto* item = new QListWidgetItem(label, _spawn_list);
      item->setData(Qt::UserRole, entry->guid);
      item->setData(SPAWN_KIND_ROLE, static_cast<unsigned>(entry->kind));

      if (entry->ref() == previously_selected)
      {
        _spawn_list->setCurrentItem(item);
      }
    }

    pending = cache->dirtyCount();
  }

  _pending->setText
    ( pending == 0 ? QString("No changes.")
                   : QString("%1 spawn(s) moved, not yet saved.").arg(pending));

  _save_button->setEnabled(pending > 0);
  _discard_button->setEnabled(pending > 0);
}

void DatabaseSpawnPanel::onLoad(bool all_tiles)
{
  // interactive: the user pressed a button, so a dialog is the right way to report a problem.
  _map_view->loadDatabaseSpawns(all_tiles, true, false);
  refresh();
}

void DatabaseSpawnPanel::onSave()
{
  std::string const result
    (_map_view->saveDatabaseChanges(_apply_to_dev->isChecked(), true));

  // Success is reported here rather than by a dialog: saveDatabaseChanges already puts it in the
  // status bar, and a modal for the common case would be noise.
  if (result.rfind("OK ", 0) == 0)
  {
    _pending->setText(QString::fromStdString(result.substr(3)));
  }

  refresh();
}

void DatabaseSpawnPanel::onDiscard()
{
  auto const answer
    ( QMessageBox::question
      ( this
      , "Database Spawns"
      , "Discard the unsaved moves? The spawns stay where they are on screen until you reload."
      , QMessageBox::Yes | QMessageBox::No
      , QMessageBox::No
      )
    );

  if (answer != QMessageBox::Yes)
  {
    return;
  }

  if (auto* cache = _map_view->databaseSpawns())
  {
    cache->clearDirty();
  }

  refresh();
}
