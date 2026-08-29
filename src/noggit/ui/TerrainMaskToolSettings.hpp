// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_UI_TERRAINMASKTOOLSETTINGS_HPP
#define NOGGIT_UI_TERRAINMASKTOOLSETTINGS_HPP

#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <QtWidgets/QWidget>

class MapView;

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QTimer;

namespace Noggit::Ui::Tools::UiCommon
{
  class ExtendedSlider;
}

namespace Noggit::Ui
{
  class TerrainMaskPreview;

  // THE PANEL FOR THE MASK BRUSH: which mask, how big, and what it currently looks like.
  //
  // Shaped after ShadowToolSettings and ErosionToolSettings -- the Tool translates input into
  // calls and holds no parameters, this widget holds the parameters and performs anything slow.
  // The tile bake in particular is here and not in the Tool for the reason ShadowToolSettings
  // states at length: Tool::onTick runs inside paintGL (MapView.cpp:5139 calls tick from there),
  // and a bake that walks 256 chunks has no business in a paint. ensureTileBaked is called only
  // from the Tool's mouse-press and mouse-move handlers, which the Qt event loop delivers with
  // no paint in progress.
  //
  // WHY IT POLLS. TerrainMaskStore is deliberately not a QObject and has no change signal (see
  // the note on the class), and this panel is not its only writer any more: the Terrain Masks
  // dialog can be open at the same time and can rename, delete, bake and clip. A 250 ms timer
  // that re-reads the store is four lines and cannot go stale; a signal would mean making the
  // store a QObject and reversing a decision that is documented and correct for its own layer.
  // TerrainMaskDialog polls the same two shared switches for the same reason.
  class TerrainMaskToolSettings : public QWidget
  {
    public:
      explicit TerrainMaskToolSettings(MapView* map_view);

      // --- The brush, read by the Tool ---

      [[nodiscard]]
      float brushRadius() const;
      [[nodiscard]]
      float hardness() const;
      [[nodiscard]]
      float strength() const;

      void changeRadius(float change);
      void changeHardness(float change);
      void changeStrength(float change);

      // Colour for the viewport brush ring: the active mask's identity hue, brightened by how
      // masked-in the ground under the cursor already is. It is the one piece of mask STATE that
      // reaches the 3D view without a rendering hook, and it is worth the two lines because it
      // answers "am I painting over something" without the eye leaving the terrain.
      //
      // The value it modulates by is up to 66 ms old -- it comes from the loupe's last refresh --
      // which at 15 Hz is under one frame of visible lag at 60 fps.
      [[nodiscard]]
      glm::vec4 cursorColor() const;

      // --- Called by the Tool ---

      // Re-reads the mask list from the store. Called when the tool is selected, because the
      // dialog may have added or removed masks while another tool was in use.
      void onToolSelected();

      // Bakes the tile under `position` for the active mask if it is not baked yet and the user
      // has left auto-bake on. MUST ONLY BE CALLED FROM A Qt EVENT HANDLER -- see the class note.
      void ensureTileBaked(glm::vec3 const& position);

      // A stroke tick happened. SAFE FROM INSIDE paintGL: it sets two flags and touches no
      // layout. The labels it invalidates are rewritten by the poll timer.
      void noteStrokePainted();

      void undoStroke();
      void redoStroke();

      QSize sizeHint() const override;

    protected:
      void showEvent(QShowEvent* event) override;
      void hideEvent(QHideEvent* event) override;

    private:
      // Everything that can change without this panel having done it: the clip switch, the paint
      // fold, the value under the cursor, the history depth. Driven by _poll.
      void refreshVolatileState();

      // The bake itself, with no "is it already baked" and no auto-bake test. ensureTileBaked
      // owns those two decisions; the panel's own button deliberately makes neither.
      void performTileBake(glm::vec3 const& position);

      // Pulls <project>/noggit_masks in the first time this panel is used, and only when nothing
      // is in the store yet.
      //
      // BOTH HALVES MATTER. Without the load, a user who reaches masks through this tool rather
      // than through the dialog never sees the masks their project already has. Without the
      // "nothing in the store yet" test, showing the panel would replace whatever is in memory --
      // TerrainMaskStore::load REPLACES the set -- and take unsaved paint with it. The dialog's
      // constructor carries the same pair, so whichever of the two the user opens first performs
      // the load and the other one leaves it alone.
      void loadProjectMasksOnce();

      void refreshMaskCombo();
      void pushBrushToPreview();
      void onMaskChosen(int index);
      void onNewMask();
      void onSaveMasks();

      void setStatus(QString const& text);

      MapView* _map_view;

      QTimer* _poll;

      QComboBox* _mask_combo;
      QPushButton* _new_button;
      QPushButton* _dialog_button;
      QPushButton* _save_button;
      QLabel* _identity_label;
      QCheckBox* _clip_enabled;
      QComboBox* _paint_combine;
      QLabel* _fold_note;

      Noggit::Ui::Tools::UiCommon::ExtendedSlider* _radius_slider;
      Noggit::Ui::Tools::UiCommon::ExtendedSlider* _hardness_slider;
      Noggit::Ui::Tools::UiCommon::ExtendedSlider* _strength_slider;

      TerrainMaskPreview* _preview;
      QComboBox* _window_combo;
      QCheckBox* _paint_only;
      QLabel* _readout_label;

      QPushButton* _undo_button;
      QPushButton* _redo_button;
      QLabel* _history_label;

      QCheckBox* _auto_bake;
      QPushButton* _bake_button;
      QLabel* _status_label;

      // Guards the widget-to-store direction while the store-to-widget direction is running.
      // Every control below is connected to a handler that writes the store, and
      // refreshVolatileState sets several of them -- without this, one poll tick would write the
      // values it had just read back over the top of a change the dialog made in between.
      bool _loading_widgets = false;

      bool _tried_project_load = false;
  };
}

#endif // NOGGIT_UI_TERRAINMASKTOOLSETTINGS_HPP
