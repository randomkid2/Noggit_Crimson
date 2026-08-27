// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/ui/GroundEffectsTool.hpp>

#include <noggit/Action.hpp>
#include <noggit/ActionManager.hpp>
#include <noggit/DBC.h>
#include <noggit/Log.h>
#include <noggit/MapChunk.h>
#include <noggit/MapTile.h>
#include <noggit/MapView.h>
#include <noggit/map_index.hpp>
#include <noggit/texture_set.hpp>
#include <noggit/ui/CurrentTexture.h>
#include <noggit/ui/FontAwesome.hpp>
#include <noggit/ui/GroundEffectSetEditor.hpp>
#include <noggit/ui/texturing_tool.hpp>
#include <noggit/ui/tools/PreviewRenderer/PreviewRenderer.hpp>
#include <noggit/ui/tools/UiCommon/ExtendedSlider.hpp>
#include <noggit/World.h>
#include <noggit/World.inl>

#include <glm/geometric.hpp>
#include <glm/vec2.hpp>

#include <QtCore/QFileInfo>
#include <QtCore/QSignalBlocker>
#include <QtCore/QStringList>
#include <QtCore/QTimer>
#include <QtGui/QIcon>
#include <QtGui/QPixmap>
#include <QtWidgets/QAbstractSpinBox>
#include <QtWidgets/QButtonGroup>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QRadioButton>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QVBoxLayout>

#include <cmath>
#include <string>

namespace
{
  // What an MCLY layer carries when it has no ground effect.
  //
  // layer_info::effectID defaults to this (texture_set.hpp:39) and so does wowdev's MCLY
  // documentation, and MapChunk::save writes whatever is in memory straight out, so a chunk Noggit
  // creates already puts 0xFFFFFFFF on disk. Every reader in this tree treats 0 and 0xFFFFFFFF
  // alike as "none" -- Selection.cpp:227 even prints 0xFFFFFFFF as 0 -- so the only thing that
  // matters is picking one and using it everywhere. This is that one.
  constexpr unsigned int GROUND_EFFECT_NONE = 0xFFFFFFFFu;

  // Chunks whose per-cell doodad mapping the placement overlay is allowed to recompute in one
  // refresh.
  //
  // TextureSet::updateDoodadMapping reads, per chunk, 8x8 cells x 8x8 alpha texels x up to three
  // alpha layers = 12,288 Alphamap::getAlpha calls, and getAlpha is an out-of-line call
  // (Alphamap.hpp:25). One full tile of 256 chunks is therefore 3,145,728 calls; this cap of 1,024
  // chunks is 12,582,912, four tiles' worth, and is what keeps a refresh triggered by the end of a
  // brush stroke from stalling the frame. Anything past the cap keeps the mapping that was read
  // from the ADT, which is exactly what the client would use, and the status line says so.
  constexpr std::size_t PLACEMENT_RECOMPUTE_BUDGET = 1024;

  // Rows the set list will build in one go.
  //
  // "Load all sets from DBC" can hand over every non-empty row in GroundEffectTexture.dbc. Colour
  // resolution is a hash lookup and does not care how many there are, but QListWidget building
  // tens of thousands of items does, and no one scrolls that far anyway -- the filter box is the
  // way to reach a set. When the cap bites, the status line says how many were hidden.
  constexpr int MAX_LIST_ROWS = 4000;

  // bg-relative colour for "this chunk carries the texture but no ground effect at all".
  //
  // info #6FAEDC from the palette, as linear 0..1: 111/255, 174/255, 220/255. Chosen because the
  // two states that already existed are a per-set random colour and pure red for "an effect id
  // that resolves to nothing", and a blue is the one hue those never land on by accident.
  constexpr float UNASSIGNED_R = 111.0f / 255.0f;
  constexpr float UNASSIGNED_G = 174.0f / 255.0f;
  constexpr float UNASSIGNED_B = 220.0f / 255.0f;

  // The texturing tool starts on this placeholder, which is never painted on real terrain. Every
  // scan here treats it as "no texture selected" rather than scanning for it and finding nothing.
  constexpr char const* NO_TEXTURE = "tileset\\generic\\black.blp";

  QString baseName(std::string const& path)
  {
    return QString::fromStdString(path).section('\\', -1).section('/', -1);
  }
}

namespace Noggit
{
    namespace Ui
    {
        GroundEffectsTool::GroundEffectsTool(texturing_tool* texturing_tool, MapView* map_view, QWidget* parent)
            : QWidget(parent, Qt::Window), _map_view(map_view), _texturing_tool(texturing_tool)
        {
            setWindowTitle("Ground Effects Tool");
            setMinimumSize(750, 600);
            setWindowFlags(Qt::Tool | Qt::WindowStaysOnTopHint);
            setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
            QHBoxLayout* main_layout = new QHBoxLayout(this);
            // Not parented on `this`: a QLayout constructed with a widget installs itself as that
            // widget's layout, and `this` already has main_layout. Qt answers that with a runtime
            // warning and drops one of them.
            QVBoxLayout* left_side_layout = new QVBoxLayout();
            QVBoxLayout* right_side_layout = new QVBoxLayout();
            main_layout->addLayout(left_side_layout);
            main_layout->addLayout(right_side_layout);

            // Render modes.
            {
                _render_group_box = new QGroupBox("Render Mode", this);
                _render_group_box->setCheckable(true);
                _render_group_box->setChecked(true);
                left_side_layout->addWidget(_render_group_box);

                auto render_layout(new QGridLayout(_render_group_box));

                _render_type_group = new QButtonGroup(_render_group_box);

                _render_active_sets = new QRadioButton("Effect Id/Set", this);
                _render_active_sets->setToolTip("Render all the loaded effect sets for this texture in matching colors");
                _render_type_group->addButton(_render_active_sets);
                render_layout->addWidget(_render_active_sets, 0, 0);

                // Restored. The shader path for this mode has always been complete
                // (terrain_frag.glsl:419-450) and TileRender uploads the stencil for every chunk
                // on load, because MapChunk's constructor already raises
                // ChunkUpdateFlags::DETAILDOODADS_EXCLUSION (MapChunk.cpp:39). The only thing
                // missing was the radio: the mode could previously be reached only as a side
                // effect of selecting the Exclusion brush, which also suppressed the other two
                // overlays.
                _render_exclusion_map = new QRadioButton("Doodads Disabled", this);
                _render_exclusion_map->setToolTip("Render chunk units where effect doodads are disabled as white, rest as black");
                _render_type_group->addButton(_render_exclusion_map);
                render_layout->addWidget(_render_exclusion_map, 0, 1);

                // If chunk contains Texture/Effect : Render as green or red if the effect layer is active or not.
                _render_placement_map = new QRadioButton("Selected Texture state", this);
                _render_placement_map->setToolTip("Render chunk unit as red if texture is present in the chunk and NOT the current \
active layer, render as green if it's active. \nThis defines which of the 4 textures' set is currently active,\
this is determined by which has the highest opacity.");
                _render_type_group->addButton(_render_placement_map);
                render_layout->addWidget(_render_placement_map, 1, 0);

                _render_active_sets->setChecked(true);

                // "Why is my grass patchy" is nearly always a chunk that carries the texture and
                // no effect id at all, and nothing in either fork made that state visible: it
                // rendered the same black as terrain that does not carry the texture. Giving it
                // its own colour turns a hunt into a glance.
                _chkbox_highlight_unassigned = new QCheckBox("Highlight chunks with no effect", this);
                _chkbox_highlight_unassigned->setChecked(true);
                _chkbox_highlight_unassigned->setToolTip
                  ("Paint chunks that use the selected texture but have no ground effect assigned in blue.");
                render_layout->addWidget(_chkbox_highlight_unassigned, 1, 1);

                _coverage_label = new QLabel("Select a texture, then scan.", this);
                _coverage_label->setWordWrap(true);
                render_layout->addWidget(_coverage_label, 2, 0, 1, 2);
            }

            _chkbox_merge_duplicates = new QCheckBox("Ignore duplicates", this);
            _chkbox_merge_duplicates->setChecked(true);
            _chkbox_merge_duplicates->setToolTip
              ("Collapse sets that differ only by id -- same doodads, weights, density and terrain "
               "type -- into one entry and one colour.");
            left_side_layout->addWidget(_chkbox_merge_duplicates);

            auto button_scan_adt = new QPushButton("Scan for sets in curr tile", this);
            left_side_layout->addWidget(button_scan_adt);

            auto button_scan_adt_loaded = new QPushButton("Scan for sets in loaded Tiles", this);
            left_side_layout->addWidget(button_scan_adt_loaded);

            auto button_load_dbc = new QPushButton("Load all sets from DBC", this);
            button_load_dbc->setToolTip
              ("List every non-empty row of GroundEffectTexture.dbc, not just the ones already used "
               "by this texture. Use the filter below to find one.");
            left_side_layout->addWidget(button_load_dbc);

            // Selection.
            auto selection_group = new QGroupBox("Effect Set Selection", this);
            selection_group->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Minimum);
            left_side_layout->addWidget(selection_group);
            auto selection_layout(new QVBoxLayout(selection_group));

            // The one control that matters most once there is more than a handful of sets. It
            // matches against the whole rendered label, which is id + every doodad file name +
            // the zone the set was found in, so "zuuldaia", "stlgra" and "820" all reach the same
            // row.
            _set_filter = new QLineEdit(this);
            _set_filter->setPlaceholderText("Filter by id, doodad name or zone...");
            _set_filter->setClearButtonEnabled(true);
            selection_layout->addWidget(_set_filter);

            _effect_sets_list = new QListWidget(this);
            selection_layout->addWidget(_effect_sets_list);
            _effect_sets_list->setViewMode(QListView::ListMode);
            _effect_sets_list->setSelectionMode(QAbstractItemView::SelectionMode::SingleSelection);
            _effect_sets_list->setSelectionBehavior(QAbstractItemView::SelectItems);
            _effect_sets_list->setUniformItemSizes(true);
            _effect_sets_list->setFixedHeight(160);
            _effect_sets_list->setIconSize(QSize(20, 20));

            // Replaces four controls that were created and never connected to anything: "Create
            // New", a "Save Set" button, an "Apply to Texture" button and a one-item combo box.
            // Authoring and bulk apply live in GroundEffectSetEditor, which actually writes the
            // DBCs and verifies the bytes landed; duplicating half of it here is what left seven
            // inert buttons on this panel.
            auto button_edit_sets = new QPushButton("Create / edit sets...", this);
            button_edit_sets->setToolTip
              ("Open the Ground Effect Sets editor on the selected set, where doodads, weights and "
               "density can be changed and saved, and a set can be applied in bulk.");
            selection_layout->addWidget(button_edit_sets);

            // Effect settings.
            {
                auto settings_group = new QGroupBox("Selected Set Settings", this);
                settings_group->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Minimum);
                right_side_layout->addWidget(settings_group);
                auto settings_layout(new QFormLayout(settings_group));

                auto set_help_label = new QLabel("A ground Effect Set contains up to 4 different doodads.\n"
                                                 "Terrain Type is used for footprints and sounds.\n"
                                                 "This panel shows the selected set; edit it with Create / edit sets.");
                settings_layout->addRow(set_help_label);

                _object_list = new QListWidget(this);
                _object_list->setItemAlignment(Qt::AlignCenter);
                _object_list->setViewMode(QListView::IconMode);
                _object_list->setWrapping(false);
                _object_list->setIconSize(QSize(100, 100));
                _object_list->setFlow(QListWidget::LeftToRight);
                _object_list->setSelectionMode(QAbstractItemView::NoSelection);
                _object_list->setAcceptDrops(false);
                _object_list->setMovement(QListView::Movement::Static);
                _object_list->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Minimum);
                _object_list->setFixedWidth(_object_list->iconSize().width() * 4 + 40); //  padding-right: 10px * 4
                _object_list->setFixedHeight(_object_list->iconSize().height() + 20);

                settings_layout->addRow(_object_list);
                for (int i = 0; i < 4; i++)
                {
                    QListWidgetItem* list_item = new QListWidgetItem(_object_list);
                    list_item->setFlags(Qt::ItemIsEnabled);
                    list_item->setIcon(Noggit::Ui::FontAwesomeIcon(Noggit::Ui::FontAwesome::plus));
                    list_item->setText(STRING_EMPTY_DISPLAY);
                    list_item->setToolTip("");
                }

                _weight_list = new QListWidget(this);
                _weight_list->setItemAlignment(Qt::AlignLeft | Qt::AlignTop);
                _weight_list->setFlow(QListWidget::LeftToRight);
                _weight_list->setMovement(QListView::Movement::Static);
                _weight_list->setSelectionMode(QAbstractItemView::NoSelection);
                _weight_list->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Minimum);
                _weight_list->setMinimumWidth(450);
                _weight_list->setFixedHeight(120);
                _weight_list->setVisible(true);
                // Was an inline "border: 1px solid darkGray" -- a Qt colour NAME, so #A9A9A9,
                // which is unrelated to any theme's stroke and lit up as four bright cells in a
                // dark panel. The accessible name is the hook the sheet reaches it by; it is
                // free of side effects on a list that already takes NoSelection, and it leaves
                // the colour where a theme can answer for it.
                _weight_list->setAccessibleName("ge_weight_list");
                settings_layout->addRow(_weight_list);

                // The list itself was created, styled and then never given a single item, so it
                // drew as an empty 450x120 well. Four cells now carry the weight the DBC actually
                // holds and, next to it, what that weight means: a weight is only meaningful
                // against the sum of the other three, and "3" tells you nothing while "3 -> 37.5%"
                // tells you how often that model wins the draw.
                for (int i = 0; i < 4; ++i)
                {
                    auto cell(new QWidget(_weight_list));
                    auto cell_layout(new QVBoxLayout(cell));
                    cell_layout->setContentsMargins(4, 4, 4, 4);
                    cell_layout->setSpacing(2);

                    cell_layout->addWidget(new QLabel(QString("Slot %1").arg(i + 1), cell));

                    _weight_spin[i] = new QSpinBox(cell);
                    _weight_spin[i]->setRange(0, 255);
                    _weight_spin[i]->setValue(1);
                    _weight_spin[i]->setReadOnly(true);
                    _weight_spin[i]->setButtonSymbols(QAbstractSpinBox::NoButtons);
                    _weight_spin[i]->setToolTip("Relative chance this slot is picked. Edit it in Create / edit sets.");
                    cell_layout->addWidget(_weight_spin[i]);

                    _weight_percent[i] = new QLabel("--", cell);
                    cell_layout->addWidget(_weight_percent[i]);

                    auto item(new QListWidgetItem(_weight_list));
                    item->setFlags(Qt::NoItemFlags);
                    item->setSizeHint(QSize(104, 92));
                    _weight_list->setItemWidget(item, cell);
                }

                _preview_renderer = new Tools::PreviewRenderer(_object_list->iconSize().width(),
                    _object_list->iconSize().height(),
                    Noggit::NoggitRenderContext::GROUND_EFFECT_PREVIEW, this);
                _preview_renderer->setVisible(false);
                // Initialize renderer.
                _preview_renderer->setModelOffscreen("world/wmo/azeroth/buildings/human_farm/farm.wmo");
                _preview_renderer->renderToPixmap();

                // Density: 0 -> 8. > 24 -> 24. This value is for the amount of doodads and on higher values for coverage.
                // Till an amount of around 24 it just increases the amount. After this the doodads begin to group.
                // In WOTLK, only 4 entries out of 25k use more than 20. In retail only 5 use more than 25. 16 or less seems standard
                _spinbox_doodads_amount = new QSpinBox(this);
                _spinbox_doodads_amount->setRange(0, 255);
                _spinbox_doodads_amount->setValue(8);
                _spinbox_doodads_amount->setReadOnly(true);
                _spinbox_doodads_amount->setButtonSymbols(QAbstractSpinBox::NoButtons);
                // Named apart from weight on purpose. The four-slot weight UI conflates the two,
                // and they are different questions: density is how many doodads a cell gets, weight
                // is which of the four models each one turns out to be.
                _spinbox_doodads_amount->setToolTip
                  ("How many doodads a chunk cell gets. Independent of the per-slot weights, which "
                   "only decide which model is drawn.");
                settings_layout->addRow("Density (doodads per cell) : ", _spinbox_doodads_amount);

                _cbbox_terrain_type = new QComboBox(this);
                _cbbox_terrain_type->setToolTip("Drives footstep sounds and the footprint the client leaves on this ground.");
                _cbbox_terrain_type->setEnabled(false);
                settings_layout->addRow("Terrain Type", _cbbox_terrain_type);

                // Two bugs lived in the three lines this replaces. setItemData was passed
                // count(), which is already one past the item just added, so no row ever carried
                // its terrain id; and setActiveGroundEffect then used the DBC id as a row index.
                // TerrainType ids are not dense and not zero-based, so the combo showed an
                // unrelated terrain for nearly every set. addItem(text, data) cannot get the index
                // wrong, and findData is the matching read.
                _cbbox_terrain_type->addItem("0  (none)", QVariant(0u));

                for (auto it = gTerrainTypeDB.begin(); it != gTerrainTypeDB.end(); ++it)
                {
                    unsigned int const terrain_id = it->getUInt(TerrainTypeDB::TerrainId);

                    _cbbox_terrain_type->addItem
                      (QString("%1  %2").arg(terrain_id).arg(it->getString(TerrainTypeDB::TerrainDesc))
                      , QVariant(terrain_id));
                }
            }

            right_side_layout->addSpacerItem(new QSpacerItem(20, 40, QSizePolicy::Minimum, QSizePolicy::Expanding));

            // Brush modes.
            {
                _brush_grup_box = new QGroupBox("Brush Mode", this);
                _brush_grup_box->setCheckable(true);
                _brush_grup_box->setChecked(false);
                left_side_layout->addWidget(_brush_grup_box);

                QVBoxLayout* brush_layout = new QVBoxLayout(_brush_grup_box);

                QHBoxLayout* brush_buttons_layout = new QHBoxLayout();
                brush_layout->addLayout(brush_buttons_layout);
                _brush_type_group = new QButtonGroup(_brush_grup_box);

                _paint_effect = new QRadioButton("Paint Effect", this);
                _paint_effect->setToolTip
                  ("Shift+LMB assigns the selected set to the layer that uses the selected texture, "
                   "on every chunk under the brush. Ctrl+LMB clears it again.\n"
                   "An effect id lives on the MCLY layer, so this is per chunk -- there is no "
                   "sub-chunk resolution to paint at.");
                _brush_type_group->addButton(_paint_effect);
                brush_buttons_layout->addWidget(_paint_effect);

                _paint_exclusion = new QRadioButton("Paint Exclusion", this);
                _paint_exclusion->setToolTip
                  ("Shift+LMB switches detail doodads off for the 8x8 cells under the brush. "
                   "Ctrl+LMB switches them back on.");
                _brush_type_group->addButton(_paint_exclusion);
                brush_buttons_layout->addWidget(_paint_exclusion);

                _paint_effect->setChecked(true);
                _paint_effect->setAutoExclusive(true);

                brush_layout->addWidget(new QLabel("Radius:", _brush_grup_box));
                _effect_radius_slider = new Noggit::Ui::Tools::UiCommon::ExtendedSlider(_brush_grup_box);
                _effect_radius_slider->setPrefix("");
                _effect_radius_slider->setRange(0, 1000);
                _effect_radius_slider->setDecimals(2);
                _effect_radius_slider->setValue(_texturing_tool->texture_brush().getRadius());
                brush_layout->addWidget(_effect_radius_slider);
            }
            left_side_layout->addSpacerItem(new QSpacerItem(20, 40, QSizePolicy::Minimum, QSizePolicy::Expanding));

            // Holding Ctrl+Z emits currentActionChanged once per undone action; without a
            // coalescer each of them would rescan every loaded tile. 120 ms is under the ~200 ms
            // at which a redraw stops feeling like a response to what you just did, and long
            // enough that a burst collapses into one pass.
            _refresh_timer = new QTimer(this);
            _refresh_timer->setSingleShot(true);
            _refresh_timer->setInterval(120);
            connect(_refresh_timer, &QTimer::timeout, this, [this] { refreshOverlay(); });

            connect(_render_group_box, &QGroupBox::clicked,
                [this](bool)
                {
                    updateTerrainUniformParams();
                    _refresh_timer->start();
                });

            auto const on_render_mode_changed = [this](bool)
                {
                    updateTerrainUniformParams();
                    // The placement overlay needs per-cell data the other two do not, so switching
                    // to it has to recompute rather than just flipping a uniform.
                    _refresh_timer->start();
                };

            connect(_render_active_sets, &QRadioButton::clicked, this, on_render_mode_changed);
            connect(_render_placement_map, &QRadioButton::clicked, this, on_render_mode_changed);
            connect(_render_exclusion_map, &QRadioButton::clicked, this, on_render_mode_changed);

            connect(_chkbox_highlight_unassigned, &QCheckBox::toggled, this, [this](bool) { refreshOverlay(); });

            // Was created with no connect at all, so toggling it changed nothing until the next
            // manual rescan -- and the checkbox is right above the Scan buttons, which is exactly
            // where you would expect it to take effect on its own.
            connect(_chkbox_merge_duplicates, &QCheckBox::toggled, this, [this](bool)
                {
                    rebuildEffectColors();
                    updateSetsList();
                });

            connect(_brush_grup_box, &QGroupBox::clicked,
                [this](bool)
                {
                    updateTerrainUniformParams();
                });

            connect(_paint_effect, &QRadioButton::clicked,
                [this](bool)
                {
                    updateTerrainUniformParams();
                });

            connect(_paint_exclusion, &QRadioButton::clicked,
                 [this](bool checked)
                 {
                     // A convenience, not a gate. The old code made the exclusion overlay come on
                     // implicitly whenever this brush was selected AND suppressed the other two
                     // overlays while it was, which meant the render radios silently stopped
                     // meaning anything. Moving the selection makes the coupling visible and
                     // leaves the user free to switch straight back.
                     if (checked && render_mode())
                     {
                         _render_exclusion_map->setChecked(true);
                     }
                     updateTerrainUniformParams();
                 });

            connect(_set_filter, &QLineEdit::textChanged, this, [this](QString const&) { updateSetsList(); });

            // Get list of ground effect id this texture uses in this ADT.
            connect(button_scan_adt, &QPushButton::clicked
                , [this]()
                {
                    _loaded_effects.clear();
                    scanTileForEffects(TileIndex(_map_view->getCamera()->position));
                    rebuildEffectColors();
                    updateSetsList();
                    refreshOverlay();
                }
            );

            connect(button_scan_adt_loaded, &QPushButton::clicked
                , [this]()
                {
                    _loaded_effects.clear();

                    if (World* world = _map_view->getWorld())
                    {
                        for (MapTile* tile : world->mapIndex.loaded_tiles())
                        {
                            scanTileForEffects(TileIndex(tile->index));
                        }
                    }

                    rebuildEffectColors();
                    updateSetsList();
                    refreshOverlay();
                }
            );

            connect(button_load_dbc, &QPushButton::clicked, this, [this]() { loadAllSetsFromDbc(); });

            connect(button_edit_sets, &QPushButton::clicked, this, [this]() { openSetEditor(); });

            QObject::connect(_effect_sets_list, &QListWidget::itemSelectionChanged, [this]()
              {
                    auto effect = getSelectedGroundEffect();
                    if (!effect.has_value())
                    {
                        return;
                    }
                    setActiveGroundEffect(effect.value());
                });

            // The live update the competing fork is judged on, and the reason it is wired to the
            // action manager rather than to a poll: an edit that changes what the overlay shows
            // always closes an action, and Action::getFlags says which kind. A texture stroke that
            // adds a layer, a bulk apply from the set editor and an exclusion stroke all land
            // here, and so does every undo and redo, so the viewport follows the terrain without
            // anything having to be pressed.
            connect(NOGGIT_ACTION_MGR, &Noggit::ActionManager::onActionEnd, this
                   , [this](Noggit::Action* action)
                {
                    if (!previewActive() || !action)
                    {
                        return;
                    }

                    int const watched = ActionFlags::eCHUNKS_TEXTURE
                                      | ActionFlags::eCHUNKS_LAYERINFO
                                      | ActionFlags::eCHUNK_DOODADS_EXCLUSION;

                    if (action->getFlags() & watched)
                    {
                        _refresh_timer->start();
                    }
                });

            connect(NOGGIT_ACTION_MGR, &Noggit::ActionManager::currentActionChanged, this
                   , [this](unsigned)
                {
                    if (previewActive())
                    {
                        _refresh_timer->start();
                    }
                });
        }

        void GroundEffectsTool::updateTerrainUniformParams()
        {
            World* world = _map_view->getWorld();

            if (!world)
            {
                return;
            }

            auto* block = world->renderer()->getTerrainParamsUniformBlock();

            if (block->draw_groundeffectid_overlay != render_active_sets_overlay())
            {
                block->draw_groundeffectid_overlay = render_active_sets_overlay();
                world->renderer()->markTerrainParamsUniformBlockDirty();
            }
            if (block->draw_groundeffect_layerid_overlay != render_placement_map_overlay())
            {
                block->draw_groundeffect_layerid_overlay = render_placement_map_overlay();
                world->renderer()->markTerrainParamsUniformBlockDirty();
            }
            if (block->draw_noeffectdoodad_overlay != render_exclusion_map_overlay())
            {
                block->draw_noeffectdoodad_overlay = render_exclusion_map_overlay();
                world->renderer()->markTerrainParamsUniformBlockDirty();
            }
        }

        std::string GroundEffectsTool::activeTexture() const
        {
            std::string texture = _texturing_tool->_current_texture->filename();

            if (texture == NO_TEXTURE)
            {
                return std::string();
            }

            return texture;
        }

        void GroundEffectsTool::scanTileForEffects(TileIndex tile_index)
        {
            std::string const active_texture = activeTexture();

            if (active_texture.empty())
            {
                return;
            }

            World* world = _map_view->getWorld();

            if (!world)
            {
                return;
            }

            // MapIndex::getTile returns nullptr for an invalid or never-loaded tile
            // (map_index.cpp:614-616) and the old code dereferenced it straight away, so pressing
            // "Scan for sets in curr tile" with the camera off the loaded set was a null
            // dereference, not an empty result.
            MapTile* tile = world->mapIndex.getTile(tile_index);

            if (!tile || !tile->finishedLoading())
            {
                return;
            }

            for (int x = 0; x < 16; x++)
            {
                for (int y = 0; y < 16; y++)
                {
                    MapChunk* chunk = tile->getChunk(static_cast<unsigned>(x), static_cast<unsigned>(y));

                    if (!chunk || !chunk->texture_set)
                    {
                        continue;
                    }

                    for (std::size_t layer_id = 0; layer_id < chunk->getTextureSet()->num(); layer_id++)
                    {
                        auto const& texture_name = chunk->getTextureSet()->filename(layer_id);

                        if (texture_name != active_texture)
                        {
                            continue;
                        }

                        unsigned int const effect_id = chunk->getTextureSet()->getEffectForLayer(layer_id);

                        if (!effect_id || effect_id == GROUND_EFFECT_NONE)
                        {
                            continue;
                        }

                        ground_effect_set ground_effect;

                        if (_ground_effect_cache.contains(effect_id))
                        {
                            ground_effect = _ground_effect_cache.at(effect_id);
                        }
                        else
                        {
                            ground_effect.load_from_id(effect_id);
                            _ground_effect_cache[effect_id] = ground_effect;
                        }

                        if (ground_effect.empty())
                            continue;

                        bool is_duplicate = false;

                        for (std::size_t i = 0; i < _loaded_effects.size(); i++)
                        {
                            auto effect_set = &_loaded_effects[i];

                            // always filter identical ids
                            if (effect_id == effect_set->ID
                                || (_chkbox_merge_duplicates->isChecked() && ground_effect == effect_set))
                            {
                                is_duplicate = true;
                                break;
                            }
                        }

                        if (!is_duplicate)
                        {
                            _loaded_effects.push_back(ground_effect);
                            // Area is probably useless if we merge since duplicates are per area.
                            _loaded_effects.back().Zone = gAreaDB.getAreaFullName(chunk->getAreaID());
                        }
                    }
                }
            }
        }

        void GroundEffectsTool::loadAllSetsFromDbc()
        {
            _loaded_effects.clear();

            std::size_t skipped_empty = 0;
            std::size_t collapsed_duplicates = 0;

            // "Ignore duplicates" has to mean the same thing here as it does after a scan, or the
            // checkbox would collapse the list one way round and not the other. GroundEffectTexture
            // carries a great many rows that differ only by id because a set was re-authored per
            // area, and a signature map collapses them in one pass instead of the O(n^2) deep
            // compare the scan path used to do.
            bool const merge_duplicates = _chkbox_merge_duplicates->isChecked();
            std::unordered_map<std::string, int> seen_signature;

            for (DBCFile::Iterator it = gGroundEffectTextureDB.begin();
                 it != gGroundEffectTextureDB.end(); ++it)
            {
                unsigned int const effect_id = it->getUInt(GroundEffectTextureDB::ID);

                if (!effect_id || effect_id == GROUND_EFFECT_NONE)
                {
                    continue;
                }

                ground_effect_set set;

                if (_ground_effect_cache.contains(effect_id))
                {
                    set = _ground_effect_cache.at(effect_id);
                }
                else
                {
                    set.load_from_id(effect_id);
                    _ground_effect_cache[effect_id] = set;
                }

                // A set with no doodad in any of its four slots places nothing, so listing it is
                // pure noise. GroundEffectTexture.dbc is mostly these.
                if (set.empty()
                 || (set.Doodads[0].empty() && set.Doodads[1].empty()
                  && set.Doodads[2].empty() && set.Doodads[3].empty()))
                {
                    ++skipped_empty;
                    continue;
                }

                if (merge_duplicates
                 && !seen_signature.emplace(set.signature(), static_cast<int>(_loaded_effects.size())).second)
                {
                    ++collapsed_duplicates;
                    continue;
                }

                _loaded_effects.push_back(set);
            }

            rebuildEffectColors();
            updateSetsList();
            refreshOverlay();

            QString message
              ( QString("%1 non-empty set(s) loaded from GroundEffectTexture.dbc, %2 empty row(s) skipped")
                  .arg(_loaded_effects.size()).arg(skipped_empty));

            if (collapsed_duplicates)
            {
                message += QString(", %1 collapsed as duplicates").arg(collapsed_duplicates);
            }

            // Appended rather than assigned: refreshOverlay has just written the coverage counts
            // into the same label, and those are the numbers that answer "did this help", so
            // replacing them with the load counts would throw away the more useful half.
            _coverage_label->setText(_coverage_label->text() + "  " + message + ".");
        }

        void GroundEffectsTool::indexEffect(unsigned int effect_id, ground_effect_set const& effect, int list_index)
        {
            _color_index_by_id[effect_id] = list_index;

            if (_chkbox_merge_duplicates->isChecked())
            {
                _index_by_signature.emplace(effect.signature(), list_index);
            }
        }

        void GroundEffectsTool::rebuildEffectColors()
        {
            _effects_colors.clear();
            _color_index_by_id.clear();
            _index_by_signature.clear();

            int color_count = 1;

            for (auto& effect : _loaded_effects)
            {
                // Same formula as in the shader.
                float partr, partg, partb;
                float r = std::modf(static_cast<float>(std::sin(glm::dot(glm::vec2(color_count), glm::vec2(12.9898, 78.233))) * 43758.5453), &partr);
                float g = std::modf(static_cast<float>(std::sin(glm::dot(glm::vec2(color_count), glm::vec2(11.5591, 70.233))) * 43569.5451), &partg);
                // Was &partg as well, so the whole-number part of the blue term was written into
                // green's scratch variable. Harmless by luck -- both are discarded -- but it left
                // partb unused and the line reading as if green and blue shared a source.
                float b = std::modf(static_cast<float>(std::sin(glm::dot(glm::vec2(color_count), glm::vec2(13.1234, 76.234))) * 43765.5452), &partb);
                color_count++;
                _effects_colors.push_back(glm::vec3(r, g, b));
            }

            for (std::size_t i = 0; i < _loaded_effects.size(); ++i)
            {
                indexEffect(_loaded_effects[i].ID, _loaded_effects[i], static_cast<int>(i));
            }
        }

        QString GroundEffectsTool::setLabel(ground_effect_set const& effect) const
        {
            QStringList names;

            for (int i = 0; i < 4; ++i)
            {
                if (!effect.Doodads[i].filename.empty())
                {
                    names << baseName(effect.Doodads[i].filename);
                }
            }

            QString label (QString::number(effect.ID));

            if (!names.isEmpty())
            {
                label += " - " + names.join(", ");
            }

            if (!effect.Zone.empty())
            {
                label += " - " + QString::fromStdString(effect.Zone);
            }

            return label;
        }

        void GroundEffectsTool::updateSetsList()
        {
            // Blocked so clearing and refilling does not fire itemSelectionChanged once per
            // intermediate state and run setActiveGroundEffect against a half-built list.
            QSignalBlocker const blocker (_effect_sets_list);

            _effect_sets_list->clear();

            QString const needle (_set_filter->text().trimmed().toLower());

            int shown = 0;
            int hidden_by_cap = 0;

            for (std::size_t i = 0; i < _loaded_effects.size(); ++i)
            {
                QString const label (setLabel(_loaded_effects[i]));

                if (!needle.isEmpty() && !label.toLower().contains(needle))
                {
                    continue;
                }

                if (shown >= MAX_LIST_ROWS)
                {
                    ++hidden_by_cap;
                    continue;
                }

                QColor const color
                  ( i < _effects_colors.size()
                      ? QColor::fromRgbF(_effects_colors[i].r, _effects_colors[i].g, _effects_colors[i].b)
                      : QColor(Qt::black));

                QListWidgetItem* list_item = new QListWidgetItem(label);
                // The row number stops being the _loaded_effects index the moment the filter hides
                // anything, so the index travels with the item instead of being inferred from
                // where it landed.
                list_item->setData(Qt::UserRole, static_cast<int>(i));
                list_item->setBackgroundColor(color);
                _effect_sets_list->addItem(list_item);

                // Built at DEVICE resolution and told its ratio, like every other bitmap that
                // feeds a widget in this tree. iconSize() is LOGICAL -- 20x20, set at line 214
                // -- and QPixmapIconEngine never magnifies an entry it was handed: qicon.cpp
                // rescales only when the stored entry is LARGER than the request, so a ratio-1
                // 20x20 pixmap answered this view's 40x40 device request with 20x20 and left
                // the painter to stretch it.
                //
                // A flat fill survives that stretch unharmed, so this one is the only find in
                // the sweep with no visible symptom today -- it is fixed because it stops being
                // harmless the moment the swatch gains a border or a glyph, and because leaving
                // one ratio-1 pixmap behind invites the next one.
                qreal const ratio(_effect_sets_list->devicePixelRatioF());

                QPixmap pixmap(_effect_sets_list->iconSize() * ratio);
                pixmap.setDevicePixelRatio(ratio);
                pixmap.fill(color);
                list_item->setIcon(QIcon(pixmap));

                ++shown;
            }

            // itemAt takes viewport COORDINATES, not a row, so itemAt(0, 0) was asking for
            // whatever happens to be drawn at the top-left pixel -- null whenever the list is
            // scrolled, which then silently skipped selecting anything.
            if (_effect_sets_list->count())
            {
                _effect_sets_list->setCurrentItem(_effect_sets_list->item(0));

                auto effect = getSelectedGroundEffect();

                if (effect.has_value())
                {
                    setActiveGroundEffect(effect.value());
                }
            }

            if (hidden_by_cap)
            {
                _coverage_label->setText
                  ( QString("Showing the first %1 of %2 matching set(s). Narrow the filter to reach the rest.")
                      .arg(MAX_LIST_ROWS).arg(shown + hidden_by_cap));
            }
        }

        void GroundEffectsTool::refreshOverlay()
        {
            World* world = _map_view->getWorld();

            if (!world)
            {
                return;
            }

            std::string const active_texture = activeTexture();

            if (active_texture.empty())
            {
                _coverage_label->setText("No texture selected. Pick one in the Texturing tool.");
                return;
            }

            bool const highlight_unassigned = _chkbox_highlight_unassigned->isChecked();
            bool const want_placement = _render_placement_map->isChecked() && render_mode();
            bool const merge_duplicates = _chkbox_merge_duplicates->isChecked();

            std::size_t tiles = 0;
            std::size_t chunks_with_texture = 0;
            std::size_t chunks_with_effect = 0;
            std::size_t chunks_without_effect = 0;
            std::size_t chunks_unresolved = 0;
            std::size_t placement_recomputed = 0;
            std::size_t placement_over_budget = 0;

            for (MapTile* tile : world->mapIndex.loaded_tiles())
            {
                ++tiles;
                tile->renderer()->setActiveRenderGEffectTexture(active_texture);

                // The mapping read from the ADT is right until this session edits the alpha maps.
                // MapTile::changed is the flag the save path itself uses to mean "edited since
                // load" (map_index.cpp:555), so it is the cheapest honest answer to "is the stored
                // mapping stale".
                bool const mapping_may_be_stale = tile->changed.load();

                for (int x = 0; x < 16; x++)
                {
                    for (int y = 0; y < 16; y++)
                    {
                        MapChunk* chunk = tile->getChunk(static_cast<unsigned>(x), static_cast<unsigned>(y));

                        if (!chunk || !chunk->texture_set)
                        {
                            continue;
                        }

                        int const chunk_index = chunk->px * 16 + chunk->py;

                        // Black is what the placement shader reads as "this chunk does not carry
                        // the selected texture" (terrain_frag.glsl:380), so the reset matters to
                        // both overlays and not only to the colour one.
                        tile->renderer()->setChunkGroundEffectColor(chunk_index, glm::vec3(0.0, 0.0, 0.0));

                        int layer_with_texture = -1;

                        for (std::size_t layer_id = 0; layer_id < chunk->getTextureSet()->num(); layer_id++)
                        {
                            if (chunk->getTextureSet()->filename(layer_id) == active_texture)
                            {
                                layer_with_texture = static_cast<int>(layer_id);
                                break;
                            }
                        }

                        if (layer_with_texture < 0)
                        {
                            continue;
                        }

                        ++chunks_with_texture;

                        unsigned int const effect_id
                          = chunk->getTextureSet()->getEffectForLayer(static_cast<std::size_t>(layer_with_texture));

                        if (!effect_id || effect_id == GROUND_EFFECT_NONE)
                        {
                            ++chunks_without_effect;

                            if (highlight_unassigned)
                            {
                                tile->renderer()->setChunkGroundEffectColor
                                  (chunk_index, glm::vec3(UNASSIGNED_R, UNASSIGNED_G, UNASSIGNED_B));
                            }
                        }
                        else
                        {
                            ++chunks_with_effect;

                            int list_index = -1;

                            auto const by_id = _color_index_by_id.find(effect_id);

                            if (by_id != _color_index_by_id.end())
                            {
                                list_index = by_id->second;
                            }
                            else if (merge_duplicates)
                            {
                                // Not in the list under its own id, but it may be the same effect
                                // wearing a different one. This used to be a deep compare against
                                // every loaded set for every chunk; the signature map answers it
                                // in one lookup, and the answer is cached back onto the id so the
                                // next chunk carrying it costs nothing at all.
                                ground_effect_set set;

                                if (_ground_effect_cache.contains(effect_id))
                                {
                                    set = _ground_effect_cache.at(effect_id);
                                }
                                else
                                {
                                    set.load_from_id(effect_id);
                                    _ground_effect_cache[effect_id] = set;
                                }

                                auto const by_signature = _index_by_signature.find(set.signature());

                                if (by_signature != _index_by_signature.end())
                                {
                                    list_index = by_signature->second;
                                }

                                _color_index_by_id[effect_id] = list_index;
                            }
                            else
                            {
                                _color_index_by_id[effect_id] = -1;
                            }

                            if (list_index >= 0 && static_cast<std::size_t>(list_index) < _effects_colors.size())
                            {
                                tile->renderer()->setChunkGroundEffectColor(chunk_index, _effects_colors[list_index]);
                            }
                            else
                            {
                                // An effect id that is on the terrain but not among the loaded
                                // sets. Pure red, as before.
                                ++chunks_unresolved;
                                tile->renderer()->setChunkGroundEffectColor(chunk_index, glm::vec3(1.0, 0.0, 0.0));
                            }
                        }

                        if (want_placement)
                        {
                            if (mapping_may_be_stale)
                            {
                                if (placement_recomputed < PLACEMENT_RECOMPUTE_BUDGET)
                                {
                                    // Safe to run here and only here: it reads the alpha maps
                                    // directly and its own note says the temporary alpha maps have
                                    // to be applied first, which is true once a stroke's action has
                                    // closed and not while one is in flight. It is also what
                                    // MapChunk::save does before writing (MapChunk.cpp:1516), so
                                    // recomputing early cannot change what lands on disk.
                                    chunk->getTextureSet()->updateDoodadMapping();
                                    ++placement_recomputed;
                                }
                                else
                                {
                                    ++placement_over_budget;
                                }
                            }

                            tile->renderer()->setChunkGroundEffectActiveData(chunk);
                        }
                    }
                }
            }

            // The producer of the per-cell "is this the winning layer" bits had zero live callers
            // -- every one of them was commented out -- so the Selected Texture state mode read
            // uniformly zero and painted every chunk carrying the texture solid red. It is called
            // above now, which is what makes that mode mean anything.

            QString summary
              ( QString("%1 loaded tile(s). Texture on %2 chunk(s): %3 with an effect, %4 with none")
                  .arg(tiles).arg(chunks_with_texture).arg(chunks_with_effect).arg(chunks_without_effect));

            if (chunks_unresolved)
            {
                summary += QString(", %1 pointing at a set that is not listed (red)").arg(chunks_unresolved);
            }

            summary += ".";

            if (placement_over_budget)
            {
                summary += QString(" %1 chunk(s) kept the doodad mapping stored in the ADT; the "
                                   "per-refresh recompute budget of %2 was reached.")
                             .arg(placement_over_budget).arg(PLACEMENT_RECOMPUTE_BUDGET);
            }

            _coverage_label->setText(summary);
        }

        void GroundEffectsTool::TextureChanged()
        {
            _loaded_effects.clear();
            _ground_effect_cache.clear();
            _color_index_by_id.clear();
            _index_by_signature.clear();
            _effects_colors.clear();

            updateSetsList();
            refreshOverlay();

            _mirrored_set_id = 0;
            _spinbox_doodads_amount->setValue(8);
            _cbbox_terrain_type->setCurrentIndex(0);

            for (int i = 0; i < 4; i++)
            {
                _weight_spin[i]->setValue(1);
                _weight_percent[i]->setText("--");
                _object_list->item(i)->setText(STRING_EMPTY_DISPLAY);
                updateDoodadPreviewRender(i);
            }
        }

        bool GroundEffectsTool::previewActive() const
        {
          return _preview_armed;
        }

        bool GroundEffectsTool::render_active_sets_overlay() const
        {
          return previewActive() && _render_active_sets->isChecked() && render_mode();
        }

        bool GroundEffectsTool::render_placement_map_overlay() const
        {
          return previewActive() && _render_placement_map->isChecked() && render_mode();
        }

        bool GroundEffectsTool::render_exclusion_map_overlay() const
        {
          return previewActive() && _render_exclusion_map->isChecked() && render_mode();
        }

        void GroundEffectsTool::change_radius(float change)
        {
          _effect_radius_slider->setValue(static_cast<float>(_effect_radius_slider->value()) + change);
        }

        void GroundEffectsTool::clearOverlayUniforms()
        {
          World* world = _map_view->_world ? _map_view->getWorld() : nullptr;

          if (!world)
          {
            return;
          }

          world->renderer()->getTerrainParamsUniformBlock()->draw_groundeffectid_overlay = false;
          world->renderer()->getTerrainParamsUniformBlock()->draw_groundeffect_layerid_overlay = false;
          world->renderer()->getTerrainParamsUniformBlock()->draw_noeffectdoodad_overlay = false;
          world->renderer()->markTerrainParamsUniformBlockDirty();
        }

        bool GroundEffectsTool::hiddenByWindowManager() const
        {
          if (isMinimized())
          {
            return true;
          }

          // This window is a Qt::Tool owned by the window the texturing dock lives in
          // (texturing_tool.cpp:240). Windows hides owned windows when their owner is minimised
          // and shows them again when it is restored, and Qt delivers both as ordinary
          // hide/show events -- so the owner's state is the only thing that separates "the user
          // put Noggit away for a moment" from "the user closed this tool".
          //
          // isMinimized() and nothing else. Qt keeps isVisible() true for a minimised window, so
          // the two are not interchangeable, and testing visibility instead would swallow the one
          // hide that genuinely means "done": TexturingTool::onDeselected calls hide() on this
          // window while tearing the texturing docks down (TexturingTool.cpp:400), and whether the
          // owner still counts as visible at that instant depends on the order those two happen
          // in.
          QWidget const* owner = _texturing_tool ? _texturing_tool->window() : nullptr;

          return owner && owner->isMinimized();
        }

        void GroundEffectsTool::dismissPreview()
        {
          _preview_armed = false;
          clearOverlayUniforms();
          hide();
        }

        //Close event triggers, hide event.
        void GroundEffectsTool::hideEvent(QHideEvent* event)
        {
          // Keeping the preview armed across a window-manager hide is the whole point: tearing it
          // down here is what made minimising Noggit end the preview, and -- because
          // texturing_tool::getTexturingMode keyed the ground-effect brush mode off the same
          // visibility -- what turned the next Shift+LMB after restoring into a texture paint
          // stroke over the terrain the user was inspecting.
          if (!hiddenByWindowManager())
          {
            _preview_armed = false;
            clearOverlayUniforms();
          }

          QWidget::hideEvent(event);
        }

        void GroundEffectsTool::showEvent(QShowEvent* event)
        {
          QWidget::showEvent(event);
          _preview_armed = true;
          updateTerrainUniformParams();
          refreshOverlay();
        }

        void GroundEffectsTool::updateDoodadPreviewRender(int slot_index)
        {
            if (slot_index < 0 || slot_index >= _object_list->count())
            {
                return;
            }

            QListWidgetItem* list_item = _object_list->item(slot_index);

            if (!list_item)
            {
                return;
            }

            QString filename = list_item->text();

            if (filename.isEmpty() || filename == STRING_EMPTY_DISPLAY)
            {
                list_item->setIcon(Noggit::Ui::FontAwesomeIcon(Noggit::Ui::FontAwesome::plus));
                list_item->setToolTip("Empty slot.");
            }
            else
            {
                // Load preview render.
                QString filepath(("world/nodxt/detail/" + filename.toStdString()).c_str());
                _preview_renderer->setModelOffscreen(filepath.toStdString());
                list_item->setIcon(*_preview_renderer->renderToPixmap());
                list_item->setToolTip(filepath);
            }
        }

        void GroundEffectsTool::openSetEditor()
        {
            auto const effect = getSelectedGroundEffect();

            auto* editor = qobject_cast<GroundEffectSetEditor*>(_set_editor.data());

            if (!editor)
            {
                // Parented on the map view rather than on this window: this one is
                // WindowStaysOnTopHint, and a dialog owned by it would be pinned above the
                // viewport it is meant to be read against.
                editor = new GroundEffectSetEditor(_map_view, _map_view);
                editor->setAttribute(Qt::WA_DeleteOnClose);
                _set_editor = editor;
            }

            editor->show();
            editor->raise();
            editor->activateWindow();

            // The point of opening it from here: the set you were looking at on the terrain is the
            // one the editor lands on, instead of making you find it again in a list of thousands.
            if (effect.has_value() && !effect->empty())
            {
                editor->showSet(effect->ID);
            }
        }

        GroundEffectsTool::~GroundEffectsTool()
        {
            delete _preview_renderer;
            _preview_renderer = nullptr;
        }

        float GroundEffectsTool::radius() const
        {
          return _effect_radius_slider->value();
        }

        ground_effect_brush_mode GroundEffectsTool::brush_mode() const
        {
            if (!_brush_grup_box->isChecked())
            {
                return ground_effect_brush_mode::none;
            }
            else if (_paint_effect->isChecked())
            {
                return ground_effect_brush_mode::effect;
            }
            else if (_paint_exclusion->isChecked())
            {
                return ground_effect_brush_mode::exclusion;
            }
            return ground_effect_brush_mode::none;
        }

        bool GroundEffectsTool::render_mode() const
        {
          return _render_group_box->isChecked();
        }

        void GroundEffectsTool::delete_renderer()
        {
          // Nulled, because the destructor deletes the same pointer. Calling both was a double
          // free waiting for its first caller.
          delete _preview_renderer;
          _preview_renderer = nullptr;
        }

        void GroundEffectsTool::paintEffect(glm::vec3 const& pos)
        {
            auto const effect = getSelectedGroundEffect();

            if (!effect.has_value() || effect->empty())
            {
                _coverage_label->setText("Select an effect set before painting one.");
                return;
            }

            applyEffectUnderBrush(pos, effect->ID);
        }

        void GroundEffectsTool::eraseEffect(glm::vec3 const& pos)
        {
            applyEffectUnderBrush(pos, GROUND_EFFECT_NONE);
        }

        void GroundEffectsTool::applyEffectUnderBrush(glm::vec3 const& pos, unsigned int effect_id)
        {
            std::string const texture = activeTexture();
            World* world = _map_view->getWorld();

            if (texture.empty() || !world)
            {
                return;
            }

            // beginAction returns the action that is ALREADY running and does not apply the flags
            // it was handed (ActionManager.cpp:64-65), so a paired endAction() would close a
            // stroke somebody else opened. The caller is a per-tick handler and normally has one
            // open for the length of the drag; when it does, this only widens its flags.
            bool const own_action = (NOGGIT_CUR_ACTION == nullptr);

            if (own_action)
            {
                NOGGIT_ACTION_MGR->beginAction(_map_view, ActionFlags::eCHUNKS_LAYERINFO);
            }
            else
            {
                NOGGIT_CUR_ACTION->addFlags(ActionFlags::eCHUNKS_LAYERINFO);
            }

            world->for_all_chunks_in_range
              ( pos, radius()
              , [&] (MapChunk* chunk) -> bool
                {
                    if (!chunk)
                    {
                        return false;
                    }

                    TextureSet* texture_set = chunk->getTextureSet();

                    if (!texture_set)
                    {
                        return false;
                    }

                    bool touched = false;

                    for (std::size_t layer = 0; layer < texture_set->num(); ++layer)
                    {
                        if (texture_set->filename(layer) != texture)
                        {
                            continue;
                        }

                        if (texture_set->getEffectForLayer(layer) == effect_id)
                        {
                            continue;
                        }

                        if (!touched)
                        {
                            // Before the write, because the snapshot is the only copy of the four
                            // MCLY entries as they were.
                            NOGGIT_CUR_ACTION->registerChunkLayerInfoChange(chunk);
                            touched = true;
                        }

                        texture_set->setEffect(layer, static_cast<int>(effect_id));
                    }

                    if (touched)
                    {
                        // The low-detail texture map is derived from the layers, same follow-up the
                        // scripting API performs after set_effect (script_chunk.cpp:53-54).
                        texture_set->lod_texture_map();
                    }

                    return touched;
                }
              );

            if (own_action)
            {
                NOGGIT_ACTION_MGR->endAction();
            }
        }

        std::optional<ground_effect_set> GroundEffectsTool::getSelectedGroundEffect()
        {
            QListWidgetItem const* item = _effect_sets_list->currentItem();

            if (!item)
            {
                return std::nullopt;
            }

            bool ok = false;
            int const index = item->data(Qt::UserRole).toInt(&ok);

            if (!ok || index < 0 || static_cast<std::size_t>(index) >= _loaded_effects.size())
            {
                return std::nullopt;
            }

            return _loaded_effects[static_cast<std::size_t>(index)];
        }

        std::optional<glm::vec3> GroundEffectsTool::getSelectedEffectColor()
        {
            QListWidgetItem const* item = _effect_sets_list->currentItem();

            if (!item)
            {
                return std::nullopt;
            }

            bool ok = false;
            int const index = item->data(Qt::UserRole).toInt(&ok);

            if (!ok || index < 0 || static_cast<std::size_t>(index) >= _effects_colors.size())
            {
                return std::nullopt;
            }

            return _effects_colors[static_cast<std::size_t>(index)];
        }

        void GroundEffectsTool::setActiveGroundEffect(ground_effect_set const& effect)
        {
            if (effect.ID && effect.ID == _mirrored_set_id)
            {
                return;
            }

            _mirrored_set_id = effect.ID;

            _spinbox_doodads_amount->setValue(static_cast<int>(effect.Amount));

            // findData, not setCurrentIndex(TerrainType). The DBC id is not a row number: the
            // combo carries a synthetic "0 (none)" first and then TerrainType.dbc in file order,
            // whose ids are neither dense nor zero-based.
            int const terrain_index = _cbbox_terrain_type->findData(QVariant(effect.TerrainType));
            _cbbox_terrain_type->setCurrentIndex(terrain_index < 0 ? 0 : terrain_index);

            // A weight only means anything against the sum of the weights of the slots that
            // actually hold a model, so empty slots are excluded from the total rather than
            // dragging the percentages down with a share they can never be drawn for.
            unsigned int total_weight = 0;

            for (int i = 0; i < 4; ++i)
            {
                if (!effect.Doodads[i].filename.empty())
                {
                    total_weight += effect.Weights[i];
                }
            }

            for (int i = 0; i < 4; ++i)
            {
                QString filename(effect.Doodads[i].filename.c_str());
                // Replace old extensions in the DBC.
                filename = filename.replace(".mdx", ".m2", Qt::CaseInsensitive);
                filename = filename.replace(".mdl", ".m2", Qt::CaseInsensitive);

                _object_list->item(i)->setText(filename.isEmpty() ? QString(STRING_EMPTY_DISPLAY) : filename);
                updateDoodadPreviewRender(i);

                _weight_spin[i]->setValue(static_cast<int>(effect.Weights[i]));

                if (effect.Doodads[i].filename.empty())
                {
                    _weight_percent[i]->setText("--");
                }
                else if (total_weight == 0)
                {
                    // Every filled slot has weight 0. The client cannot pick any of them, and that
                    // is worth saying rather than printing a division by zero as 0.0%.
                    _weight_percent[i]->setText("never");
                }
                else
                {
                    _weight_percent[i]->setText
                      (QString::number(100.0 * effect.Weights[i] / total_weight, 'f', 1) + "%");
                }
            }
        }

        void ground_effect_set::load_from_id(unsigned int effect_id)
        {
            if (!effect_id || (effect_id == GROUND_EFFECT_NONE))
            {
                return;
            }

            if (!gGroundEffectTextureDB.CheckIfIdExists(effect_id))
            {
                return;
            }

            try
            {
                DBCFile::Record GErecord = gGroundEffectTextureDB.getByID(effect_id);
                Name = std::to_string(effect_id);
                ID = GErecord.getUInt(GroundEffectTextureDB::ID);
                Amount = GErecord.getUInt(GroundEffectTextureDB::Amount);
                TerrainType = GErecord.getUInt(GroundEffectTextureDB::TerrainType);

                for (int i = 0; i < 4; ++i)
                {
                    Weights[i] = GErecord.getUInt(GroundEffectTextureDB::Weights + i);
                    unsigned const curDoodadId
                    {
                        GErecord.getUInt(GroundEffectTextureDB::Doodads + i)
                    };

                    if (!curDoodadId)
                    {
                        continue;
                    }

                    if (!gGroundEffectDoodadDB.CheckIfIdExists(curDoodadId))
                    {
                        continue;
                    }

                    Doodads[i].ID = curDoodadId;
                    QString filename = gGroundEffectDoodadDB.getByID(curDoodadId).getString(GroundEffectDoodadDB::Filename);

                    filename.replace(".mdx", ".m2", Qt::CaseInsensitive);
                    filename.replace(".mdl", ".m2", Qt::CaseInsensitive);

                    Doodads[i].filename = filename.toStdString();
                }
            }
            catch (GroundEffectTextureDB::NotFound)
            {
                ID = 0;
                LogError << "Couldn't find ground effect Id : " << effect_id << " in GroundEffectTexture.dbc" << std::endl;
            }
        }

        std::string ground_effect_set::signature() const
        {
          std::string key;
          key.reserve(128);

          key += std::to_string(TerrainType);
          key += '|';
          key += std::to_string(Amount);

          for (int i = 0; i < 4; ++i)
          {
            key += '|';
            key += Doodads[i].filename;
            key += '#';
            key += std::to_string(Weights[i]);
          }

          return key;
        }

        bool ground_effect_set::empty() const
        {
          return !ID;
        }

        bool ground_effect_set::operator== (ground_effect_set* effect2)
        {
          return (TerrainType == effect2->TerrainType && Amount == effect2->Amount
            && Doodads[0] == &effect2->Doodads[0] && Doodads[1] == &effect2->Doodads[1]
            && Doodads[2] == &effect2->Doodads[2] && Doodads[3] == &effect2->Doodads[3]
            && Weights[0] == effect2->Weights[0] && Weights[1] == effect2->Weights[1]
            && Weights[2] == effect2->Weights[2] && Weights[3] == effect2->Weights[3]
            );
        }

        bool ground_effect_doodad::empty() const
        {
          return filename.empty();
        }

        bool ground_effect_doodad::operator== (ground_effect_doodad* doodad2)
        {
          return filename == doodad2->filename;
        }
}
}
