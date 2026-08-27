// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_UI_ALPHAINTEGRITYREPORT_HPP
#define NOGGIT_UI_ALPHAINTEGRITYREPORT_HPP

#include <noggit/terrain/AlphaIntegrity.hpp>

#include <QtWidgets/QDialog>

#include <cstddef>
#include <vector>

class MapChunk;
class MapView;

class QCheckBox;
class QComboBox;
class QLabel;
class QListWidget;
class QPushButton;

namespace Noggit::Ui
{
  // Finds and repairs the alpha map states Noggit::AlphaIntegrity defines, over the chunks that
  // are actually loaded.
  //
  // NAME COLLISION, deliberate and worth stating once. Noggit::AlphaIntegrityReport (a value
  // struct, AlphaIntegrity.hpp:197) is one chunk's findings; Noggit::Ui::AlphaIntegrityReport is
  // this window. They are in different namespaces and never ambiguous to the compiler, but the
  // .cpp aliases the struct to ChunkReport so a human reading a line can tell which is meant.
  //
  // WHAT THIS DOES NOT CLAIM TO FIND. Two repairs already ran before any chunk reached this code
  // and are not repeated:
  //
  //   - Alphamap::readNotCompressed duplicates row 62 into 63 and column 62 into 63 for chunks
  //     whose flags say that border is padding.
  //   - TextureSet::convertToBigAlpha turns the old format's relative weights into absolute ones,
  //     on load, for every chunk.
  //
  // So the border class here OVERLAPS the first of those, and the window says so rather than
  // offering to fix something that was already fixed -- see the note built in the constructor.
  // Over-opacity overlaps neither: it is a property of the converted weights that nothing in the
  // load path looks at.
  //
  // The border class also carries a false positive that AlphaIntegrityOptions cannot express.
  // check_border is decided per CHUNK, from two chunk flags, but whether a layer's alpha was
  // compressed is decided per LAYER -- and a compressed layer delivers 4096 genuine bytes, where
  // row 63 is real data and disagreeing with row 62 is what the artist painted. This window
  // resolves that per layer off the retained MCLY flags, suppresses those rows, counts what it
  // suppressed, and refuses border repair on any chunk holding compressed alpha, because
  // repairAlphaIntegrity's border pass is all-layers and cannot be aimed.
  //
  // The counted-coverage machinery is not decoration. A scan whose scope resolved to nothing --
  // an unloaded tile, chunks with one texture and therefore no stored alpha -- produces exactly
  // the same "no problems" as a genuinely clean map, and the second is a claim while the first is
  // an absence of one. Coverage is reported beside every result so the two cannot be confused,
  // and an unrun scan is a third state again.
  class AlphaIntegrityReport : public QDialog
  {
    Q_OBJECT

    public:
      explicit AlphaIntegrityReport(MapView* map_view, QWidget* parent = nullptr);

    private:
      // How much the user should care. Not a property of the defect class alone: the module's own
      // documentation separates a stack that is one or two over everywhere (rounding drift) from
      // one that is hundreds over (layers pasted together), and only the second is a visible
      // hole in the terrain.
      enum class Severity
      {
        Note,
        Warning,
        Error
      };

      // One row. Several per chunk are normal -- a chunk can be over-opaque AND carry an empty
      // layer AND have two layers with an unduplicated border.
      struct Problem
      {
        AlphaChunkAddress address;
        AlphaDefect defect = AlphaDefect::OverOpaque;

        // Explicit-layer index (0 here is chunk texture layer 1), or -1 when the defect belongs to
        // the whole stack. Over-opacity is a property of the SUM, so blaming one layer for it
        // would be a fiction.
        int layer = -1;

        std::size_t texels = 0;
        // Worst excess for over-opacity, worst per-texel delta for a border. 0 for an empty layer,
        // which has no magnitude -- it is empty or it is not.
        int magnitude = 0;

        Severity severity = Severity::Note;

        // Border rows only. True when some layer of this chunk holds compressed alpha, so the
        // all-layers border pass would overwrite real data on the way to fixing this one.
        bool border_repair_unsafe = false;
      };

      // What the scan reached, as opposed to what it found.
      struct Coverage
      {
        std::size_t tiles_in_scope = 0;
        std::size_t tiles_examined = 0;
        std::size_t tiles_not_loaded = 0;

        std::size_t chunks_visited = 0;
        std::size_t chunks_absent = 0;
        std::size_t chunks_without_texture_set = 0;
        // One texture covers the chunk, so there is no stored alpha and nothing to be wrong.
        std::size_t chunks_single_texture = 0;
        // Had at least one explicit alpha layer, i.e. was genuinely examined. THE number that
        // decides whether a clean result means anything.
        std::size_t chunks_examined = 0;

        // Border findings dropped because that layer's alpha was compressed. Reported, never
        // silent: a finding the user cannot see is the same failure mode as a scan that examined
        // nothing, just at a smaller scale.
        std::size_t border_findings_suppressed = 0;
      };

      enum class RepairOutcome
      {
        Changed,
        NoChange,
        // The tile unloaded between the scan and the repair.
        ChunkGone,
        // applyToTextureSet refused the write; see onRepairAll for the two ways that happens.
        WriteRefused
      };

      // The counters a single repairChunkAt call cannot express through its return value.
      struct RepairTally
      {
        std::size_t layers_removed = 0;
        std::size_t border_declined = 0;
      };

      void onScan();
      void onRepairSelected();
      void onRepairAll();
      void onCopyReport();

      void rebuildList();
      void showSelected();
      void updateCoverageLabel();
      void updateButtons();

      // Index into _problems for the highlighted row, or -1.
      //
      // Carried in the item's data, never taken from currentRow(): the list is filtered by
      // severity and capped in length, so the row number and the _problems index are different
      // numbers and confusing them repairs a chunk the user is not looking at.
      int selectedProblemIndex() const;

      // The live chunk at an address, or null.
      //
      // Addresses are stored rather than MapChunk pointers because tiles unload while this window
      // is open -- the viewport drops distant ones as the camera moves -- and a stored pointer
      // would be dangling by the time a repair button reached it. Resolving late costs two array
      // lookups and cannot dangle.
      MapChunk* resolveChunk(AlphaChunkAddress const& address) const;

      void focusOn(AlphaChunkAddress const& address);

      // Repairs one chunk. The caller owns the surrounding ActionManager action, which is what
      // makes a whole repair-all collapse into one undo step.
      RepairOutcome repairChunkAt( AlphaChunkAddress const& address
                                 , bool fix_over_opacity
                                 , bool fix_border
                                 , bool drop_empty_layers
                                 , RepairTally& tally
                                 );

      // Re-runs the scan and puts `message` in the detail pane afterwards, so a repair report is
      // not immediately overwritten by the rescan it triggers.
      void rescanAfterRepair(QString const& message);

      MapView* _map_view;

      // Third state. False means no scan has been run, which is not the same as a scan that found
      // nothing, and the window must never let those look alike.
      bool _scan_has_run = false;

      Coverage _coverage;
      // The shared roll-up, kept so the copied report speaks the same vocabulary as
      // AlphaScanResult::toReport() everywhere else in the tree rather than a second dialect.
      AlphaScanResult _totals;
      std::vector<Problem> _problems;

      QComboBox* _scope;
      QComboBox* _strategy;
      QCheckBox* _show_notes;
      QCheckBox* _repair_over_opacity;
      QCheckBox* _repair_border;
      QCheckBox* _remove_empty_layers;
      QListWidget* _problem_list;
      QLabel* _coverage_label;
      QLabel* _list_footer;
      QLabel* _detail;
      QPushButton* _repair_selected;
      QPushButton* _repair_all;
      QPushButton* _copy_report;
  };
}

#endif
