// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/ui/AlphaIntegrityReport.hpp>

#include <noggit/Action.hpp>
#include <noggit/ActionManager.hpp>
#include <noggit/Alphamap.hpp>
#include <noggit/Log.h>
#include <noggit/MapChunk.h>
#include <noggit/MapHeaders.h>
#include <noggit/MapTile.h>
#include <noggit/MapView.h>
#include <noggit/TileIndex.hpp>
#include <noggit/World.h>
#include <noggit/map_index.hpp>
#include <noggit/texture_set.hpp>

#include <math/trig.hpp>

#include <QtCore/QSignalBlocker>
#include <QtCore/QStringList>
#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <exception>
#include <iterator>

// Before the using-directive, so the qualified name cannot be read as the dialog. See the note in
// the header about the collision.
using ChunkReport = Noggit::AlphaIntegrityReport;

using namespace Noggit::Ui;

namespace
{
  // The list widget stops being usable long before the problem count does: an ADT set that a
  // converter mangled can produce five figures of rows, and QListWidget builds a QListWidgetItem
  // for each. The cap is on the WIDGET only -- every count shown, every repair performed and the
  // copied report all cover the full set, so capping cannot make the map look cleaner than it is.
  constexpr int MAX_LIST_ROWS = 2000;

  // At or below this, an over-opaque sum or a border delta is rounding drift rather than damage.
  // The number comes from the module's own framing: AlphaIntegrity.hpp:203 contrasts "1 over
  // everywhere" (drift) with "300 over" (layers pasted together), and 2 leaves room for a
  // round-trip through the 4-bit format, whose quantum is 17.
  constexpr int ROUNDING_DRIFT = 2;

  // Camera pitch when a row is focused.
  //
  // POSITIVE IS DOWN in this tree, and getting the sign wrong aims the whole feature at the sky.
  // Camera::direction builds the look vector as (sin(yaw)cos(pitch), -sin(pitch), cos(yaw)cos(pitch))
  // (Camera.cpp:82-86), so y is NEGATIVE -- i.e. the camera looks down -- for positive pitch.
  // MapView::focusOnSpawn solves the same quantity rather than naming it and gets the same sign:
  // pitch = asin(-d.y / |d|) (MapView.cpp:899), which is positive whenever the target is below the
  // eye. It always is here: move_camera_with_auto_height puts the eye 50 units above the terrain
  // directly over the chunk centre (MapView.cpp:4032-4044), so the exact solved angle is +90, and
  // Camera::pitch clamps to [-80, 80] (Camera.cpp:56). +80 is therefore both the clamped result of
  // the focusOnSpawn formula and as close to straight down as the camera goes -- which is the view
  // that answers the question, because an alpha map defect is a defect in what the GROUND shows.
  constexpr float FOCUS_PITCH_DEGREES = 80.f;

  struct WaitCursor
  {
    WaitCursor() { QGuiApplication::setOverrideCursor(Qt::WaitCursor); }
    ~WaitCursor() { QGuiApplication::restoreOverrideCursor(); }
  };

  QString addressLabel(Noggit::AlphaChunkAddress const& address)
  {
    return QString::fromStdString(address.label());
  }

  // Whether one explicit layer's alpha arrived compressed, in which case its last row and column
  // are real data and the border check is meaningless on it.
  //
  // Stack index k is chunk texture layer k+1; layer 0 stores no alpha and is never in the stack.
  //
  // The flags are the ones the file had. MapChunk::save clears FLAG_ALPHA_COMPRESSED in the
  // OUTPUT buffer only (MapChunk.cpp:1647) and never touches _layers_info, so this keeps
  // answering "how was the data now in memory decoded?" across a save -- which is the question,
  // and is why it looks stale but is not.
  bool layerWasCompressed(TextureSet* texture_set, std::size_t stack_layer)
  {
    return (texture_set->flag(stack_layer + 1) & FLAG_ALPHA_COMPRESSED) != 0;
  }

  // Removes the border findings that are not defects, in place, and returns how many were removed.
  //
  // ONE filter, called by every consumer of a ChunkReport in this window, because the alternative
  // is what the coverage machinery exists to prevent: the roll-up, the problem list and the detail
  // pane each describing a different set while being printed next to each other.
  std::size_t suppressCompressedBorderFindings(TextureSet* texture_set, ChunkReport& report)
  {
    if (!texture_set)
    {
      return 0;
    }

    auto& mismatches = report.border_mismatches;

    auto const kept_end
      ( std::remove_if
          ( mismatches.begin()
          , mismatches.end()
          , [texture_set] (Noggit::AlphaBorderMismatch const& mismatch)
            {
              return layerWasCompressed(texture_set, mismatch.layer);
            }
          ));

    std::size_t const dropped (static_cast<std::size_t>(std::distance(kept_end, mismatches.end())));

    mismatches.erase(kept_end, mismatches.end());

    return dropped;
  }

  // Whether ANY layer of the chunk is compressed. Decides repairability rather than reporting,
  // because repairAlphaIntegrity's border pass walks every layer and cannot be aimed at one.
  bool anyLayerWasCompressed(TextureSet* texture_set)
  {
    for (std::size_t layer = 1; layer < texture_set->num(); ++layer)
    {
      if (texture_set->flag(layer) & FLAG_ALPHA_COMPRESSED)
      {
        return true;
      }
    }

    return false;
  }
}

AlphaIntegrityReport::AlphaIntegrityReport(MapView* map_view, QWidget* parent)
  : QDialog(parent)
  , _map_view(map_view)
{
  setWindowTitle("Alpha Map Integrity");
  setMinimumSize(860, 640);

  auto outer (new QVBoxLayout(this));

  // --- what this is, and what already happened without asking --------------------------------
  //
  // First thing in the window rather than a tooltip, because the single most likely misreading of
  // a border row is "Noggit forgot to fix this", when in fact the load-time fix ran and the row
  // means something narrower.
  auto preamble (new QLabel
    ( "Checks the alpha weights of loaded chunks for states an ADT can store but no renderer can\n"
      "show. Two fixes already ran on load and are NOT repeated here: the last row and column of\n"
      "4-bit alpha maps were duplicated, and the old relative weights were converted to absolute.\n\n"
      "So a BORDER row overlaps that first fix: on a chunk whose flags say the border is padding the\n"
      "duplication already ran, and a row here means the alpha has been painted since the tile\n"
      "loaded. Layers whose alpha was compressed are excluded -- their last row is real data, not\n"
      "padding -- and the count of what was excluded is shown with the coverage figures.\n"
      "OVER-OPAQUE overlaps neither fix and is the class worth acting on."
    , this));
  preamble->setWordWrap(true);
  outer->addWidget(preamble);

  // --- scan ----------------------------------------------------------------------------------
  auto scan_box (new QGroupBox("Scan", this));
  auto scan_row (new QHBoxLayout(scan_box));

  scan_row->addWidget(new QLabel("Scope", this));

  _scope = new QComboBox(this);
  _scope->addItem("This ADT tile", 0);
  _scope->addItem("All loaded tiles", 1);
  _scope->setToolTip
    ("Only tiles that are already resident are scanned. Tiles you have not visited are not in\n"
     "scope at all -- loading the whole map to check it is a different operation with a very\n"
     "different cost.");
  scan_row->addWidget(_scope, 1);

  auto scan_button (new QPushButton("Scan", this));
  scan_row->addWidget(scan_button);

  outer->addWidget(scan_box);

  _coverage_label = new QLabel(this);
  _coverage_label->setWordWrap(true);
  outer->addWidget(_coverage_label);

  // --- results -------------------------------------------------------------------------------
  auto result_box (new QGroupBox("Problems", this));
  auto result_layout (new QVBoxLayout(result_box));

  _show_notes = new QCheckBox("Show notes (empty layers, sub-pixel border drift)", this);
  _show_notes->setChecked(true);
  _show_notes->setToolTip
    ("Notes describe waste and noise, not anything visibly wrong. Untick to see only the rows\n"
     "that change what is drawn.");
  result_layout->addWidget(_show_notes);

  _problem_list = new QListWidget(this);
  _problem_list->setToolTip("Selecting a row moves the camera to that chunk.");
  result_layout->addWidget(_problem_list, 1);

  _list_footer = new QLabel(this);
  _list_footer->setWordWrap(true);
  result_layout->addWidget(_list_footer);

  outer->addWidget(result_box, 1);

  _detail = new QLabel(this);
  _detail->setWordWrap(true);
  _detail->setTextInteractionFlags(Qt::TextSelectableByMouse);
  outer->addWidget(_detail);

  // --- repair --------------------------------------------------------------------------------
  auto repair_box (new QGroupBox("Repair", this));
  auto repair_layout (new QVBoxLayout(repair_box));

  _repair_over_opacity = new QCheckBox("Over-opaque texels", this);
  _repair_over_opacity->setChecked(true);
  _repair_over_opacity->setToolTip
    ("Brings each texel's stored weights back to at most 255, which is what lets the chunk's\n"
     "first texture show again.");
  repair_layout->addWidget(_repair_over_opacity);

  auto strategy_row (new QHBoxLayout());
  strategy_row->addSpacing(20);
  strategy_row->addWidget(new QLabel("Strategy", this));

  _strategy = new QComboBox(this);
  _strategy->addItem("Clip the upper layers", static_cast<int>(Noggit::OverOpacityRepair::ClipUpperLayers));
  _strategy->addItem("Scale every layer proportionally"
                    , static_cast<int>(Noggit::OverOpacityRepair::ScaleProportionally));
  _strategy->setToolTip
    ("Clipping takes the excess out of the highest layers and leaves the lower ones byte for\n"
     "byte -- right for drift of a point or two. Scaling preserves the ratios between layers,\n"
     "so the visible blend survives -- right when a stack is grossly over and clipping would\n"
     "delete an upper layer outright.");
  strategy_row->addWidget(_strategy, 1);
  repair_layout->addLayout(strategy_row);

  // OFF by default, and the only one of the three that is off for a reason about data rather than
  // about scope.
  //
  // A border row can only exist on a chunk whose flags say that border is padding -- and on such a
  // chunk Alphamap::readNotCompressed already duplicated it at load. So the disagreement the row
  // describes was created AFTER the load, which on unmodified data means the user painted across
  // the north or east edge since opening the tile. "Repairing" it copies row 62 over row 63 and
  // column 62 over column 63, i.e. reverts that paint -- and it would otherwise have survived the
  // round trip, because MapChunk::save writes do_not_fix_alpha_map = 1 on the normal save path
  // (MapChunk.cpp:1477). A pre-ticked box that quietly discards edits is not a default; the prose
  // in the preamble is a description, not consent.
  _repair_border = new QCheckBox
    ("Unduplicated last row / column -- REVERTS alpha painted along those edges since load", this);
  _repair_border->setChecked(false);
  _repair_border->setToolTip
    ("Off by default because it is the one destructive option here. On a chunk whose flags say the\n"
     "border is padding -- the only kind this is ever applied to -- the load-time duplication has\n"
     "already run, so a border row means the alpha was painted since the tile loaded. Ticking this\n"
     "copies row 62 over row 63 and column 62 over column 63 on every layer, discarding that paint\n"
     "along the chunk's last row and column.\n\n"
     "Never applied to a chunk holding compressed alpha on any layer -- the border pass rewrites\n"
     "every layer at once, and a compressed layer's last row is real data.");
  repair_layout->addWidget(_repair_border);

  _remove_empty_layers = new QCheckBox("Remove layers that are empty after repair", this);
  _remove_empty_layers->setChecked(false);
  _remove_empty_layers->setToolTip
    ("Drops the texture layer entirely, not just its alpha. Off by default because it changes\n"
     "the chunk's layer count and texture list, and because clipping can empty a layer that was\n"
     "not empty before -- so this can remove more layers than the list shows.");
  repair_layout->addWidget(_remove_empty_layers);

  auto repair_buttons (new QHBoxLayout());
  repair_buttons->addStretch(1);

  _repair_selected = new QPushButton("Repair selected", this);
  _repair_selected->setToolTip("Repairs only the class of problem the highlighted row describes.");
  repair_buttons->addWidget(_repair_selected);

  _repair_all = new QPushButton("Repair all", this);
  _repair_all->setToolTip
    ("Repairs every problem the scan found of the classes ticked above -- including rows the\n"
     "filter is hiding and rows beyond the display cap. One undo step for the whole run.");
  repair_buttons->addWidget(_repair_all);

  repair_layout->addLayout(repair_buttons);

  outer->addWidget(repair_box);

  auto bottom (new QHBoxLayout());

  _copy_report = new QPushButton("Copy report", this);
  _copy_report->setToolTip("Puts the coverage figures and every problem on the clipboard.");
  bottom->addWidget(_copy_report);
  bottom->addStretch(1);

  auto close_button (new QPushButton("Close", this));
  bottom->addWidget(close_button);
  outer->addLayout(bottom);

  connect(scan_button, &QPushButton::clicked, this, &AlphaIntegrityReport::onScan);
  connect(_repair_selected, &QPushButton::clicked, this, &AlphaIntegrityReport::onRepairSelected);
  connect(_repair_all, &QPushButton::clicked, this, &AlphaIntegrityReport::onRepairAll);
  connect(_copy_report, &QPushButton::clicked, this, &AlphaIntegrityReport::onCopyReport);
  connect(close_button, &QPushButton::clicked, this, &QWidget::close);

  // rebuildList alone, because it now refreshes the selection-dependent buttons itself. The
  // coverage headline is deliberately NOT rebuilt: it counts every problem found, not the rows
  // this filter happens to be showing, and re-deriving it from the filter is how the two numbers
  // would start disagreeing.
  connect(_show_notes, &QCheckBox::toggled, this, [this] (bool) { rebuildList(); });

  // currentItemChanged rather than itemClicked so arrow keys walk the map too. rebuildList blocks
  // this signal while it refills, or every intermediate selection would fly the camera somewhere.
  connect(_problem_list, &QListWidget::currentItemChanged, this
         , [this] (QListWidgetItem*, QListWidgetItem*) { showSelected(); });

  // Deliberately NOT scanning on construction. A scan walks every loaded chunk, and a window that
  // does that before the user has chosen a scope has already spent the cost of the answer to a
  // question that was not asked.
  _detail->setText("Nothing scanned yet.");
  updateCoverageLabel();
  updateButtons();
}

void AlphaIntegrityReport::onScan()
{
  WaitCursor const busy;

  _problems.clear();
  _totals = Noggit::AlphaScanResult();
  _coverage = Coverage();

  World* world = _map_view->getWorld();

  if (!world)
  {
    _scan_has_run = false;
    _detail->setText("No world is loaded.");
    rebuildList();
    updateCoverageLabel();
    updateButtons();
    return;
  }

  auto const scan_tile
  (
    [this] (MapTile* tile)
    {
      ++_coverage.tiles_in_scope;

      if (!tile || !tile->finishedLoading())
      {
        ++_coverage.tiles_not_loaded;
        return;
      }

      ++_coverage.tiles_examined;

      // 16x16 by hand, and NOT through World::for_all_chunks_on_tile, which calls
      // mapIndex.setChanged(tile) on entry (World.inl:16) -- that would mark every tile it
      // touched as unsaved during what the user asked to be a read-only scan.
      for (unsigned int chunk_z = 0; chunk_z < 16; ++chunk_z)
      {
        for (unsigned int chunk_x = 0; chunk_x < 16; ++chunk_x)
        {
          ++_coverage.chunks_visited;

          MapChunk* chunk = tile->getChunk(chunk_x, chunk_z);

          if (!chunk)
          {
            ++_coverage.chunks_absent;
            continue;
          }

          AlphaChunkAddress address;
          address.tile_x = static_cast<int>(tile->index.x);
          address.tile_z = static_cast<int>(tile->index.z);
          address.chunk_x = static_cast<int>(chunk_x);
          address.chunk_z = static_cast<int>(chunk_z);

          TextureSet* texture_set = chunk->getTextureSet();

          ChunkReport report = Noggit::AlphaIntegrityCollector::checkChunk<MapChunk>(chunk);

          // SUPPRESSION HAPPENS HERE, before anything counts the findings, and that ordering is
          // the point. A border mismatch on a layer whose alpha arrived compressed is not a defect
          // -- readCompressed handed over 4096 genuine bytes and row 63 is what the artist painted
          // -- so it is dropped from the rows below. Dropping it from the rows but not from the
          // roll-up would make _totals describe a strictly larger set than the window and the
          // copied report do, and those two are printed next to each other: the headline would say
          // "1 problem(s) across 30 ... chunk(s)", or say "No problems" directly above a clipboard
          // payload claiming 30 chunks with defects. One filtered report feeds both.
          //
          // What was dropped is still reported, as a count, in the coverage text -- a finding
          // removed in silence is indistinguishable from a check that never ran.
          _coverage.border_findings_suppressed
            += suppressCompressedBorderFindings(texture_set, report);

          // Fed to the shared roll-up on the same terms collectTile uses -- every non-null chunk,
          // including the ones with nothing to check -- so chunksScanned() here means what it
          // means everywhere else that prints an AlphaScanResult.
          _totals.addChunk(address, report);

          if (report.layer_count == 0)
          {
            // Nothing was examined. Which of the two reasons it was is worth separating: "one
            // texture" is the normal state of untouched terrain, "no texture set" is not.
            if (texture_set)
            {
              ++_coverage.chunks_single_texture;
            }
            else
            {
              ++_coverage.chunks_without_texture_set;
            }

            continue;
          }

          ++_coverage.chunks_examined;

          if (report.over_opaque_texels != 0)
          {
            Problem problem;
            problem.address = address;
            problem.defect = AlphaDefect::OverOpaque;
            problem.texels = report.over_opaque_texels;
            problem.magnitude = report.max_excess;
            problem.severity = report.max_excess <= ROUNDING_DRIFT ? Severity::Warning
                                                                   : Severity::Error;
            _problems.push_back(problem);
          }

          for (std::size_t layer : report.empty_layers)
          {
            Problem problem;
            problem.address = address;
            problem.defect = AlphaDefect::EmptyLayer;
            problem.layer = static_cast<int>(layer);
            problem.severity = Severity::Note;
            _problems.push_back(problem);
          }

          // Computed once per chunk rather than per mismatch: it is a property of the chunk, and
          // it is what decides whether the border rows below can be acted on at all.
          bool const chunk_has_compressed_alpha
            (texture_set && anyLayerWasCompressed(texture_set));

          // Already filtered above, so every mismatch still here is a real one.
          for (Noggit::AlphaBorderMismatch const& mismatch : report.border_mismatches)
          {
            Problem problem;
            problem.address = address;
            problem.defect = AlphaDefect::BorderMismatch;
            problem.layer = static_cast<int>(mismatch.layer);
            problem.texels = mismatch.total();
            problem.magnitude = mismatch.max_delta;
            problem.severity = mismatch.max_delta <= ROUNDING_DRIFT ? Severity::Note
                                                                    : Severity::Warning;
            problem.border_repair_unsafe = chunk_has_compressed_alpha;
            _problems.push_back(problem);
          }
        }
      }
    }
  );

  if (_scope->currentData().toInt() == 1)
  {
    // loaded_tiles() already filters to non-null and finishedLoading (map_index.cpp:29), so
    // tiles_not_loaded stays 0 on this path -- and the coverage text says which tiles were in
    // scope at all, because "all loaded tiles" is a much smaller set than a map.
    for (MapTile* tile : world->mapIndex.loaded_tiles())
    {
      scan_tile(tile);
    }
  }
  else
  {
    scan_tile(world->mapIndex.getTile(::TileIndex(_map_view->cameraPosition())));
  }

  _scan_has_run = true;

  rebuildList();
  updateCoverageLabel();
  updateButtons();

  _detail->setText("Select a row to fly to the chunk it names.");

  Log << "Alpha map integrity: " << _totals.summary() << std::endl;
}

void AlphaIntegrityReport::rebuildList()
{
  QSignalBlocker const blocker (_problem_list);

  _problem_list->clear();

  bool const show_notes = _show_notes->isChecked();

  int shown = 0;
  std::size_t filtered = 0;
  std::size_t capped = 0;

  for (std::size_t i = 0; i < _problems.size(); ++i)
  {
    Problem const& problem = _problems[i];

    if (!show_notes && problem.severity == Severity::Note)
    {
      ++filtered;
      continue;
    }

    if (shown >= MAX_LIST_ROWS)
    {
      ++capped;
      continue;
    }

    QString severity;

    switch (problem.severity)
    {
      case Severity::Error:   severity = "error";   break;
      case Severity::Warning: severity = "warning"; break;
      case Severity::Note:    severity = "note";    break;
    }

    QString description;

    switch (problem.defect)
    {
      case AlphaDefect::OverOpaque:
        description = QString("over-opaque: %1 texel(s), worst +%2 -- the first texture cannot show there")
                        .arg(problem.texels).arg(problem.magnitude);
        break;

      case AlphaDefect::EmptyLayer:
        description = QString("layer %1 is empty: paints nothing, costs an MCLY entry and a texture reference")
                        .arg(problem.layer + 1);
        break;

      case AlphaDefect::BorderMismatch:
        description = QString("layer %1 border: %2 texel(s) not duplicated, worst delta %3")
                        .arg(problem.layer + 1).arg(problem.texels).arg(problem.magnitude);

        if (problem.border_repair_unsafe)
        {
          description += " -- NOT REPAIRABLE: another layer of this chunk holds compressed alpha";
        }
        break;
    }

    auto* item = new QListWidgetItem
      ( QString("[%1]  %2  |  %3").arg(severity, -7).arg(addressLabel(problem.address), description)
      , _problem_list);

    // The _problems index, not the row. See selectedProblemIndex.
    item->setData(Qt::UserRole, static_cast<int>(i));

    ++shown;
  }

  if (!_scan_has_run)
  {
    _list_footer->setText(QString());
  }
  else
  {
    QStringList notes;
    notes << QString("Showing %1 of %2 problem(s).").arg(shown).arg(_problems.size());

    if (filtered != 0)
    {
      notes << QString("%1 note(s) hidden by the filter.").arg(filtered);
    }

    if (capped != 0)
    {
      notes << QString("%1 beyond the %2-row display cap -- still counted, still repaired by "
                       "Repair all, still in the copied report.").arg(capped).arg(MAX_LIST_ROWS);
    }

    _list_footer->setText(notes.join(" "));
  }

  // Refilling the list DESTROYS the selection -- clear() deletes every item, so currentItem() is
  // null and selectedProblemIndex() is -1 the moment this function returns -- and it does so under
  // a QSignalBlocker, so currentItemChanged never fires and nothing downstream notices on its own.
  // Anything derived from the selection is therefore refreshed here rather than at the call sites,
  // because every call site has the same obligation and one of them (the notes filter) forgot it
  // and left "Repair selected" enabled with nothing selected.
  updateButtons();
}

void AlphaIntegrityReport::updateCoverageLabel()
{
  if (!_scan_has_run)
  {
    // The third state, and the one a plain "0 problems" would silently impersonate.
    _coverage_label->setStyleSheet("font-style: italic;");
    _coverage_label->setText
      ("No scan has been run yet. Nothing below describes this map -- choose a scope and press Scan.");
    return;
  }

  QString headline;

  if (_coverage.chunks_examined == 0)
  {
    // Loudest state in the window on purpose. Every count below is zero, "no problems" is
    // literally true, and it means nothing whatsoever.
    _coverage_label->setStyleSheet("font-weight: bold;");
    headline = "NOTHING WAS CHECKED. No chunk in scope carried a stored alpha layer, so this scan "
               "makes no claim about the map at all.";
  }
  else if (_problems.empty())
  {
    _coverage_label->setStyleSheet(QString());
    headline = QString("No problems in the %1 chunk(s) that carry alpha layers.")
                 .arg(_coverage.chunks_examined);
  }
  else
  {
    _coverage_label->setStyleSheet(QString());
    headline = QString("%1 problem(s) across %2 of the %3 chunk(s) that carry alpha layers.")
                 .arg(_problems.size())
                 .arg(_totals.chunksWithDefects())
                 .arg(_coverage.chunks_examined);
  }

  QString coverage
    ( QString("Coverage: %1 tile(s) in scope, %2 examined, %3 not loaded. "
              "%4 chunk(s) visited -- %5 with alpha layers, %6 with a single texture (no stored "
              "alpha to check), %7 with no texture set, %8 absent.")
        .arg(_coverage.tiles_in_scope)
        .arg(_coverage.tiles_examined)
        .arg(_coverage.tiles_not_loaded)
        .arg(_coverage.chunks_visited)
        .arg(_coverage.chunks_examined)
        .arg(_coverage.chunks_single_texture)
        .arg(_coverage.chunks_without_texture_set)
        .arg(_coverage.chunks_absent));

  if (_coverage.border_findings_suppressed != 0)
  {
    coverage += QString("\n%1 border finding(s) suppressed on layers whose alpha was compressed: "
                        "their last row and column are real data, not padding.")
                  .arg(_coverage.border_findings_suppressed);
  }

  _coverage_label->setText(headline + "\n" + coverage);
}

void AlphaIntegrityReport::updateButtons()
{
  _repair_all->setEnabled(_scan_has_run && !_problems.empty());
  _repair_selected->setEnabled(selectedProblemIndex() >= 0);
  _copy_report->setEnabled(_scan_has_run);
}

int AlphaIntegrityReport::selectedProblemIndex() const
{
  auto const* item = _problem_list->currentItem();

  if (!item)
  {
    return -1;
  }

  bool ok = false;
  int const index = item->data(Qt::UserRole).toInt(&ok);

  return (ok && index >= 0 && static_cast<std::size_t>(index) < _problems.size()) ? index : -1;
}

MapChunk* AlphaIntegrityReport::resolveChunk(AlphaChunkAddress const& address) const
{
  World* world = _map_view->getWorld();

  if (!world)
  {
    return nullptr;
  }

  ::TileIndex const index ( static_cast<std::size_t>(address.tile_x)
                          , static_cast<std::size_t>(address.tile_z));

  // getTile is bounds-checked and returns null outside 64x64 (map_index.cpp:614), so a garbage
  // address cannot index the array.
  MapTile* tile = world->mapIndex.getTile(index);

  if (!tile || !tile->finishedLoading())
  {
    return nullptr;
  }

  return tile->getChunk(static_cast<unsigned int>(address.chunk_x)
                       , static_cast<unsigned int>(address.chunk_z));
}

void AlphaIntegrityReport::focusOn(AlphaChunkAddress const& address)
{
  MapChunk* chunk = resolveChunk(address);

  if (!chunk)
  {
    return;
  }

  // The move goes through MapView::move_camera_with_auto_height, the same public camera move the
  // minimap and bookmark navigation use: it makes the GL context current, waits for the tile when
  // it is not resident, resolves the terrain height under the target and raises the camera-dirty
  // flag. MapView::focusOnSpawn cannot be called instead -- it takes a SpawnRef and resolves the
  // target through the database spawn cache, which is null in every session that has not loaded
  // server spawns, i.e. every session this window is used in.
  //
  // Only the aim is added, because move_camera_with_auto_height leaves the camera pointing
  // wherever it already pointed, which after any amount of flying around is usually the sky.
  _map_view->move_camera_with_auto_height(chunk->vcenter);
  _map_view->getCamera()->pitch(math::degrees(FOCUS_PITCH_DEGREES));

  // Two separate flags, and both are needed. move_camera_with_auto_height sets the camera-moved
  // flag but nothing repaints on its own; invalidate() is what asks for the frame. focusOnSpawn
  // raises the same pair for the same reason (MapView.cpp:899-900).
  _map_view->setCameraDirty();
  _map_view->invalidate();
}

void AlphaIntegrityReport::showSelected()
{
  updateButtons();

  int const index = selectedProblemIndex();

  if (index < 0)
  {
    return;
  }

  Problem const& problem = _problems[static_cast<std::size_t>(index)];

  focusOn(problem.address);

  MapChunk* chunk = resolveChunk(problem.address);

  if (!chunk)
  {
    _detail->setText
      ( QString("%1 is no longer loaded. Re-scan before repairing.")
          .arg(addressLabel(problem.address)));
    return;
  }

  // Recomputed rather than stored: it is one chunk, it costs microseconds, and it is guaranteed
  // to describe the chunk as it is now rather than as it was when the scan ran.
  ChunkReport report = Noggit::AlphaIntegrityCollector::checkChunk<MapChunk>(chunk);

  // Through the same filter the scan used, because the paragraph appended below states that
  // compressed layers are not reported here -- and an unfiltered toReport() would list them two
  // lines above that sentence.
  std::size_t const suppressed
    (suppressCompressedBorderFindings(chunk->getTextureSet(), report));

  QString text (addressLabel(problem.address) + "\n" + QString::fromStdString(report.toReport()));

  if (suppressed != 0)
  {
    text += QString("\n  %1 border finding(s) on this chunk are not listed above: those layers' "
                    "alpha was compressed, so their last row and column are real data.")
              .arg(suppressed);
  }

  if (problem.defect == AlphaDefect::BorderMismatch)
  {
    text += "\n  This class overlaps the load-time duplication, which has already run on this "
            "chunk -- so this row means the alpha was painted since the tile loaded. Layers whose "
            "own alpha was compressed are not reported here at all; their last row is real data.";

    if (problem.border_repair_unsafe)
    {
      text += "\n  Repair is refused for this chunk. Another of its layers does hold compressed "
              "alpha, and the border pass rewrites every layer at once, so fixing this one would "
              "overwrite that one's genuine last row and column.";
    }
  }

  if (problem.defect == AlphaDefect::EmptyLayer)
  {
    text += "\n  Repairing this removes the texture layer, not just its alpha. Nothing drawn "
            "changes; the layer's weight is already zero everywhere.";
  }

  _detail->setText(text);
}

AlphaIntegrityReport::RepairOutcome AlphaIntegrityReport::repairChunkAt
  ( AlphaChunkAddress const& address
  , bool fix_over_opacity
  , bool fix_border
  , bool drop_empty_layers
  , RepairTally& tally
  )
{
  MapChunk* chunk = resolveChunk(address);

  if (!chunk)
  {
    return RepairOutcome::ChunkGone;
  }

  TextureSet* texture_set = chunk->getTextureSet();

  if (!texture_set)
  {
    return RepairOutcome::ChunkGone;
  }

  Noggit::AlphaLayerStack const before
    (Noggit::AlphaIntegrityCollector::readTextureSet(texture_set));

  if (before.empty())
  {
    return RepairOutcome::NoChange;
  }

  // Border repair is withdrawn, not just discouraged, on any chunk holding compressed alpha:
  // restoreBorder walks every layer of the stack, so there is no way to fix the padded layers
  // without overwriting the real last row and column of the compressed one. Losing a repair is
  // recoverable; losing the data is not.
  bool const border_is_safe = !anyLayerWasCompressed(texture_set);

  if (fix_border && !border_is_safe)
  {
    ++tally.border_declined;
  }

  Noggit::AlphaRepairOptions repair;
  repair.repair_over_opacity = fix_over_opacity;
  repair.repair_border = fix_border && border_is_safe;
  repair.over_opacity_strategy
    = static_cast<Noggit::OverOpacityRepair>(_strategy->currentData().toInt());

  Noggit::AlphaRepairResult const result
    ( Noggit::repairAlphaIntegrity
        ( before
        , Noggit::AlphaIntegrityCollector::optionsForChunk<MapChunk>(chunk)
        , repair
        ));

  bool changed = false;

  if (result.changed())
  {
    // Taken before the first write, and only for chunks that are actually about to be written:
    // registerChunkTextureChange is idempotent per chunk (Action.cpp:671-675), so the second call
    // below is free, but snapshotting a chunk that turns out to need nothing would put a no-op
    // pair into the undo step and copy three alpha maps for it.
    NOGGIT_CUR_ACTION->registerChunkTextureChange(chunk);

    if (!Noggit::AlphaIntegrityCollector::applyToTextureSet(texture_set, result.layers))
    {
      // Two ways in: the chunk's layer count moved between the read and the write, or the repair
      // produced values for a layer that has no stored alpha map. The second is unreachable in
      // practice -- a layer with no alpha map reads as zero, and neither clipping nor scaling nor
      // the border copy can raise a zero layer -- but it is checked rather than assumed, and the
      // snapshot above means undo recovers even from a partial write.
      return RepairOutcome::WriteRefused;
    }

    changed = true;
  }

  if (drop_empty_layers && !result.layers_to_drop.empty())
  {
    NOGGIT_CUR_ACTION->registerChunkTextureChange(chunk);

    // Descending, because eraseTexture shifts every layer above the removed one down by one
    // (texture_set.cpp:234-248); ascending would erase the wrong layers after the first.
    //
    // +1 on the index: AlphaLayerStack index 0 is chunk texture layer 1. Layer 0 has no stored
    // alpha and is never in the stack, so this can never erase the base texture.
    for (auto it = result.layers_to_drop.rbegin(); it != result.layers_to_drop.rend(); ++it)
    {
      texture_set->eraseTexture(*it + 1);
      ++tally.layers_removed;
      changed = true;
    }
  }

  if (changed && chunk->mt)
  {
    // registerChunkUpdate, which markDirty and eraseTexture both raise, is a render-refresh flag
    // and nothing more. The save path tests MapTile::changed (map_index.cpp:555 and :584), so
    // without this the ADT is skipped by saveChanged and the repair is lost on unload.
    _map_view->getWorld()->mapIndex.setChanged(chunk->mt);
  }

  return changed ? RepairOutcome::Changed : RepairOutcome::NoChange;
}

void AlphaIntegrityReport::onRepairSelected()
{
  int const index = selectedProblemIndex();

  if (index < 0)
  {
    _detail->setText("Select a row first.");
    return;
  }

  // Copied, not referenced: the rescan at the end of this function clears _problems.
  Problem const problem = _problems[static_cast<std::size_t>(index)];

  // The row is the instruction. Repairing only the class the user pointed at is what makes
  // per-problem repair different from a small Repair all -- a chunk that is both over-opaque and
  // has an unduplicated border keeps the border when the over-opaque row is the one repaired.
  bool const fix_over = problem.defect == AlphaDefect::OverOpaque;
  bool const fix_border = problem.defect == AlphaDefect::BorderMismatch;
  bool const drop_empty = problem.defect == AlphaDefect::EmptyLayer;

  _map_view->context()->makeCurrent(_map_view->context()->surface());
  OpenGL::context::scoped_setter const _ (::gl, _map_view->context());

  RepairTally tally;
  RepairOutcome outcome = RepairOutcome::NoChange;

  NOGGIT_ACTION_MGR->beginAction(_map_view, Noggit::ActionFlags::eCHUNKS_TEXTURE);

  try
  {
    outcome = repairChunkAt(problem.address, fix_over, fix_border, drop_empty, tally);
  }
  catch (std::exception const& e)
  {
    // endAction has to run on this path too, or the action manager is left with a live action and
    // every later beginAction quietly returns it instead of starting a new one (ActionManager.cpp:64).
    NOGGIT_ACTION_MGR->endAction();

    LogError << "Alpha map repair failed: " << e.what() << std::endl;
    _detail->setText(QString("Repair failed: %1").arg(e.what()));
    return;
  }

  NOGGIT_ACTION_MGR->endAction();

  QString message;

  switch (outcome)
  {
    case RepairOutcome::Changed:
      message = QString("Repaired %1.").arg(addressLabel(problem.address));

      if (tally.layers_removed != 0)
      {
        message += QString(" %1 empty layer(s) removed.").arg(tally.layers_removed);
      }

      message += " One undo step; the ADT is marked changed and still has to be saved.";
      break;

    case RepairOutcome::NoChange:
      message = tally.border_declined != 0
              ? QString("%1 was left alone: it holds compressed alpha, and the border pass "
                        "rewrites every layer at once, so repairing it would destroy real data.")
                  .arg(addressLabel(problem.address))
              : QString("%1 needed no change -- it was repaired already, or the class you picked "
                        "does not apply to it.").arg(addressLabel(problem.address));
      break;

    case RepairOutcome::ChunkGone:
      message = QString("%1 is no longer loaded. Nothing was written.")
                  .arg(addressLabel(problem.address));
      break;

    case RepairOutcome::WriteRefused:
      message = QString("%1 refused the write -- its layer count changed under the scan. Undo, "
                        "then re-scan.").arg(addressLabel(problem.address));
      break;
  }

  Log << message.toStdString() << std::endl;
  rescanAfterRepair(message);
}

void AlphaIntegrityReport::onRepairAll()
{
  if (!_scan_has_run || _problems.empty())
  {
    return;
  }

  bool const fix_over = _repair_over_opacity->isChecked();
  bool const fix_border = _repair_border->isChecked();
  bool const drop_empty = _remove_empty_layers->isChecked();

  if (!fix_over && !fix_border && !drop_empty)
  {
    _detail->setText("Nothing is ticked in Repair, so there is nothing to do.");
    return;
  }

  auto const selected
  (
    [&] (AlphaDefect defect)
    {
      switch (defect)
      {
        case AlphaDefect::OverOpaque:     return fix_over;
        case AlphaDefect::BorderMismatch: return fix_border;
        case AlphaDefect::EmptyLayer:     return drop_empty;
      }

      return false;
    }
  );

  // wants_border is carried per chunk rather than left to the checkbox alone so that the
  // "border repair declined" count means what it says. Without it, a chunk that is merely
  // over-opaque and happens to hold compressed alpha would be counted as a refused border repair
  // it never asked for, and the report would invent a problem.
  struct Target
  {
    AlphaChunkAddress address;
    bool wants_border = false;
  };

  // Distinct chunks, in scan order. Adjacent comparison is exact rather than a shortcut: the scan
  // emits every problem of one chunk consecutively, so a repeat of an earlier address is
  // impossible. A general search would be O(problems x chunks), which on a badly converted map
  // set is tens of millions of comparisons for a set that is already unique.
  std::vector<Target> targets;

  for (Problem const& problem : _problems)
  {
    if (!selected(problem.defect))
    {
      continue;
    }

    if (targets.empty() || !(targets.back().address == problem.address))
    {
      targets.push_back(Target{problem.address, false});
    }

    if (problem.defect == AlphaDefect::BorderMismatch)
    {
      targets.back().wants_border = true;
    }
  }

  if (targets.empty())
  {
    _detail->setText("No problem found matches the classes ticked in Repair.");
    return;
  }

  WaitCursor const busy;

  _map_view->context()->makeCurrent(_map_view->context()->surface());
  OpenGL::context::scoped_setter const _ (::gl, _map_view->context());

  std::size_t changed = 0;
  std::size_t unchanged = 0;
  std::size_t gone = 0;
  std::size_t refused = 0;
  RepairTally tally;

  // ONE action around the whole run, which is what makes the whole run one undo step. Chunks
  // register themselves into it as they are about to be written, so the step holds exactly the
  // chunks that changed.
  NOGGIT_ACTION_MGR->beginAction(_map_view, Noggit::ActionFlags::eCHUNKS_TEXTURE);

  try
  {
    for (Target const& target : targets)
    {
      switch (repairChunkAt( target.address
                           , fix_over
                           , fix_border && target.wants_border
                           , drop_empty
                           , tally
                           ))
      {
        case RepairOutcome::Changed:      ++changed;   break;
        case RepairOutcome::NoChange:     ++unchanged; break;
        case RepairOutcome::ChunkGone:    ++gone;      break;
        case RepairOutcome::WriteRefused: ++refused;   break;
      }
    }
  }
  catch (std::exception const& e)
  {
    NOGGIT_ACTION_MGR->endAction();

    LogError << "Alpha map repair-all failed after " << changed << " chunk(s): "
             << e.what() << std::endl;

    rescanAfterRepair
      ( QString("Repair stopped after %1 chunk(s): %2. What was written is in one undo step.")
          .arg(changed).arg(e.what()));
    return;
  }

  NOGGIT_ACTION_MGR->endAction();

  QStringList parts;
  parts << QString("Repaired %1 of %2 chunk(s) in one undo step.").arg(changed).arg(targets.size());

  if (tally.layers_removed != 0)
  {
    parts << QString("%1 empty layer(s) removed.").arg(tally.layers_removed);
  }

  if (tally.border_declined != 0)
  {
    parts << QString("%1 chunk(s) kept their border: they hold compressed alpha, whose last row "
                     "and column are real data.").arg(tally.border_declined);
  }

  if (unchanged != 0)
  {
    parts << QString("%1 needed no change.").arg(unchanged);
  }

  if (gone != 0)
  {
    parts << QString("%1 had unloaded since the scan and were skipped.").arg(gone);
  }

  if (refused != 0)
  {
    parts << QString("%1 refused the write -- their layer count changed under the scan.")
               .arg(refused);
  }

  if (changed != 0)
  {
    parts << "The ADTs are marked changed and still have to be saved.";
  }

  QString const message (parts.join(" "));

  Log << message.toStdString() << std::endl;
  rescanAfterRepair(message);
}

void AlphaIntegrityReport::rescanAfterRepair(QString const& message)
{
  // Re-scanning is not optional. Repair changes what the rows describe, and removing a layer
  // renumbers the ones above it, so every stored layer index above a removal is wrong the instant
  // it happens. Setting the message afterwards, because onScan writes the detail pane too.
  onScan();
  _detail->setText(message);
}

void AlphaIntegrityReport::onCopyReport()
{
  if (!_scan_has_run)
  {
    return;
  }

  QStringList lines;

  lines << QString::fromStdString(_totals.toReport()).trimmed();
  lines << QString();
  lines << _coverage_label->text();
  lines << QString();

  // Every problem, not the visible rows: a report that inherits the window's filter and display
  // cap would be a different claim than the window makes above it.
  lines << QString("Problems (%1):").arg(_problems.size());

  for (Problem const& problem : _problems)
  {
    QString description;

    switch (problem.defect)
    {
      case AlphaDefect::OverOpaque:
        description = QString("over-opaque, %1 texel(s), worst +%2")
                        .arg(problem.texels).arg(problem.magnitude);
        break;

      case AlphaDefect::EmptyLayer:
        description = QString("layer %1 empty").arg(problem.layer + 1);
        break;

      case AlphaDefect::BorderMismatch:
        description = QString("layer %1 border, %2 texel(s), worst delta %3")
                        .arg(problem.layer + 1).arg(problem.texels).arg(problem.magnitude);
        break;
    }

    lines << QString("  %1: %2").arg(addressLabel(problem.address), description);
  }

  QGuiApplication::clipboard()->setText(lines.join("\n"));

  _detail->setText(QString("Report copied: %1 problem(s).").arg(_problems.size()));
}
