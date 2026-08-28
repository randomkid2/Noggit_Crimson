// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/ui/AmbientOcclusionDialog.hpp>

#include <noggit/Action.hpp>
#include <noggit/ActionManager.hpp>
#include <noggit/terrain/AmbientOcclusion.hpp>
#include <noggit/Log.h>
#include <noggit/MapChunk.h>
#include <noggit/MapTile.h>
#include <noggit/MapView.h>
#include <noggit/TileIndex.hpp>
#include <noggit/World.h>
#include <noggit/map_index.hpp>

#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QProgressDialog>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QVBoxLayout>

#include <cstddef>
#include <vector>

using namespace Noggit::Ui;

namespace
{
  // Scope combo payloads. Carried as item data rather than read off currentIndex(), the same
  // discipline the list widgets in this tree use: an index is only meaningful while nothing has
  // been inserted above it.
  constexpr int SCOPE_CURRENT_TILE = 0;
  constexpr int SCOPE_ALL_LOADED = 1;

  constexpr int BLEND_MULTIPLY = 0;
  constexpr int BLEND_REPLACE = 1;

  constexpr int CHUNKS_PER_TILE = 16 * 16;

  // Above this many tiles the bake is a multi-minute operation whose undo history will not fit in
  // the action stack, so it asks first. Not a hard cap: a whole-map bake is a legitimate thing to
  // want, it just should not start by surprise.
  constexpr std::size_t LARGE_SCOPE_TILE_WARNING = 16;

  // One tile in scope, plus the two facts about it that only onBake knows.
  //
  // The MapTile* is safe to hold for the length of the bake only because `changed` is set on it
  // first; see the comment at the pinning loop in onBake. The TileIndex is kept alongside so the
  // epilogue can address the tile without dereferencing the pointer.
  struct ScopeTile
  {
    MapTile* tile = nullptr;
    ::TileIndex index {0, 0};
    bool pinned_by_us = false;
    bool snapshot_taken = false;
  };

  // The 8-neighbourhood of a tile, restricted to tiles that are loaded AND finished parsing.
  //
  // tileLoaded() rather than a null test on getTile(): getTile hands back a MapTile the async
  // loader may still be writing mVertices into, and reading it from here is a data race on
  // exactly the array the height field is built from.
  //
  // Neighbours are only READ. Nothing here calls setChanged on them, which is the whole reason
  // this walks MapIndex by hand instead of using World::for_all_chunks_on_tile -- that marks the
  // tile it visits as unsaved (World.inl:16), so borrowing a neighbour for margin would put a
  // save prompt in front of a user who never edited it.
  std::vector<MapTile*> loadedNeighbours(MapIndex& index, MapTile* tile)
  {
    std::vector<MapTile*> out;

    int const tile_x = static_cast<int>(tile->index.x);
    int const tile_z = static_cast<int>(tile->index.z);

    for (int dz = -1; dz <= 1; ++dz)
    {
      for (int dx = -1; dx <= 1; ++dx)
      {
        if (dx == 0 && dz == 0)
        {
          continue;
        }

        int const x = tile_x + dx;
        int const z = tile_z + dz;

        // Signed arithmetic on purpose: TileIndex::x/z are size_t, so tile_x - 1 at the west edge
        // of the map would wrap to a huge value instead of going negative.
        if (x < 0 || x > 63 || z < 0 || z > 63)
        {
          continue;
        }

        ::TileIndex const neighbour
          (static_cast<std::size_t>(x), static_cast<std::size_t>(z));

        if (!index.tileLoaded(neighbour))
        {
          continue;
        }

        if (MapTile* loaded = index.getTile(neighbour))
        {
          out.push_back(loaded);
        }
      }
    }

    return out;
  }

  QDoubleSpinBox* makeDoubleSpin( QWidget* parent
                                , double minimum
                                , double maximum
                                , double value
                                , double step
                                , int decimals
                                )
  {
    auto* box = new QDoubleSpinBox(parent);
    box->setRange(minimum, maximum);
    box->setDecimals(decimals);
    box->setSingleStep(step);
    box->setValue(value);
    return box;
  }
}

AmbientOcclusionDialog::AmbientOcclusionDialog(MapView* map_view, QWidget* parent)
  : QDialog(parent)
  , _map_view(map_view)
{
  setWindowTitle("Bake Ambient Occlusion");
  setMinimumWidth(520);

  auto outer (new QVBoxLayout(this));

  // --- scope -----------------------------------------------------------------------------
  auto scope_box (new QGroupBox("Scope", this));
  auto scope_layout (new QVBoxLayout(scope_box));

  _scope = new QComboBox(this);
  _scope->addItem("This ADT tile (under the camera)", SCOPE_CURRENT_TILE);
  _scope->addItem("All loaded tiles", SCOPE_ALL_LOADED);
  scope_layout->addWidget(_scope);

  _use_neighbours = new QCheckBox("Use neighbouring loaded tiles as context", this);
  _use_neighbours->setChecked(true);
  _use_neighbours->setToolTip
    ("A vertex on the tile border searches its horizon over terrain that is not in the field, so\n"
     "without context it comes back unoccluded and every tile boundary reads as a bright cross.\n"
     "Neighbouring tiles are only read -- they are not modified and not marked unsaved.");
  scope_layout->addWidget(_use_neighbours);

  auto scan_row (new QHBoxLayout());
  auto scan_button (new QPushButton("Scan scope", this));
  scan_button->setToolTip("Report how many chunks in scope already carry vertex colour.");
  scan_row->addWidget(scan_button);
  scan_row->addStretch(1);
  scope_layout->addLayout(scan_row);

  outer->addWidget(scope_box);

  // --- the hazard ------------------------------------------------------------------------
  //
  // This box is the reason the dialog exists in this shape. MCCV is a multiplier that plenty of
  // maps use for hand-painted colour, and a bake that overwrites it is unrecoverable once the ADT
  // is saved. Multiply is the default and Replace asks for confirmation.
  auto blend_box (new QGroupBox("Existing vertex colour", this));
  auto blend_layout (new QVBoxLayout(blend_box));

  _blend_mode = new QComboBox(this);
  _blend_mode->addItem("Multiply into existing colour (non-destructive)", BLEND_MULTIPLY);
  _blend_mode->addItem("Replace existing colour", BLEND_REPLACE);
  blend_layout->addWidget(_blend_mode);

  _blend_explainer = new QLabel(this);
  _blend_explainer->setWordWrap(true);
  blend_layout->addWidget(_blend_explainer);

  outer->addWidget(blend_box);

  // --- sampling --------------------------------------------------------------------------
  auto sampling_box (new QGroupBox("Sampling", this));
  auto sampling_form (new QFormLayout(sampling_box));

  // Ranges match AmbientOcclusionSettings::sanitized() exactly, so nothing the UI can produce is
  // silently clamped to a different number than the one on screen.
  _direction_count = new QSpinBox(this);
  _direction_count->setRange(1, 256);
  _direction_count->setValue(16);
  _direction_count->setToolTip
    ("Azimuths sampled around each vertex. Cost is directions x steps per vertex.");
  sampling_form->addRow("Directions", _direction_count);

  _step_count = new QSpinBox(this);
  _step_count->setRange(1, 256);
  _step_count->setValue(12);
  _step_count->setToolTip
    ("Radial samples along each direction. Too few and a narrow ridge is stepped over entirely.");
  sampling_form->addRow("Steps per direction", _step_count);

  _max_radius = makeDoubleSpin(this, 0.0, 2000.0, 20.0, 1.0, 2);
  _max_radius->setSuffix(" yd");
  _max_radius->setToolTip
    ("Horizon search radius. 20 is a little over two chunk-units either side -- large enough for a\n"
     "gully, small enough that a distant mountain does not flatten a whole valley into shadow.\n"
     "Zero disables the bake entirely.");
  sampling_form->addRow("Ray length", _max_radius);

  _bias_degrees = makeDoubleSpin(this, 0.0, 89.0, 0.0, 1.0, 2);
  _bias_degrees->setSuffix(" deg");
  _bias_degrees->setToolTip
    ("Horizon elevations below this count as no horizon at all. Zero means the estimator reports\n"
     "exactly what it measures; a few degrees is worth setting on real terrain, where otherwise\n"
     "every gentle uniform slope occludes itself and the map merely gets darker overall.");
  sampling_form->addRow("Horizon bias", _bias_degrees);

  _cosine_weighted = new QCheckBox("Cosine (Lambert) weighting", this);
  _cosine_weighted->setChecked(true);
  _cosine_weighted->setToolTip
    ("Weight the hemisphere by the incident angle rather than by solid angle alone. Both forms are\n"
     "open at a flat horizon and dark at a vertical wall; the weighted one darkens more slowly and\n"
     "is what matches a rendered reference.");
  sampling_form->addRow(QString(), _cosine_weighted);

  outer->addWidget(sampling_box);

  // --- shading ---------------------------------------------------------------------------
  auto shading_box (new QGroupBox("Shading", this));
  auto shading_form (new QFormLayout(shading_box));

  _strength = makeDoubleSpin(this, 0.0, 8.0, 1.0, 0.1, 2);
  _strength->setToolTip
    ("Scales the measured occlusion. The result is still clamped to [0, 1], so above 1 this\n"
     "saturates rather than escaping the range. Zero bakes nothing.");
  shading_form->addRow("Strength", _strength);

  _open_factor = makeDoubleSpin(this, 0.0, 2.0, 1.0, 0.05, 3);
  _open_factor->setToolTip
    ("Vertex colour multiplier where nothing occludes. 1.0 is neutral -- leave it there unless the\n"
     "intent is to brighten the whole map, which is not what an AO bake is for.");
  shading_form->addRow("Open multiplier", _open_factor);

  _occluded_factor = makeDoubleSpin(this, 0.0, 2.0, 0.45, 0.05, 3);
  _occluded_factor->setToolTip
    ("Multiplier in the darkest crevice. Clamped to at most the open multiplier, which is what\n"
     "guarantees AO can only ever darken.");
  shading_form->addRow("Occluded multiplier", _occluded_factor);

  _contrast = makeDoubleSpin(this, 0.05, 20.0, 1.0, 0.1, 2);
  _contrast->setToolTip
    ("Exponent applied to occlusion before the interpolation. Above 1 confines darkening to the\n"
     "deepest pockets; below 1 spreads it.");
  shading_form->addRow("Contrast", _contrast);

  auto tint_row (new QHBoxLayout());
  char const* const tint_labels[3] = {"R", "G", "B"};

  for (int channel = 0; channel < 3; ++channel)
  {
    tint_row->addWidget(new QLabel(tint_labels[channel], this));
    _shadow_tint[channel] = makeDoubleSpin(this, 0.0, 1.0, 1.0, 0.05, 3);
    tint_row->addWidget(_shadow_tint[channel], 1);
  }

  auto tint_holder (new QWidget(this));
  tint_holder->setLayout(tint_row);
  tint_holder->setToolTip
    ("Tint of the SHADOW end only. An unoccluded vertex stays exactly neutral, so no hue is\n"
     "introduced where there is no occlusion. Lifting blue slightly is the usual sky-bounce cheat.");
  shading_form->addRow("Shadow tint", tint_holder);

  outer->addWidget(shading_box);

  _status = new QLabel(this);
  _status->setWordWrap(true);
  outer->addWidget(_status);

  auto button_row (new QHBoxLayout());
  button_row->addStretch(1);

  auto close_button (new QPushButton("Close", this));
  button_row->addWidget(close_button);

  auto bake_button (new QPushButton("Bake", this));
  bake_button->setDefault(true);
  button_row->addWidget(bake_button);
  outer->addLayout(button_row);

  connect(bake_button, &QPushButton::clicked, this, &AmbientOcclusionDialog::onBake);
  connect(scan_button, &QPushButton::clicked, this, &AmbientOcclusionDialog::onScanScope);
  connect(close_button, &QPushButton::clicked, this, &QDialog::close);

  auto const refresh_explainer = [this]
  {
    if (blendMode() == Noggit::AoBlendMode::Replace)
    {
      _blend_explainer->setText
        ("Overwrites every vertex colour in scope. Any hand-painted colour there is LOST. Chunks\n"
         "with no vertex colour layer get one. Use this to re-bake -- multiplying a second time\n"
         "darkens the terrain again.");
    }
    else
    {
      _blend_explainer->setText
        ("Multiplies the bake into whatever colour is already there, so hand-painted hue survives\n"
         "and a chunk with no vertex colour behaves as if it were neutral. Not idempotent: baking\n"
         "twice multiplies twice and the terrain gets darker each time. Undo, or use Replace, to\n"
         "re-bake.");
    }
  };

  connect(_blend_mode, qOverload<int>(&QComboBox::currentIndexChanged), this
         , [refresh_explainer] (int) { refresh_explainer(); });

  connect(_scope, qOverload<int>(&QComboBox::currentIndexChanged), this
         , [this] (int) { onScanScope(); });

  refresh_explainer();
  onScanScope();
}

Noggit::AmbientOcclusionSettings AmbientOcclusionDialog::aoSettings() const
{
  Noggit::AmbientOcclusionSettings settings;

  settings.direction_count = _direction_count->value();
  settings.step_count = _step_count->value();
  settings.max_radius = static_cast<float>(_max_radius->value());
  settings.strength = static_cast<float>(_strength->value());
  settings.bias_degrees = static_cast<float>(_bias_degrees->value());
  settings.cosine_weighted = _cosine_weighted->isChecked();

  // Sanitized here rather than left to the estimator, because the height field's margin is built
  // from max_radius: handing the field an unsanitized radius and the estimator a sanitized one
  // would size the margin for a search that never happens.
  return settings.sanitized();
}

Noggit::AmbientOcclusionColorSettings AmbientOcclusionDialog::colorSettings() const
{
  Noggit::AmbientOcclusionColorSettings settings;

  settings.open_factor = static_cast<float>(_open_factor->value());
  settings.occluded_factor = static_cast<float>(_occluded_factor->value());
  settings.contrast = static_cast<float>(_contrast->value());
  settings.shadow_tint_r = static_cast<float>(_shadow_tint[0]->value());
  settings.shadow_tint_g = static_cast<float>(_shadow_tint[1]->value());
  settings.shadow_tint_b = static_cast<float>(_shadow_tint[2]->value());

  return settings.sanitized();
}

Noggit::AoBlendMode AmbientOcclusionDialog::blendMode() const
{
  return _blend_mode->currentData().toInt() == BLEND_REPLACE
       ? Noggit::AoBlendMode::Replace
       : Noggit::AoBlendMode::Multiply;
}

std::vector<MapTile*> AmbientOcclusionDialog::tilesInScope() const
{
  std::vector<MapTile*> tiles;

  World* world = _map_view->getWorld();

  if (_scope->currentData().toInt() == SCOPE_ALL_LOADED)
  {
    // loaded_tiles() already filters on finishedLoading() (map_index.cpp:29-33).
    for (MapTile* tile : world->mapIndex.loaded_tiles())
    {
      tiles.push_back(tile);
    }

    return tiles;
  }

  MapTile* tile = world->mapIndex.getTile(::TileIndex(_map_view->cameraPosition()));

  if (tile && tile->finishedLoading())
  {
    tiles.push_back(tile);
  }

  return tiles;
}

void AmbientOcclusionDialog::onScanScope()
{
  std::vector<MapTile*> const tiles (tilesInScope());

  if (tiles.empty())
  {
    _status->setText("No loaded tile in that scope. Fly to a tile, or load one, and scan again.");
    return;
  }

  std::size_t painted = 0;
  std::size_t total = 0;

  for (MapTile* tile : tiles)
  {
    for (unsigned chunk_z = 0; chunk_z < 16; ++chunk_z)
    {
      for (unsigned chunk_x = 0; chunk_x < 16; ++chunk_x)
      {
        MapChunk* chunk = tile->getChunk(chunk_x, chunk_z);

        if (!chunk)
        {
          continue;
        }

        ++total;

        if (chunk->hasColors())
        {
          ++painted;
        }
      }
    }
  }

  _status->setText
    ( QString("%1 tile(s), %2 chunk(s) in scope. %3 already carry vertex colour%4")
        .arg(tiles.size())
        .arg(total)
        .arg(painted)
        .arg(painted == 0
              ? QString(" -- nothing to overwrite.")
              : QString(", which Multiply preserves and Replace destroys."))
    );
}

void AmbientOcclusionDialog::bakeOneTile( MapTile* tile
                                        , Noggit::AmbientOcclusionSettings const& ao_settings
                                        , Noggit::AmbientOcclusionColorSettings const& color_settings
                                        , Noggit::AoBlendMode blend
                                        , QProgressDialog& progress
                                        , int& progress_value
                                        , bool& cancelled
                                        , TileBakeResult& result
                                        )
{
  World* world = _map_view->getWorld();

  // Margin equal to the search radius: without it the border vertices search over clamp-to-edge
  // terrain and come back unoccluded. The field is NaN-filled, and anything no chunk writes stays
  // NaN and is ignored by the estimator rather than guessed at.
  Noggit::HeightField field
    ( Noggit::AmbientOcclusionBaker::makeTileHeightField<MapTile, MapChunk>
        (tile, ao_settings.max_radius));

  if (_use_neighbours->isChecked())
  {
    for (MapTile* neighbour : loadedNeighbours(world->mapIndex, tile))
    {
      Noggit::AmbientOcclusionBaker::addTileToField<MapTile, MapChunk>(neighbour, field);
    }
  }

  // Only fills holes that have a finite neighbour to fill from, so an unloaded neighbour's area
  // stays NaN instead of being invented.
  Noggit::fillGaps(field);

  for (unsigned chunk_z = 0; chunk_z < 16; ++chunk_z)
  {
    for (unsigned chunk_x = 0; chunk_x < 16; ++chunk_x)
    {
      // Re-fetched every iteration rather than hoisted: `tile` is pinned for the whole run (see
      // onBake), so this pointer is stable, but the chunk pointer is only borrowed for the length
      // of one iteration and never held across the setValue() pump below.
      MapChunk* chunk = tile->getChunk(chunk_x, chunk_z);

      if (chunk)
      {
        // The action is opened here, on the first chunk that is actually going to be snapshotted,
        // and is closed by the caller. One action per tile rather than one for the whole bake:
        // registerChunkVertexColorChange dedupes with a linear scan over a vector of ~1.7 KB
        // elements, so a single action covering N chunks streams O(N^2) bytes. Bounding N at the
        // 256 chunks of one tile bounds that at something measured in tens of megabytes; letting
        // it run to a whole-map 256,000 would be tens of terabytes, plus a redo snapshot
        // allocated in one resize that no longer fits in a 32-bit process.
        if (result.chunks_snapshotted == 0)
        {
          NOGGIT_ACTION_MGR->beginAction(_map_view, Noggit::ActionFlags::eCHUNKS_VERTEX_COLOR);
        }

        // Before bakeChunk, not after: the action snapshots the chunk's current mccv[] and that is
        // the only copy of the pre-bake colour that will exist.
        NOGGIT_CUR_ACTION->registerChunkVertexColorChange(chunk);
        ++result.chunks_snapshotted;

        if (Noggit::AmbientOcclusionBaker::bakeChunk<MapChunk>
              (chunk, field, ao_settings, color_settings, blend))
        {
          ++result.chunks_baked;
        }
      }

      // setValue on a modal QProgressDialog pumps the event loop, which is what keeps the window
      // painting and the Cancel button live -- and which also lets MapView's frame timer fire.
      // Nothing that must survive that pump is held in a local here: see onBake for why the tiles
      // themselves are safe.
      progress.setValue(++progress_value);

      if (progress.wasCanceled())
      {
        cancelled = true;
        return;
      }
    }
  }
}

void AmbientOcclusionDialog::onBake()
{
  // beginAction returns the ALREADY RUNNING action when one exists and does not apply the flags it
  // was passed (ActionManager.cpp:64-65). Calling endAction() after that would close somebody
  // else's action and file our chunk edits under it, so refuse instead of nesting.
  if (NOGGIT_CUR_ACTION)
  {
    _status->setText
      ("Another edit is still in progress. Release the mouse button, or finish the current tool "
       "stroke, and bake again.");
    return;
  }

  Noggit::AmbientOcclusionSettings const ao_settings (aoSettings());
  Noggit::AmbientOcclusionColorSettings const color_settings (colorSettings());
  Noggit::AoBlendMode const blend (blendMode());

  if (!ao_settings.valid())
  {
    _status->setText
      ("These settings cannot darken anything -- ray length and strength both have to be above "
       "zero. Nothing was changed.");
    return;
  }

  std::vector<MapTile*> const tiles (tilesInScope());

  if (tiles.empty())
  {
    _status->setText("No loaded tile in that scope, so there is nothing to bake.");
    return;
  }

  World* world = _map_view->getWorld();

  // Pin every tile in scope BEFORE anything below can pump the event loop -- which the two
  // confirmation message boxes do just as much as the progress dialog does, since QDialog::exec()
  // is an event loop.
  //
  // This is the whole defence against a use-after-free, and it is worth spelling out. Further
  // down, QProgressDialog::setValue() pumps on every one of the 256 chunks of every tile. Modality
  // stops *input* reaching the rest of the application; it does not stop timers. MapView's frame
  // timer keeps firing (MapView.cpp:4828-4830), paintGL() calls tick() unconditionally, and tick()
  // calls MapIndex::unloadTiles() (MapView.cpp:5362), which deletes any loaded MapTile further
  // than unload_dist from the camera. unloadTiles refuses to delete a tile whose `changed` flag is
  // set (map_index.cpp:523), and until the pin count described below existed that was the only
  // lever available here.
  //
  // So the MapTile* held in `tiles`, and every MapChunk* an undo action is about to store, are
  // live across pumps that are entitled to free them -- unless the flag is set first. Setting it
  // on the tile currently being baked is not enough: the tiles waiting their turn are the ones the
  // camera has left behind, and they are exactly the ones far enough away to be unloaded.
  //
  // `pinned_by_us` is remembered so the flag can be taken back off tiles that end up contributing
  // nothing -- the whole scope if the user backs out of a confirmation, or a tile with no chunk
  // data. A tile that has had even one chunk snapshotted keeps the flag for good: an undo action
  // holds pointers into it.
  //
  // This borrows the unsaved-changes flag to express a lifetime, and that overload is what made
  // MapIndex::unloadTiles unable to tell an edited tile from a visited one -- the memory leak
  // fixed in map_index.cpp. MapIndex::pinTile()/unpinTile() now say exactly this and nothing else:
  // undeletable, not dirty, reference counted so two overlapping scopes cannot release each
  // other's tiles. Moving this loop and release_untouched_pins onto them is the right follow-up
  // and is deliberately NOT done in the same change as the leak fix, because the bake must keep
  // working identically while the two mechanisms coexist: setChanged() still sets `changed`, and
  // unloadTiles still refuses on `changed`, so nothing here has changed behaviour.
  std::vector<ScopeTile> scope;
  scope.reserve(tiles.size());

  for (MapTile* tile : tiles)
  {
    ScopeTile entry;
    entry.tile = tile;
    entry.index = tile->index;
    entry.pinned_by_us = !tile->changed.load();

    if (entry.pinned_by_us)
    {
      world->mapIndex.setChanged(tile);
    }

    scope.push_back(entry);
  }

  // Releases the pin on every tile that has not had a chunk snapshotted, and only on tiles this
  // call was the one to pin. Run on every exit path from here on, so backing out of a confirmation
  // leaves the unsaved-changes flags exactly as it found them.
  auto const release_untouched_pins = [&scope, world]
  {
    for (ScopeTile const& entry : scope)
    {
      if (entry.pinned_by_us && !entry.snapshot_taken && world->mapIndex.tileLoaded(entry.index))
      {
        world->mapIndex.unsetChanged(entry.index);
      }
    }
  };

  if (blend == Noggit::AoBlendMode::Replace)
  {
    // Undo covers this within the session, but a save in between does not, and vertex paint is
    // hand work. Worth one click.
    auto const answer
      ( QMessageBox::warning
          ( this
          , "Bake Ambient Occlusion"
          , QString("Replace overwrites the vertex colour of every chunk on %1 tile(s).\n\n"
                    "Any hand-painted colour there is lost. This is undoable until you save.\n\n"
                    "Continue?").arg(scope.size())
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

  // A whole-map scope is minutes of work and, now that the bake is one undo step per tile, more
  // undo steps than the history can hold. Say so with the real numbers before starting, rather
  // than leaving the user to infer it from a crawling progress bar.
  if (scope.size() > LARGE_SCOPE_TILE_WARNING)
  {
    unsigned const undo_limit = NOGGIT_ACTION_MGR->limit();

    auto const answer
      ( QMessageBox::warning
          ( this
          , "Bake Ambient Occlusion"
          , QString("%1 tiles are in scope -- %2 chunks. This can take several minutes, and only a "
                    "smaller scope makes it faster.\n\n"
                    "It lands on the undo stack as one step per tile, and the stack keeps only the "
                    "most recent %3, so the earliest tiles baked will not be undoable. Nothing is "
                    "written to disk until you save, but nor is a bake this size easy to reverse.\n\n"
                    "Continue?")
              .arg(scope.size()).arg(scope.size() * CHUNKS_PER_TILE).arg(undo_limit)
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

  int const total_steps = static_cast<int>(scope.size()) * CHUNKS_PER_TILE;

  QProgressDialog progress ("Baking ambient occlusion...", "Cancel", 0, total_steps, this);

  // ApplicationModal rather than the WindowModal used elsewhere in this tree, for two reasons that
  // are specific to this operation:
  //
  //   - setValue() only pumps the event loop while the dialog is modal, and without that pumping
  //     the window never repaints and Cancel never arrives.
  //   - that same pumping lets INPUT through to whatever is not blocked. This dialog is opened
  //     with WA_DeleteOnClose, so a user closing it mid-bake would destroy `this` underneath a
  //     running member function; and a click in the viewport could start a second action while
  //     ours is open. Blocking the application closes both of those.
  //
  // What modality does NOT close, and what the pinning above exists for, is the timer path: Qt
  // delivers timer and paint events to a blocked window regardless of modality, so MapView's frame
  // timer keeps driving paintGL() -> tick() -> unloadTiles() for the whole bake. Do not read the
  // modality here as making anything held across setValue() safe. It does not.
  progress.setWindowModality(Qt::ApplicationModal);
  progress.setMinimumDuration(0);
  progress.setAutoClose(false);
  progress.setAutoReset(false);
  progress.setValue(0);

  std::size_t chunks_baked = 0;
  std::size_t tiles_touched = 0;
  std::size_t undo_steps = 0;
  bool cancelled = false;
  int progress_value = 0;
  QString failure;

  for (std::size_t i = 0; i < scope.size() && !cancelled && failure.isEmpty(); ++i)
  {
    ScopeTile& entry = scope[i];

    progress.setLabelText
      ( QString("Baking tile %1, %2  (%3 of %4)...")
          .arg(entry.index.x).arg(entry.index.z).arg(i + 1).arg(scope.size()));

    TileBakeResult result;

    try
    {
      bakeOneTile
        (entry.tile, ao_settings, color_settings, blend, progress, progress_value, cancelled, result);
    }
    catch (std::exception const& e)
    {
      failure = QString::fromUtf8(e.what());
    }

    // Deliberately outside the catch, and deliberately guarded by its own try.
    //
    // Two things are going on. First, the action has to be closed on every path or the next tool
    // stroke silently joins this one and the undo stack is wrong from then on. Second,
    // endAction() -> Action::finish() allocates the redo snapshot, so it is itself a plausible
    // source of std::bad_alloc; calling it from inside a catch block that is handling a
    // std::bad_alloc would let the second one escape onBake, and there is no handler above a Qt
    // signal emission, so that is a process abort with every unsaved ADT lost.
    if (NOGGIT_CUR_ACTION)
    {
      try
      {
        NOGGIT_ACTION_MGR->endAction();
        ++undo_steps;
      }
      catch (std::exception const& e)
      {
        if (failure.isEmpty())
        {
          failure = QString::fromUtf8(e.what());
        }

        LogError << "Ambient occlusion bake: endAction failed: " << e.what() << std::endl;
      }
    }

    chunks_baked += result.chunks_baked;
    entry.snapshot_taken = result.chunks_snapshotted != 0;

    if (result.chunks_baked != 0)
    {
      ++tiles_touched;
    }
  }

  // Take the pin back off tiles that contributed nothing: never reached because the user
  // cancelled, or reached and found no chunk data. A tile that had even one chunk snapshotted
  // keeps the flag, because an undo action now holds MapChunk* pointers into it and letting it
  // unload would make those dangle.
  release_untouched_pins();

  progress.close();

  if (!failure.isEmpty())
  {
    LogError << "Ambient occlusion bake failed: " << failure.toStdString() << std::endl;

    QString const message
      ( QString("The bake failed partway through.\n\n%1\n\nWhat was already baked is on the undo "
                "stack as %2 step(s), one per tile.").arg(failure).arg(undo_steps));

    _status->setText(message);
    QMessageBox::critical(this, "Bake Ambient Occlusion", message);
    return;
  }

  if (chunks_baked == 0)
  {
    _status->setText
      ( cancelled ? QString("Cancelled before anything was baked.")
                  : QString("No chunk was baked. The tiles in scope have no chunk data."));
    return;
  }

  QString const message
    ( QString("%1%2 chunk(s) baked across %3 tile(s), %4. %5 undo step(s), one per tile. Save the "
              "ADTs to keep it.")
        .arg(cancelled ? QString("Cancelled after ") : QString())
        .arg(chunks_baked)
        .arg(tiles_touched)
        .arg(blend == Noggit::AoBlendMode::Replace
              ? QString("replacing existing vertex colour")
              : QString("multiplied into existing vertex colour"))
        .arg(undo_steps)
    );

  _status->setText(message);
  Log << message.toStdString() << std::endl;
}
