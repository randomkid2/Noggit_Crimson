// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/ui/DatabaseSpawnPanel.hpp>

#include <noggit/MapView.h>
#include <noggit/World.h>
#include <noggit/database/DatabaseSettings.hpp>
#include <noggit/database/SchemaIntrospector.hpp>
#include <noggit/database/SpawnSceneCache.hpp>
#include <noggit/database/TileCoordinates.hpp>
#include <noggit/database/WorldDatabaseConnection.hpp>
#include <noggit/map_index.hpp>
#include <noggit/ui/SpawnTilePicker.hpp>

// SpawnQuery.hpp, opengl/context.hpp and opengl/scoped.hpp are deliberately gone with the load
// loop that used them: loading now runs entirely inside MapView::loadDatabaseSpawnsForTiles, which
// owns the queries and the OpenGL context guard they need. onScanMapForSpawns still counts, so the
// connection and introspection headers stay.

#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QMessageBox>
#include <QtCore/QSignalBlocker>
#include <QtCore/QTimer>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QStyle>
#include <QtWidgets/QVBoxLayout>

#include <cmath>
#include <exception>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

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

#ifdef USE_MYSQL_UID_STORAGE
  // Table and column names for the whole-map occupancy scan.
  //
  // Named constants checked against information_schema below rather than written into the SQL
  // text on faith -- HARD RULE 3. The scan refuses to run against a schema that does not have
  // them instead of degrading, because an occupancy overlay that is quietly wrong is worse than
  // no overlay: it would tell the user which tiles to skip.
  constexpr char const* TABLE_CREATURE = "creature";
  constexpr char const* TABLE_GAMEOBJECT = "gameobject";
  constexpr char const* COLUMN_MAP = "map";
  constexpr char const* COLUMN_POSITION_X = "position_x";
  constexpr char const* COLUMN_POSITION_Y = "position_y";

  // Spawns per tile for one table, in ONE statement.
  //
  // The alternative -- SpawnQuery::countTile per tile -- is two statements per tile, so 8192 for
  // the map. Letting the server do the bucketing instead returns at most 4096 rows from one
  // query, which is what makes a whole-map overlay affordable enough to offer at all.
  //
  // The bucket expression is floor(32 - coordinate / TILE_SIZE), which is
  // TileCoordinates::tileForPosition written in SQL. It is a HINT, not the authority: the server
  // computes it over a FLOAT column in its own arithmetic, so a spawn sitting exactly on a tile
  // edge can land in the neighbouring bucket from the one loadTile would attribute it to. That
  // moves one spawn between two adjacent counts; it never invents or loses a populated tile, and
  // the load itself still goes through SpawnQuery, which is the authority.
  //
  // The result is in the DATABASE frame: the first column is Database::TileIndex::x (from world
  // x) and the second is ::y (from world y). Converting to the ADT frame is the caller's job and
  // must go through toAdtFileIndex.
  std::string tileOccupancySql
    ( Noggit::Database::SchemaModel const& schema
    , std::string const& table
    , std::uint16_t map
    )
  {
    if (!schema.hasTable(table))
    {
      throw Noggit::Database::SchemaCapabilityError("there is no " + table + " table");
    }

    for (char const* column : {COLUMN_MAP, COLUMN_POSITION_X, COLUMN_POSITION_Y})
    {
      if (!schema.hasColumn(table, column))
      {
        throw Noggit::Database::SchemaCapabilityError
          (table + " has no " + column + " column");
      }
    }

    // Written out from the shared constant rather than repeated as a literal, so the SQL and
    // TileCoordinates cannot drift apart. Twelve significant digits reproduces 533.33333 exactly.
    std::ostringstream tile_size;
    tile_size << std::setprecision(12) << Noggit::Database::TILE_SIZE;

    auto const bucket = [&tile_size] (char const* column)
    {
      return "FLOOR(" + std::to_string(Noggit::Database::TILE_ORIGIN) + " - `"
           + std::string(column) + "` / " + tile_size.str() + ")";
    };

    // CAST to SIGNED so the value arrives as "31" rather than as a decimal with a fractional
    // part; ResultRow is all strings and the parse below is integer-only.
    //
    // The bucket expression is repeated in GROUP BY rather than referenced by its alias or by
    // ordinal: MySQL 8 removed GROUP BY ordinals, and grouping by a select alias is a MySQL
    // extension other servers reject.
    return "SELECT CAST(" + bucket(COLUMN_POSITION_X) + " AS SIGNED)"
           ", CAST(" + bucket(COLUMN_POSITION_Y) + " AS SIGNED)"
           ", COUNT(*)"
           "\nFROM `" + table + "`"
           "\nWHERE `" + std::string(COLUMN_MAP) + "` = " + std::to_string(map) +
           "\nGROUP BY " + bucket(COLUMN_POSITION_X) + ", " + bucket(COLUMN_POSITION_Y);
  }

  // One integer field of a result row, defensively. A row that is short or holds a NULL decodes
  // to `fallback` rather than throwing out of the scan loop.
  long long rowInteger(Noggit::Database::ResultRow const& row, std::size_t index, long long fallback)
  {
    if (index >= row.size() || row[index].empty())
    {
      return fallback;
    }

    try
    {
      return std::stoll(row[index]);
    }
    catch (std::exception const&)
    {
      return fallback;
    }
  }
#endif
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

  // --- choosing tiles ----------------------------------------------------------------------
  // "This tile" and "all loaded tiles" are the two ends of a range with nothing in between, and
  // the middle is what the work actually needs: the handful of ADTs a camp or a road spans.
  auto tile_box (new QGroupBox("Tiles to load", this));
  auto tile_layout (new QVBoxLayout(tile_box));

  _tile_picker = new SpawnTilePicker(this);
  _tile_picker->world(_map_view->getWorld());
  _tile_picker->camera(_map_view->getCamera());
  _tile_picker->setToolTip
    ("Click a tile to pick it. Shift-click paints a 3x3 block, ctrl-click removes, drag paints,\n"
     "right-click clears. Cyan is spawn density from the last scan.\n"
     "\n"
     "Green outline: spawns loaded and on screen.\n"
     "Amber outline: spawns loaded, but the ADT is not streamed in, so nothing is drawn for them.\n"
     "The editor only renders spawns for tiles it has streamed -- roughly a 5x5 block around the\n"
     "camera -- so a pick further away loads and edits correctly but shows nothing.");
  tile_layout->addWidget(_tile_picker);

  _tile_status = new QLabel(this);
  _tile_status->setWordWrap(true);
  tile_layout->addWidget(_tile_status);

  auto tile_button_row (new QHBoxLayout());
  auto select_loaded (new QPushButton("Pick loaded", this));
  select_loaded->setToolTip("Picks every tile the editor currently has streamed in.");
  auto clear_selection (new QPushButton("Clear", this));

  _scan_button = new QPushButton("Scan map", this);
  _scan_button->setToolTip
    ("Counts spawns per tile for the whole map in two queries, then tints the grid by density.\n"
     "Run once; nothing is queried again while the grid repaints.");

  tile_button_row->addWidget(select_loaded);
  tile_button_row->addWidget(clear_selection);
  tile_button_row->addWidget(_scan_button);
  tile_layout->addLayout(tile_button_row);

  _load_selected = new QPushButton("Load picked tiles", this);
  _load_selected->setToolTip
    ("Replaces the spawn overlay with the spawns of the picked tiles.\n"
     "Tiles that are not streamed in are still loaded -- they are listed, editable and saved --\n"
     "but nothing is drawn for them until the camera is there.");
  tile_layout->addWidget(_load_selected);

  layout->addWidget(tile_box);

  // --- the spawns ------------------------------------------------------------------------
  _spawn_list = new QListWidget(this);
  _spawn_list->setToolTip("Select a spawn, then tick Move mode and click in the world to place it.");
  layout->addWidget(_spawn_list, 1);

  // --- creating --------------------------------------------------------------------------
  // Placement is deliberately entry-id driven rather than offering a browsable list. A world
  // database holds tens of thousands of templates and the person placing one already knows which
  // entry they want -- it is the number they looked up in the database or in a wiki. A picker
  // over that many rows is a second project, and it would still be slower than typing 299.
  auto create_box (new QGroupBox("Place a new spawn", this));
  auto create_layout (new QVBoxLayout(create_box));

  auto entry_row (new QHBoxLayout());

  _place_kind = new QComboBox(this);
  _place_kind->addItem("Creature");
  _place_kind->addItem("Gameobject");
  _place_kind->setToolTip
    ("Which template table the entry id names. creature.guid and gameobject.guid are separate\n"
     "sequences, so the same number means different things in the two.");
  entry_row->addWidget(_place_kind);

  entry_row->addWidget(new QLabel("Entry", this));

  _place_entry = new QSpinBox(this);
  // The full unsigned range the template's entry column can hold, minus the sign bit QSpinBox
  // costs us. Real entries are well inside it; the range exists so a custom template with a high
  // id can be typed rather than silently clamped to something else that does exist.
  _place_entry->setRange(0, 2147483647);
  _place_entry->setToolTip("creature_template.entry or gameobject_template.entry.");
  entry_row->addWidget(_place_entry, 1);

  create_layout->addLayout(entry_row);

  _place_mode = new QCheckBox("Place mode - click in the world to create one", this);
  _place_mode->setToolTip
    ("The next left click in the viewport places a spawn at the point under the cursor.\n"
     "Stays on, so a row of guards is a row of clicks. Nothing is written until you save.");
  create_layout->addWidget(_place_mode);

  auto place_here (new QPushButton("Place at the camera", this));
  place_here->setToolTip
    ("Places one at the point the camera is over, without needing to aim a click.");
  create_layout->addWidget(place_here);

  create_layout->addWidget
    ( new QLabel("The guid is chosen by the server when the changeset is applied.", this));

  layout->addWidget(create_box);

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

  _delete_button = new QPushButton("Delete this spawn", this);
  _delete_button->setToolTip
    ("Takes it out of the world now. A spawn that came from the database becomes a DELETE in the\n"
     "changeset; one you placed yourself simply goes away and emits nothing. Discard puts either\n"
     "one back.");
  move_layout->addWidget(_delete_button);

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

  connect(place_here, &QPushButton::clicked, this, &DatabaseSpawnPanel::onPlaceAtCamera);

  // valueChanged, not editingFinished: this only enables a checkbox, so reacting to every step
  // costs nothing and the alternative leaves place mode greyed out until focus moves elsewhere.
  connect(_place_entry, QOverload<int>::of(&QSpinBox::valueChanged), this
         , [this] (int) { updateSpawnActions(); });
  connect(_delete_button, &QPushButton::clicked, this, &DatabaseSpawnPanel::onDelete);

  // Exactly one mode may be armed. Resolved here rather than left to whichever branch MapView
  // tests first, because that ordering is invisible from the panel and a click that sometimes
  // moves and sometimes creates is unusable.
  connect(_place_mode, &QCheckBox::toggled, this, [this] (bool on)
    {
      if (on)
      {
        _move_mode->setChecked(false);
      }
    });

  connect(_move_mode, &QCheckBox::toggled, this, [this] (bool on)
    {
      if (on)
      {
        _place_mode->setChecked(false);
      }
    });

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

  connect(select_loaded, &QPushButton::clicked, this
         , [this] { _tile_picker->selectLoadedTiles(); });
  connect(clear_selection, &QPushButton::clicked, this
         , [this] { _tile_picker->clearSelection(); });
  connect(_scan_button, &QPushButton::clicked, this, &DatabaseSpawnPanel::onScanMapForSpawns);
  connect(_load_selected, &QPushButton::clicked, this
         , &DatabaseSpawnPanel::onLoadSelectedTiles);

  // The picker has no idea what a spawn is; it reports that the picked set changed and this is
  // what turns that into a count, an estimate and a button state.
  connect(_tile_picker, &SpawnTilePicker::selectionChanged, this
         , &DatabaseSpawnPanel::updateTileStatus);

  // Which tiles are streamed in changes as the camera flies, and nothing informs this panel when
  // it does -- so the green / amber markers would be right when they were computed and wrong a few
  // seconds later, which is the same wrong answer the split exists to stop giving. Polled rather
  // than hooked because the streaming path has no signal to hook, and skipped while the dock is
  // hidden so a closed panel costs nothing.
  _load_state_poll = new QTimer(this);
  _load_state_poll->setInterval(LOAD_STATE_POLL_MS);

  connect(_load_state_poll, &QTimer::timeout, this, [this]
    {
      if (!isVisible())
      {
        return;
      }

      // Not refresh(): that rebuilds the list widget, which would fight the user's selection and
      // scroll position every LOAD_STATE_POLL_MS. Only the overlay and the caption it feeds.
      updateLoadedTileOverlay();
      updateTileStatus();
    });

  _load_state_poll->start();

  updateTileStatus();
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

  updateSpawnActions();

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

bool DatabaseSpawnPanel::placeMode() const
{
  return _place_mode->isChecked();
}

bool DatabaseSpawnPanel::placeCreature() const
{
  return _place_kind->currentIndex() == 0;
}

std::uint32_t DatabaseSpawnPanel::placeEntry() const
{
  int const value (_place_entry->value());

  return value <= 0 ? 0u : static_cast<std::uint32_t>(value);
}

void DatabaseSpawnPanel::onPlaceAtCamera()
{
  if (placeEntry() == 0)
  {
    QMessageBox::information
      ( this
      , "Database spawns"
      , "Enter the creature_template or gameobject_template entry id to place first."
      );

    return;
  }

  // The camera's own position, not the terrain cursor: this button exists for the case where
  // there is no useful cursor, and a spawn a few yards in the air settles onto the ground the
  // moment the user drags it. Aiming precisely is what place mode is for.
  _map_view->createDatabaseSpawn
    (placeCreature(), placeEntry(), _map_view->cameraPosition(), true);

  refresh();
}

void DatabaseSpawnPanel::onDelete()
{
  Noggit::Database::SpawnRef const spawn (selectedSpawn());

  if (!spawn.valid())
  {
    QMessageBox::information
      (this, "Database spawns", "Select a spawn in the list first.");

    return;
  }

  auto const* cache = _map_view->databaseSpawns();

  // Whether this deletion will produce SQL decides whether it is worth a confirmation. Asked of
  // the cache rather than inferred from the guid, so there is one authority on what "was in the
  // database" means -- see SpawnSceneEntry::is_new.
  bool from_database = true;

  if (cache)
  {
    for (auto const* entry : cache->allEntries())
    {
      if (entry->ref() == spawn)
      {
        from_database = !entry->is_new;
        break;
      }
    }
  }

  if (from_database)
  {
    auto const answer
      ( QMessageBox::question
        ( this
        , "Database spawns"
        , QString("Delete %1 guid %2?\n\nThe changeset will carry a DELETE for it, and for its "
                  "creature_addon row where it has one. Nothing is written until you save, and "
                  "Discard puts it back.")
            .arg(spawn.kind == Noggit::Database::SpawnKind::CREATURE ? "creature" : "gameobject")
            .arg(spawn.guid)
        , QMessageBox::Yes | QMessageBox::No
        , QMessageBox::No
        )
      );

    if (answer != QMessageBox::Yes)
    {
      return;
    }
  }

  _map_view->deleteDatabaseSpawn(spawn, true);

  refresh();
}

void DatabaseSpawnPanel::updateSpawnActions()
{
  _delete_button->setEnabled(selectedSpawn().valid());

  // Place mode is unusable without an entry id, and an armed mode that answers every click with
  // the same dialog is worse than one that cannot be armed: it swallows clicks the terrain tools
  // would otherwise have got, once per attempt, until the user works out why.
  bool const placeable (placeEntry() != 0);

  _place_mode->setEnabled(placeable);

  if (!placeable)
  {
    _place_mode->setChecked(false);
  }
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
  std::size_t moved = 0;
  std::size_t placed = 0;
  std::size_t deleted = 0;

  if (cache)
  {
    for (auto const* entry : cache->allEntries())
    {
      // Three states, three markers. "+" is a spawn the user placed, and it needs to be
      // distinguishable from a moved one at a glance: its guid is the editor's invention and
      // will not be the guid the database issues, so anybody writing that number down is writing
      // down the wrong thing.
      QString const label
        ( QString("%1%2  %3  %4")
            .arg(entry->is_new ? "+ " : (entry->dirty ? "* " : "  "))
            .arg(entry->is_new ? QString("(new)") : QString::number(entry->guid))
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

    // pendingCount, not dirtyCount: a session that placed or deleted spawns and moved none has
    // changes to save, and gating the Save button on moves alone silently threw them away.
    pending = cache->pendingCount();

    moved = cache->dirtyEntries().size();
    placed = cache->newEntries().size();
    deleted = cache->removedSpawns().size();
  }

  // Broken down rather than totalled, because the three are undone and reviewed differently and
  // "3 changes" tells the user nothing about what the changeset will contain.
  _pending->setText
    ( pending == 0
        ? QString("No changes.")
        : QString("Unsaved: %1 moved, %2 placed, %3 deleted.").arg(moved).arg(placed).arg(deleted)
    );

  _save_button->setEnabled(pending > 0);
  _discard_button->setEnabled(pending > 0);

  updateSpawnActions();

  // In this order: the overlay walk is what produces the drawn / not-drawn counts, and the status
  // line now reports them.
  updateLoadedTileOverlay();
  updateTileStatus();
}

void DatabaseSpawnPanel::updateLoadedTileOverlay()
{
  auto const* cache = _map_view->databaseSpawns();
  auto const* world = _map_view->getWorld();

  _loaded_tiles_drawn = 0;
  _loaded_tiles_not_drawn = 0;

  if (!cache)
  {
    _tile_picker->setLoadedTiles({});
    return;
  }

  // 4096 lookups into a std::map of at most a few hundred entries, run when something changed --
  // not while painting. The cache exposes tile() rather than an iterator over what it holds, and
  // probing is cheap enough that asking for more surface would be the wrong trade.
  std::vector<SpawnTilePicker::TileLoadState> states
    (SpawnTilePicker::TILE_BUFFER_SIZE, SpawnTilePicker::TileLoadState::NONE);

  for (int adt_x = 0; adt_x < 64; ++adt_x)
  {
    for (int adt_z = 0; adt_z < 64; ++adt_z)
    {
      // ADT-filename frame, matching the cache's own key order (SpawnSceneCache.hpp:286) and the
      // picker's buffer order.
      ::TileIndex const adt_tile
        (static_cast<std::size_t>(adt_x), static_cast<std::size_t>(adt_z));

      if (!cache->tile(adt_tile))
      {
        continue;
      }

      // The whole point of the split. WorldRender iterates mapIndex.loaded_tiles()
      // (WorldRender.cpp:122) and asks the cache for a tile only from inside that loop
      // (WorldRender.cpp:658-660), and loaded_tiles() is exactly the tileLoaded() predicate
      // (map_index.cpp:29-33, :609-612). So a cached tile the index has not streamed is a tile
      // whose spawns exist in every sense except the visible one.
      bool const streamed (world && world->mapIndex.tileLoaded(adt_tile));

      states[static_cast<std::size_t>(64 * adt_x + adt_z)]
        = streamed ? SpawnTilePicker::TileLoadState::DRAWN
                   : SpawnTilePicker::TileLoadState::CACHED_ONLY;

      if (streamed)
      {
        ++_loaded_tiles_drawn;
      }
      else
      {
        ++_loaded_tiles_not_drawn;
      }
    }
  }

  _tile_picker->setLoadedTiles(std::move(states));
}

std::size_t DatabaseSpawnPanel::pickedTilesNotStreamed() const
{
  auto const* world = _map_view->getWorld();

  if (!world)
  {
    return 0;
  }

  std::size_t not_streamed = 0;

  for (auto const& adt_tile : _tile_picker->selectedAdtTiles())
  {
    if (!world->mapIndex.tileLoaded(adt_tile))
    {
      ++not_streamed;
    }
  }

  return not_streamed;
}

void DatabaseSpawnPanel::updateTileStatus()
{
  std::size_t const picked (_tile_picker->selectedCount());

  bool const over_limit (picked > MAX_SELECTED_TILES);

  QString text
    ( QString("%1 / %2 tiles picked").arg(picked).arg(MAX_SELECTED_TILES));

  if (over_limit)
  {
    // The bound is stated here, in the same place the count is, rather than only at the point of
    // refusal -- a limit the user meets for the first time in an error dialog after a long drag
    // is a limit that was hidden.
    text += QString(" - over the limit, deselect %1").arg(picked - MAX_SELECTED_TILES);
  }
  else if (_tile_picker->hasSpawnCounts())
  {
    text += QString(" - about %1 spawns").arg(_tile_picker->scannedSpawnsInSelection());
  }
  else if (picked > 0)
  {
    text += QString(" - spawn counts unknown, press Scan map");
  }

  // Said BEFORE the load, not only after it. The editor streams roughly a 5x5 block around the
  // camera and the grid lets any existing tile be picked, so a pick well away from the camera
  // loads correctly and shows nothing -- and being told that only once the viewport is already
  // empty is being told it too late. The pick is still allowed; see onLoadSelectedTiles.
  if (!over_limit && picked > 0)
  {
    std::size_t const not_streamed (pickedTilesNotStreamed());

    if (not_streamed > 0)
    {
      text += QString("\n%1 of them are not streamed in - those spawns will load and be editable, "
                      "but nothing will be drawn for them until the camera is there.")
                .arg(not_streamed);
    }
  }

  // What the overlay is currently showing, in words, so the amber cells have a caption rather
  // than needing to be guessed at.
  if (_loaded_tiles_not_drawn > 0)
  {
    text += QString("\nLoaded: %1 tile(s) drawn (green), %2 loaded but off screen (amber).")
              .arg(_loaded_tiles_drawn).arg(_loaded_tiles_not_drawn);
  }
  else if (_loaded_tiles_drawn > 0)
  {
    text += QString("\nLoaded: %1 tile(s), all drawn.").arg(_loaded_tiles_drawn);
  }

  _tile_status->setText(text);

  // #d04040 was a red of its own, close enough to the palette's bad colour to look like a
  // mistake and far enough off to be one. A dynamic property needs the unpolish/polish pair --
  // Qt only re-evaluates attribute selectors when it is told the widget's properties moved --
  // and this runs on a selection change rather than per frame, so the cost is irrelevant.
  _tile_status->setProperty("state", over_limit ? QVariant("error") : QVariant());
  _tile_status->style()->unpolish(_tile_status);
  _tile_status->style()->polish(_tile_status);

  _load_selected->setEnabled(picked > 0 && !over_limit);
}

void DatabaseSpawnPanel::onScanMapForSpawns()
{
#ifdef USE_MYSQL_UID_STORAGE
  if (!Noggit::Database::DatabaseSettings::isEnabled())
  {
    QMessageBox::critical
      ( this
      , "Database spawns"
      , "The database feature is not enabled. Turn it on and set the connection details in "
        "Settings first."
      );

    return;
  }

  try
  {
    // READ_ONLY, for the same reason MapView::loadDatabaseSpawns is: this only ever counts, and
    // asking for a write-capable connection would make the layer refuse against anything but the
    // dev schema -- which is the opposite of what an overlay over a real world database needs.
    Noggit::Database::WorldDatabaseConnection connection
      ( Noggit::Database::DatabaseSettings::readConnectionConfig()
      , Noggit::Database::AccessMode::READ_ONLY
      , Noggit::Database::DatabaseSettings::readWritableSchema()
      );

    Noggit::Database::SchemaModel const schema
      (Noggit::Database::SchemaIntrospector::readModel(connection, connection.schema()));

    auto const map_id (static_cast<std::uint16_t>(_map_view->getWorld()->getMapID()));

    // Indexed 64 * adt_x + adt_z, the picker's frame. The query answers in the DATABASE frame,
    // which is why every row goes through toAdtFileIndex below rather than being written straight
    // into this.
    std::vector<std::uint32_t> counts (SpawnTilePicker::TILE_BUFFER_SIZE, 0);

    std::uint64_t total = 0;

    for (char const* table : {TABLE_CREATURE, TABLE_GAMEOBJECT})
    {
      for (auto const& row : connection.query(tileOccupancySql(schema, table, map_id)))
      {
        // -1: off the grid, so the range check below rejects it. Real spawns far outside the
        // representable world do exist in world databases and must not index the buffer.
        long long const db_tile_x (rowInteger(row, 0, -1));
        long long const db_tile_y (rowInteger(row, 1, -1));
        long long const count (rowInteger(row, 2, 0));

        if (db_tile_x < 0 || db_tile_x >= Noggit::Database::TILES_PER_SIDE
            || db_tile_y < 0 || db_tile_y >= Noggit::Database::TILES_PER_SIDE
            || count <= 0)
        {
          continue;
        }

        // The one frame change in this function. Database::TileIndex is (x from world x, y from
        // world y); the picker is ADT-filename (x, z). toAdtFileIndex transposes them; assigning
        // field by field would stay in range, pass every check, and tint a tile 9.6 km away.
        Noggit::Database::AdtFileIndex const adt
          ( Noggit::Database::toAdtFileIndex
              ( Noggit::Database::TileIndex
                {static_cast<int>(db_tile_x), static_cast<int>(db_tile_y)}
              )
          );

        counts[static_cast<std::size_t>(64 * adt.x + adt.z)]
          += static_cast<std::uint32_t>(count);

        total += static_cast<std::uint64_t>(count);
      }
    }

    _tile_picker->setSpawnCounts(std::move(counts));
    updateTileStatus();

    QMessageBox::information
      ( this
      , "Database spawns"
      , QString("%1 spawn(s) counted on map %2. The grid is tinted by density; the counts are a "
                "hint from two grouped queries, not a per-tile query per repaint.")
          .arg(total).arg(map_id)
      );
  }
  catch (std::exception const& e)
  {
    QMessageBox::critical
      ( this
      , "Database spawns"
      , QString("Could not scan the map. ") + e.what()
      );
  }
#else
  QMessageBox::information
    ( this
    , "Database spawns"
    , "This build has no database support. Reconfigure with -DUSE_SQL=ON."
    );
#endif
}

void DatabaseSpawnPanel::onLoadSelectedTiles()
{
  // ADT-filename frame, straight from the grid, and handed on in that frame.
  // MapView::loadDatabaseSpawnsForTiles takes ::TileIndex and does the transposition to
  // Noggit::Database::TileIndex itself through fromAdtFileIndex, so converting here would apply it
  // twice -- which compiles, stays inside 0..63 and reads a tile about 9.6 km away.
  std::vector<::TileIndex> const adt_tiles (_tile_picker->selectedAdtTiles());

  if (adt_tiles.empty())
  {
    QMessageBox::information
      (this, "Database spawns", "Pick at least one tile on the grid first.");

    return;
  }

  // The one pre-flight check that is genuinely this widget's: MAX_SELECTED_TILES is a property of
  // the grid, not of loading. Everything else that used to live here -- the feature check, the
  // connection, the introspection, the unsaved-changes warning, the spawn-count threshold, the
  // lazy construction of the scene cache and the OpenGL context guard around clear()/setTile() --
  // is now MapView's single copy. The duplicate was not just drift risk: it was the reason a first
  // load in a session had to be "primed" through the camera tile, which failed whenever the camera
  // was not over a streamed ADT even though every picked tile was fine.
  if (adt_tiles.size() > MAX_SELECTED_TILES)
  {
    QMessageBox::critical
      ( this
      , "Database spawns"
      , QString("%1 tiles picked, and the limit is %2.\n\nEvery spawn queues an asynchronous "
                "model load on this thread, so a very large pick presents as a hang rather than "
                "as slow progress.")
          .arg(adt_tiles.size()).arg(MAX_SELECTED_TILES)
      );

    return;
  }

  // Counted before the load, because the load is what makes the answer stale: nothing here changes
  // what is streamed, but the message below is about these tiles specifically.
  std::size_t const not_streamed (pickedTilesNotStreamed());

  // interactive: a human pressed the button, so dialogs are the right channel. Not forced: the
  // spawn-count confirmation is exactly the question this button should be asking.
  std::string const result (_map_view->loadDatabaseSpawnsForTiles(adt_tiles, true, false));

  // Rebuilds the list, recomputes the drawn / not-drawn overlay and rewrites the status line.
  // Unconditional, including on failure and on cancellation: MapView clears the cache only after
  // every question has been answered, so on those paths this simply redraws what is already true.
  refresh();

  if (result.rfind("OK ", 0) != 0)
  {
    // MapView has already shown the error dialog. Nothing to add, and the label must not be
    // overwritten with "ERR ..." -- it describes unsaved changes, which an error did not alter.
    return;
  }

  // "OK cancelled" is not a load. Leaving the label alone is the whole handling.
  if (result == "OK cancelled")
  {
    return;
  }

  // After refresh(), not before: refresh() rewrites this label from the pending count, which is
  // zero immediately after a load, so a summary written first would be replaced by "No changes."
  QString summary
    ( QString::fromStdString(result.substr(3))
    + QString(" over %1 picked tile(s).").arg(adt_tiles.size()));

  if (not_streamed > 0)
  {
    summary += QString("\n%1 of those tiles are not streamed in, so their spawns are loaded and "
                       "editable but not drawn.").arg(not_streamed);
  }

  _pending->setText(summary);

  auto const* cache = _map_view->databaseSpawns();

  // The case where the viewport goes completely empty and every other signal says success. The
  // grid is amber, the status line says it and the list is full -- but a user who picked a block
  // far from the camera is entitled to be told once, plainly, rather than left to work out why the
  // screen did not change.
  //
  // Only when NOTHING can be drawn: a partly visible load is described by the overlay and does not
  // need a modal. And only when something actually loaded -- picking empty tiles far away shows
  // nothing for a reason that has nothing to do with streaming, and saying "not streamed" there
  // would send the user off to fly somewhere pointlessly.
  if (not_streamed == adt_tiles.size() && cache && !cache->allEntries().empty())
  {
    QMessageBox::information
      ( this
      , "Database spawns"
      , QString("Loaded, but nothing is on screen.\n\nNone of the %1 picked tile(s) are streamed "
                "in -- the editor only renders spawns for ADTs it has loaded, which is roughly a "
                "5x5 block around the camera. The spawns are in the list, can be moved and will be "
                "in the changeset; fly the camera over the area to see them.")
          .arg(adt_tiles.size())
      );
  }
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
      , "Discard every unsaved change?\n\nMoves are forgotten, spawns you placed are removed, and "
        "spawns you deleted come back. The moved spawns stay where they are on screen until you "
        "reload -- only the pending changeset is cleared."
      , QMessageBox::Yes | QMessageBox::No
      , QMessageBox::No
      )
    );

  if (answer != QMessageBox::Yes)
  {
    return;
  }

  // Through MapView, not through the cache directly: dropping a placed spawn releases the last
  // reference to its Model, whose destructor destroys OpenGL vertex arrays and throws when no
  // context is bound -- a terminate rather than an error. MapView owns the context; this widget
  // does not.
  //
  // This is also the only undo the database spawn system has. Nothing under src/noggit/database
  // registers with ActionManager, so Ctrl+Z does not reach a spawn move, placement or deletion,
  // and this button is what puts them back.
  _map_view->discardDatabaseSpawnChanges();

  refresh();
}
