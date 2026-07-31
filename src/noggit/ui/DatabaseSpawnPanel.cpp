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

using namespace Noggit::Ui;

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
  connect(_orientation, &QSpinBox::editingFinished, this, [this]
    {
      std::uint32_t const guid = selectedGuid();

      if (guid == 0)
      {
        return;
      }

      if (auto* cache = _map_view->databaseSpawns())
      {
        constexpr double DEGREES_TO_RADIANS = 3.14159265358979323846 / 180.0;

        cache->rotateTo(guid, _orientation->value() * DEGREES_TO_RADIANS);
        _map_view->markSpawnOverlayDirty();
        refresh();
      }
    }
  );

  refresh();
}

void DatabaseSpawnPanel::onSelectionChanged()
{
  auto* cache = _map_view->databaseSpawns();

  if (!cache)
  {
    return;
  }

  std::uint32_t const guid = selectedGuid();

  cache->setSelected(guid);

  // Show the spawn's current facing without marking it dirty -- reading a value must never look
  // like an edit, or every click in the list would queue a change to save.
  for (auto const* entry : cache->allEntries())
  {
    if (entry->guid != guid)
    {
      continue;
    }

    constexpr double RADIANS_TO_DEGREES = 180.0 / 3.14159265358979323846;

    double const orientation
      ( entry->kind == Noggit::Database::SpawnKind::CREATURE
          ? entry->creature.orientation
          : entry->gameobject.orientation
      );

    QSignalBlocker const blocker (_orientation);
    _orientation->setValue(static_cast<int>(orientation * RADIANS_TO_DEGREES) % 360);
    break;
  }

  _map_view->markSpawnOverlayDirty();
}

void DatabaseSpawnPanel::onFocus()
{
  std::uint32_t const guid = selectedGuid();

  if (guid == 0)
  {
    return;
  }

  _map_view->focusOnSpawn(guid);
}

bool DatabaseSpawnPanel::moveMode() const
{
  return _move_mode->isChecked();
}

std::uint32_t DatabaseSpawnPanel::selectedGuid() const
{
  auto const* item = _spawn_list->currentItem();

  // The guid is carried as item data rather than parsed back out of the label, so the label stays
  // free to change without silently breaking selection.
  return item ? item->data(Qt::UserRole).toUInt() : 0u;
}

void DatabaseSpawnPanel::refresh()
{
  auto const* cache = _map_view->databaseSpawns();

  // Preserved across the rebuild, otherwise every move would drop the selection and the next
  // click would do nothing.
  std::uint32_t const previously_selected = selectedGuid();

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

      if (entry->guid == previously_selected)
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
