// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_UI_TEXTURELAYERMANAGER_HPP
#define NOGGIT_UI_TEXTURELAYERMANAGER_HPP

#include <noggit/texturing/TextureLayerOps.hpp>

#include <QtWidgets/QDialog>

#include <functional>
#include <string>

class MapView;

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSpinBox;

namespace Noggit::Ui
{
  // THE LAYER BUDGET WINDOW: everything that spends or reclaims a chunk's four MCLY slots, in one
  // place, because they are one problem seen from two ends.
  //
  //   LAYER REPLACEMENT and PREPARE AREA spend the budget deliberately -- put this texture in that
  //   slot, make sure this palette fits before I start painting. They are the remedy half of Smart
  //   Paint; the mode selector on the Paint tab is the other half, and it applies during a stroke.
  //
  //   LAYER HYGIENE reclaims it. Rule-driven texturing and Live Auto Texture write a decision per
  //   8x8 chunk unit, so a rule that wins on two units out of sixty-four still costs a whole slot,
  //   and a rule set that has been edited a few times leaves layers behind holding one or two
  //   alpha values nobody can see. Four layers per chunk is a hard budget and those layers are
  //   spending it, plus a texture bind and the fill rate to blend them. This fork built the
  //   machine that manufactures that garbage and shipped no broom.
  //
  // The two hygiene operations are the two from Noggit Green's clearing tool (wowdev/noggit3) that
  // do something this tree cannot already do: purging duplicate layers and purging layers whose
  // alpha never rises above a threshold. Green's other nine checkboxes are thin wrappers over
  // clearTextures, eraseTextures and removeTexture, which exist here already, and are deliberately
  // not reimplemented. No code was taken; the two operations are the idea.
  //
  // EVERY BUTTON HERE IS ONE UNDO STEP for every chunk it touched. See the note above
  // runInOneAction in TextureLayerOps.cpp.
  class TextureLayerManager : public QDialog
  {
    Q_OBJECT

    public:
      explicit TextureLayerManager(MapView* map_view, QWidget* parent = nullptr);

    private:
      // The scope the three Apply buttons share, resolved at the moment the button is pressed
      // against the terrain cursor. This window is modeless and the cursor keeps moving, so the
      // result line names the tile the run actually landed on rather than leaving the user to
      // guess.
      Noggit::LayerOpScopeRequest currentScope() const;

      // Wraps one operation: makes the GL context current, runs it, and prints what it did.
      void runOperation(std::function<Noggit::LayerOpResult()> const& operation);

      void reportResult(Noggit::LayerOpResult const& result);

      // The texture currently selected in the texturing tool, unix-normalised, or empty.
      std::string selectedTexturePath() const;

      QGroupBox* buildScopeGroup();
      QGroupBox* buildReplacementGroup();
      QGroupBox* buildPrepareGroup();
      QGroupBox* buildHygieneGroup();

      MapView* _map_view;

      QComboBox* _scope_combo = nullptr;
      QDoubleSpinBox* _radius_spin = nullptr;

      QSpinBox* _slot_spin = nullptr;
      QLineEdit* _replacement_texture = nullptr;
      QComboBox* _alpha_handling = nullptr;

      QCheckBox* _clear_overlays = nullptr;
      QCheckBox* _evict_to_fit = nullptr;
      QListWidget* _palette = nullptr;

      QCheckBox* _purge_duplicates = nullptr;
      QCheckBox* _purge_threshold = nullptr;
      QSpinBox* _threshold_spin = nullptr;

      QLabel* _status = nullptr;
  };
}

#endif // NOGGIT_UI_TEXTURELAYERMANAGER_HPP
