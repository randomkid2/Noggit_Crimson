// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/ui/GroundEffectSetEditor.hpp>

#include <noggit/DBC.h>
#include <noggit/MapChunk.h>
#include <noggit/MapTile.h>
#include <noggit/MapView.h>
#include <noggit/World.h>
#include <noggit/map_index.hpp>
#include <noggit/texture_set.hpp>
#include <noggit/Log.h>

#include <QtCore/QSignalBlocker>
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

  QString summarise(std::uint32_t id, std::string const files[DOODAD_SLOTS])
  {
    QStringList names;

    for (std::size_t i = 0; i < DOODAD_SLOTS; ++i)
    {
      if (files[i].empty())
      {
        continue;
      }

      QString name (QString::fromStdString(files[i]));
      names << name.section('\\', -1).section('/', -1);
    }

    return QString("%1  %2").arg(id, 6).arg(names.isEmpty() ? QString("(empty)") : names.join(", "));
  }
}

GroundEffectSetEditor::GroundEffectSetEditor(MapView* map_view, QWidget* parent)
  : QDialog(parent)
  , _map_view(map_view)
{
  setWindowTitle("Ground Effect Sets");
  setMinimumSize(760, 460);

  auto outer (new QHBoxLayout(this));

  // --- left: the library -----------------------------------------------------------------
  auto left (new QVBoxLayout());
  left->addWidget(new QLabel("Effect sets in GroundEffectTexture.dbc", this));

  // GroundEffectTexture.dbc runs to tens of thousands of rows and the overwhelming majority are
  // empty, so an unfiltered list is unusable -- finding the set you just made means scrolling past
  // twenty thousand "(empty)" entries.
  _filter = new QLineEdit(this);
  _filter->setPlaceholderText("Filter by id or doodad name...");
  _filter->setClearButtonEnabled(true);
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
  list_buttons->addWidget(new_button);
  list_buttons->addWidget(duplicate_button);
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
    row->addWidget(_doodad_weight[i]);

    doodad_form->addRow(QString("Slot %1").arg(i + 1), row);
  }

  right->addWidget(doodad_box);

  auto settings_box (new QGroupBox("Settings", this));
  auto settings_form (new QFormLayout(settings_box));

  _amount = new QSpinBox(this);
  _amount->setRange(0, 255);
  _amount->setToolTip("How densely the doodads are scattered. 0 places none at all.");
  settings_form->addRow("Density", _amount);

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
  _bulk_scope->addItem("This ADT tile", 0);
  _bulk_scope->addItem("All loaded tiles", 1);
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
  connect(save_button, &QPushButton::clicked, this, &GroundEffectSetEditor::onSaveDbc);
  connect(apply_button, &QPushButton::clicked, this, &GroundEffectSetEditor::onBulkApply);

  connect(_set_list, &QListWidget::currentRowChanged, this, [this] (int) { showSelected(); });
  connect(_filter, &QLineEdit::textChanged, this, [this] (QString const&) { rebuildList(); });
  connect(_hide_empty, &QCheckBox::toggled, this, [this] (bool) { rebuildList(); });
  connect(rescan, &QPushButton::clicked, this, [this] { refreshTextureList(); });

  // Rescanned when the scope changes, because "all loaded tiles" can contain textures this tile
  // does not, and offering a texture that is not in scope would put the old confusing
  // "nothing changed" result straight back.
  connect(_bulk_scope, qOverload<int>(&QComboBox::currentIndexChanged), this
         , [this] (int) { refreshTextureList(); });

  reloadFromDbc();
  refreshTextureList();
}

void GroundEffectSetEditor::refreshTextureList()
{
  // Counted per texture so the list can say how much terrain each one covers -- with several
  // similar tileset names, the layer count is usually what identifies the one you mean.
  //
  // The scan lives on MapView so the dev bridge's `textures` command answers from the same code,
  // which is what lets this be checked without a human at the window.
  auto const layers_by_texture
    (_map_view->terrainTexturesInScope(_bulk_scope->currentData().toInt() == 1));

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
    (QString("%1 texture(s) on the terrain in scope.").arg(_target_texture->count()));
}

void GroundEffectSetEditor::reloadFromDbc()
{
  _sets.clear();

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

  _status->setText(QString("%1 set(s) loaded.").arg(_sets.size()));
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
     && set.doodad_files[2].empty() && set.doodad_files[3].empty());

    // An empty set is always shown when it is the one being edited, otherwise creating a set and
    // filling it in would make it vanish from under the cursor on the first keystroke.
    if (hide_empty && empty && static_cast<int>(i) != previously_selected)
    {
      continue;
    }

    QString const label (summarise(set.id, set.doodad_files));

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
    set.doodad_files[i] = _doodad_path[i]->text().toStdString();
    set.weights[i] = static_cast<std::uint32_t>(_doodad_weight[i]->value());
  }

  set.amount = static_cast<std::uint32_t>(_amount->value());
  set.terrain_type = static_cast<std::uint32_t>(_terrain_type->currentData().toUInt());

  if (auto* item = _set_list->currentItem())
  {
    item->setText(summarise(set.id, set.doodad_files));
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

void GroundEffectSetEditor::onNewSet()
{
  applyEditsToSelected();

  EffectSet set;
  set.id = nextFreeId(gGroundEffectTextureDB);

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
  set.id = nextFreeId(gGroundEffectTextureDB);

  // Ids are reassigned on save, so a duplicate made before saving must not reuse the source's id.
  for (auto const& existing : _sets)
  {
    if (existing.id == set.id)
    {
      ++set.id;
    }
  }

  _sets.push_back(set);

  rebuildList();
  selectSetById(set.id);

  _status->setText(QString("Duplicated into set %1.").arg(set.id));
}

void GroundEffectSetEditor::onSaveDbc()
{
  applyEditsToSelected();

  std::size_t written_doodads = 0;
  std::size_t written_sets = 0;

  try
  {
    for (auto& set : _sets)
    {
      // Each named file needs a GroundEffectDoodad row. Reuse one when the same file is already
      // in the DBC -- otherwise every save would grow the file with duplicates of the same path.
      for (std::size_t i = 0; i < DOODAD_SLOTS; ++i)
      {
        if (set.doodad_files[i].empty())
        {
          set.doodad_ids[i] = 0;
          continue;
        }

        std::uint32_t found = 0;

        for (DBCFile::Iterator it = gGroundEffectDoodadDB.begin();
             it != gGroundEffectDoodadDB.end(); ++it)
        {
          if (set.doodad_files[i] == it->getString(GroundEffectDoodadDB::Filename))
          {
            found = it->getUInt(GroundEffectDoodadDB::ID);
            break;
          }
        }

        if (found == 0)
        {
          found = nextFreeId(gGroundEffectDoodadDB);

          DBCFile::Record record (gGroundEffectDoodadDB.addRecord(found));
          record.writeString(GroundEffectDoodadDB::Filename, set.doodad_files[i]);
          record.write<std::uint32_t>(GroundEffectDoodadDB::Flags, 0);

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

  QString const message
    ( QString("Saved. %1 new set(s), %2 new doodad row(s), written to the project's "
              "DBFilesClient folder.").arg(written_sets).arg(written_doodads));

  _status->setText(message);
  Log << message.toStdString() << std::endl;

  reloadFromDbc();
}

void GroundEffectSetEditor::onBulkApply()
{
  applyEditsToSelected();

  int const row = _set_list->currentRow();

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

  bool const all_tiles = _bulk_scope->currentData().toInt() == 1;

  std::size_t chunks_changed = 0;
  std::size_t layers_changed = 0;

  auto const apply_to_tile = [&] (MapTile* tile)
  {
    if (!tile)
    {
      return;
    }

    for (int z = 0; z < 16; ++z)
    {
      for (int x = 0; x < 16; ++x)
      {
        MapChunk* chunk = tile->getChunk(static_cast<unsigned>(x), static_cast<unsigned>(z));

        if (!chunk || !chunk->texture_set)
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

          chunk->texture_set->setEffect(layer, static_cast<int>(effect_id));
          ++layers_changed;
          touched = true;
        }

        if (touched)
        {
          // Same follow-up the scripting API performs after set_effect: the low-detail texture
          // map is derived from the layers, and the chunk has to be marked so the change is
          // actually written out.
          chunk->texture_set->lod_texture_map();
          chunk->registerChunkUpdate(ChunkUpdateFlags::FLAGS);
          ++chunks_changed;
        }
      }
    }
  };

  World* world = _map_view->getWorld();

  if (all_tiles)
  {
    for (MapTile* tile : world->mapIndex.loaded_tiles())
    {
      apply_to_tile(tile);
    }
  }
  else
  {
    apply_to_tile(world->mapIndex.getTile(::TileIndex(_map_view->cameraPosition())));
  }

  if (chunks_changed == 0)
  {
    _status->setText
      (QString("No layer uses \"%1\" in that scope, so nothing changed.")
         .arg(QString::fromStdString(texture)));
    return;
  }

  QString const message
    ( QString("Set %1 applied to %2 layer(s) across %3 chunk(s). Save the ADTs to keep it.")
        .arg(effect_id).arg(layers_changed).arg(chunks_changed));

  _status->setText(message);
  Log << message.toStdString() << std::endl;
}
