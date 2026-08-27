// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_LIGHTEDITOR_HPP
#define NOGGIT_LIGHTEDITOR_HPP

#include <QWidget>
#include <noggit/MapView.h>
#include <QtWidgets/qtreewidget.h>

#include <map>
#include <string>
#include <unordered_map>
#include <vector>

class LightViewPreview;
class LightViewEditor;
class Sky;
class SkyParam;

namespace Noggit
{
  // Forward-declared rather than pulling in <noggit/Sky.h>, which this header does not otherwise
  // need. Both uses below are parameters in a DECLARATION -- one by reference, one by value -- and
  // a by-value parameter only requires the complete type where the function is defined or called,
  // not where it is declared. Sky.h:211 has the definition; LightEditor.cpp includes it.
  struct LightSnapshot;
}

class QPushButton;
class QDoubleSpinBox;
class QDial;
class QSpinBox;
class QListWidget;
class QListWidgetItem;
class QComboBox;
class QTabWidget;
class QCheckBox;

namespace Noggit::Ui::Tools
{

	static const std::map <int, std::string> sky_color_names_map = {
		{0	, "Light Direct (diffuse)"},
		{1	, "Light Ambiant - Sky"},
		{2	, "Sky Top"},
		{3	, "Sky Midle"},
		{4	, "Sky Band 1"},
		{5	, "Sky Band 2"},
		{6	, "Sky Smog"},
		{7	, "Base Fog"},
		{8	, "Shadow Opacity(Unused)"}, //Unknown/unused in 3.3.5. in new format this row was remvoed and moved to index 17 (after river far)
		{9	, "Sun"},
		{10	, "Cloud Sun"},
		{11	, "Cloud Emissive"},
		{12	, "Cloud Layer 1 Ambiant"},
		{13	, "Cloud Layer 2 Ambiant"}, // Unknown / unused in 3.3.5 ? This value was ported to Cloud Layer 2 Ambient Color in the new format
		{14	, "Ocean Shallow"},
		{15	, "Ocean Deep"},
		{16	, "River Shallow"},
		{17	, "River Deep"}
	};

	static const std::map <int, std::string> sky_float_values_names_map = {
	{0	, "Fog Distance"},
	{1	, "Fog Multiplier"},
	{2	, "Celestial Glow Through"},
	{3	, "Cloud Density"},
	{4	, "Unkown/Unused1"},
	{5	, "Unkown/Unused2"}
	 };

	// last official light.dbc id naming from .lit client files
	int constexpr blizzlikeNameDefinitionsEnd = 418;
	// last blizzzard light.dbc id
	int constexpr blizzlikeSkiesEndWrath = 2538;

	// The Light.dbc id -> name table loaded from noggit-definitions/light_dbc_names.csv.
	//
	// A function with a local static, not a `static std::unordered_map` at namespace scope in a
	// header: that gave every translation unit including this file its OWN copy, so the cross-map
	// browser -- which lives in a second .cpp -- would have read an empty map and named every one
	// of the 378 known lights "Unnamed Light" while the panel next to it showed the real names.
	std::unordered_map<int, std::string>& lightNameDefinitions();

	// The name to show for a light id. Free, so the panel and the browser cannot disagree.
	std::string lightDisplayName(int light_id, bool global, bool light_zone = false);

	class LightBrowser;
	class LightEditor;

	class LightningInfoDialog : public QWidget
	{
		Q_OBJECT
	public:
		LightningInfoDialog(LightEditor* editor, QWidget* parent = nullptr);

	// private:
		// current lightning info preview widgets
		LightEditor* _editor;

		QDial* _time_dial;
		QSpinBox* TimeSelectorHour;
		QSpinBox* TimeSelectorMin;

		QLabel* _highest_weight_sky_label;
		QLabel* _current_lightning_colors_labels[18]{ 0 };
		QLabel* _current_lightning_floats_labels[6]{ 0 };

		QLabel* _river_shallow_alpha_label_label;
		QLabel* _river_deep_alpha_label;
		QLabel* _ocean_shallow_alpha_label;
		QLabel* _ocean_deep_alpha_label;
		QLabel* _glow_label;
		QLabel* _highlight_label;
	};

  class LightEditor : public QWidget
  {
    Q_OBJECT
  public:
    LightEditor(MapView* map_view, QWidget* parent = nullptr);
    ~LightEditor() override;

		void UpdateToolTime(); // update on time change
		void updateActiveLights(); // only need to update on position change
		void updateLightningInfo(); // needs to be updated on time change AND position change

		void UpdateWorldTime();
		void updateLightning();

		// A light was picked in the viewport. Opens it in the edit tab and moves the list to it.
		void onLightSelectedInViewport(int light_id);

		// Rewrites the position and radius spin boxes from the selected Sky, signals blocked.
		//
		// Called at the END of a gizmo drag and after a ctrl+wheel resize, never during a drag:
		// setValue re-enters the valueChanged handlers, and running that per frame both fights the
		// user for the caret and writes the value back into the Sky it just came from.
		void refreshSelectedLightFields();

		[[nodiscard]] int selectedLightId() const;

		// Captures `light_id` -- which may belong to ANY map, not only the loaded one -- as a
		// snapshot. The loaded map's copy wins when there is one, because that is where the user's
		// unsaved edits live; everything else comes straight out of Light.dbc.
		bool snapshotForLight(int light_id, Noggit::LightSnapshot& out);

		// Captures `light_id` into the process-wide clipboard. Used by the Copy button and by the
		// cross-map browser's Copy.
		bool copyLightFromDbc(int light_id);

		// Pastes the clipboard into the loaded map as a new, unsaved light.
		void pasteLightFromClipboard(bool deep_copy_params);

		// Pastes one snapshot without going through the clipboard, so Duplicate and the browser's
		// Paste do not silently replace what the user copied.
		void pasteLightSnapshot(Noggit::LightSnapshot snapshot, bool deep_copy_params);

    World* _world;
    MapView* _map_view;

		LightningInfoDialog* _lightning_info_dialog;

  private:

		int _selected_sky_id = 0;
		Sky* get_selected_sky() const;

		// Rebuilds the current-map list from Skies, and reapplies the filter text.
		//
		// One path, used by duplicate, paste, delete and the initial fill, so the four cannot drift
		// into producing differently formatted rows or forgetting the icon on a global light.
		void rebuildLightList();
		void selectLightListRow(int light_id);
		void applyLightListFilter();

		// The SkyParam the param combo box is pointing at, or nullptr.
		//
		// Every slider and checkbox on the Light Params group used to reach it as
		// get_selected_sky()->getParam(index).value() -- two unchecked dereferences in a row, on a
		// combo box index that can name a slot holding no param at all.
		[[nodiscard]] SkyParam* selectedParam() const;

		void reportSaveRefusal(std::string const& reason);

		std::vector<LightViewPreview*> LightsPreview;
		std::vector<LightViewEditor*> ActiveEditor;

		QWidget* _light_editing_widget;
		QTabWidget* lightning_tabs;

		// QDial* _time_dial;
		QListWidget* _active_lights_tree;

		QLineEdit* _light_tree_filter;
		QListWidget* _light_tree;

		LightBrowser* _light_browser = nullptr;

		// The light list is built from Light.dbc until Skies exists, and from Skies afterwards.
		//
		// It has to be, because this panel is constructed in MapView::createGUI -- at the end of the
		// MapView constructor -- while WorldRender::upload, which is what creates Skies, does not run
		// until the first initializeGL. The two sources agree on every saved light; they differ on
		// unsaved ones (which cannot exist yet at construction) and on the borrowed global light a
		// map without one gets, which is not a row of this map in the DBC. updateActiveLights flips
		// this the first time Skies is there, and LightTool calls that when the tool is selected.
		bool _light_list_from_skies = false;

		QPushButton* _copy_light_button;
		QPushButton* _paste_light_button;
		QPushButton* _delete_light_button;
		QPushButton* _make_global_light_button;
		QCheckBox* _deep_copy_params_chk;

		QPushButton* save_current_sky_button;
		QLabel* lightid_label;

		QCheckBox* global_light_chk;
		QCheckBox* zone_light_chk;
		QLineEdit* name_line_edit;
		QDoubleSpinBox* pos_x_spin;
		QDoubleSpinBox* pos_y_spin;
		QDoubleSpinBox* pos_z_spin;
		QDoubleSpinBox* inner_radius_spin;
		QDoubleSpinBox* outer_radius_spin;
		QCheckBox* link_radii_chk;
		QLabel* _save_state_label;

		QLabel* _nb_param_users;
    QComboBox* param_combobox;
    QSlider* glow_slider;
    QCheckBox* highlight_sky_checkbox;

    QLineEdit* skybox_model_lineedit;
		QCheckBox* skybox_flag_1;
		QCheckBox* skybox_flag_2;

    QSlider* shallow_water_alpha_slider;
    QSlider* deep_water_alpha_slider;
    QSlider* shallow_ocean_alpha_slider;
    QSlider* deep_ocean_alpha_slider;

    void load_light_param(int param_id);
		void loadSelectSky(Sky* new_sky);
  };


}

#endif //NOGGIT_LIGHTEDITOR_HPP




