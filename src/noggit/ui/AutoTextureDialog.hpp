// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_UI_AUTOTEXTUREDIALOG_HPP
#define NOGGIT_UI_AUTOTEXTUREDIALOG_HPP

#include <noggit/terrain/TerrainRules.hpp>
#include <noggit/TileIndex.hpp>

#include <QtWidgets/QDialog>

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

class MapChunk;
class MapTile;
class MapView;

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QGroupBox;
class QLabel;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;

namespace Noggit::Ui
{
  // Authoring and application of a TerrainRuleSet: "rock above 60 degrees, snow above 400, grass
  // elsewhere", applied to a tile or to everything loaded.
  //
  // The rule evaluator itself is TerrainRules.{hpp,cpp} and knows nothing about Noggit; this is the
  // window that fills a rule set in, shows what it would do, and then paints it. The split matters
  // because the interesting part -- precedence, range edge cases, coverage -- is under test on a
  // bare machine, and none of it needs a GL context.
  //
  // PREVIEW IS NOT OPTIONAL. Apply stays disabled until a preview has been run against the current
  // rules, and any edit puts it back. The failure mode of rule-driven
  // texturing is not a wrong texture, which is obvious the moment you look at it -- it is terrain
  // that NO rule claims, which is silently left holding whatever was painted there before and looks
  // exactly like terrain the rules got right. The only defence is making the user read the
  // unmatched count before committing.
  //
  // THE SCOPE IS PINNED THE SAME WAY, but it cannot be pinned by a widget signal: "this ADT tile"
  // means the tile the CAMERA is over, this dialog is modeless, and the camera keeps moving while
  // the report is being read. So the preview records the tiles it actually walked, Apply walks
  // exactly those, and it refuses outright if the scope has resolved to something else in the
  // meantime -- because the unmatched-unit count the user agreed to was computed for the old one.
  //
  // RULES ARE EVALUATED PER 8x8 CHUNK UNIT, not per alphamap texel. That is the resolution the
  // chunk's own texture weighting and doodad mapping already work at (texture_set.cpp:1475), the
  // inner vertex of a unit carries the averaged normal for free (see
  // TerrainRuleCollector::sampleChunkUnit), and 64 decisions per chunk instead of 4096 is what
  // makes "all loaded tiles" finish.
  class AutoTextureDialog : public QDialog
  {
    Q_OBJECT

    public:
      explicit AutoTextureDialog(MapView* map_view, QWidget* parent = nullptr);

    private:
      // What one preview pass measured. Kept whole so Apply can restate the numbers the user
      // actually agreed to rather than recomputing them and possibly disagreeing.
      struct PreviewResult
      {
        Noggit::TerrainRuleCoverage coverage;
        // Indexed by rule index, so a disabled or shadowed rule shows up as a zero here rather
        // than being absent. Two rules naming the same texture stay distinguishable, which the
        // coverage tally alone cannot do -- it keys on texture.
        std::vector<std::size_t> units_per_rule;
        std::size_t chunks_scanned = 0;
        std::size_t tiles_scanned = 0;
        // The scope as it resolved when this preview ran, in ascending index order. Apply walks
        // this list rather than resolving the scope a second time; see the note above the class.
        // Empty for a preview that never ran.
        std::vector<TileIndex> tiles;
        // Chunks whose winning textures alone exceed the four layers an MCNK can hold. These
        // cannot be satisfied however the paint is ordered.
        std::size_t chunks_over_layer_limit = 0;
        // Chunks where the winning textures fit but only after existing layers are evicted.
        std::size_t chunks_needing_eviction = 0;
      };

      QGroupBox* buildRuleBox();
      QGroupBox* buildScopeBox();

      // Rules as authored. TerrainRuleSet is rebuilt from this on demand rather than held, because
      // it recomputes its precedence order on every mutation and the list is edited keystroke by
      // keystroke.
      std::vector<Noggit::TerrainRule> _rules;

      Noggit::TerrainRuleSet buildRuleSet() const;

      void rebuildRuleList();

      // Index into _rules for the highlighted row, or -1.
      //
      // Read out of the item's data, never from currentRow(). There is no filter on this list
      // today, but the two have already been confused twice elsewhere in this tree the moment one
      // was added, and the fix is cheaper than the bug.
      int selectedRuleIndex() const;

      void showSelectedRule();

      // Writes the widget state back into the selected rule and reports whether that actually
      // changed it. Does NOT touch the preview.
      //
      // Separate from applyEditsToSelectedRule because the two callers that are not widget signals
      // -- Preview and Apply -- call this to flush a half-typed edit and would be destroying the
      // very preview they are about to use if the write-back invalidated unconditionally.
      bool commitSelectedRuleEdits();

      // commitSelectedRuleEdits plus the invalidation every editing signal wants.
      void applyEditsToSelectedRule();

      void onAddRule();
      void onDuplicateRule();
      void onRemoveRule();
      void moveSelectedRule(int offset);

      void onPreview();
      void onApply();

      // Drops the preview and disables Apply. Called from every edit path.
      void invalidatePreview();

      // Textures actually painted in the chosen scope, plus the Texturing tool's current pick.
      void refreshTextureList();

      bool allLoadedTiles() const;

      // The tiles the scope selection names RIGHT NOW, in ascending index order. Reads the camera
      // for the single-tile case, so two calls a second apart can disagree; that is precisely why
      // the answer is stored in PreviewResult rather than asked for again at apply time.
      std::vector<TileIndex> resolveScope() const;

      // The one walk over a resolved scope, shared by preview and apply.
      //
      // Takes the tile list rather than resolving it, so that both passes are guaranteed to be
      // looking at the same terrain. Deliberately not TerrainRuleCollector::collectTileUnits:
      // both callers need chunk boundaries -- preview to count a chunk's distinct winners against
      // the four-layer limit, apply to register one undo entry and commit one alphamap per chunk
      // -- and running two different walks is exactly how the numbers a user approved stop being
      // the numbers that get painted.
      void forEachChunkInScope( std::vector<TileIndex> const& tiles
                              , std::function<void(MapTile*, MapChunk*)> const& visit
                              ) const;

      QString describeRule(std::size_t index) const;

      MapView* _map_view;

      // Suppresses the write-back path.
      //
      // Two situations need it, and both are the same shape: showSelectedRule pushes a rule into
      // twelve widgets, each of which emits, and halfway through that the widget state is a
      // mixture of the new rule and the old one. The constructor has the harsher version -- the
      // widgets are created in order and the first setValue fires before the widgets a handler
      // would touch exist at all -- so it is held down for the whole of construction.
      bool _updating_ui = true;

      PreviewResult _preview;
      bool _preview_fresh = false;

      QListWidget* _rule_list;

      QGroupBox* _rule_box;
      QComboBox* _texture;
      // Unbounded is a real, common state -- "grass elsewhere" is a rule with four unbounded
      // endpoints -- and TerrainRange spells it with an infinity that no spin box can hold.
      // A tick box per endpoint says so explicitly; the alternative, treating 0 as "no minimum",
      // silently turns every slope rule into one bounded at 0.
      QCheckBox* _slope_min_bounded;
      QCheckBox* _slope_max_bounded;
      QCheckBox* _height_min_bounded;
      QCheckBox* _height_max_bounded;
      QDoubleSpinBox* _slope_min;
      QDoubleSpinBox* _slope_max;
      QDoubleSpinBox* _height_min;
      QDoubleSpinBox* _height_max;
      QSpinBox* _strength;
      QSpinBox* _priority;
      QCheckBox* _enabled;

      QComboBox* _scope;
      QPushButton* _apply_button;
      QPlainTextEdit* _report;
      QLabel* _status;
  };
}

#endif // NOGGIT_UI_AUTOTEXTUREDIALOG_HPP
