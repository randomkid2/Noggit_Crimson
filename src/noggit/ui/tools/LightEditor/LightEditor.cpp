// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include "LightEditor.hpp"
#include "LightBrowser.hpp"
#include <noggit/application/Configuration/NoggitApplicationConfiguration.hpp>
#include <noggit/application/NoggitApplication.hpp>
#include <noggit/DBC.h>
#include <noggit/MapView.h>
#include <noggit/Model.h>
#include <noggit/ui/FontAwesome.hpp>
#include <noggit/ui/tools/ToolPanel/ToolWidgetStyle.hpp>
#include <noggit/ui/widgets/LightViewWidget.h>
#include <noggit/World.h>

#include <format>
#include <map>
#include <string>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDial>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QStyle>
#include <QStringList>
#include <QSlider>
#include <QTabWidget>
#include <QTextStream>
#include <QTreeWidget>
#include <QVBoxLayout>


using namespace Noggit::Ui::Tools;

std::unordered_map<int, std::string>& Noggit::Ui::Tools::lightNameDefinitions()
{
  static std::unordered_map<int, std::string> definitions;
  return definitions;
}

std::string Noggit::Ui::Tools::lightDisplayName(int light_id, bool global, bool light_zone)
{
	auto const& definitions = lightNameDefinitions();
	auto const named = definitions.find(light_id);

	if (named != definitions.end() && !named->second.empty())
	{
		return named->second;
	}

	if (global)
		return "Global Light";

	if (light_zone)
		return "Unnamed Zone Light";

	return "Unnamed Light";
}

LightEditor::LightEditor(MapView* map_view, QWidget* parent)
: QWidget(parent)
, _map_view(map_view)
, _world(map_view->getWorld())
{
	// Registered here rather than handed over by LightTool, because ToolPanel::registerTool
	// reparents this widget into the panel's scroll area straight after construction -- a
	// findChild<LightEditor*> from MapView would then miss it. Cleared again by ~LightEditor.
	map_view->setLightEditor(this);

	// The dock's shared shell -- zero margins, S3 between sections, 250px floor. This layout set
	// no margins, so it took QStyle::PM_LayoutLeftMargin (13px on windowsvista here) on top of
	// ToolPanel's own 12px. See ToolWidgetStyle.hpp.
	auto layout = ToolPanelStyle::toolColumn(this);

	lightning_tabs = new QTabWidget(this);
	layout->addWidget(lightning_tabs);

	// light select tab
	auto light_selection_widget = new QWidget(lightning_tabs);
	auto light_selection_layout = new QVBoxLayout(light_selection_widget);
	light_selection_layout->setContentsMargins(0, 0, 0, 0);
	lightning_tabs->addTab(light_selection_widget, "Light Selection");

	// light edit tab
	_light_editing_widget = new QWidget(lightning_tabs);
	_light_editing_widget->setEnabled(false);
	auto light_editing_layout = new QVBoxLayout(_light_editing_widget);
	light_editing_layout->setContentsMargins(0, 0, 0, 0);
	lightning_tabs->addTab(_light_editing_widget, "Edit Light");

	QPushButton* lightningInfoDialogButton = new QPushButton("View Lightning Info", this);
	lightningInfoDialogButton->setIcon(Noggit::Ui::FontAwesomeIcon(Noggit::Ui::FontAwesome::sun));
	light_selection_layout->addWidget(lightningInfoDialogButton);

	_lightning_info_dialog = new LightningInfoDialog(this, this);

	// TODO can save this as a nice reusable text+tooltip indicator label widget
	QLabel* active_lights_label = new QLabel("Active Lights :", this);
	// Create a separate label for the icon
	QLabel* active_lights_icon_label = new QLabel(this);
	QIcon infoIcon = QApplication::style()->standardIcon(QStyle::SP_MessageBoxInformation);
	QPixmap infoPixmap = infoIcon.pixmap(16, 16);
	active_lights_icon_label->setPixmap(infoPixmap);
	// Set the tooltip text
	active_lights_icon_label->setToolTip("Current active Lights at camera position."
												"\nThe global light is used when no other light is active, or blended with them when not within inner radius."
												"\nDouble Click a row to edit it.");
	// Add both labels to a horizontal layout
	QHBoxLayout* active_ligths_label_layout = new QHBoxLayout(light_selection_widget);
	// active_ligths_label_layout->setContentsMargins(0, 0, 0, 0);
	active_ligths_label_layout->addWidget(active_lights_icon_label);
	active_ligths_label_layout->addWidget(active_lights_label);
	active_ligths_label_layout->addStretch();

	light_selection_layout->addLayout(active_ligths_label_layout);

	// current active lights tree
	_active_lights_tree = new QListWidget(this);
	light_selection_layout->addWidget(_active_lights_tree);
	// _light_tree->setWindowTitle("Current map lights");
	_active_lights_tree->setViewMode(QListView::ListMode);
	_active_lights_tree->setSelectionMode(QAbstractItemView::SingleSelection);
	_active_lights_tree->setSelectionBehavior(QAbstractItemView::SelectItems);
	_active_lights_tree->setFixedHeight(60);
	_active_lights_tree->setUniformItemSizes(true);
	// _active_lights_tree->setContextMenuPolicy(Qt::CustomContextMenu);

	// QPushButton* GetCurrentSkyButton = new QPushButton("Edit current position's light", this);
	// GetCurrentSkyButton->setToolTip("Selection the highest weight light at camera's position");
	// GetCurrentSkyButton->setIcon(Noggit::Ui::FontAwesomeIcon(Noggit::Ui::FontAwesome::cog));
	// light_selection_layout->addWidget(GetCurrentSkyButton);

	// auto lightningBox = new ExpanderWidget(this);
	// auto lightningBox_content = new QWidget(this);
	// auto lightningBox_content_layout = new QFormLayout(lightningBox_content);
	// lightningBox_content_layout->setAlignment(Qt::AlignTop);
	// lightningBox->setExpanderTitle("Current Lightning");

// 	_highest_weight_sky_label = new QLabel("None/Not initialized", this);
// 	lightningBox_content_layout->addRow("Highest Weight Light", _highest_weight_sky_label);
// 
// 	// current colors preview
// 	for (int i = 0; i < NUM_SkyColorNames; ++i)
// 	{
// 		std::string color_name = sky_color_names_map.at(i);
// 
// 		_current_lightning_colors_labels[i] = new QLabel(this);
// 		QLabel* colorIconLabel = _current_lightning_colors_labels[i];
// 		QPixmap colorIcon(16, 16);  // Create a 16x16 px pixmap
// 		colorIcon.fill(Qt::transparent);  // Transparent background
// 
// 		// Use QPainter to draw a red square icon
// 		QPainter painter(&colorIcon);
// 		painter.setBrush(QBrush(Qt::black));  // Set the brush to red
// 		painter.setPen(Qt::NoPen);  // No border
// 		painter.drawRect(0, 0, 16, 16);  // Draw the square
// 		colorIconLabel->setPixmap(colorIcon);  // Set the pixmap
// 
// 		lightningBox_content_layout->addRow(color_name.c_str(), colorIconLabel);
// 	}
// 	// current float params preview
// 	for (int i = 0; i < NUM_SkyFloatParamsNames; ++i)
// 	{
// 		_current_lightning_floats_labels[i] = new QLabel("0", this);
// 
// 		lightningBox_content_layout->addRow(sky_float_values_names_map.at(i).c_str(), _current_lightning_floats_labels[i]);
// 	}
// 	// light params preview
// 	{
// 		_river_shallow_alpha_label_label = new QLabel("0", this);
// 		lightningBox_content_layout->addRow("Shallow Water Alpha", _river_shallow_alpha_label_label);
// 		_river_deep_alpha_label = new QLabel("0", this);
// 		lightningBox_content_layout->addRow("Deep Water Alpha", _river_deep_alpha_label);
// 		_ocean_shallow_alpha_label = new QLabel("0", this);
// 		lightningBox_content_layout->addRow("Shallow Ocean Alpha", _ocean_shallow_alpha_label);
// 		_ocean_deep_alpha_label = new QLabel("0", this);
// 		lightningBox_content_layout->addRow("Deep Water Alpha", _ocean_deep_alpha_label);
// 		_glow_label = new QLabel("0", this);
// 		lightningBox_content_layout->addRow("Glow", _glow_label);
// 		_highlight_label = new QLabel("0", this);
// 		lightningBox_content_layout->addRow("Highlight Sky", _highlight_label);
// 	}

	// lightningBox->addPage(lightningBox_content);
	// light_selection_layout->addWidget(lightningBox);

	light_selection_layout->addWidget(new QLabel("Current Map Lights :", this));


	QHBoxLayout* filter_tree_ledit_layout = new QHBoxLayout(light_selection_widget);
	filter_tree_ledit_layout->addWidget(new QLabel("Filter :", this));
	_light_tree_filter = new QLineEdit(this);
	filter_tree_ledit_layout->addWidget(_light_tree_filter);
	// filter_tree_ledit_layout->addStretch();
	light_selection_layout->addLayout(filter_tree_ledit_layout);

	_light_tree = new QListWidget(this);
	light_selection_layout->addWidget(_light_tree);
	// _light_tree->setWindowTitle("Current map lights");
	_light_tree->setViewMode(QListView::ListMode);
	_light_tree->setSelectionMode(QAbstractItemView::SingleSelection);
	_light_tree->setSelectionBehavior(QAbstractItemView::SelectItems);
	_light_tree->setFixedHeight(580);
	_light_tree->setUniformItemSizes(true);


	// load name definitions from csv file
	std::string definitions_path = Noggit::Application::NoggitApplication::instance()->getConfiguration()->ApplicationNoggitDefinitionsPath
															 + "\\light_dbc_names.csv";
	QString qPath = QString::fromStdString(definitions_path);
	QFile file(qPath);

	bool found_definitions = false;
	// load light names definition from csv file.
	if (file.open(QIODevice::ReadOnly | QIODevice::Text)) 
	{
		found_definitions = true;

		QTextStream in(&file);

		// Skip the header line
		std::string headerLine = in.readLine().toStdString();
		assert(headerLine == "ID,Name"); // "ID,Name,mapid"

		while (!in.atEnd()) 
		{
			QString line = in.readLine();
			QStringList fields = line.split(',');

			// Ensure there are at least two fields (ID and Name)
			assert(fields.size() == 2);
			if (fields.size() < 2) {
				continue;
			}
			bool ok;
			/*
			int map_id = fields[2].toInt(&ok);
			// only load this map ?
			if (map_id != _world->getMapID())
				continue;
			*/

			int id = fields[0].toInt(&ok);
			assert(ok);
			std::string const map_name = fields[1].toStdString();

			if (map_name.empty())
				continue;

			lightNameDefinitions()[id] = map_name;
		}
		file.close();
	}

	// The list is filled from Skies and not from Light.dbc directly.
	//
	// The old fill iterated gLightDB and filtered on the map id, which meant a light created by
	// Duplicate -- which now writes nothing until Save -- would have been absent from its own list
	// until the DBC was written. Skies holds exactly the same set plus the unsaved ones, and it is
	// the set the viewport and the gizmo already work from, so there is one source of truth.
	rebuildLightList();

	QPushButton* GetSelectedSkyButton = new QPushButton("Edit selected light", this);
	GetSelectedSkyButton->setIcon(Noggit::Ui::FontAwesomeIcon(Noggit::Ui::FontAwesome::cog));
	light_selection_layout->addWidget(GetSelectedSkyButton);

	QPushButton* addNewSkyButton = new QPushButton("Duplicate selected light", this);
	addNewSkyButton->setIcon(Noggit::Ui::FontAwesomeIcon(Noggit::Ui::FontAwesome::plus));
	addNewSkyButton->setToolTip("Creates a copy of the selected light at the camera, as an UNSAVED "
														 "light. Nothing is written to any DBC until you press "
														 "\"Save Light\" on the Edit Light tab.");
	light_selection_layout->addWidget(addNewSkyButton);

	QHBoxLayout* clipboard_layout = new QHBoxLayout();

	_copy_light_button = new QPushButton("Copy", this);
	_copy_light_button->setIcon(Noggit::Ui::FontAwesomeIcon(Noggit::Ui::FontAwesome::copy));
	_copy_light_button->setToolTip("Copies the selected light to the light clipboard. The clipboard "
																"survives loading another map, which is how a light is moved "
																"between continents.");
	clipboard_layout->addWidget(_copy_light_button);

	_paste_light_button = new QPushButton("Paste", this);
	_paste_light_button->setIcon(Noggit::Ui::FontAwesomeIcon(Noggit::Ui::FontAwesome::paste));
	_paste_light_button->setToolTip("Pastes the light clipboard into THIS map at the camera, as an "
																 "unsaved light.");
	_paste_light_button->setEnabled(Noggit::lightClipboard().valid);
	clipboard_layout->addWidget(_paste_light_button);

	light_selection_layout->addLayout(clipboard_layout);

	_deep_copy_params_chk = new QCheckBox("Paste with independent colours", this);
	_deep_copy_params_chk->setChecked(true);
	// Default ON, because the alternative is the trap. A LightParams id is shared between every
	// light that references it -- the Edit Light tab already reports "this param is used N times"
	// -- so pasting Stormwind's light and then adjusting its sky colour would adjust Stormwind's
	// too, on the original map, with nothing on screen saying so. Unticking it costs 1 + 18 + 6
	// fewer new DBC rows per weather slot and is the right choice only when the copy is meant to
	// track the original.
	_deep_copy_params_chk->setToolTip("On: the pasted light gets its own LightParams, LightIntBand "
																	 "and LightFloatBand rows, so editing its colours affects "
																	 "nothing else.\nOff: it shares the source's parameter rows, so "
																	 "editing its colours also edits every other light using them "
																	 "-- including on the map it came from.");
	light_selection_layout->addWidget(_deep_copy_params_chk);

	QPushButton* browseLightsButton = new QPushButton("Browse lights from all maps...", this);
	browseLightsButton->setIcon(Noggit::Ui::FontAwesomeIcon(Noggit::Ui::FontAwesome::search));
	light_selection_layout->addWidget(browseLightsButton);

	_delete_light_button = new QPushButton("Delete light", this);
	_delete_light_button->setIcon(Noggit::Ui::FontAwesomeIcon(Noggit::Ui::FontAwesome::times));
	light_selection_layout->addWidget(_delete_light_button);

	QPushButton* portToSkyButton = new QPushButton("Fly to light", this);
	portToSkyButton->setIcon(Noggit::Ui::FontAwesomeIcon(Noggit::Ui::FontAwesome::running));
	light_selection_layout->addWidget(portToSkyButton);

	// Only useful on a map with no global light of its own, and disabled until updateActiveLights
	// can ask Skies -- which does not exist yet at construction time.
	_make_global_light_button = new QPushButton("Give this map its own global light", this);
	_make_global_light_button->setIcon(Noggit::Ui::FontAwesomeIcon(Noggit::Ui::FontAwesome::sun));
	_make_global_light_button->setEnabled(false);
	_make_global_light_button->setToolTip
		("This map has no global light of its own, so Noggit is showing it Azeroth's (Light.dbc row "
		 "1) as a stand-in.\nSaving that stand-in would rewrite Azeroth's lighting, so the editor "
		 "refuses.\nThis button copies it into a new light belonging to this map instead.");
	light_selection_layout->addWidget(_make_global_light_button);

	light_selection_layout->addStretch();

	// global settings ********************************************************************************************** //
	// TODO : name lights on laoding instead
	light_editing_layout->addWidget(new QLabel("Selected Light :", this), 0);
	lightid_label = new QLabel("No light selected", this);
	light_editing_layout->addWidget(lightid_label);

	save_current_sky_button = new QPushButton("Save Light (write DBCs)", this);
	save_current_sky_button->setEnabled(false);
	save_current_sky_button->setToolTip("Writes LightSkybox.dbc, LightIntBand.dbc, "
																		 "LightFloatBand.dbc, LightParams.dbc and Light.dbc, in that "
																		 "order, into the project's DBFilesClient folder.\nThe whole set "
																		 "is validated first; if anything is wrong nothing is written.");
	light_editing_layout->addWidget(save_current_sky_button);

	_save_state_label = new QLabel(this);
	_save_state_label->setWordWrap(true);
	_save_state_label->hide();
	light_editing_layout->addWidget(_save_state_label);

	QGroupBox* global_values_group = new QGroupBox("Global settings", this);
	// alpha_values_group->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
	// auto global_values_layout = new QGridLayout(global_values_group);
	auto global_values_layout = new QFormLayout(global_values_group);

	QHBoxLayout* checkboxes_layout = new QHBoxLayout();

	global_light_chk = new QCheckBox("Global Light", this);
	QString global_light_tooltip_str = "Hint : The map's global light will be used when the player isn't within any other light radius."
																				 "\nThere can only be one g lobal Light per map"
																				 "\nGlobal Lights are defined by having X:0, Y:0, Z:0 coordinates";
	global_light_chk->setToolTip(global_light_tooltip_str);
	global_light_chk->setDisabled(true);
	// global_values_layout->addRow(global_light_chk);

	zone_light_chk = new QCheckBox("Zone Light", this);
	zone_light_chk->setToolTip("Hint : This light is used by a Zone Light (Polygon).");
	zone_light_chk->setDisabled(true);
	// global_values_layout->addRow(global_light_chk);
	checkboxes_layout->addWidget(global_light_chk);
	checkboxes_layout->addWidget(zone_light_chk);

	global_values_layout->addRow(checkboxes_layout);


	name_line_edit = new QLineEdit(this);
	// name_line_edit->setDisabled(true);
	// global_values_layout->addRow("Name:", name_line_edit);

	// Create a small button
	QPushButton* save_name_button = new QPushButton(this);  // You can set any text or icon for the button
	save_name_button->setIcon(Noggit::Ui::FontAwesomeIcon(Noggit::Ui::FontAwesome::save));
	// small_button->setFixedSize(20, 20);  // Set a small size for the button
	save_name_button->setToolTip("Save Name to \"noggit-definitions\\light_dbc_names.csv\""
															 "\nBlizzard names are datamined up to AQ40."
															"NOT YET IMPLEMENTED");
	save_name_button->setEnabled(false);

	// Create an HBoxLayout to hold the QLineEdit and button together
	QHBoxLayout* name_layout = new QHBoxLayout();
	name_layout->addWidget(new QLabel("Name:"));
	name_layout->addWidget(name_line_edit);
	name_layout->addWidget(save_name_button);

	global_values_layout->addRow(name_layout);

	pos_x_spin = new QDoubleSpinBox(this);
	pos_x_spin->setRange(-17066.66656 * 2, 17066.66656 * 2); // size = �17066.66656
	pos_x_spin->setValue(0);
	pos_x_spin->setSingleStep(50);
	pos_x_spin->setEnabled(false);
	global_values_layout->addRow("Position X:", pos_x_spin);

	pos_y_spin = new QDoubleSpinBox(this);
	pos_y_spin->setRange(-17066.66656 * 2, 17066.66656 * 2); // size = �17066.66656
	pos_y_spin->setValue(0);
	pos_y_spin->setSingleStep(50);
	pos_y_spin->setEnabled(false);
	global_values_layout->addRow("Position Y:", pos_y_spin);

	pos_z_spin = new QDoubleSpinBox(this);
	pos_z_spin->setRange(-17066.66656 * 2, 17066.66656*2); // ???? highest seen in 3.3.5 is 33,360
	pos_z_spin->setValue(0);
	pos_z_spin->setSingleStep(50);
	pos_z_spin->setEnabled(false);
	global_values_layout->addRow("Position Z:", pos_z_spin);
	
	link_radii_chk = new QCheckBox("Keep the inner/outer ratio when resizing", this);
	link_radii_chk->setChecked(true);
	// On by default because it is what "make this light bigger" means. The band between r1 and r2
	// is the falloff -- the client blends the light with the global one as
	// (r2 - distance) / (r2 - r1) -- so changing r2 alone does not scale a light, it reshapes its
	// edge. With this ticked, editing either radius scales the other by the same factor and the
	// falloff keeps its proportions.
	global_values_layout->addRow(link_radii_chk);

	inner_radius_spin = new QDoubleSpinBox(this);
	inner_radius_spin->setRange(0, 100000); // max seen in dbc is 3871 (139363 �E36 )
	inner_radius_spin->setValue(0);
	inner_radius_spin->setSingleStep(50);
	inner_radius_spin->setEnabled(false);
	global_values_layout->addRow("Inner Radius:", inner_radius_spin);

	outer_radius_spin = new QDoubleSpinBox(this);
	outer_radius_spin->setRange(0, 100000); // max seen in dbc is 3871 (139363 �E36 )
	outer_radius_spin->setValue(0);
	outer_radius_spin->setSingleStep(50);
	outer_radius_spin->setEnabled(false);
	global_values_layout->addRow("Outer Radius:", outer_radius_spin);

	light_editing_layout->addWidget(global_values_group);

	// BELOW IS PARAM SPECIFIC SETTINGS
	auto warning_label = new QLabel("Warning : Can't currently change param id,\n changes will affect all users of this param");
	// "orange" is a Qt colour name, unrelated to any theme's warning colour. QLabel[state="warn"]
	// is the mechanism the sheet already ships for exactly this. Set before the first polish, so
	// no unpolish/polish pair is needed.
	warning_label->setProperty("state", "warn");
	light_editing_layout->addWidget(warning_label);

	light_editing_layout->addWidget(new QLabel("Param Type :", this));
	param_combobox = new QComboBox(this);
	param_combobox->setEnabled(false);
	light_editing_layout->addWidget(param_combobox);
	// NUM_SkyParamsNames
	param_combobox->addItem("Clear Weather"); // Used in clear weather.
	param_combobox->addItem("Clear Weather Underwater"); // Used in clear weather while being underwater.
	param_combobox->addItem("Storm Weather"); // Used in rainy/snowy/sandstormy weather.
	param_combobox->addItem("Storm Weather Underwater"); // Used in rainy/snowy/sandstormy weather while being underwater.
	param_combobox->addItem("Death Effect"); // ParamsDeath. Only 4 and in newer ones 3 are used as value here (with some exceptions). Changing this seems to have no effect in 3.3.5a (is death light setting hardcoded?)
	param_combobox->addItem("unknown param 1");
	param_combobox->addItem("unknown param 2");
	param_combobox->addItem("unknown param 3");

	QGroupBox* light_param_group = new QGroupBox("Light Params", this);
	light_editing_layout->addWidget(light_param_group);
	auto light_param_layout = new QVBoxLayout(light_param_group);

	_nb_param_users = new QLabel(this);
	light_param_layout->addWidget(_nb_param_users);
	// QGroupBox* alpha_values_group = new QGroupBox("Alpha Values", this);
	// alpha_values_group->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
	auto param_grid_layout = new QGridLayout(light_param_group);
	light_param_layout->addLayout(param_grid_layout);

	param_grid_layout->addWidget(new QLabel("Glow:", this),0,0);
	glow_slider = new QSlider(Qt::Horizontal, this);
	glow_slider->setRange(0, 100); // between 0 and 1, increases by 0.05. Multiplying everything by 100 cuz Qslider doesn't seem to support floats
	glow_slider->setTickInterval(5);
	glow_slider->setSingleStep(5);
	glow_slider->setValue(50);
	glow_slider->setEnabled(false);
	param_grid_layout->addWidget(glow_slider,0,1);

	param_grid_layout->addWidget(new QLabel("Highlight Sky:", this), 1, 0);
	highlight_sky_checkbox = new QCheckBox(this);
	highlight_sky_checkbox->setCheckState(Qt::Unchecked);
	highlight_sky_checkbox->setEnabled(false);
	param_grid_layout->addWidget(highlight_sky_checkbox, 1, 1);

	param_grid_layout->addWidget(new QLabel("Skybox model:", this), 2, 0);
	skybox_model_lineedit = new QLineEdit(this);
	skybox_model_lineedit->setEnabled(false);
	param_grid_layout->addWidget(skybox_model_lineedit, 2, 1);

	skybox_flag_1 = new QCheckBox("Full day Skybox", this);
	skybox_flag_1->setCheckState(Qt::Unchecked);
	skybox_flag_1->setEnabled(false);
	skybox_flag_1->setToolTip("animation syncs with time of day (uses animation 0, time of day is just in percentage).");
	param_grid_layout->addWidget(skybox_flag_1, 3, 0, 1, 2);
	skybox_flag_2 = new QCheckBox("Combine Procedural And Skybox", this);
	skybox_flag_2->setCheckState(Qt::Unchecked);
	skybox_flag_2->setEnabled(false);
	skybox_flag_2->setToolTip("render stars, sun and moons and clouds as well.");
	param_grid_layout->addWidget(skybox_flag_2, 4, 0, 1, 2);
	

	// Alpha values ********************************************************************************************** //

	QGroupBox* alpha_values_group = new QGroupBox("Alpha Values", light_param_group);	
	// light_editing_layout->addWidget(alpha_values_group);
	light_param_layout->addWidget(alpha_values_group);

	// alpha_values_group->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
	auto alpha_values_layout = new QGridLayout(alpha_values_group);

	alpha_values_layout->addWidget(new QLabel("Shallow Water", alpha_values_group), 0, 0);
	shallow_water_alpha_slider = new QSlider(Qt::Horizontal, alpha_values_group);
	shallow_water_alpha_slider->setRange(0, 100); // between 0 and 1, increases by 0.05. Multiplying everything by 100 cuz Qslider doesn't seem to support floats
	shallow_water_alpha_slider->setTickInterval(5);
	shallow_water_alpha_slider->setSingleStep(5);
	shallow_water_alpha_slider->setValue(100);
	shallow_water_alpha_slider->setEnabled(false);
	alpha_values_layout->addWidget(shallow_water_alpha_slider, 0, 1);

	alpha_values_layout->addWidget(new QLabel("Deep Water", alpha_values_group), 1, 0);
	deep_water_alpha_slider = new QSlider(Qt::Horizontal, alpha_values_group);
	deep_water_alpha_slider->setRange(0, 100); // between 0 and 1, increases by 0.05. Multiplying everything by 100 cuz Qslider doesn't seem to support floats
	deep_water_alpha_slider->setTickInterval(5);
	deep_water_alpha_slider->setSingleStep(5);
	deep_water_alpha_slider->setValue(100);
	deep_water_alpha_slider->setEnabled(false);
	alpha_values_layout->addWidget(deep_water_alpha_slider, 1, 1);

	alpha_values_layout->addWidget(new QLabel("Shallow Ocean", alpha_values_group), 2, 0);
	shallow_ocean_alpha_slider = new QSlider(Qt::Horizontal, alpha_values_group);
	shallow_ocean_alpha_slider->setRange(0, 100); // between 0 and 1, increases by 0.05. Multiplying everything by 100 cuz Qslider doesn't seem to support floats
	shallow_ocean_alpha_slider->setTickInterval(5);
	shallow_ocean_alpha_slider->setSingleStep(5);
	shallow_ocean_alpha_slider->setValue(100);
	shallow_ocean_alpha_slider->setEnabled(false);
	alpha_values_layout->addWidget(shallow_ocean_alpha_slider, 2, 1);

	alpha_values_layout->addWidget(new QLabel("Deep Ocean", alpha_values_group), 3, 0);
	deep_ocean_alpha_slider = new QSlider(Qt::Horizontal, alpha_values_group);
	deep_ocean_alpha_slider->setRange(0, 100); // between 0 and 1, increases by 0.05. Multiplying everything by 100 cuz Qslider doesn't seem to support floats
	deep_ocean_alpha_slider->setTickInterval(5);
	deep_ocean_alpha_slider->setSingleStep(5);
	deep_ocean_alpha_slider->setValue(100);
	deep_ocean_alpha_slider->setEnabled(false);
	alpha_values_layout->addWidget(deep_ocean_alpha_slider, 3, 1);

	// Color values ********************************************************************************************** //
	QGroupBox* color_values_group = new QGroupBox("Light color values", this);
	// alpha_values_group->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
	auto color_values_layout = new QGridLayout(color_values_group);

	for (int i = 0; i < NUM_SkyColorNames; ++i)
	{
		std::string color_name = sky_color_names_map.at(i);

		LightViewPreview* LightPrev = new LightViewPreview(QString("%1 Color").arg(color_name.c_str()),
			QSize(180, LIGHT_VIEW_PREVIEW_HEIGHT));
		LightsPreview.push_back(LightPrev);
		color_values_layout->addWidget(LightPrev, i, 0);

		int availableWidth = color_values_group->width() - color_values_layout->contentsMargins().left()
							- color_values_layout->contentsMargins().right() - color_values_layout->spacing();
		int test_suggestedWidth = LightPrev->sizeHint().width();
		int test_suggestedWidth2 = LightPrev->minimumSizeHint().width();

		connect(LightPrev, &LightViewPreview::LeftClicked, [this, i, LightPrev]()
			{
				Sky* curr_sky = get_selected_sky();

				if (!curr_sky)
					return;
				if (!curr_sky->getParam(param_combobox->currentIndex()))
					return;

				LightViewEditor* Editor = new LightViewEditor(_map_view, curr_sky->getParam(param_combobox->currentIndex()).value(), SkyColorNames(i), this);
				ActiveEditor.push_back(Editor);
				Editor->show();

				connect(Editor, &LightViewEditor::Delete, [=](LightViewEditor* self)
					{
						for (int i = 0; i < ActiveEditor.size(); ++i)
							if (ActiveEditor[i] == self)
								ActiveEditor.erase(ActiveEditor.begin() + i, ActiveEditor.begin() + i);
					});

				connect(Editor, &LightViewEditor::UpdatePixmap, [this, LightPrev](const QPixmap Updated)
					{
						LightPrev->UpdatePixmap(Updated);
					});
			});
	}

	light_editing_layout->addWidget(color_values_group);


	connect(lightningInfoDialogButton, &QPushButton::clicked, [=]() {

		_lightning_info_dialog->show();
		});

	connect(_active_lights_tree, &QListWidget::itemDoubleClicked, this, [=](QListWidgetItem* item)
		{
			unsigned int selected_light_id = item->data(Qt::UserRole + 1).toUInt();

			Sky* sky = _map_view->getWorld()->renderer()->skies()->findSkyById(selected_light_id);
			if (sky)
				loadSelectSky(sky);
		});


	// connect(GetCurrentSkyButton, &QPushButton::clicked, [=]() {
	// 
	// 	// Sky* new_sky = _map_view->getWorld()->renderer()->skies()->findSkyWeights(map_view->getCamera()->position); // this just returns the global sky
	// 	// Sky* new_sky = _map_view->getWorld()->renderer()->skies()->findClosestSkyByDistance(map_view->getCamera()->position);
	// 	Sky* default_sky = _map_view->getWorld()->renderer()->skies()->findClosestSkyByWeight();
	// 	if (default_sky == nullptr)
	// 		return; // todo error
	// 	else
	// 	{
	// 		loadSelectSky(default_sky);
	// 	}
	// 	});

	connect(_light_tree_filter, &QLineEdit::textChanged, this, [this](QString const&)
		{
			applyLightListFilter();
		});

	connect(GetSelectedSkyButton, &QPushButton::clicked, [=]() 
		{
			auto const& selected_items = _light_tree->selectedItems();
			if (selected_items.size())
			{
				unsigned int selected_light_id = selected_items.back()->data(Qt::UserRole + 1).toUInt();

				Sky* sky = _map_view->getWorld()->renderer()->skies()->findSkyById(selected_light_id);
				if (sky)
					loadSelectSky(sky);
			}
		});

	connect(_light_tree, &QListWidget::itemDoubleClicked, this, [=](QListWidgetItem* item)
		{
			int const selected_light_id = item->data(Qt::UserRole + 1).toInt();

			if (auto& skies = _map_view->getWorld()->renderer()->skies())
			{
				if (Sky* const sky = skies->findSkyById(selected_light_id))
				{
					loadSelectSky(sky);
				}
			}
		});


	connect(addNewSkyButton, &QPushButton::clicked, [=]() {

		Sky* const old_sky = get_selected_sky();

		if (!old_sky)
		{
			QMessageBox::information(this, "Duplicate light"
				, "Select a light in the list first."
				, QMessageBox::Ok);
			return;
		}

		// The missing `return` here is the defect that let a map end up with two global lights: the
		// old code raised this warning and then fell straight through into the create call below it.
		if (old_sky->global)
		{
			QMessageBox::warning(this, "Duplicate light"
				, "A global light cannot be duplicated: it is global precisely because it sits at "
				  "0, 0, 0, and a map can have exactly one light there."
				, QMessageBox::Ok);
			return;
		}

		// Not copyLightFromDbc: Duplicate has no business replacing whatever the user has on the
		// clipboard, and the two operations only ever shared an implementation by accident.
		Noggit::LightSnapshot snapshot;

		if (!snapshotForLight(old_sky->getId(), snapshot))
		{
			return;
		}

		pasteLightSnapshot(snapshot, _deep_copy_params_chk->isChecked());
		});

	connect(_copy_light_button, &QPushButton::clicked, [=]() {

		Sky* const sky = get_selected_sky();

		if (!sky)
		{
			QMessageBox::information(this, "Copy light", "Select a light first.", QMessageBox::Ok);
			return;
		}

		if (copyLightFromDbc(sky->getId()))
		{
			_paste_light_button->setEnabled(true);
		}
		});

	connect(_paste_light_button, &QPushButton::clicked, [=]() {

		pasteLightFromClipboard(_deep_copy_params_chk->isChecked());
		});

	connect(browseLightsButton, &QPushButton::clicked, [=]() {

		// Built once and kept, because it walks all of Light.dbc and both name tables to fill its
		// tree. Reopening it should not pay for that again.
		if (!_light_browser)
		{
			_light_browser = new LightBrowser(this, this);

			connect(_light_browser, &LightBrowser::copyLightRequested, this, [this] (int light_id)
				{
					if (copyLightFromDbc(light_id))
					{
						_paste_light_button->setEnabled(true);
					}
				});

			connect(_light_browser, &LightBrowser::pasteLightRequested, this
				, [this] (int light_id, bool deep_copy)
				{
					Noggit::LightSnapshot snapshot;

					if (snapshotForLight(light_id, snapshot))
					{
						pasteLightSnapshot(snapshot, deep_copy);
					}
				});
		}

		_light_browser->show();
		_light_browser->raise();
		_light_browser->activateWindow();
		});

	connect(_delete_light_button, &QPushButton::clicked, [=]() {

		Sky* const sky = get_selected_sky();

		if (!sky)
		{
			QMessageBox::information(this, "Delete light", "Select a light first.", QMessageBox::Ok);
			return;
		}

		int const light_id = sky->getId();
		bool const on_disk = !sky->is_new_record;

		QString question
			(QString("Delete light %1 (%2)?")
				.arg(light_id)
				.arg(QString::fromStdString(lightDisplayName(light_id, sky->global, sky->zone_light))));

		// Two genuinely different consequences, so two genuinely different questions. An unsaved
		// light has never touched a file; a saved one is about to have its row removed from a DBC
		// the client reads.
		question += on_disk
			? "\n\nThis removes row " + QString::number(light_id) + " from Light.dbc and writes the "
				"file immediately.\n\nIts LightParams / LightIntBand / LightFloatBand rows are left "
				"alone on purpose: parameter rows are shared between lights, and deleting them would "
				"recolour unrelated zones."
			: "\n\nThis light has never been saved, so nothing on disk changes.";

		if (QMessageBox::question(this, "Delete light", question
			, QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
		{
			return;
		}

		auto& skies = _map_view->getWorld()->renderer()->skies();

		if (!skies)
		{
			return;
		}

		std::string error;

		if (!skies->deleteSky(light_id, error))
		{
			QMessageBox::warning(this, "Delete light"
				, "Nothing was deleted.\n\n" + QString::fromStdString(error), QMessageBox::Ok);
			return;
		}

		if (_selected_sky_id == light_id)
		{
			_selected_sky_id = 0;
			_light_editing_widget->setEnabled(false);
			save_current_sky_button->setEnabled(false);
			lightid_label->setText("No light selected");
		}

		rebuildLightList();
		updateLightning();
		});

	connect(_make_global_light_button, &QPushButton::clicked, [=]() {

		auto& skies = _map_view->getWorld()->renderer()->skies();

		if (!skies || !skies->using_fallback_global)
		{
			QMessageBox::information(this, "Global light"
				, "This map already has a global light of its own.", QMessageBox::Ok);
			return;
		}

		Sky const* fallback = nullptr;

		for (Sky const& sky : skies->skies)
		{
			if (sky.is_fallback_global)
			{
				fallback = &sky;
				break;
			}
		}

		if (!fallback)
		{
			return;
		}

		// Read straight out of Light.dbc rather than through copyLightFromDbc, so this does not
		// quietly replace whatever the user has on the clipboard.
		Noggit::LightSnapshot snapshot;

		if (!Noggit::lightSnapshotFromDbc(fallback->getId(), snapshot))
		{
			QMessageBox::warning(this, "Global light"
				, "Could not read the fallback light out of Light.dbc.", QMessageBox::Ok);
			return;
		}

		// 0,0,0 with no radii IS a global light. Copied from the borrowed one so the new light
		// starts from the lighting already on screen rather than from black.
		snapshot.pos = glm::vec3(0.0f, 0.0f, 0.0f);
		snapshot.r1 = 0.0f;
		snapshot.r2 = 0.0f;

		std::string error;

		// Always a deep copy. A map's global light is the one light every zone on it falls back to,
		// and having it share Azeroth's parameter rows means every colour change made here also
		// changes Azeroth -- which is the exact failure this button exists to prevent.
		Sky* const created = skies->pasteLight(snapshot, true, error);

		if (!created)
		{
			QMessageBox::warning(this, "Global light"
				, "Nothing was created.\n\n" + QString::fromStdString(error), QMessageBox::Ok);
			return;
		}

		lightNameDefinitions()[created->getId()] = "Global Light";

		_make_global_light_button->setEnabled(false);

		rebuildLightList();
		loadSelectSky(created);
		selectLightListRow(created->getId());
		updateLightning();

		QMessageBox::information(this, "Global light"
			, QString("Light %1 is now this map's global light, with its own LightParams rows. It "
								"exists only in memory -- press Save Light to write it.")
				.arg(created->getId())
			, QMessageBox::Ok);
		});

	connect(portToSkyButton, &QPushButton::clicked, [=]() {

		Sky* const sky = get_selected_sky();

		if (!sky)
		{
			return;
		}

		if (sky->global)
		{
			QMessageBox::information(this, "Fly to light"
				, "The global light has no position -- it is global because it sits at 0, 0, 0 and "
				  "applies to the whole map."
				, QMessageBox::Ok);
			return;
		}

		// Framed from the outer radius rather than a fixed distance, so a 40 yard light and a 3000
		// yard one both end up filling roughly the same amount of screen. focusOnPoint force-loads
		// the ADT the point sits in, which matters here: a light routinely sits over a tile the
		// world has streamed out.
		_map_view->focusOnPoint(sky->pos, std::max(50.0f, sky->r2 * 1.2f), true);
		});

	connect(param_combobox, qOverload<int>(&QComboBox::currentIndexChanged), [this](int index) {
		
		Sky* curr_sky = get_selected_sky();

		if (!curr_sky)
		{
			assert(false);
			return;
		}
		if (!curr_sky->skyParams[index])
			return;


		curr_sky->curr_sky_param = index;
		load_light_param(index);

		// update rendering to selected param
		updateLightning();

		});

	connect(save_current_sky_button, &QPushButton::clicked, [=]() {
		Sky* curr_sky = get_selected_sky();

		if (!curr_sky)
		{
			return;
		}

		std::string reason;

		// Asked before the write, so the message names the problem rather than the symptom. The
		// same check runs again as save_to_dbc's first statement, so a caller that skips this
		// cannot get past it either.
		if (!curr_sky->validateForSave(reason))
		{
			reportSaveRefusal(reason);
			return;
		}

		if (!curr_sky->save_to_dbc())
		{
			reportSaveRefusal("The write was refused after validation had passed, which means a DBC "
												"changed underneath it. The log names the exact record.");
			return;
		}

		_save_state_label->setProperty("state", "ok");

		// A dynamic property set after the widget has been polished does nothing until the style is
		// told to look again. Without this the label keeps whatever colour the previous message
		// gave it -- a red "nothing was written" staying red under a successful save.
		_save_state_label->style()->unpolish(_save_state_label);
		_save_state_label->style()->polish(_save_state_label);

		_save_state_label->setText(QString("Saved light %1. LightSkybox.dbc, LightIntBand.dbc, "
																			 "LightFloatBand.dbc, LightParams.dbc and Light.dbc were "
																			 "written to the project, in that order.")
																			 .arg(curr_sky->getId()));
		_save_state_label->show();

		// The row loses its unsaved marker, and a duplicated light stops being new.
		rebuildLightList();
		});

	// The five geometry spin boxes write straight into the selected Sky.
	//
	// get_selected_sky() can return nullptr -- a light can be deleted while its values are still in
	// these boxes -- and the previous versions dereferenced it unconditionally. loadSelectSky blocks
	// these signals while it fills the boxes, so a null here means the selection really did go away
	// rather than that the panel is mid-refresh.
	connect(pos_x_spin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double v) {
		if (Sky* const sky = get_selected_sky())
		{
			sky->pos.x = static_cast<float>(v);
			updateLightning();
		}
		});

	connect(pos_y_spin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double v) {
		if (Sky* const sky = get_selected_sky())
		{
			// The panel's Y box is Noggit's Z. A display convention only, applied symmetrically here
			// and in refreshSelectedLightFields; nothing between the Sky and the DBC swaps them.
			sky->pos.z = static_cast<float>(v);
			updateLightning();
		}
		});

	connect(pos_z_spin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double v) {
		if (Sky* const sky = get_selected_sky())
		{
			sky->pos.y = static_cast<float>(v);
			updateLightning();
		}
		});

	connect(inner_radius_spin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double v) {
		Sky* const sky = get_selected_sky();

		if (!sky)
		{
			return;
		}

		float const previous = sky->r1;
		sky->r1 = static_cast<float>(v);

		// The partner scales by the same FACTOR, not by the same offset: the falloff is a ratio, so
		// a light doubled in size should keep the same proportion of hard centre to soft edge.
		if (link_radii_chk->isChecked() && previous > 0.0f && sky->r1 > 0.0f)
		{
			QSignalBlocker const blocker(outer_radius_spin);
			sky->r2 = sky->r2 * (sky->r1 / previous);
			outer_radius_spin->setValue(sky->r2);
		}

		updateLightning();
		});

	connect(outer_radius_spin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double v) {
		Sky* const sky = get_selected_sky();

		if (!sky)
		{
			return;
		}

		float const previous = sky->r2;
		sky->r2 = static_cast<float>(v);

		if (link_radii_chk->isChecked() && previous > 0.0f && sky->r2 > 0.0f)
		{
			QSignalBlocker const blocker(inner_radius_spin);
			sky->r1 = sky->r1 * (sky->r2 / previous);
			inner_radius_spin->setValue(sky->r1);
		}

		updateLightning();
		});

	// The six controls below all divided an int by 100, which is integer division: every slider
	// value from 0 to 99 became 0.0 and 100 became 1.0. Glow, both water alphas and both ocean
	// alphas were therefore not adjustable at all -- they snapped between fully off and fully on --
	// and that same 0 or 1 is what went into LightParams.dbc on the next save.
	connect(glow_slider, &QSlider::valueChanged, this, [this](int v) {
		if (SkyParam* const param = selectedParam())
		{
			param->set_glow(v / 100.0f);
			updateLightning();
		}
		});

	connect(highlight_sky_checkbox, &QCheckBox::stateChanged, this, [this](int state) {
		if (SkyParam* const param = selectedParam())
		{
			param->set_highlight_sky(state != Qt::Unchecked);
			updateLightning();
		}
		});

	connect(shallow_water_alpha_slider, &QSlider::valueChanged, this, [this](int v) {
		if (SkyParam* const param = selectedParam())
		{
			param->set_river_shallow_alpha(v / 100.0f);
			updateLightning();
		}
		});

	connect(deep_water_alpha_slider, &QSlider::valueChanged, this, [this](int v) {
		if (SkyParam* const param = selectedParam())
		{
			param->set_river_deep_alpha(v / 100.0f);
			updateLightning();
		}
		});

	connect(shallow_ocean_alpha_slider, &QSlider::valueChanged, this, [this](int v) {
		if (SkyParam* const param = selectedParam())
		{
			param->set_ocean_shallow_alpha(v / 100.0f);
			updateLightning();
		}
		});

	connect(deep_ocean_alpha_slider, &QSlider::valueChanged, this, [this](int v) {
		if (SkyParam* const param = selectedParam())
		{
			param->set_ocean_deep_alpha(v / 100.0f);
			updateLightning();
		}
		});

	// connect(skybox_model_lineedit, &QLineEdit::textChanged, [&](std::string v) {
	// All four skybox handlers used to write through getParam(index).value(), an unchecked
	// std::optional dereference on a slot the combo box can legitimately be sitting on while it
	// holds no param -- loadSelectSky greys those entries out, and greying an item out does not
	// stop currentIndex() naming it.
	QLineEdit::connect(skybox_model_lineedit, &QLineEdit::textChanged, this, [this]
	{
		SkyParam* const param = selectedParam();

		if (!param)
		{
			return;
		}

		auto const text = skybox_model_lineedit->text().toStdString();

		if (text.empty())
			param->skybox.reset();
		else
			param->skybox.emplace(text.c_str(), _world->getRenderContext());

		updateLightning();
	});

	connect(skybox_flag_1, &QCheckBox::stateChanged, this, [this](int state) {
		if (SkyParam* const param = selectedParam())
		{
			if (state)
				param->skyboxFlags |= (1 << 0);
			else
				param->skyboxFlags &= ~(1 << 0);
		}
	});

	connect(skybox_flag_2, &QCheckBox::stateChanged, this, [this](int state) {
		if (SkyParam* const param = selectedParam())
		{
			if (state)
				param->skyboxFlags |= (1 << 1);
			else
				param->skyboxFlags &= ~(1 << 1);
		}
	});

	// connect(floats_editor_button, &QPushButton::clicked, [=]() {
	// LightFloatsEditor * Editor = new LightFloatsEditor(_map_view, _curr_sky->skyParams[param_combobox->currentIndex()], this);
	// 
	// 	Editor->show();
	// );

}

void LightEditor::loadSelectSky(Sky* _curr_sky)
{
	assert(_curr_sky->getId());
	if (!_curr_sky->getId())
		return;

	_selected_sky_id = _curr_sky->getId();

	// The one choke point. Every path that selects a light -- the list, the "Edit selected light"
	// button, a viewport pick, a paste -- ends up here, so writing the selection into Skies here is
	// what keeps the panel and the viewport gizmo from disagreeing about which light is selected.
	// Sky::_selected was declared, initialised and read by Sky::selected() and written by nothing
	// at all before this line existed.
	if (auto& skies = _map_view->getWorld()->renderer()->skies())
	{
		skies->setSelectedLight(_selected_sky_id);
	}

	// The viewport has to repaint: the gizmo appears at the newly selected light and nothing else
	// in this call path is an input event.
	_map_view->markSpawnOverlayDirty();

	// Whatever the last save or paste said was about a different light.
	_save_state_label->hide();

	QSignalBlocker const _1(pos_x_spin);
	QSignalBlocker const _2(pos_y_spin);
	QSignalBlocker const _3(pos_z_spin);
	QSignalBlocker const _4(inner_radius_spin);
	QSignalBlocker const _5(outer_radius_spin);


	// disable combobox param items if param is not set or doesn't exist
	// in the future allow to add/edit param id
	for (int i = 0; i < NUM_SkyParamsNames; ++i)
	{
		auto sky_param = _curr_sky->skyParams[i];
		if (!sky_param)
		{
			QStandardItemModel* model = qobject_cast<QStandardItemModel*>(param_combobox->model());

			if (model) {
				QStandardItem* item = model->item(i);
				if (item) {
					item->setFlags(item->flags() & ~Qt::ItemIsEnabled);
				}
			}
		}
	}

	_light_editing_widget->setEnabled(true);
	lightning_tabs->setCurrentWidget(_light_editing_widget);

	save_current_sky_button->setEnabled(true);
	// maybe move the inits to a separate function
	// global values
	std::string light_name = lightDisplayName(_curr_sky->Id, _curr_sky->global, _curr_sky->zone_light);

	std::stringstream ss;
	ss << _curr_sky->Id << "-" << light_name;
	if (_curr_sky->global && _map_view->getWorld()->renderer()->skies()->using_fallback_global)
		ss << " (Fallback)";
	lightid_label->setText(QString::fromStdString(ss.str().c_str()));

	// name_line_edit->setText(QString::fromStdString(_curr_sky->name));
	name_line_edit->setText(QString::fromStdString(light_name));

	global_light_chk->setChecked(_curr_sky->global);
	pos_x_spin->setEnabled(!_curr_sky->global);
	pos_y_spin->setEnabled(!_curr_sky->global);
	pos_z_spin->setEnabled(!_curr_sky->global);
	inner_radius_spin->setEnabled(!_curr_sky->global);
	outer_radius_spin->setEnabled(!_curr_sky->global);

	zone_light_chk->setChecked(_curr_sky->zone_light);

	pos_x_spin->setValue(_curr_sky->pos.x);
	pos_y_spin->setValue(_curr_sky->pos.z); // swap Z and Y
	pos_z_spin->setValue(_curr_sky->pos.y);
	inner_radius_spin->setValue(_curr_sky->r1);
	outer_radius_spin->setValue(_curr_sky->r2);

	param_combobox->setEnabled(true);

	{
	  QSignalBlocker const __(param_combobox);
	  param_combobox->setCurrentIndex(_curr_sky->curr_sky_param);
	}

	load_light_param(param_combobox->currentIndex());
}

int LightEditor::selectedLightId() const
{
	return _selected_sky_id;
}

SkyParam* LightEditor::selectedParam() const
{
	Sky* const sky = get_selected_sky();

	if (!sky)
	{
		return nullptr;
	}

	int const index = param_combobox->currentIndex();

	if (index < 0 || index >= NUM_SkyParamsNames)
	{
		return nullptr;
	}

	auto const param = sky->getParam(index);

	return param.has_value() ? param.value() : nullptr;
}

void LightEditor::applyLightListFilter()
{
	QString const text = _light_tree_filter->text();

	for (int i = 0; i < _light_tree->count(); ++i)
	{
		QListWidgetItem* const item = _light_tree->item(i);

		item->setHidden(!text.isEmpty() && !item->text().contains(text, Qt::CaseInsensitive));
	}
}

void LightEditor::selectLightListRow(int light_id)
{
	for (int i = 0; i < _light_tree->count(); ++i)
	{
		QListWidgetItem* const item = _light_tree->item(i);

		if (item->data(Qt::UserRole + 1).toInt() != light_id)
		{
			continue;
		}

		// Blocked, because setCurrentItem emits currentItemChanged and this function is reached
		// from inside loadSelectSky -- which is what a row change would call straight back into.
		QSignalBlocker const blocker(_light_tree);

		// A row hidden by the filter cannot be made current in a way the user can see, so the
		// filter is lifted for it. Selecting a light in the viewport and having the panel appear to
		// select nothing is worse than a filter that quietly stops applying to one row.
		item->setHidden(false);

		_light_tree->setCurrentItem(item);
		_light_tree->scrollToItem(item);
		return;
	}
}

void LightEditor::rebuildLightList()
{
	int const previously_selected = _selected_sky_id;

	struct Row
	{
		int id;
		bool global;
		bool zone_light;
		bool unsaved;
	};

	std::vector<Row> rows;

	auto& skies = _map_view->getWorld()->renderer()->skies();

	if (skies)
	{
		_light_list_from_skies = true;

		for (Sky const& sky : skies->skies)
		{
			rows.push_back({sky.getId(), sky.global, sky.zone_light, sky.is_new_record});
		}
	}
	else
	{
		for (DBCFile::Iterator i = gLightDB.begin(); i != gLightDB.end(); ++i)
		{
			if (i->getInt(LightDB::Map) != static_cast<int>(_world->getMapID()))
			{
				continue;
			}

			bool const global = (i->getFloat(LightDB::PositionX) == 0.0f
													 && i->getFloat(LightDB::PositionY) == 0.0f
													 && i->getFloat(LightDB::PositionZ) == 0.0f);

			rows.push_back({static_cast<int>(i->getUInt(LightDB::ID)), global, false, false});
		}
	}

	// By id, because Skies keeps its vector sorted by outer radius with the global last (see
	// Sky::operator<) and a list that reorders itself every time a radius changes is unusable.
	std::sort(rows.begin(), rows.end()
		, [] (Row const& a, Row const& b) { return a.id < b.id; });

	QSignalBlocker const blocker(_light_tree);

	_light_tree->clear();

	for (Row const& row : rows)
	{
		QListWidgetItem* const item = new QListWidgetItem();
		item->setData(Qt::UserRole + 1, QVariant(row.id));

		std::stringstream ss;

		// A leading asterisk for a light that exists only in memory, so "Duplicate, then close
		// Noggit" cannot look like it saved something. Duplicate deliberately no longer writes.
		if (row.unsaved)
		{
			ss << "* ";
		}

		ss << row.id << "-" << lightDisplayName(row.id, row.global, row.zone_light);

		item->setText(QString::fromStdString(ss.str()));

		if (row.global)
		{
			item->setIcon(Noggit::Ui::FontAwesomeIcon(Noggit::Ui::FontAwesome::sun));
		}

		_light_tree->addItem(item);
	}

	applyLightListFilter();

	if (previously_selected)
	{
		selectLightListRow(previously_selected);
	}
}

void LightEditor::onLightSelectedInViewport(int light_id)
{
	auto& skies = _map_view->getWorld()->renderer()->skies();

	if (!skies)
	{
		return;
	}

	Sky* const sky = skies->findSkyById(light_id);

	if (!sky)
	{
		return;
	}

	// MapView::selectLight has already written the selection into Skies -- which is what the
	// viewport gizmo reads -- so this only has to catch the panel up.
	loadSelectSky(sky);
	selectLightListRow(light_id);
}

void LightEditor::refreshSelectedLightFields()
{
	Sky* const sky = get_selected_sky();

	if (!sky)
	{
		return;
	}

	QSignalBlocker const _1(pos_x_spin);
	QSignalBlocker const _2(pos_y_spin);
	QSignalBlocker const _3(pos_z_spin);
	QSignalBlocker const _4(inner_radius_spin);
	QSignalBlocker const _5(outer_radius_spin);

	pos_x_spin->setValue(sky->pos.x);
	pos_y_spin->setValue(sky->pos.z); // the panel's Y is Noggit's Z; see the spin box handlers
	pos_z_spin->setValue(sky->pos.y);
	inner_radius_spin->setValue(sky->r1);
	outer_radius_spin->setValue(sky->r2);
}

void LightEditor::reportSaveRefusal(std::string const& reason)
{
	_save_state_label->setProperty("state", "error");

	// The property is changed after the widget has been polished, so the style has to be told; a
	// bare setProperty leaves the label looking exactly as it did before. QLabel[state="error"] and
	// [state="ok"] are both already in the theme -- this adds no styling of its own.
	_save_state_label->style()->unpolish(_save_state_label);
	_save_state_label->style()->polish(_save_state_label);

	_save_state_label->setText("Nothing was written.\n" + QString::fromStdString(reason));
	_save_state_label->show();

	QMessageBox::warning(this, "Light not saved"
		, "Nothing was written to any DBC.\n\n" + QString::fromStdString(reason)
		, QMessageBox::Ok);
}

bool LightEditor::snapshotForLight(int light_id, Noggit::LightSnapshot& out)
{
	auto& skies = _map_view->getWorld()->renderer()->skies();

	// A light of the LOADED map is captured from Skies, not from the DBC, because Skies is where
	// the user's unsaved position, radius and parameter changes live. For every other map -- which
	// is the whole point of the browser -- only the DBC row exists.
	if (skies)
	{
		Sky const* const sky = skies->findSkyById(light_id);

		if (sky && skies->snapshotLight
			(light_id, lightDisplayName(light_id, sky->global, sky->zone_light), out))
		{
			return true;
		}
	}

	if (!Noggit::lightSnapshotFromDbc(light_id, out))
	{
		QMessageBox::warning(this, "Light browser"
			, QString("Light.dbc has no readable row %1.").arg(light_id), QMessageBox::Ok);
		return false;
	}

	out.name = lightDisplayName(light_id, out.pos == glm::vec3(0.f, 0.f, 0.f));

	return true;
}

bool LightEditor::copyLightFromDbc(int light_id)
{
	Noggit::LightSnapshot snapshot;

	if (!snapshotForLight(light_id, snapshot))
	{
		return false;
	}

	Noggit::setLightClipboard(snapshot);

	return true;
}

void LightEditor::pasteLightFromClipboard(bool deep_copy_params)
{
	if (!Noggit::lightClipboard().valid)
	{
		QMessageBox::information(this, "Paste light", "The light clipboard is empty."
			, QMessageBox::Ok);
		return;
	}

	// A copy, not a reference: pasteLightSnapshot rewrites the position, and the clipboard is a
	// singleton that the next paste has to find unchanged.
	pasteLightSnapshot(Noggit::lightClipboard(), deep_copy_params);
}

void LightEditor::pasteLightSnapshot(Noggit::LightSnapshot snapshot, bool deep_copy_params)
{
	Noggit::LightSnapshot const& clipboard = snapshot;

	auto& skies = _map_view->getWorld()->renderer()->skies();

	if (!skies)
	{
		return;
	}

	Noggit::LightSnapshot placed = clipboard;

	// Dropped at the camera rather than at the source coordinates. A light pasted from another
	// continent at its original position would land somewhere the user is not -- often outside
	// this map's grid entirely -- and the first thing they would have to do is go and find it.
	//
	// A global light is the exception, because its position IS 0,0,0 by definition; moving it to
	// the camera would turn it into an ordinary light with a zero radius, which lights nothing.
	if (!(clipboard.pos.x == 0.0f && clipboard.pos.y == 0.0f && clipboard.pos.z == 0.0f))
	{
		placed.pos = _map_view->getCamera()->position;
	}

	std::string error;

	Sky* const pasted = skies->pasteLight(placed, deep_copy_params, error);

	if (!pasted)
	{
		QMessageBox::warning(this, "Paste light"
			, "Nothing was pasted.\n\n" + QString::fromStdString(error), QMessageBox::Ok);
		return;
	}

	// Named after its source so the new row is recognisable before anyone renames it. The name
	// table is this editor's own CSV, not a DBC, so this costs nothing on disk and is lost on
	// restart -- which is honest, because the name was never saved anywhere.
	std::string const source_name = clipboard.name.empty()
		? lightDisplayName(clipboard.light_id, false)
		: clipboard.name;

	lightNameDefinitions()[pasted->getId()] = "Copy of " + source_name;

	rebuildLightList();
	loadSelectSky(pasted);
	selectLightListRow(pasted->getId());
	updateLightning();

	_save_state_label->setProperty("state", "warn");
	_save_state_label->style()->unpolish(_save_state_label);
	_save_state_label->style()->polish(_save_state_label);
	_save_state_label->setText
		(QString("Light %1 was created from light %2 of map %3, and exists only in memory. "
						 "Press Save Light to write it.")
			.arg(pasted->getId()).arg(clipboard.light_id).arg(clipboard.map_id));
	_save_state_label->show();
}

void LightEditor::UpdateToolTime()
{
	QSignalBlocker const blocker(_lightning_info_dialog->_time_dial);
	_lightning_info_dialog->_time_dial->setValue(_map_view->getWorld()->time);

	QSignalBlocker TimeHourBlocker(_lightning_info_dialog->TimeSelectorHour);
	QSignalBlocker TimeminutesBlocker(_lightning_info_dialog->TimeSelectorMin);

	int ConvertedTime = _map_view->getWorld()->time * (24 * 60) / MAX_TIME_VALUE;

	int Hour = floor(ConvertedTime / 60);
	int Min = ConvertedTime % 60;

	_lightning_info_dialog->TimeSelectorHour->setValue(Hour);
	_lightning_info_dialog->TimeSelectorMin->setValue(Min);

	UpdateWorldTime();
}

void LightEditor::updateActiveLights()
{
	// The list on the tab above was built from Light.dbc, because this panel is constructed at the
	// end of MapView's constructor and Skies is not created until the first initializeGL. This is
	// the first callback that runs with Skies guaranteed to exist -- LightTool::onSelected calls it
	// -- so it is where the list is rebuilt from the real model. Once per map load, not per frame.
	if (!_light_list_from_skies && _map_view->getWorld()->renderer()->skies())
	{
		rebuildLightList();
	}

	// Cheap enough to re-answer every call: it is one bool read, and the alternative is a flag that
	// goes stale the moment the map gains a global light.
	if (auto& skies = _map_view->getWorld()->renderer()->skies())
	{
		_make_global_light_button->setEnabled(skies->using_fallback_global);
	}

	_active_lights_tree->clear();
	
	Sky* global_sky = nullptr;

	// if we have one light with 1.0 global is entirely replaced, otherwise they are blended with global.
	bool use_global = true;

	int active_count = 0;
	for (auto& sky : _map_view->getWorld()->renderer()->skies()->skies)
	{
		if (sky.global)
			global_sky = &sky;

		// global always has weight 0.0f
		if (sky.weight > 0.f)
		{
			if (sky.weight == 1.0f)
				use_global = false;

			// create item, copypasta from the other tree widget
			QListWidgetItem* item = new QListWidgetItem();

			std::stringstream ss;
			unsigned int light_id = sky.getId();
			item->setData(Qt::UserRole + 1, QVariant(light_id));

			std::string const light_name = lightDisplayName(light_id, false, sky.zone_light);
			// std::string const sky_percent = std::to_string(sky.weight * 100.0f) + "% :";

			ss << "[" << std::fixed << std::setprecision(1) << (sky.weight * 100.0f);
			ss << "%] " << light_id << "-" << light_name;
			item->setText(QString(ss.str().c_str()));
			_active_lights_tree->addItem(item);
			active_count++;
		}
	}

	if (use_global && global_sky)
	{
		QListWidgetItem* item = new QListWidgetItem();

		std::stringstream ss;
		unsigned int light_id = global_sky->getId();
		item->setData(Qt::UserRole + 1, QVariant(light_id));

		std::string const light_name = lightDisplayName(light_id, true);

		ss << "[Global] " << light_id << "-" << light_name;
		item->setText(QString(ss.str().c_str()));
		_active_lights_tree->addItem(item);

		// item->setIcon(Noggit::Ui::FontAwesomeIcon(Noggit::Ui::FontAwesome::sun));
	}

}

void Noggit::Ui::Tools::LightEditor::updateLightningInfo()
{
	Skies* const skies = _map_view->getWorld()->renderer()->skies().get();

	Sky* const highest_weight_sky = _map_view->getWorld()->renderer()->skies()->findClosestSkyByWeight();

	std::string light_name = lightDisplayName(highest_weight_sky->Id, highest_weight_sky->global, highest_weight_sky->zone_light);
	std::stringstream ss;
	ss << highest_weight_sky->Id << "-" << light_name;
	_lightning_info_dialog->_highest_weight_sky_label->setText(QString::fromStdString(ss.str().c_str()));

	// color params
	for (int i = 0; i < NUM_SkyColorNames; ++i)
	{
		QLabel* colorIconLabel = _lightning_info_dialog->_current_lightning_colors_labels[i];
		QPixmap colorIcon(16, 16);  // Create a 16x16 px pixmap
		// colorIcon.fill(Qt::transparent);  // Transparent background

		glm::vec3 color = skies->color_set[i];

		QColor customColor = QColor::fromRgbF(color.r, color.g, color.b);

		QPainter painter(&colorIcon);
		painter.setBrush(QBrush(customColor));
		painter.setPen(Qt::NoPen);
		painter.drawRect(0, 0, 16, 16);
		colorIconLabel->setPixmap(colorIcon);
		std::stringstream ss;
		ss << "R:" << (int)(color.r * 255.f);
		ss << "\nG:" << (int)(color.g * 255.f);
		ss << "\nB:" << (int)(color.b * 255.f);
		colorIconLabel->setToolTip(ss.str().c_str());
	}
	// float params
	{
		float fog_distance = skies->fog_distance_end();
		_lightning_info_dialog->_current_lightning_floats_labels[SKY_FOG_DISTANCE]->setText(QString::number(fog_distance, 'f', 2));

		// actual value in storage is multiplier, not start distance
		float fog_multiplier = skies->fog_distance_multiplier();
		float fog_start_distance = skies->fog_distance_start();
		// display format : 16000 (0.1)
		QString formattedString = QString("%1 (%2)").arg(QString::number(fog_multiplier, 'f', 2)).arg(QString::number(fog_start_distance, 'f', 2));
		auto debug_str = formattedString.toStdString();
		_lightning_info_dialog->_current_lightning_floats_labels[SKY_FOG_MULTIPLIER]->setText(formattedString);

		float celestial_glow= skies->celestial_glow();
		_lightning_info_dialog->_current_lightning_floats_labels[SKY_CELESTIAL_GLOW]->setText(QString::number(celestial_glow, 'f', 2));

		float cloud_density = skies->cloud_density();
		_lightning_info_dialog->_current_lightning_floats_labels[SKY_CLOUD_DENSITY]->setText(QString::number(cloud_density, 'f', 2));

		float float_param_unk4 = skies->unknown_float_param4();
		_lightning_info_dialog->_current_lightning_floats_labels[SKY_UNK_FLOAT_PARAM_4]->setText(QString::number(float_param_unk4, 'f', 2));

		float float_param_unk5 = skies->unknown_float_param5();
		_lightning_info_dialog->_current_lightning_floats_labels[SKY_UNK_FLOAT_PARAM_5]->setText(QString::number(float_param_unk5, 'f', 2));
	}
	// light params
	{
		_lightning_info_dialog->_river_shallow_alpha_label_label->setText(QString::number(skies->river_shallow_alpha(), 'f', 2));
		_lightning_info_dialog->_river_deep_alpha_label->setText(QString::number(skies->river_deep_alpha(), 'f', 2));
		_lightning_info_dialog->_ocean_shallow_alpha_label->setText(QString::number(skies->ocean_shallow_alpha(), 'f', 2));
		_lightning_info_dialog->_ocean_deep_alpha_label->setText(QString::number(skies->ocean_deep_alpha(), 'f', 2));
		_lightning_info_dialog->_glow_label->setText(QString::number(skies->glow(), 'f', 2));

		auto param_opt = highest_weight_sky->getCurrentParam();
		if (param_opt.has_value())
		{
			SkyParam* sky_param = param_opt.value();
			bool highlight = sky_param->highlight_sky();
			_lightning_info_dialog->_highlight_label->setText(highlight ? "True" : "False");
		}

	}
}

void LightEditor::updateLightning()
{
	_world->renderer()->skies()->force_update();
	updateLightningInfo();

}

LightEditor::~LightEditor()
{
	// MapView keeps a raw back-pointer to this panel so a viewport pick can reach it. MapView owns
	// the tools, LightTool owns this widget, so MapView is still alive here -- but it must not be
	// left holding a pointer to a destroyed panel if the tool is ever torn down first.
	if (_map_view)
	{
		_map_view->setLightEditor(nullptr);
	}
}

void LightEditor::UpdateWorldTime()
{
	if (ActiveEditor.size() == 0)
		return;

	for (int i = 0; i < ActiveEditor.size(); ++i)
		ActiveEditor[i]->UpdateWorldTime();
}

Sky* Noggit::Ui::Tools::LightEditor::get_selected_sky() const
{
	if(!_selected_sky_id)
		return nullptr;

	return _map_view->getWorld()->renderer()->skies()->findSkyById(_selected_sky_id);
}

void LightEditor::load_light_param(int param_id)
{
	Sky* curr_sky = get_selected_sky();

	if (!curr_sky)
	{
		assert(false);
		return;
	}

	auto param_opt = curr_sky->getParam(param_id);
	if (!param_opt.has_value())
 		return;

	SkyParam * sky_param = param_opt.value();

	QSignalBlocker const _1(glow_slider);
	QSignalBlocker const _2(highlight_sky_checkbox);
	QSignalBlocker const _3(shallow_water_alpha_slider);
	QSignalBlocker const _4(deep_water_alpha_slider);
	QSignalBlocker const _5(shallow_ocean_alpha_slider);
	QSignalBlocker const _6(deep_ocean_alpha_slider);

	QSignalBlocker const _7(skybox_model_lineedit);
	QSignalBlocker const _8(skybox_flag_1);
	QSignalBlocker const _9(skybox_flag_2);
	
	int nb_user = 0;
	try
	{
		DBCFile::Record data = gLightDB.getByID(curr_sky->Id);

		for (DBCFile::Iterator i = gLightDB.begin(); i != gLightDB.end(); ++i)
		{
			for (int l = 0; l < NUM_SkyParamsNames; l++)
			{
				if (i->getInt(LightDB::DataIDs + l) == data.getInt(LightDB::DataIDs + param_combobox->currentIndex()))
					nb_user++;
			}
		}
	}
	catch (...)
	{

	}
	
	std::stringstream ss;
	ss << "LightParam Id : " << sky_param->Id << "\nThis param is used " << nb_user << " times.";
	_nb_param_users->setText(QString::fromStdString(ss.str().c_str()));
	
	glow_slider->setSliderPosition(sky_param->glow() * 100);
	glow_slider->setEnabled(true);
	// Qt::CheckState(bool) is not Qt::Checked. The enum is Unchecked = 0, PartiallyChecked = 1,
	// Checked = 2 -- so a highlighted sky was shown as a tri-state "partially checked" box, never
	// as a tick, on every light that had the flag set.
	highlight_sky_checkbox->setCheckState
		(sky_param->highlight_sky() ? Qt::Checked : Qt::Unchecked);
	highlight_sky_checkbox->setEnabled(true);
		// alpha values
	shallow_water_alpha_slider->setSliderPosition(sky_param->river_shallow_alpha() * 100);
	shallow_water_alpha_slider->setEnabled(true);
	deep_water_alpha_slider->setSliderPosition(sky_param->river_deep_alpha() * 100);
	deep_water_alpha_slider->setEnabled(true);
	shallow_ocean_alpha_slider->setSliderPosition(sky_param->ocean_shallow_alpha() * 100);
	shallow_ocean_alpha_slider->setEnabled(true);
	deep_ocean_alpha_slider->setSliderPosition(sky_param->ocean_deep_alpha() * 100);
	deep_ocean_alpha_slider->setEnabled(true);
	// color values
	for (int i = 0; i < NUM_SkyColorNames; ++i)
	{
	  LightsPreview[i]->SetPreview(sky_param->colorRows[i]);
	  //_color_value_Buttons[i]->setText(QString::fromStdString(std::format("{} / 16 values", sky_param->colorRows[i].size())));
	}
	
	// skybox
	skybox_model_lineedit->setText("");
	if (sky_param->skybox.has_value())
	{
	  skybox_model_lineedit->setText(QString::fromStdString(sky_param->skybox.value().model.get()->file_key().filepath()));
	}
	skybox_model_lineedit->setEnabled(true);
	
	skybox_flag_1->setChecked(false);
	skybox_flag_2->setChecked(false);

	if (sky_param->skyboxFlags & (1 << 0))
	  skybox_flag_1->setChecked(true);

	if (sky_param->skyboxFlags & (1 << 1))
	  skybox_flag_2->setChecked(true); // was skybox_flag_1, so bit 1 was invisible and bit 0 lied

	skybox_flag_1->setEnabled(true);
	skybox_flag_2->setEnabled(true);

}

Noggit::Ui::Tools::LightningInfoDialog::LightningInfoDialog(LightEditor* editor, QWidget* parent)
	:_editor(editor)
	, QWidget(parent)
{
	setWindowTitle("Lightning Info");
	setWindowFlags(Qt::Dialog);
	
	// THIS DIALOG'S THREE COLUMNS WERE NEVER LAID OUT, and the three lines that did it are the
	// same defect ZoneIDBrowser carried. Each column was constructed as new QVBoxLayout(THIS) on
	// a widget that already owned main_layout: the constructor cannot install a second layout
	// (QWidget::setLayout warns and returns) but it HAS already taken the widget as its QObject
	// parent, and QLayout::addChildLayout refuses any layout that already has a parent -- so
	// every one of the three addLayout calls below returned without adding anything.
	//
	// The dial, the two time spin boxes and every label and field in the other two columns were
	// therefore parented to the dialog by their layouts and then given geometry by nothing, i.e.
	// drawn stacked in the top-left corner at the default 100x30. Parentless layouts here, and
	// addLayout does the owning -- which is what the code always meant.
	auto main_layout = new QHBoxLayout(this);

	auto layout_column1 = new QVBoxLayout;
	main_layout->addLayout(layout_column1);
	auto layout_column2 = new QVBoxLayout;
	main_layout->addLayout(layout_column2);
	auto layout_column3 = new QVBoxLayout;
	main_layout->addLayout(layout_column3);

	// initialize widgets /////

	layout_column1->addWidget(new QLabel("Set current time :", this));
	_time_dial = new QDial(this);
	layout_column1->addWidget(_time_dial);
	_time_dial->setRange(0, DAY_DURATION); // Time Values from 0 to 2880 where each number represents a half minute from midnight to midnight
	_time_dial->setWrapping(true);
	_time_dial->setSliderPosition((int)_editor->_world->time); // to get ingame orientation
	// _time_dial->setInvertedAppearance(true); // sets the position at top
	_time_dial->setToolTip("Time (24hours)");
	_time_dial->setSingleStep(180); // ticks are 360 units (1/8 = 3 hours)

	TimeSelectorHour = new QSpinBox(this);
	TimeSelectorHour->setMinimum(0);
	TimeSelectorHour->setMaximum(23);

	TimeSelectorMin = new QSpinBox(this);
	TimeSelectorMin->setMinimum(0);
	TimeSelectorMin->setMaximum(59);

	// Parentless for the same reason as the three columns above.
	auto time_hlayout = new QHBoxLayout;
	time_hlayout->addWidget(new QLabel(tr("Hours"), this));
	time_hlayout->addWidget(TimeSelectorHour);
	time_hlayout->addWidget(new QLabel(tr("Minutes"), this));
	time_hlayout->addWidget(TimeSelectorMin);
	time_hlayout->addStretch();
	layout_column1->addLayout(time_hlayout);

	connect(_time_dial, &QDial::valueChanged, [&](int v) // [this]
		{
			_editor->_map_view->getWorld()->time = v;
			_editor->updateLightning();
			_editor->UpdateWorldTime();

			QSignalBlocker TimeHourBlocker(TimeSelectorHour);
			QSignalBlocker TimeminutesBlocker(TimeSelectorMin);

			int ConvertedTime = v * (24 * 60) / MAX_TIME_VALUE;

			int Hour = floor(ConvertedTime / 60);
			int Min = ConvertedTime % 60;

			TimeSelectorHour->setValue(Hour);
			TimeSelectorMin->setValue(Min);
		}
	);

	connect(TimeSelectorHour, &QSpinBox::textChanged, [=](QString)
		{

			int Time = ((TimeSelectorHour->value() * 60) + TimeSelectorMin->value()) * MAX_TIME_VALUE / (23 * 60 + 59);

			_editor->_map_view->getWorld()->time = Time;

			QSignalBlocker TimeHourBlocker(_time_dial);
			_time_dial->setValue(Time);

			_editor->updateLightning();
			_editor->UpdateWorldTime();
		});

	connect(TimeSelectorMin, &QSpinBox::textChanged, [=](QString)
		{
			int Time = ((TimeSelectorHour->value() * 60) + TimeSelectorMin->value()) * MAX_TIME_VALUE / (23 * 60 + 59);

			_editor->_map_view->getWorld()->time = Time;

			QSignalBlocker TimeHourBlocker(_time_dial);
			_time_dial->setValue(Time);

			_editor->updateLightning();
			_editor->UpdateWorldTime();
		});

	// current colors preview
	for (int i = 0; i < NUM_SkyColorNames; ++i)
	{
		std::string color_name = sky_color_names_map.at(i);

		_current_lightning_colors_labels[i] = new QLabel(this);
		QLabel* colorIconLabel = _current_lightning_colors_labels[i];
		QPixmap colorIcon(32, 16);  // Create a 16x16 px pixmap
		// colorIcon.fill(Qt::transparent);  // Transparent background

		// Use QPainter to draw a red square icon
		QPainter painter(&colorIcon);
		painter.setBrush(QBrush(Qt::black));  // Set the brush to red
		painter.setPen(Qt::NoPen);  // No border
		painter.drawRect(0, 0, 32, 16);  // Draw the square
		colorIconLabel->setPixmap(colorIcon);  // Set the pixmap

		// lightningBox_content_layout->addRow(color_name.c_str(), colorIconLabel);
	}
	// current float params preview
	for (int i = 0; i < NUM_SkyFloatParamsNames; ++i)
	{
		_current_lightning_floats_labels[i] = new QLabel("0", this);

		// lightningBox_content_layout->addRow(sky_float_values_names_map.at(i).c_str(), _current_lightning_floats_labels[i]);
	}
	// light params preview
	{
		_river_shallow_alpha_label_label = new QLabel("0", this);
		// lightningBox_content_layout->addRow("Shallow Water Alpha", _river_shallow_alpha_label_label);
		_river_deep_alpha_label = new QLabel("0", this);
		//lightningBox_content_layout->addRow("Deep Water Alpha", _river_deep_alpha_label);
		_ocean_shallow_alpha_label = new QLabel("0", this);
		//lightningBox_content_layout->addRow("Shallow Ocean Alpha", _ocean_shallow_alpha_label);
		_ocean_deep_alpha_label = new QLabel("0", this);
		//lightningBox_content_layout->addRow("Deep Ocean Alpha", _ocean_deep_alpha_label);
		_glow_label = new QLabel("0", this);
		//lightningBox_content_layout->addRow("Glow", _glow_label);
		_highlight_label = new QLabel("0", this);
		//lightningBox_content_layout->addRow("Highlight Sky", _highlight_label);
	}
	///////////////////////////

	layout_column1->addWidget(new QLabel("Highest Weight Light : ", this));
	_highest_weight_sky_label = new QLabel("None/Not initialized", this);
	layout_column1->addWidget(_highest_weight_sky_label);
	// layout_column1->addItem("Highest Weight Light", _highest_weight_sky_label);

	// Light group
	{
		QGroupBox* light_group = new QGroupBox("Light", this);
		auto light_layout = new QFormLayout(light_group);

		int index = LIGHT_GLOBAL_DIFFUSE;
		light_layout->addRow(sky_color_names_map.at(index).c_str(), _current_lightning_colors_labels[index]);

		index = LIGHT_GLOBAL_AMBIENT;
		light_layout->addRow(sky_color_names_map.at(index).c_str(), _current_lightning_colors_labels[index]);

		light_layout->addRow("Glow", _glow_label);
		light_layout->addRow("Highlight Sky", _highlight_label);

		index = SHADOW_OPACITY;
		light_layout->addRow(sky_color_names_map.at(index).c_str(), _current_lightning_colors_labels[index]);

		index = SKY_UNK_FLOAT_PARAM_4;
		light_layout->addRow("Unknown_float_param4", _current_lightning_floats_labels[index]);

		index = SKY_UNK_FLOAT_PARAM_5;
		light_layout->addRow("Unknown_float_param5", _current_lightning_floats_labels[index]);

		layout_column1->addWidget(light_group);
	}

	// Fog group
	{
		QGroupBox* fog_group = new QGroupBox("Fog", this);
		auto fog_layout = new QFormLayout(fog_group);

		int index = SKY_FOG_COLOR;
		fog_layout->addRow(sky_color_names_map.at(index).c_str(), _current_lightning_colors_labels[index]);

		// TODO density

		index = SKY_FOG_DISTANCE;
		fog_layout->addRow("Fog Farclip", _current_lightning_floats_labels[index]);

		index = SKY_FOG_MULTIPLIER;
		fog_layout->addRow("Fog Nearclip", _current_lightning_floats_labels[index]);

		layout_column2->addWidget(fog_group);
	}

	// Sky group
	{
		QGroupBox* light_group = new QGroupBox("Sky", this);
		auto light_layout = new QFormLayout(light_group);

		int index = SKY_COLOR_TOP;
		light_layout->addRow(sky_color_names_map.at(index).c_str(), _current_lightning_colors_labels[index]);

		index = SKY_COLOR_MIDDLE;
		light_layout->addRow(sky_color_names_map.at(index).c_str(), _current_lightning_colors_labels[index]);

		index = SKY_COLOR_BAND1;
		light_layout->addRow(sky_color_names_map.at(index).c_str(), _current_lightning_colors_labels[index]);

		index = SKY_COLOR_BAND2;
		light_layout->addRow(sky_color_names_map.at(index).c_str(), _current_lightning_colors_labels[index]);

		index = SKY_COLOR_SMOG;
		light_layout->addRow(sky_color_names_map.at(index).c_str(), _current_lightning_colors_labels[index]);

		index = SUN_COLOR;
		light_layout->addRow(sky_color_names_map.at(index).c_str(), _current_lightning_colors_labels[index]);

		index = SUN_CLOUD_COLOR;
		light_layout->addRow(sky_color_names_map.at(index).c_str(), _current_lightning_colors_labels[index]);

		layout_column2->addWidget(light_group);
	}

	// Clouds group
	{
		QGroupBox* light_group = new QGroupBox("Clouds", this);
		auto light_layout = new QFormLayout(light_group);

		int index = CLOUD_EMISSIVE_COLOR;
		light_layout->addRow(sky_color_names_map.at(index).c_str(), _current_lightning_colors_labels[index]);

		index = CLOUD_LAYER1_AMBIENT_COLOR;
		light_layout->addRow(sky_color_names_map.at(index).c_str(), _current_lightning_colors_labels[index]);

		index = CLOUD_LAYER2_AMBIENT_COLOR;
		light_layout->addRow(sky_color_names_map.at(index).c_str(), _current_lightning_colors_labels[index]);

		// Cloud type (params.dbc)

		index = SKY_CELESTIAL_GLOW;
		light_layout->addRow(sky_float_values_names_map.at(index).c_str(), _current_lightning_floats_labels[index]);

		index = SKY_CLOUD_DENSITY;
		light_layout->addRow(sky_float_values_names_map.at(index).c_str(), _current_lightning_floats_labels[index]);

		layout_column3->addWidget(light_group);
	}

	// Water group
	{
		QGroupBox* light_group = new QGroupBox("Water", this);
		auto light_layout = new QFormLayout(light_group);

		int index = RIVER_COLOR_LIGHT;
		light_layout->addRow(sky_color_names_map.at(index).c_str(), _current_lightning_colors_labels[index]);
		light_layout->addRow("River Close Alpha", _river_shallow_alpha_label_label);

		index = RIVER_COLOR_DARK;
		light_layout->addRow(sky_color_names_map.at(index).c_str(), _current_lightning_colors_labels[index]);
		light_layout->addRow("River Far Alpha", _river_deep_alpha_label);

		index = OCEAN_COLOR_LIGHT;
		light_layout->addRow(sky_color_names_map.at(index).c_str(), _current_lightning_colors_labels[index]);
		light_layout->addRow("Ocean Close Alpha", _ocean_shallow_alpha_label);


		index = OCEAN_COLOR_DARK;
		light_layout->addRow(sky_color_names_map.at(index).c_str(), _current_lightning_colors_labels[index]);
		light_layout->addRow("Ocean Far Alpha", _ocean_deep_alpha_label);


		layout_column3->addWidget(light_group);
	}

	main_layout->addLayout(layout_column1);
	layout_column1->addStretch();
	main_layout->addLayout(layout_column2);
	layout_column2->addStretch();
	main_layout->addLayout(layout_column3);
	layout_column3->addStretch();

}
