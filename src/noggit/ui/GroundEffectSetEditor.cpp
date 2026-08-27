// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/ui/GroundEffectSetEditor.hpp>

#include <noggit/Action.hpp>
#include <noggit/ActionManager.hpp>
#include <noggit/DBC.h>
#include <noggit/MapChunk.h>
#include <noggit/MapTile.h>
#include <noggit/MapView.h>
#include <noggit/World.h>
#include <noggit/World.inl>
#include <noggit/map_index.hpp>
#include <noggit/texture_set.hpp>
#include <noggit/Log.h>
#include <noggit/project/CurrentProject.hpp>

#include <ClientData.hpp>

#include <QtCore/QDateTime>
#include <QtCore/QFileInfo>
#include <QtCore/QSignalBlocker>
#include <QtCore/QStringList>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

using namespace Noggit::Ui;

namespace
{
  // New sets start here rather than at max(id)+1.
  //
  // Blizzard's GroundEffectTexture ids run well below this in 3.3.5, so a custom set is
  // recognisable on sight and a later client patch that adds rows cannot collide with one of
  // yours. The same reasoning as the 990001+ range the database fixtures use.
  constexpr std::uint32_t CUSTOM_ID_BASE = 50000;

  // GroundEffectTexture.dbc layout, from DBC.h: Doodads occupy fields 1-4 and Weights 5-8.
  constexpr std::size_t DOODAD_SLOTS = 4;

  constexpr int SCOPE_THIS_ADT = 0;
  constexpr int SCOPE_ALL_LOADED = 1;
  constexpr int SCOPE_AREA = 2;
  constexpr int SCOPE_ZONE = 3;

  // 256 chunks is exactly one ADT. Past that the apply is reaching outside the tile the user is
  // standing on, which is the point at which it stops being obvious what is about to change.
  constexpr std::size_t CONFIRM_CHUNK_THRESHOLD = 256;

  // Problems named in a warning dialog before the rest are counted rather than listed.
  constexpr int MAX_LISTED_PROBLEMS = 12;

  QString baseName(std::string const& path)
  {
    return QString::fromStdString(path).section('\\', -1).section('/', -1);
  }

  // Where DBCFile::save() puts a DBC, reconstructed rather than read back: the path is built
  // inside save() from the project root and the file's own `filename`, and both are private with
  // no accessor (DBCFile.h:120, DBCFile.cpp:68-79). Kept byte-for-byte identical to that code,
  // including the backslash separator, since normalizeFilenameUnix is what resolves it.
  QString projectDbcPath(QString const& dbc_name)
  {
    QString root (QString::fromStdString(Noggit::Project::CurrentProject::get()->ProjectPath));

    if (!(root.endsWith('\\') || root.endsWith('/')))
    {
      root += "/";
    }

    return QString::fromStdString
      ( BlizzardArchive::ClientData::normalizeFilenameUnix
          ((root + "DBFilesClient\\" + dbc_name).toStdString()));
  }

  // Empty when the file really landed, otherwise why it did not.
  //
  // DBCFile::save() opens an ofstream and never tests it (DBCFile.cpp:79-91), so a read-only
  // project directory, a full disk or a locked file all return normally and leave the caller
  // announcing a save that did not happen. Nothing in DBCFile can report this, and DBCFile is
  // not ours to change here, so the check is done against the filesystem instead.
  QString verifyDbcWritten(QString const& dbc_name, DBCFile const& dbc, QDateTime const& started)
  {
    QFileInfo const info (projectDbcPath(dbc_name));

    if (!info.exists())
    {
      return QString("%1 was not created.").arg(dbc_name);
    }

    // WDBC magic plus four uint32 header fields, then recordSize bytes per record. The string
    // table follows and is not counted -- stringSize has no accessor -- so this is a lower bound,
    // which is all that is needed to catch the truncated write a full disk produces.
    qint64 const minimum_size
      ( 20 + static_cast<qint64>(dbc.getRecordCount()) * static_cast<qint64>(dbc.getRecordSize()));

    if (info.size() < minimum_size)
    {
      return QString("%1 is %2 bytes, short of the %3 its %4 record(s) need.")
               .arg(dbc_name).arg(info.size()).arg(minimum_size).arg(dbc.getRecordCount());
    }

    // A copy left by an earlier session passes the size check even when this write was refused,
    // so the timestamp is what separates "written now" from "was already there". Two seconds of
    // slack because FAT-family filesystems store mtime at two-second granularity.
    if (info.lastModified() < started.addSecs(-2))
    {
      return QString("%1 was not modified -- an older copy from %2 is still on disk.")
               .arg(dbc_name).arg(info.lastModified().toString(Qt::ISODate));
    }

    return QString();
  }
}

GroundEffectSetEditor::GroundEffectSetEditor(MapView* map_view, QWidget* parent)
  : QDialog(parent)
  , _map_view(map_view)
{
  setWindowTitle("Ground Effect Sets");
  setMinimumSize(820, 520);

  auto outer (new QHBoxLayout(this));

  // --- left: the library -----------------------------------------------------------------
  auto left (new QVBoxLayout());
  left->addWidget(new QLabel("Effect sets in GroundEffectTexture.dbc", this));

  // GroundEffectTexture.dbc runs to tens of thousands of rows and the overwhelming majority are
  // empty, so an unfiltered list is unusable -- finding the set you just made means scrolling past
  // twenty thousand "(empty)" entries.
  _filter = new QLineEdit(this);
  _filter->setPlaceholderText("Filter by id, doodad name or zone...");
  _filter->setClearButtonEnabled(true);
  _filter->setToolTip
    ("Matches the whole row: the id, every doodad file name, and the zone the set was found in on "
     "the loaded terrain. Press Rescan below after loading more tiles to widen the zone names.");
  left->addWidget(_filter);

  _hide_empty = new QCheckBox("Hide empty sets", this);
  _hide_empty->setChecked(true);
  _hide_empty->setToolTip
    ("An empty set has no doodads and does nothing. Untick to see every row in the DBC.");
  left->addWidget(_hide_empty);

  _set_list = new QListWidget(this);
  left->addWidget(_set_list, 1);

  auto list_buttons (new QHBoxLayout());
  auto new_button (new QPushButton("New", this));
  auto duplicate_button (new QPushButton("Duplicate", this));
  auto delete_button (new QPushButton("Delete", this));
  delete_button->setToolTip
    ("Removes an unsaved set outright. A set that is already in the DBC is emptied instead -- see "
     "the message it gives you for why.");
  list_buttons->addWidget(new_button);
  list_buttons->addWidget(duplicate_button);
  list_buttons->addWidget(delete_button);
  left->addLayout(list_buttons);

  outer->addLayout(left, 1);

  // --- right: the set being edited -------------------------------------------------------
  auto right (new QVBoxLayout());

  auto doodad_box (new QGroupBox("Doodads", this));
  auto doodad_form (new QFormLayout(doodad_box));

  for (std::size_t i = 0; i < DOODAD_SLOTS; ++i)
  {
    auto row (new QHBoxLayout());

    _doodad_path[i] = new QLineEdit(this);
    _doodad_path[i]->setPlaceholderText("Tileset\\...\\SomeGrass01.m2");
    row->addWidget(_doodad_path[i], 1);

    _doodad_weight[i] = new QSpinBox(this);
    // The client picks between the slots by weight, so 0 means "never" and is a legitimate way to
    // disable one slot without clearing its path.
    _doodad_weight[i]->setRange(0, 255);
    _doodad_weight[i]->setValue(1);
    _doodad_weight[i]->setPrefix("weight ");
    _doodad_weight[i]->setToolTip
      ("Which model wins the draw, relative to the other filled slots. It does not change how many "
       "doodads there are -- that is Density, below.");
    row->addWidget(_doodad_weight[i]);

    // A raw weight is meaningless on its own: 3 is a majority against 1 and a rounding error
    // against 200. The competing fork prints a fixed 25% per slot; this recomputes as you type,
    // ignores slots with no model, and says "never" when every filled slot is weighted 0.
    _weight_percent[i] = new QLabel("--", this);
    _weight_percent[i]->setMinimumWidth(52);
    _weight_percent[i]->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    row->addWidget(_weight_percent[i]);

    doodad_form->addRow(QString("Slot %1").arg(i + 1), row);
  }

  right->addWidget(doodad_box);

  auto settings_box (new QGroupBox("Settings", this));
  auto settings_form (new QFormLayout(settings_box));

  _amount = new QSpinBox(this);
  _amount->setRange(0, 255);
  // Named apart from weight deliberately. They answer different questions and the four-slot weight
  // UI in both forks invites reading them as the same knob.
  _amount->setToolTip("How many doodads a chunk cell gets. The weights only choose which model each "
                      "one is. 0 places none at all.");
  settings_form->addRow("Density (per cell)", _amount);

  _terrain_type = new QComboBox(this);
  _terrain_type->setToolTip("Drives footstep sounds and the effect the client plays on this ground.");
  settings_form->addRow("Terrain type", _terrain_type);

  right->addWidget(settings_box);

  // --- bulk apply ------------------------------------------------------------------------
  auto bulk_box (new QGroupBox("Apply selected set to the current texture", this));
  auto bulk_layout (new QVBoxLayout(bulk_box));

  bulk_layout->addWidget
    ( new QLabel("Only layers using the chosen texture are changed. Other textures on the same\n"
                 "chunk are left alone, so giving dirt an effect leaves brick and stone untouched.\n\n"
                 "The per-cell layer map is derived from your alpha maps automatically on save,\n"
                 "so this is the only manual step.", this));

  auto scope_row (new QHBoxLayout());
  scope_row->addWidget(new QLabel("Scope", this));

  _bulk_scope = new QComboBox(this);
  _bulk_scope->addItem("This ADT tile", SCOPE_THIS_ADT);
  _bulk_scope->addItem("All loaded tiles", SCOPE_ALL_LOADED);
  _bulk_scope->addItem("Current area (subzone)", SCOPE_AREA);
  _bulk_scope->addItem("Current zone", SCOPE_ZONE);
  _bulk_scope->setToolTip
    ("Area and zone are read from each chunk's own area id, not from tile boundaries, so they stop "
     "exactly where the zone does. Both are limited to tiles that are loaded.");
  scope_row->addWidget(_bulk_scope, 1);
  bulk_layout->addLayout(scope_row);

  // The target texture is chosen HERE, from what is actually painted in scope, rather than read
  // from the Texturing tool. See refreshTextureList for why.
  auto texture_row (new QHBoxLayout());
  texture_row->addWidget(new QLabel("Texture", this));

  _target_texture = new QComboBox(this);
  _target_texture->setToolTip("Textures found on the terrain in the chosen scope.");
  _target_texture->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
  texture_row->addWidget(_target_texture, 1);

  auto rescan (new QPushButton("Rescan", this));
  rescan->setToolTip("Re-read the terrain, after painting textures or loading more tiles.");
  texture_row->addWidget(rescan);
  bulk_layout->addLayout(texture_row);

  auto bulk_row (new QHBoxLayout());
  bulk_row->addStretch(1);

  // The answer to the one genuinely dangerous button in either fork. A global apply with no way to
  // find out first what it will touch is a button you press and then hope; this reports the exact
  // tile, chunk and layer counts and writes nothing.
  auto count_button (new QPushButton("Count only", this));
  count_button->setToolTip("Report exactly what Apply would change, without changing anything.");
  bulk_row->addWidget(count_button);

  auto apply_button (new QPushButton("Apply", this));
  bulk_row->addWidget(apply_button);
  bulk_layout->addLayout(bulk_row);

  right->addWidget(bulk_box);

  _status = new QLabel(this);
  _status->setWordWrap(true);
  right->addWidget(_status);

  right->addStretch(1);

  auto save_button (new QPushButton("Save DBCs to project", this));
  save_button->setToolTip
    ("Writes GroundEffectTexture.dbc and GroundEffectDoodad.dbc into this project's "
     "DBFilesClient folder. The client installation is never modified.");
  right->addWidget(save_button);

  outer->addLayout(right, 2);

  connect(new_button, &QPushButton::clicked, this, &GroundEffectSetEditor::onNewSet);
  connect(duplicate_button, &QPushButton::clicked, this, &GroundEffectSetEditor::onDuplicateSet);
  connect(delete_button, &QPushButton::clicked, this, &GroundEffectSetEditor::onDeleteSet);
  connect(save_button, &QPushButton::clicked, this, &GroundEffectSetEditor::onSaveDbc);
  connect(apply_button, &QPushButton::clicked, this, &GroundEffectSetEditor::onBulkApply);
  connect(count_button, &QPushButton::clicked, this, &GroundEffectSetEditor::onBulkCount);

  connect(_set_list, &QListWidget::currentRowChanged, this, [this] (int) { showSelected(); });
  connect(_filter, &QLineEdit::textChanged, this, [this] (QString const&) { rebuildList(); });
  connect(_hide_empty, &QCheckBox::toggled, this, [this] (bool) { rebuildList(); });
  connect(rescan, &QPushButton::clicked, this, [this] { refreshTextureList(); });

  for (std::size_t i = 0; i < DOODAD_SLOTS; ++i)
  {
    connect(_doodad_weight[i], qOverload<int>(&QSpinBox::valueChanged), this
           , [this] (int) { updateWeightPercentages(); });
    connect(_doodad_path[i], &QLineEdit::textChanged, this
           , [this] (QString const&) { updateWeightPercentages(); });
  }

  // Rescanned when the scope changes, because "all loaded tiles" can contain textures this tile
  // does not, and offering a texture that is not in scope would put the old confusing
  // "nothing changed" result straight back.
  connect(_bulk_scope, qOverload<int>(&QComboBox::currentIndexChanged), this
         , [this] (int) { refreshTextureList(); });

  reloadFromDbc();
  refreshTextureList();
}

void GroundEffectSetEditor::showSet(std::uint32_t id)
{
  selectSetById(id);
}

void GroundEffectSetEditor::refreshTextureList()
{
  // Counted per texture so the list can say how much terrain each one covers -- with several
  // similar tileset names, the layer count is usually what identifies the one you mean.
  //
  // The scan lives on MapView so the dev bridge's `textures` command answers from the same code,
  // which is what lets this be checked without a human at the window.
  auto const layers_by_texture
    (_map_view->terrainTexturesInScope(_bulk_scope->currentData().toInt() != SCOPE_THIS_ADT));

  // Preserved so a rescan after painting does not silently retarget the Apply button.
  QString const previous (_target_texture->currentData().toString());

  _target_texture->clear();

  for (auto const& entry : layers_by_texture)
  {
    QString const full (QString::fromStdString(entry.first));

    _target_texture->addItem
      ( QString("%1  (%2 layer%3)")
          .arg(full.section('\\', -1).section('/', -1))
          .arg(entry.second)
          .arg(entry.second == 1 ? "" : "s")
      , full
      );
  }

  // The same pass rebuilds the zone index, because it is the same walk over the same chunks and
  // doing it twice would double the cost of every Rescan.
  std::size_t named_sets = 0;
  {
    _zone_by_set_id.clear();

    std::unordered_map<int, std::string> area_names;

    if (World* world = _map_view->getWorld())
    {
      for (MapTile* tile : world->mapIndex.loaded_tiles())
      {
        for (int z = 0; z < 16; ++z)
        {
          for (int x = 0; x < 16; ++x)
          {
            MapChunk* chunk = tile->getChunk(static_cast<unsigned>(x), static_cast<unsigned>(z));

            if (!chunk || !chunk->texture_set)
            {
              continue;
            }

            int const area_id = chunk->getAreaID();

            for (std::size_t layer = 0; layer < chunk->texture_set->num(); ++layer)
            {
              std::uint32_t const effect = chunk->texture_set->effect(layer);

              if (!effect || effect == 0xFFFFFFFFu || _zone_by_set_id.count(effect))
              {
                continue;
              }

              // AreaDB::getAreaFullName resolves through DBCFile::getByID, which is a linear scan
              // (DBCFile.cpp:144-153) over roughly 2,700 rows in 3.3.5. Twenty-five loaded tiles
              // are 6,400 chunks, so calling it per chunk would be about 17 million row
              // comparisons for a list of names that repeats a handful of times.
              auto const cached = area_names.find(area_id);

              if (cached == area_names.end())
              {
                area_names[area_id] = AreaDB::getAreaFullName(area_id);
              }

              _zone_by_set_id[effect] = area_names[area_id];
              ++named_sets;
            }
          }
        }
      }
    }
  }

  rebuildList();

  if (_target_texture->count() == 0)
  {
    _status->setText("No terrain textures found in that scope. Is the tile loaded?");
    return;
  }

  int index = _target_texture->findData(previous);

  // Falling back to the Texturing tool's selection is a convenience, not the source of truth --
  // it is only honoured when that texture is genuinely present in scope.
  if (index < 0)
  {
    index = _target_texture->findData(QString::fromStdString(_map_view->selectedTexturePath()));
  }

  _target_texture->setCurrentIndex(index < 0 ? 0 : index);

  _status->setText
    ( QString("%1 texture(s) on the terrain in scope. %2 effect set(s) matched to a zone name.")
        .arg(_target_texture->count()).arg(named_sets));
}

void GroundEffectSetEditor::reloadFromDbc()
{
  _sets.clear();
  _unresolved_doodads = 0;

  for (DBCFile::Iterator it = gGroundEffectTextureDB.begin();
       it != gGroundEffectTextureDB.end(); ++it)
  {
    EffectSet set;
    set.id = it->getUInt(GroundEffectTextureDB::ID);
    set.amount = it->getUInt(GroundEffectTextureDB::Amount);
    set.terrain_type = it->getUInt(GroundEffectTextureDB::TerrainType);

    for (std::size_t i = 0; i < DOODAD_SLOTS; ++i)
    {
      set.doodad_ids[i] = it->getUInt(GroundEffectTextureDB::Doodads + i);
      set.weights[i] = it->getUInt(GroundEffectTextureDB::Weights + i);

      // A doodad id of 0 means the slot is unused. Resolving it would throw.
      if (set.doodad_ids[i] != 0 && gGroundEffectDoodadDB.CheckIfIdExists(set.doodad_ids[i]))
      {
        set.doodad_files[i]
          = gGroundEffectDoodadDB.getByID(set.doodad_ids[i]).getString(GroundEffectDoodadDB::Filename);
      }
      else if (set.doodad_ids[i] != 0)
      {
        // A reference to a GroundEffectDoodad row this DBC does not have. It cannot be shown as a
        // path and it must not be silently dropped: the save path used to zero the id of every
        // slot whose file name was empty, which turned an incomplete GroundEffectDoodad.dbc into
        // permanent data loss across the whole file the first time anyone pressed Save.
        set.doodad_unresolved[i] = true;
        ++_unresolved_doodads;
      }
    }

    _sets.push_back(std::move(set));
  }

  std::sort(_sets.begin(), _sets.end()
           , [] (EffectSet const& a, EffectSet const& b) { return a.id < b.id; });

  rebuildList();

  // Populated once from TerrainType.dbc rather than hardcoded: the ids are client data and a
  // custom client may well have added to them.
  if (_terrain_type->count() == 0)
  {
    _terrain_type->addItem("0  (none)", 0);

    for (DBCFile::Iterator it = gTerrainTypeDB.begin(); it != gTerrainTypeDB.end(); ++it)
    {
      unsigned int const id = it->getUInt(TerrainTypeDB::TerrainId);

      _terrain_type->addItem
        (QString("%1  %2").arg(id).arg(it->getString(TerrainTypeDB::TerrainDesc)), id);
    }
  }

  if (!_sets.empty())
  {
    _set_list->setCurrentRow(0);
  }

  QString message (QString("%1 set(s) loaded.").arg(_sets.size()));

  if (_unresolved_doodads)
  {
    message += QString(" %1 doodad slot(s) point at a GroundEffectDoodad row this project does not "
                       "have; their ids are preserved as they are.").arg(_unresolved_doodads);
  }

  _status->setText(message);
}

QString GroundEffectSetEditor::summariseSet(EffectSet const& set) const
{
  QStringList names;

  for (std::size_t i = 0; i < DOODAD_SLOTS; ++i)
  {
    if (set.doodad_files[i].empty())
    {
      if (set.doodad_unresolved[i])
      {
        names << QString("#%1?").arg(set.doodad_ids[i]);
      }

      continue;
    }

    names << baseName(set.doodad_files[i]);
  }

  QString label
    ( QString("%1  %2").arg(set.id, 6).arg(names.isEmpty() ? QString("(empty)") : names.join(", ")));

  auto const zone = _zone_by_set_id.find(set.id);

  if (zone != _zone_by_set_id.end() && !zone->second.empty())
  {
    label += "  -  " + QString::fromStdString(zone->second);
  }

  return label;
}

void GroundEffectSetEditor::rebuildList()
{
  int const previously_selected = selectedSetIndex();

  // Blocked so the clear/refill does not fire currentRowChanged for every intermediate state,
  // which would run showSelected against a half-built list.
  QSignalBlocker const blocker (_set_list);

  _set_list->clear();

  QString const needle (_filter->text().trimmed().toLower());
  bool const hide_empty = _hide_empty->isChecked();

  std::size_t shown = 0;

  for (std::size_t i = 0; i < _sets.size(); ++i)
  {
    EffectSet const& set = _sets[i];

    bool const empty
      ( set.doodad_files[0].empty() && set.doodad_files[1].empty()
     && set.doodad_files[2].empty() && set.doodad_files[3].empty()
     && !set.doodad_unresolved[0] && !set.doodad_unresolved[1]
     && !set.doodad_unresolved[2] && !set.doodad_unresolved[3]);

    // An empty set is always shown when it is the one being edited, otherwise creating a set and
    // filling it in would make it vanish from under the cursor on the first keystroke.
    if (hide_empty && empty && static_cast<int>(i) != previously_selected)
    {
      continue;
    }

    QString const label (summariseSet(set));

    if (!needle.isEmpty() && !label.toLower().contains(needle))
    {
      continue;
    }

    auto* item = new QListWidgetItem(label, _set_list);
    item->setData(Qt::UserRole, static_cast<int>(i));

    if (static_cast<int>(i) == previously_selected)
    {
      _set_list->setCurrentItem(item);
    }

    ++shown;
  }

  _status->setText
    ( QString("%1 of %2 set(s) shown.").arg(shown).arg(_sets.size()));
}

void GroundEffectSetEditor::selectSetById(std::uint32_t id)
{
  // A set the filter hides cannot be selected, so a brand new empty set would be created and
  // then be unreachable. Clearing the filter is the honest fix: the user asked for this set to
  // exist, so it has to be the one in front of them.
  for (int row = 0; row < _set_list->count(); ++row)
  {
    auto* item = _set_list->item(row);
    int const index = item->data(Qt::UserRole).toInt();

    if (static_cast<std::size_t>(index) < _sets.size() && _sets[index].id == id)
    {
      _set_list->setCurrentItem(item);
      showSelected();
      return;
    }
  }

  _filter->clear();
  _hide_empty->setChecked(false);
  rebuildList();

  for (int row = 0; row < _set_list->count(); ++row)
  {
    auto* item = _set_list->item(row);
    int const index = item->data(Qt::UserRole).toInt();

    if (static_cast<std::size_t>(index) < _sets.size() && _sets[index].id == id)
    {
      _set_list->setCurrentItem(item);
      showSelected();
      return;
    }
  }
}

int GroundEffectSetEditor::selectedSetIndex() const
{
  auto const* item = _set_list->currentItem();

  if (!item)
  {
    return -1;
  }

  bool ok = false;
  int const index = item->data(Qt::UserRole).toInt(&ok);

  return ok ? index : -1;
}

void GroundEffectSetEditor::showSelected()
{
  int const row = selectedSetIndex();

  if (row < 0 || static_cast<std::size_t>(row) >= _sets.size())
  {
    return;
  }

  EffectSet const& set = _sets[static_cast<std::size_t>(row)];

  for (std::size_t i = 0; i < DOODAD_SLOTS; ++i)
  {
    _doodad_path[i]->setText(QString::fromStdString(set.doodad_files[i]));
    _doodad_weight[i]->setValue(static_cast<int>(set.weights[i]));
  }

  _amount->setValue(static_cast<int>(set.amount));

  int const terrain_index = _terrain_type->findData(set.terrain_type);
  _terrain_type->setCurrentIndex(terrain_index < 0 ? 0 : terrain_index);

  updateWeightPercentages();
}

void GroundEffectSetEditor::updateWeightPercentages()
{
  // Read from the widgets rather than from the selected set, so the numbers move while the spin
  // box is being edited instead of only after the edit is committed back.
  unsigned int total = 0;

  for (std::size_t i = 0; i < DOODAD_SLOTS; ++i)
  {
    if (!_doodad_path[i]->text().trimmed().isEmpty())
    {
      total += static_cast<unsigned int>(_doodad_weight[i]->value());
    }
  }

  for (std::size_t i = 0; i < DOODAD_SLOTS; ++i)
  {
    if (_doodad_path[i]->text().trimmed().isEmpty())
    {
      _weight_percent[i]->setText("--");
    }
    else if (total == 0)
    {
      // Every filled slot is weighted 0, so the client can never pick any of them. Printing 0.0%
      // four times would read as a rounding artefact rather than as a set that draws nothing.
      _weight_percent[i]->setText("never");
    }
    else
    {
      _weight_percent[i]->setText
        (QString::number(100.0 * _doodad_weight[i]->value() / total, 'f', 1) + "%");
    }
  }
}

void GroundEffectSetEditor::applyEditsToSelected()
{
  int const row = selectedSetIndex();

  if (row < 0 || static_cast<std::size_t>(row) >= _sets.size())
  {
    return;
  }

  EffectSet& set = _sets[static_cast<std::size_t>(row)];

  for (std::size_t i = 0; i < DOODAD_SLOTS; ++i)
  {
    set.doodad_files[i] = _doodad_path[i]->text().trimmed().toStdString();
    set.weights[i] = static_cast<std::uint32_t>(_doodad_weight[i]->value());

    // Typing a path over an unresolvable id resolves the slot: the save path will now find or
    // create a GroundEffectDoodad row for it, and the old dangling id is what the user replaced.
    if (!set.doodad_files[i].empty())
    {
      set.doodad_unresolved[i] = false;
    }
  }

  set.amount = static_cast<std::uint32_t>(_amount->value());
  set.terrain_type = static_cast<std::uint32_t>(_terrain_type->currentData().toUInt());

  if (auto* item = _set_list->currentItem())
  {
    item->setText(summariseSet(set));
  }
}

std::uint32_t GroundEffectSetEditor::nextFreeId(DBCFile& dbc) const
{
  std::uint32_t candidate = CUSTOM_ID_BASE;

  while (dbc.CheckIfIdExists(candidate))
  {
    ++candidate;
  }

  return candidate;
}

std::uint32_t GroundEffectSetEditor::nextFreeSetId() const
{
  auto const taken_in_session = [this] (std::uint32_t id)
  {
    return std::any_of (_sets.begin(), _sets.end()
                       , [id] (EffectSet const& set) { return set.id == id; });
  };

  // Both conditions are rechecked after every bump: skipping a session id can land the candidate
  // back on a DBC row, so testing the two sources in sequence rather than in one loop would let
  // a collision through.
  std::uint32_t candidate = CUSTOM_ID_BASE;

  while (gGroundEffectTextureDB.CheckIfIdExists(candidate) || taken_in_session(candidate))
  {
    ++candidate;
  }

  return candidate;
}

void GroundEffectSetEditor::onNewSet()
{
  applyEditsToSelected();

  EffectSet set;

  // Not nextFreeId: an unsaved set is not in the DBC yet, so two presses of New would both be
  // given the first free DBC id. onSaveDbc would then take the addRecord branch twice for that
  // id and DBCFile::addRecord throws AlreadyExists on the second (DBCFile.cpp:187), losing the
  // whole save.
  set.id = nextFreeSetId();

  _sets.push_back(set);

  // Rebuilt rather than appended, because the filter decides whether the new row is visible at
  // all -- appending straight to the widget would put it on screen while the filter says it
  // should not be, and the next keystroke would make it disappear.
  rebuildList();
  selectSetById(set.id);

  _status->setText(QString("New set %1. Fill in at least one doodad, then save.").arg(set.id));
}

void GroundEffectSetEditor::onDuplicateSet()
{
  applyEditsToSelected();

  int const row = selectedSetIndex();

  if (row < 0 || static_cast<std::size_t>(row) >= _sets.size())
  {
    return;
  }

  EffectSet set = _sets[static_cast<std::size_t>(row)];

  // Replaces a bump-on-collision loop that only worked by accident: it incremented past each
  // clashing id without rechecking the ids it had already passed, so it could still land on one.
  set.id = nextFreeSetId();

  _sets.push_back(set);

  rebuildList();
  selectSetById(set.id);

  _status->setText(QString("Duplicated into set %1.").arg(set.id));
}

std::size_t GroundEffectSetEditor::countReferences(std::uint32_t effect_id) const
{
  std::size_t references = 0;

  World* world = _map_view->getWorld();

  if (!world)
  {
    return 0;
  }

  for (MapTile* tile : world->mapIndex.loaded_tiles())
  {
    for (int z = 0; z < 16; ++z)
    {
      for (int x = 0; x < 16; ++x)
      {
        MapChunk* chunk = tile->getChunk(static_cast<unsigned>(x), static_cast<unsigned>(z));

        if (!chunk || !chunk->texture_set)
        {
          continue;
        }

        for (std::size_t layer = 0; layer < chunk->texture_set->num(); ++layer)
        {
          if (chunk->texture_set->effect(layer) == effect_id)
          {
            ++references;
          }
        }
      }
    }
  }

  return references;
}

void GroundEffectSetEditor::onDeleteSet()
{
  applyEditsToSelected();

  int const row = selectedSetIndex();

  if (row < 0 || static_cast<std::size_t>(row) >= _sets.size())
  {
    _status->setText("Select a set first.");
    return;
  }

  std::uint32_t const id = _sets[static_cast<std::size_t>(row)].id;

  std::size_t const references = countReferences(id);
  bool const in_dbc = gGroundEffectTextureDB.CheckIfIdExists(id);

  if (references)
  {
    auto const answer
      ( QMessageBox::warning
          ( this
          , "Ground Effect Sets"
          , QString("%1 layer(s) on the loaded terrain still point at set %2. Removing it leaves "
                    "them referencing an id that places nothing.\n\nOnly the loaded tiles were "
                    "counted; unloaded ones may reference it too.\n\nContinue?")
              .arg(references).arg(id)
          , QMessageBox::Yes | QMessageBox::Cancel
          , QMessageBox::Cancel
          )
      );

    if (answer != QMessageBox::Yes)
    {
      _status->setText("Cancelled. Nothing was changed.");
      return;
    }
  }

  if (!in_dbc)
  {
    // Never written, so it exists only in this list and dropping it costs nothing.
    _sets.erase(_sets.begin() + row);
    rebuildList();
    showSelected();

    _status->setText(QString("Set %1 discarded. It had not been saved.").arg(id));
    return;
  }

  // Emptied rather than physically removed, and this is a deliberate refusal to use
  // DBCFile::removeRecord.
  //
  // That function memmoves recordSize * (recordCount - row) bytes from one record past the row
  // being dropped (DBCFile.cpp:254, :271). `data` holds records and nothing else -- the string
  // table is a separate vector (DBCFile.cpp:56-60) -- so the source range ends at
  // recordSize * (recordCount + 1), which is exactly one record beyond the end of the buffer. The
  // surviving bytes come out right only because the resize on the next line truncates the garbage
  // it copied in; the READ is a heap overrun on every call. The correct length is
  // recordSize * (recordCount - row - 1).
  //
  // An all-zero row is also a better answer for the file: DBC records are addressed by id, not by
  // position, so shrinking the file buys nothing, while an emptied set places no doodads -- which
  // is precisely what deleting one is for.
  EffectSet& set = _sets[static_cast<std::size_t>(row)];

  for (std::size_t i = 0; i < DOODAD_SLOTS; ++i)
  {
    set.doodad_files[i].clear();
    set.doodad_ids[i] = 0;
    set.doodad_unresolved[i] = false;
    set.weights[i] = 0;
  }

  set.amount = 0;
  set.terrain_type = 0;

  rebuildList();
  showSelected();

  _status->setText
    ( QString("Set %1 emptied. It stays in the DBC as a row that places nothing -- rows are found "
              "by id, not by position, so removing one would gain nothing and Noggit's own "
              "DBCFile::removeRecord reads a record past the end of its buffer. Save to write it.")
        .arg(id));
}

QStringList GroundEffectSetEditor::validateSets() const
{
  QStringList fatal;

  std::unordered_set<std::uint32_t> seen;

  // Only what would corrupt the write itself is fatal, and a duplicate id is the whole of that
  // list. Deliberately not "id 0" or "no doodads": _sets holds every row that was read out of the
  // DBC, so any rule applied to all of them can be tripped by a stock Blizzard row and would then
  // block every save the user ever makes for something that is not theirs and does no harm -- an
  // existing id, 0 included, takes the getByID branch and is simply edited in place.
  for (EffectSet const& set : _sets)
  {
    if (!seen.insert(set.id).second)
    {
      // Two rows claiming one id cannot both survive. If the id is new to the DBC, addRecord
      // throws AlreadyExists on the second (DBCFile.cpp:187) -- and throws after the sets earlier
      // in the loop have already been committed to the in-memory DBC, with no way back. If the id
      // is already there, both take the getByID branch and the second silently overwrites the
      // first. Neither is worth finding out about afterwards.
      fatal << QString("Set id %1 appears twice in the list. One of them would be lost -- delete "
                       "or renumber it first.").arg(set.id);
    }
  }

  return fatal;
}

void GroundEffectSetEditor::onSaveDbc()
{
  applyEditsToSelected();

  // Run to completion BEFORE the first addRecord. onSaveDbc mutates two process-wide DBCFile
  // objects in place and there is no rollback; refusing at the first write leaves half the sets
  // committed to memory and the other half not.
  QStringList const fatal (validateSets());

  if (!fatal.isEmpty())
  {
    QString const message
      ( QString("Nothing was written. Fix these first:\n\n%1").arg(fatal.join("\n")));

    _status->setText(message);
    QMessageBox::critical(this, "Ground Effect Sets", message);
    return;
  }

  // Content problems on sets this tool minted. Not fatal, because _sets holds every row in the
  // DBC and a stock client row is not this user's problem -- so only the custom range is checked,
  // and the user is asked rather than refused.
  QStringList warnings;

  for (EffectSet const& set : _sets)
  {
    if (set.id < CUSTOM_ID_BASE)
    {
      continue;
    }

    bool any_model = false;
    unsigned int total_weight = 0;

    for (std::size_t i = 0; i < DOODAD_SLOTS; ++i)
    {
      if (set.doodad_files[i].empty())
      {
        continue;
      }

      any_model = true;
      total_weight += set.weights[i];

      QString const lowered (QString::fromStdString(set.doodad_files[i]).toLower());

      if (!lowered.endsWith(".m2") && !lowered.endsWith(".mdx") && !lowered.endsWith(".mdl"))
      {
        warnings << QString("Set %1 slot %2: \"%3\" does not end in .m2, .mdx or .mdl.")
                      .arg(set.id).arg(i + 1).arg(QString::fromStdString(set.doodad_files[i]));
      }
    }

    if (any_model && total_weight == 0)
    {
      warnings << QString("Set %1 has models but every weight is 0, so the client draws none of them.")
                    .arg(set.id);
    }
  }

  if (!warnings.isEmpty())
  {
    QStringList listed (warnings.mid(0, MAX_LISTED_PROBLEMS));

    if (warnings.size() > MAX_LISTED_PROBLEMS)
    {
      listed << QString("...and %1 more.").arg(warnings.size() - MAX_LISTED_PROBLEMS);
    }

    auto const answer
      ( QMessageBox::warning
          ( this
          , "Ground Effect Sets"
          , QString("These sets will be written as they are:\n\n%1\n\nSave anyway?")
              .arg(listed.join("\n"))
          , QMessageBox::Yes | QMessageBox::Cancel
          , QMessageBox::Cancel
          )
      );

    if (answer != QMessageBox::Yes)
    {
      _status->setText("Nothing was written.");
      return;
    }
  }

  std::size_t written_doodads = 0;
  std::size_t written_sets = 0;

  // Taken before the save so the mtime check below has something to compare against.
  QDateTime const started (QDateTime::currentDateTime());

  try
  {
    // Built once instead of scanning GroundEffectDoodad.dbc from the top for every filled slot of
    // every set. getByID and the old inner loop are both linear (DBCFile.cpp:144-153), so with the
    // roughly 2,000 doodad rows and 3,000 sets a stock 3.3.5 DBC pair holds, the old shape was on
    // the order of 12 million string comparisons per save.
    std::unordered_map<std::string, std::uint32_t> doodad_id_by_file;

    for (DBCFile::Iterator it = gGroundEffectDoodadDB.begin();
         it != gGroundEffectDoodadDB.end(); ++it)
    {
      doodad_id_by_file.emplace(it->getString(GroundEffectDoodadDB::Filename)
                               , it->getUInt(GroundEffectDoodadDB::ID));
    }

    for (auto& set : _sets)
    {
      // Each named file needs a GroundEffectDoodad row. Reuse one when the same file is already
      // in the DBC -- otherwise every save would grow the file with duplicates of the same path.
      for (std::size_t i = 0; i < DOODAD_SLOTS; ++i)
      {
        if (set.doodad_files[i].empty())
        {
          // An id whose GroundEffectDoodad row this project does not have keeps its value. Zeroing
          // it -- which is what this branch used to do unconditionally -- destroys the only record
          // of what the slot pointed at, for every such slot in the whole file, on the first save.
          if (!set.doodad_unresolved[i])
          {
            set.doodad_ids[i] = 0;
          }

          continue;
        }

        auto const known = doodad_id_by_file.find(set.doodad_files[i]);
        std::uint32_t found = (known == doodad_id_by_file.end()) ? 0 : known->second;

        if (found == 0)
        {
          found = nextFreeId(gGroundEffectDoodadDB);

          DBCFile::Record record (gGroundEffectDoodadDB.addRecord(found));
          record.writeString(GroundEffectDoodadDB::Filename, set.doodad_files[i]);
          record.write<std::uint32_t>(GroundEffectDoodadDB::Flags, 0);

          doodad_id_by_file.emplace(set.doodad_files[i], found);
          ++written_doodads;
        }

        set.doodad_ids[i] = found;
      }

      // An existing row is edited in place; only genuinely new sets are appended.
      bool const exists = gGroundEffectTextureDB.CheckIfIdExists(set.id);

      DBCFile::Record record
        ( exists ? gGroundEffectTextureDB.getByID(set.id)
                 : gGroundEffectTextureDB.addRecord(set.id)
        );

      // Every field of the record is written, including the ones that did not change. addRecord
      // resizes `data` and stamps only the id (DBCFile.cpp:190-192), so a record that skipped a
      // field would inherit whatever the vector's growth left there.
      for (std::size_t i = 0; i < DOODAD_SLOTS; ++i)
      {
        record.write<std::uint32_t>(GroundEffectTextureDB::Doodads + i, set.doodad_ids[i]);
        record.write<std::uint32_t>(GroundEffectTextureDB::Weights + i, set.weights[i]);
      }

      record.write<std::uint32_t>(GroundEffectTextureDB::Amount, set.amount);
      record.write<std::uint32_t>(GroundEffectTextureDB::TerrainType, set.terrain_type);

      if (!exists)
      {
        ++written_sets;
      }
    }

    gGroundEffectDoodadDB.save();
    gGroundEffectTextureDB.save();
  }
  catch (std::exception const& e)
  {
    // Writing a DBC touches a shared global, so a half-written state is worth naming loudly
    // rather than leaving the user to wonder which of the two files landed.
    LogError << "Saving ground effect DBCs failed: " << e.what() << std::endl;

    QMessageBox::critical
      ( this
      , "Ground Effect Sets"
      , QString("Could not save the DBCs.\n\n%1\n\nReload the project before editing further.")
          .arg(e.what())
      );
    return;
  }

  QStringList problems;

  for (QString const& problem : { verifyDbcWritten("GroundEffectDoodad.dbc", gGroundEffectDoodadDB, started)
                                , verifyDbcWritten("GroundEffectTexture.dbc", gGroundEffectTextureDB, started)
                                })
  {
    if (!problem.isEmpty())
    {
      problems << problem;
    }
  }

  if (!problems.isEmpty())
  {
    // Deliberately not reloading: the rows are in the in-memory DBCs and in _sets, so leaving both
    // alone is what lets the user fix the permissions or free the disk and press Save again.
    QString const failure
      ( QString("The save did not land on disk.\n\n%1\n\nYour edits are still in memory. Fix the "
                "problem and save again -- closing Noggit now loses them.")
          .arg(problems.join("\n")));

    LogError << failure.toStdString() << std::endl;

    _status->setText(failure);
    QMessageBox::critical(this, "Ground Effect Sets", failure);
    return;
  }

  QString const message
    ( QString("Saved. %1 new set(s), %2 new doodad row(s), written to the project's "
              "DBFilesClient folder.").arg(written_sets).arg(written_doodads));

  _status->setText(message);
  Log << message.toStdString() << std::endl;

  reloadFromDbc();
}

GroundEffectSetEditor::BulkScope GroundEffectSetEditor::resolveScope() const
{
  BulkScope scope;

  World* world = _map_view->getWorld();

  if (!world)
  {
    scope.problem = "No world is loaded.";
    return scope;
  }

  int const mode = _bulk_scope->currentData().toInt();

  if (mode == SCOPE_THIS_ADT)
  {
    MapTile* tile = world->mapIndex.getTile(::TileIndex(_map_view->cameraPosition()));

    if (!tile || !tile->finishedLoading())
    {
      scope.problem = "The tile under the camera is not loaded.";
      return scope;
    }

    scope.tiles.push_back(tile);
    scope.description = QString("ADT %1, %2").arg(tile->index.x).arg(tile->index.z);
    return scope;
  }

  // loaded_tiles() already filters on finishedLoading() (map_index.cpp:29-33).
  for (MapTile* tile : world->mapIndex.loaded_tiles())
  {
    scope.tiles.push_back(tile);
  }

  if (scope.tiles.empty())
  {
    scope.problem = "No tile is loaded.";
    return scope;
  }

  if (mode == SCOPE_ALL_LOADED)
  {
    scope.description = QString("%1 loaded tile(s)").arg(scope.tiles.size());
    return scope;
  }

  // for_maybe_chunk_at rather than for_chunk_at: the latter calls mapIndex.setChanged on the tile
  // it visits (World.inl:53), which would mark a tile unsaved just for asking what area the camera
  // is standing in.
  auto const area_under_camera
    ( world->for_maybe_chunk_at
        ( _map_view->cameraPosition()
        , [] (MapChunk* chunk) { return chunk ? chunk->getAreaID() : 0; }));

  if (!area_under_camera.has_value() || area_under_camera.value() <= 0)
  {
    scope.problem = "The chunk under the camera has no area id, so there is no area or zone to "
                    "scope to. Assign one with the Area tool first.";
    return scope;
  }

  std::uint32_t const area_id = static_cast<std::uint32_t>(area_under_camera.value());

  if (mode == SCOPE_AREA)
  {
    scope.filter_by_area = true;
    scope.area_id = area_id;
    scope.description
      = QString("area %1 (%2) across %3 loaded tile(s)")
          .arg(area_id)
          .arg(QString::fromStdString(AreaDB::getAreaFullName(static_cast<int>(area_id))))
          .arg(scope.tiles.size());
    return scope;
  }

  // AreaTable's Region field is the parent zone; it is 0 for a row that is already a zone
  // (DBC.cpp:161-177), in which case the area under the camera IS the zone.
  std::uint32_t const parent = AreaDB::get_area_parent(static_cast<int>(area_id));

  scope.filter_by_zone = true;
  scope.zone_id = parent ? parent : area_id;
  scope.description
    = QString("zone %1 (%2) across %3 loaded tile(s)")
        .arg(scope.zone_id)
        .arg(QString::fromStdString(AreaDB::getAreaFullName(static_cast<int>(scope.zone_id))))
        .arg(scope.tiles.size());

  return scope;
}

bool GroundEffectSetEditor::chunkInScope(BulkScope const& scope, MapChunk* chunk) const
{
  if (!chunk)
  {
    return false;
  }

  if (!scope.filter_by_area && !scope.filter_by_zone)
  {
    return true;
  }

  int const area_id = chunk->getAreaID();

  if (area_id <= 0)
  {
    return false;
  }

  if (scope.filter_by_area)
  {
    return static_cast<std::uint32_t>(area_id) == scope.area_id;
  }

  if (static_cast<std::uint32_t>(area_id) == scope.zone_id)
  {
    return true;
  }

  // Cached because get_area_parent goes through DBCFile::getByID, a linear scan over roughly 2,700
  // AreaTable rows (DBCFile.cpp:144-153). Twenty-five loaded tiles are 6,400 chunks, so an
  // uncached zone scope would be about 17 million row comparisons for an answer that repeats a few
  // dozen times.
  auto const cached = _area_parent_cache.find(area_id);

  if (cached != _area_parent_cache.end())
  {
    return cached->second == scope.zone_id;
  }

  std::uint32_t const parent = AreaDB::get_area_parent(area_id);
  _area_parent_cache[area_id] = parent;

  return parent == scope.zone_id;
}

GroundEffectSetEditor::BulkTally GroundEffectSetEditor::tally
  (BulkScope const& scope, std::string const& texture, std::uint32_t effect_id) const
{
  BulkTally result;

  for (MapTile* tile : scope.tiles)
  {
    bool tile_counted = false;

    for (int z = 0; z < 16; ++z)
    {
      for (int x = 0; x < 16; ++x)
      {
        MapChunk* chunk = tile->getChunk(static_cast<unsigned>(x), static_cast<unsigned>(z));

        if (!chunk || !chunk->texture_set || !chunkInScope(scope, chunk))
        {
          continue;
        }

        std::size_t layers_here = 0;

        for (std::size_t layer = 0; layer < chunk->texture_set->num(); ++layer)
        {
          if (chunk->texture_set->filename(layer) != texture)
          {
            continue;
          }

          // A layer that already carries this effect is not a change and must not be counted as
          // one, or the confirmation would overstate what is about to happen.
          if (chunk->texture_set->effect(layer) == effect_id)
          {
            continue;
          }

          ++layers_here;
        }

        if (layers_here)
        {
          result.layers += layers_here;
          ++result.chunks;

          if (!tile_counted)
          {
            ++result.tiles;
            tile_counted = true;
          }
        }
      }
    }
  }

  return result;
}

void GroundEffectSetEditor::onBulkCount()
{
  applyEditsToSelected();

  int const row = selectedSetIndex();

  if (row < 0 || static_cast<std::size_t>(row) >= _sets.size())
  {
    _status->setText("Select a set first.");
    return;
  }

  std::uint32_t const effect_id = _sets[static_cast<std::size_t>(row)].id;
  std::string const texture (_target_texture->currentData().toString().toStdString());

  if (texture.empty())
  {
    _status->setText("No texture chosen. Press Rescan to read the terrain in scope.");
    return;
  }

  BulkScope const scope (resolveScope());

  if (!scope.problem.isEmpty())
  {
    _status->setText(scope.problem);
    return;
  }

  BulkTally const planned (tally(scope, texture, effect_id));

  _status->setText
    ( QString("Apply would set %1 on %2 layer(s) across %3 chunk(s) in %4 tile(s), within %5. "
              "Nothing has been changed.")
        .arg(effect_id).arg(planned.layers).arg(planned.chunks).arg(planned.tiles)
        .arg(scope.description));
}

void GroundEffectSetEditor::onBulkApply()
{
  applyEditsToSelected();

  // beginAction returns the ALREADY RUNNING action when one exists and does not apply the flags it
  // was passed (ActionManager.cpp:64-65), so a paired endAction() would close somebody else's
  // action and file these chunk edits under it. Refuse rather than nest.
  if (NOGGIT_CUR_ACTION)
  {
    _status->setText
      ("Another edit is still in progress. Release the mouse button, or finish the current tool "
       "stroke, and apply again.");
    return;
  }

  // selectedSetIndex, not currentRow: with the filter or "hide empty" on, the row number and the
  // _sets index are different numbers, and currentRow would apply whichever set happens to sit at
  // that offset in the unfiltered vector rather than the one on screen.
  int const row = selectedSetIndex();

  if (row < 0 || static_cast<std::size_t>(row) >= _sets.size())
  {
    _status->setText("Select a set first.");
    return;
  }

  std::uint32_t const effect_id = _sets[static_cast<std::size_t>(row)].id;

  std::string const texture (_target_texture->currentData().toString().toStdString());

  if (texture.empty())
  {
    _status->setText("No texture chosen. Press Rescan to read the terrain in scope.");
    return;
  }

  BulkScope const scope (resolveScope());

  if (!scope.problem.isEmpty())
  {
    _status->setText(scope.problem);
    return;
  }

  BulkTally const planned (tally(scope, texture, effect_id));

  if (planned.layers == 0)
  {
    _status->setText
      (QString("No layer uses \"%1\" in %2 without already carrying set %3, so nothing changed.")
         .arg(QString::fromStdString(texture)).arg(scope.description).arg(effect_id));
    return;
  }

  World* world = _map_view->getWorld();

  // Pin every tile in scope BEFORE the confirmation below, because QMessageBox::exec is an event
  // loop and MapView's frame timer keeps firing through it: paintGL calls tick(), tick() calls
  // MapIndex::unloadTiles, and unloadTiles deletes any loaded tile far enough from the camera --
  // sparing only tiles whose `changed` flag is set (map_index.cpp:470). Every MapTile* held here,
  // and every MapChunk* an undo action is about to store, is live across that.
  //
  // `pinned_by_us` is remembered so the flag comes back off tiles this call turns out not to
  // touch, including the whole scope when the user backs out.
  struct ScopeTile
  {
    MapTile* tile;
    ::TileIndex index;
    bool pinned_by_us;
    bool touched;
  };

  std::vector<ScopeTile> pins;
  pins.reserve(scope.tiles.size());

  for (MapTile* tile : scope.tiles)
  {
    ScopeTile entry{tile, tile->index, !tile->changed.load(), false};

    if (entry.pinned_by_us)
    {
      world->mapIndex.setChanged(tile);
    }

    pins.push_back(entry);
  }

  auto const release_untouched_pins = [&pins, world]
  {
    for (ScopeTile const& entry : pins)
    {
      if (entry.pinned_by_us && !entry.touched && world->mapIndex.tileLoaded(entry.index))
      {
        world->mapIndex.unsetChanged(entry.index);
      }
    }
  };

  // The competing fork offers Global (Entire Map) with no undo and no preview of what it will
  // touch. This asks, with the real numbers, for anything wider than the tile the user is standing
  // on -- and it is undoable, which is the actual improvement.
  if (scope.tiles.size() > 1 || planned.chunks > CONFIRM_CHUNK_THRESHOLD)
  {
    unsigned const undo_limit = NOGGIT_ACTION_MGR->limit();

    QString question
      ( QString("Set %1 will be written to %2 layer(s) across %3 chunk(s) in %4 tile(s), within "
                "%5.\n\nThis is undoable until you save.")
          .arg(effect_id).arg(planned.layers).arg(planned.chunks).arg(planned.tiles)
          .arg(scope.description));

    if (planned.tiles > undo_limit)
    {
      question += QString(" It lands on the undo stack as one step per tile, and the stack keeps "
                          "only the most recent %1, so the earliest %2 tile(s) will not be "
                          "undoable.")
                    .arg(undo_limit).arg(planned.tiles - undo_limit);
    }

    question += "\n\nContinue?";

    auto const answer
      ( QMessageBox::warning
          ( this
          , "Ground Effect Sets"
          , question
          , QMessageBox::Yes | QMessageBox::Cancel
          , QMessageBox::Cancel
          )
      );

    if (answer != QMessageBox::Yes)
    {
      release_untouched_pins();
      _status->setText("Cancelled. Nothing was changed.");
      return;
    }
  }

  std::size_t chunks_changed = 0;
  std::size_t layers_changed = 0;
  std::size_t undo_steps = 0;

  // One undo step per tile, not one for the whole apply. registerChunkLayerInfoChange dedupes by
  // walking the vector it has already built (Action.cpp:887-891), so a single action covering N
  // chunks costs N(N-1)/2 pointer comparisons: 32,640 for one tile's 256 chunks, but 3.3e10 for a
  // 1,000-tile map. Per tile also keeps each step small enough to be worth undoing.
  for (ScopeTile& entry : pins)
  {
    bool action_open = false;

    for (int z = 0; z < 16; ++z)
    {
      for (int x = 0; x < 16; ++x)
      {
        MapChunk* chunk = entry.tile->getChunk(static_cast<unsigned>(x), static_cast<unsigned>(z));

        if (!chunk || !chunk->texture_set || !chunkInScope(scope, chunk))
        {
          continue;
        }

        bool touched = false;

        for (std::size_t layer = 0; layer < chunk->texture_set->num(); ++layer)
        {
          if (chunk->texture_set->filename(layer) != texture)
          {
            continue;
          }

          if (chunk->texture_set->effect(layer) == effect_id)
          {
            continue;
          }

          if (!action_open)
          {
            NOGGIT_ACTION_MGR->beginAction(_map_view, Noggit::ActionFlags::eCHUNKS_LAYERINFO);
            action_open = true;
            ++undo_steps;
          }

          if (!touched)
          {
            // Before the write: the snapshot is the only copy of the four MCLY entries as they
            // were, and Action::undo memcpys them straight back (Action.cpp:239-250).
            NOGGIT_CUR_ACTION->registerChunkLayerInfoChange(chunk);
            touched = true;
          }

          chunk->texture_set->setEffect(layer, static_cast<int>(effect_id));
          ++layers_changed;
        }

        if (touched)
        {
          // Same follow-up the scripting API performs after set_effect (script_chunk.cpp:53-54): the
          // low-detail texture map is derived from the layers, and the render side has to be told
          // the MCLY flags moved.
          chunk->texture_set->lod_texture_map();
          chunk->registerChunkUpdate(ChunkUpdateFlags::FLAGS);
          ++chunks_changed;
          entry.touched = true;
        }
      }
    }

    if (action_open)
    {
      NOGGIT_ACTION_MGR->endAction();
    }

    if (entry.touched)
    {
      // registerChunkUpdate is a render-refresh flag and nothing more -- MapChunk's constructor
      // sets the same bits for every chunk that ever loads (MapChunk.cpp:35), so it cannot mean
      // "unsaved". The save path tests MapTile::changed instead (map_index.cpp:555 and :584), and
      // neither setEffect (texture_set.cpp:1717) nor registerChunkUpdate touches it. Without this
      // the ADT is skipped by saveChanged and the edit is lost on unload.
      world->mapIndex.setChanged(entry.tile);
    }
  }

  release_untouched_pins();

  if (chunks_changed == 0)
  {
    _status->setText
      (QString("No layer uses \"%1\" in %2, so nothing changed.")
         .arg(QString::fromStdString(texture)).arg(scope.description));
    return;
  }

  QString const message
    ( QString("Set %1 applied to %2 layer(s) across %3 chunk(s) in %4, as %5 undo step(s). "
              "Save the ADTs to keep it.")
        .arg(effect_id).arg(layers_changed).arg(chunks_changed).arg(scope.description)
        .arg(undo_steps));

  _status->setText(message);
  Log << message.toStdString() << std::endl;
}
