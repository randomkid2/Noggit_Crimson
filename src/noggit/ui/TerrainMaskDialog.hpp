// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_UI_TERRAINMASKDIALOG_HPP
#define NOGGIT_UI_TERRAINMASKDIALOG_HPP

#include <noggit/terrain/TerrainMaskFilters.hpp>

#include <QtWidgets/QDialog>

#include <cstddef>
#include <string>

class MapView;

namespace Noggit
{
  // Declared in TerrainMaskStore.hpp, which this header deliberately does not include: the only use
  // here is a returned pointer, and pulling the store in would put TerrainMask's storage map in
  // front of every translation unit that merely wants to open this dialog.
  struct NamedTerrainMask;
}

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QStackedWidget;

namespace Noggit::Ui
{
  // Authoring named masks: a filter stack, a bake, and a switch that clips every brush to the
  // result.
  //
  // WHAT A MASK IS FOR. Every tool in this editor applies uniformly inside its radius. A mask is
  // the answer to "apply, but only here", where "here" is a region too complicated to hold a brush
  // steady over -- the valley floor, everything above the treeline, the drainage lines, everywhere
  // the road already is. Deriving that region once and naming it makes every existing tool sharper
  // without adding a tool.
  //
  // THE ONE THING THAT MAKES THIS SAFE, and the reason it can be built at all: A MASK NEVER TOUCHES
  // THE FILE FORMAT. It is an editor-side sidecar under the project directory, it decides only
  // where other operations apply, and it bakes down to nothing. No ADT, no MCAL, no MCNK flag ever
  // learns that a mask existed. That is why a mask can be as elaborate as it likes at zero risk to
  // a client that has to load the result.
  //
  // WHY CURVATURE IS THE FILTER THAT MATTERS. The Automatic Texturing dialog next door matches on
  // slope and height, and those two cannot distinguish a valley floor from the shoulder above it --
  // both can sit at the same height with the same slope. Curvature is the axis that separates them,
  // and it is what puts gravel in drainage lines and lichen on convex shoulders. Adding it here
  // gives the rule engine a capability it cannot currently express, by way of a mask rather than by
  // way of a new rule field.
  //
  // BAKING IS EXPLICIT, and that is a decision rather than an omission. A mask could rebake itself
  // whenever terrain changed under it; it would then be a thing that silently redefines itself
  // between two strokes, and the only thing worse than a stale mask is a moving one. The dialog
  // reports how many loaded tiles are baked and the user presses the button.
  //
  // MASK EDITS ARE NOT UNDOABLE, which is stated in the window rather than hidden. The
  // ActionManager records ADT chunk snapshots; a mask is not ADT data and does not belong in that
  // stack. The safety net is the property above -- a bad mask changes no file, and the worst it can
  // do is clip the next edit somewhere unintended, which is visible at once.
  class TerrainMaskDialog : public QDialog
  {
    Q_OBJECT

    public:
      explicit TerrainMaskDialog(MapView* map_view, QWidget* parent = nullptr);

    private:
      // --- Construction ---

      QGroupBox* buildMaskListGroup();
      QGroupBox* buildStackGroup();
      QGroupBox* buildLayerPropertiesGroup();
      QGroupBox* buildBakeGroup();

      // --- Model <-> widgets ---

      // Repopulates the mask list from the store and reselects `select` if it still exists.
      void refreshMaskList(std::string const& select);
      // Repopulates the layer list for the selected mask.
      void refreshLayerList(int select_row);
      // Pushes the selected layer's fields into the property widgets.
      void loadLayerIntoWidgets();
      // Pulls the property widgets back into the selected layer.
      void commitWidgetsToLayer();

      void refreshStatus();

      // The currently selected mask, or null. RE-FOUND EVERY TIME rather than cached, because
      // TerrainMaskStore keeps masks in a std::vector and create/remove reallocate it -- a held
      // pointer would dangle after any list edit. See TerrainMaskStore::create.
      Noggit::NamedTerrainMask* selectedMask();
      // Index of the selected layer, or -1.
      int selectedLayerRow() const;

      std::string projectPath() const;

      // --- Actions ---

      void onNewMask();
      void onDuplicateMask();
      void onRenameMask();
      void onDeleteMask();
      void onMaskSelectionChanged();
      void onClipToggled(bool enabled);

      void onAddLayer();
      void onRemoveLayer();
      void onMoveLayer(int delta);
      void onLayerSelectionChanged();
      void onLayerEdited();

      void onBakeLoadedTiles();
      void onClearBake();
      void onSave();
      void onReload();

      MapView* _map_view;

      // Guards the widget-to-model direction while the model-to-widget direction is running. Every
      // property widget is connected to onLayerEdited, and loadLayerIntoWidgets sets all of them --
      // without this, selecting a layer would immediately write the previous layer's values into
      // the newly selected one.
      bool _loading_widgets = false;

      QListWidget* _mask_list = nullptr;
      QPushButton* _new_button = nullptr;
      QPushButton* _duplicate_button = nullptr;
      QPushButton* _rename_button = nullptr;
      QPushButton* _delete_button = nullptr;

      QCheckBox* _clip_enabled = nullptr;

      QListWidget* _layer_list = nullptr;
      QComboBox* _add_kind = nullptr;
      QPushButton* _add_button = nullptr;
      QPushButton* _remove_button = nullptr;
      QPushButton* _up_button = nullptr;
      QPushButton* _down_button = nullptr;

      QComboBox* _combine = nullptr;
      QDoubleSpinBox* _opacity = nullptr;
      QCheckBox* _layer_enabled = nullptr;
      QCheckBox* _layer_invert = nullptr;

      QCheckBox* _low_bounded = nullptr;
      QDoubleSpinBox* _range_low = nullptr;
      QCheckBox* _high_bounded = nullptr;
      QDoubleSpinBox* _range_high = nullptr;
      QDoubleSpinBox* _range_feather = nullptr;
      QLabel* _range_units = nullptr;

      // One page per filter kind, so the kind-specific controls are never visible for a kind that
      // ignores them.
      QStackedWidget* _kind_pages = nullptr;
      QSpinBox* _curvature_step = nullptr;
      QLabel* _curvature_scale = nullptr;
      QDoubleSpinBox* _noise_wavelength = nullptr;
      QSpinBox* _noise_octaves = nullptr;
      QDoubleSpinBox* _noise_gain = nullptr;
      QSpinBox* _noise_seed = nullptr;
      QLineEdit* _texture = nullptr;
      QLineEdit* _area_ids = nullptr;
      QDoubleSpinBox* _constant = nullptr;

      QComboBox* _paint_combine = nullptr;

      QPushButton* _bake_button = nullptr;
      QPushButton* _clear_bake_button = nullptr;
      QPushButton* _save_button = nullptr;
      QPushButton* _reload_button = nullptr;

      QLabel* _status = nullptr;
      QLabel* _memory = nullptr;
      QPlainTextEdit* _problems = nullptr;
  };
}

#endif // NOGGIT_UI_TERRAINMASKDIALOG_HPP
