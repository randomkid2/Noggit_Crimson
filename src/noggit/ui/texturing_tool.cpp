// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/application/Configuration/NoggitApplicationConfiguration.hpp>
#include <noggit/application/NoggitApplication.hpp>
#include <noggit/MapView.h>
#include <noggit/project/CurrentProject.hpp>
#include <noggit/TabletManager.hpp>
#include <noggit/TextureManager.h>
#include <noggit/ui/Checkbox.hpp>
#include <noggit/ui/CurrentTexture.h>
#include <noggit/ui/DesignTokens.hpp>
#include <noggit/ui/GroundEffectsTool.hpp>
#include <noggit/ui/texture_swapper.hpp>
#include <noggit/ui/texturing_tool.hpp>
#include <noggit/ui/TexturingGUI.h>
#include <noggit/texturing/TextureLayerPolicy.hpp>
#include <noggit/ui/tools/ToolPanel/ToolWidgetStyle.hpp>
#include <noggit/ui/tools/UiCommon/expanderwidget.h>
#include <noggit/ui/tools/UiCommon/ExtendedSlider.hpp>
#include <noggit/ui/tools/UiCommon/ImageMaskSelector.hpp>
#include <noggit/World.h>

#include <QClipboard>
#include <QMessageBox>
#include <QPainter>
#include <QSettings>
#include <QStyle>
#include <QStyleOptionSlider>
#include <ClientData.hpp>

#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QTabWidget>

#define _USE_MATH_DEFINES
#include <math.h>

#include <utility>

namespace Noggit
{
  namespace Ui
  {
    texturing_tool::texturing_tool ( const glm::vec3* camera_pos
                                   , MapView* map_view
                                   , BoolToggleProperty* show_quick_palette
                                   , QWidget* parent
                                   )
      : QWidget(parent)
      , _brush_level(255)
      , _show_unpaintable_chunks(false)
      , _spray_size(1.0f)
      , _spray_pressure(2.0f)
      , _anim_prop(true)
      , _anim_speed_prop(1)
      , _anim_rotation_prop(4)
      , _overbright_prop(false)
      , _texturing_mode(texturing_mode::paint)
      , _map_view(map_view)
    {
      setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
      // The dock's shared shell. This layout previously set no margins at all, so it took
      // QStyle::PM_LayoutLeftMargin -- 13px on windowsvista here -- on top of ToolPanel's own
      // 12px, while the tab content below pinned 9. See ToolWidgetStyle.hpp.
      auto layout (Tools::ToolPanelStyle::toolColumn (this));

      _texture_brush.init();
      _inner_brush.init();
      _spray_brush.init();

      _current_texture = new current_texture(true, this);
      _current_texture->resize(QSize(225, 225));
      layout->addWidget (_current_texture);
      layout->setAlignment(_current_texture, Qt::AlignHCenter);

      tabs = new QTabWidget(this);

      auto tool_widget (new QWidget (this));
      tool_widget->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
      auto tool_layout (new QVBoxLayout (tool_widget));
      tool_layout->setAlignment(Qt::AlignTop);
      // Inside a QTabWidget pane, which the theme already pads, so this adds nothing of its own
      // -- the same zero-gutter, S3-between-sections rule ToolWidgetStyle.hpp states for the
      // tool widget itself. dressToolLayout is not used here because it also pins a 250px
      // minimum width, which belongs to the tool, not to one of its tabs.
      tool_layout->setContentsMargins(0, 0, 0, 0);
      tool_layout->setSpacing(Design::S3);

      auto slider_layout (new QGridLayout);
      slider_layout->setContentsMargins(0, 0, 0, 0);
      slider_layout->setHorizontalSpacing(12);
      tool_layout->addItem(slider_layout);

      // These two used to be constructed as new QVBoxLayout(tool_widget) -- i.e. handed a parent
      // widget that already owns tool_layout -- and then immediately re-parented by addLayout.
      // Qt accepts it but warns "Attempting to add QLayout to QWidget which already has a
      // layout" twice into log.txt on every texturing tool built. They are parentless now and
      // addLayout does the owning, which is what the code already meant.
      //
      // Spacing 2 is the caption-to-control gap. The gap BETWEEN settings is added explicitly
      // with addSpacing below, because a QVBoxLayout has one spacing value and these columns
      // need two: tight under a caption so the pair reads as one thing, loose between pairs so
      // the three settings do not run together. That was the real defect in this column --
      // caption and slider sat exactly as far apart as two unrelated settings did.
      auto slider_layout_left (new QVBoxLayout);
      slider_layout_left->setContentsMargins(0, 0, 0, 0);
      slider_layout_left->setSpacing(2);
      slider_layout->addLayout(slider_layout_left, 0, 0);
      auto slider_layout_right(new QVBoxLayout);
      slider_layout_right->setContentsMargins(0, 0, 0, 0);
      slider_layout_right->setSpacing(2);
      slider_layout->addLayout(slider_layout_right, 0, 1);

      // These three keep their caption above rather than moving the text into the slider's own
      // prefix row the way the terrain tool does, and that is deliberate rather than an
      // oversight. Measured with the real theme at the dock's content width: folding the
      // captions in saves 74px of height but takes the tab's minimum width from 229px to 262px,
      // because this column shares its row with the vertical opacity slider and the label then
      // has to fit BESIDE the value instead of above it. The dock's minimum is 265px including
      // its scroll bar, so 262px is over budget and every Paint tab would gain a horizontal
      // scroll bar. Height is worth having; a sideways scroll bar on the most used tool is not.
      //
      // Each caption now sits 2px above its own slider and 10px below the previous setting.
      slider_layout_left->addWidget(new QLabel("Hardness:", tool_widget));
      _hardness_slider = new Noggit::Ui::Tools::UiCommon::ExtendedSlider(tool_widget);
      _hardness_slider->setPrefix("");
      _hardness_slider->setRange (0, 1);
      _hardness_slider->setDecimals(2);
      _hardness_slider->setSingleStep(0.05f);
      _hardness_slider->setValue(0.5f);
      slider_layout_left->addWidget(_hardness_slider);

      slider_layout_left->addSpacing(10);
      slider_layout_left->addWidget(new QLabel("Radius:", tool_widget));
      _radius_slider = new Noggit::Ui::Tools::UiCommon::ExtendedSlider(tool_widget);
      _radius_slider->setPrefix("");
      _radius_slider->setRange (0, 1000);
      _radius_slider->setDecimals (2);
      _radius_slider->setValue(_texture_brush.getRadius());
      slider_layout_left->addWidget (_radius_slider);

      slider_layout_left->addSpacing(10);
      slider_layout_left->addWidget(new QLabel("Pressure:", tool_widget));
      _pressure_slider = new Noggit::Ui::Tools::UiCommon::ExtendedSlider(tool_widget);
      _pressure_slider->setPrefix("");
      _pressure_slider->setRange (0, 1.0f);
      _pressure_slider->setDecimals (2);
      _pressure_slider->setValue (0.9f);
      slider_layout_left->addWidget (_pressure_slider);

      slider_layout_right->addWidget(new QLabel("Opacity:", tool_widget));
      _brush_level_slider = new OpacitySlider(Qt::Orientation::Vertical, tool_widget);
      _brush_level_slider->setRange (0, 255);
      _brush_level_slider->setToolTip("Opacity");
      _brush_level_slider->setSliderPosition (_brush_level);

      _brush_level_slider->setObjectName("texturing_brush_level_slider");
      
      // TODO : couldn't figure out how to make QSlider::groove:vertical::background-color work, the themes broke it. so made a scuffed subclass of QSlider with a custom paintevent

      /*
      QString _brush_level_slider_style =

          "QSlider#texturing_brush_level_slider::groove:vertical { \n "
          "  background-color: qlineargradient(x1:0.5, y1:0, x2:0.5, y2:1, stop: 0 black, stop: 1 white) !important; \n "
          "  width: 35px; \n"
          "  margin: 0 0 0 0; \n "
          "} \n "
          "QSlider#texturing_brush_level_slider::handle:vertical { \n"
          "  background-color: red; \n"
          "  height: 5px; \n"
          "} \n"
          "QSlider#texturing_brush_level_slider::vertical { \n"
          "  width: 35px; \n"
          "} \n"
          ;
      _brush_level_slider->setStyleSheet(_brush_level_slider_style);
*/

      slider_layout_right->addWidget(_brush_level_slider, 0, Qt::AlignHCenter);

      _brush_level_spin = new QSpinBox(tool_widget);
      _brush_level_spin->setRange(0, 255);
      _brush_level_spin->setValue(_brush_level);
      _brush_level_spin->setSingleStep(5);
      slider_layout_right->addSpacing(4);
      slider_layout_right->addWidget(_brush_level_spin);

      QSettings settings;
      bool use_classic_ui = settings.value("classicUI", false).toBool();

      _show_unpaintable_chunks_cb = new QCheckBox("Show unpaintable chunks", tool_widget);
      _show_unpaintable_chunks_cb->setChecked(false);
      if (!use_classic_ui)
          _show_unpaintable_chunks_cb->hide();
      tool_layout->addWidget(_show_unpaintable_chunks_cb);

      connect(_show_unpaintable_chunks_cb, &QCheckBox::toggled, [=](bool checked)
          {
              _map_view->getWorld()->renderer()->getTerrainParamsUniformBlock()->draw_paintability_overlay = checked;
              _map_view->getWorld()->renderer()->markTerrainParamsUniformBlockDirty();
          });

      // SMART PAINT -- the way through the four-layer wall, sitting directly under the checkbox
      // that shows you where the wall is.
      //
      // A chunk holds four MCLY entries. When all four are in use and none of them is paintable
      // away, TextureSet::get_texture_index_or_add returns -1 and the stroke does nothing at all:
      // no error, no cursor change, nothing in the log. The red paintability overlay already says
      // WHICH chunks those are; this says what to do about them.
      _layer_budget_group = new QGroupBox("Layer budget", tool_widget);
      _layer_budget_group->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
      auto layer_budget_layout (new QVBoxLayout (_layer_budget_group));

      _layer_full_mode = new QComboBox(_layer_budget_group);
      // Short strings on purpose. This column measured 229px of minimum width and the dock's own
      // minimum is 265px including its scroll bar (see the note on the hardness/radius captions
      // above); a combo sized to its longest item would spend that headroom on a label. The
      // tooltip carries the full sentence instead.
      _layer_full_mode->addItem("Skip full chunks");
      _layer_full_mode->addItem("Replace least visible");
      _layer_full_mode->addItem("Replace nominated");
      _layer_full_mode->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
      _layer_full_mode->setMinimumContentsLength(12);
      _layer_full_mode->setToolTip
        ( "What happens when a texture has to go onto a chunk that already holds four.\n\n"
          "Skip: do nothing, which is what the brush has always done.\n"
          "Replace least visible: evict the layer with the smallest total alpha over the chunk.\n"
          "Replace nominated: evict the layer holding the texture named below, and skip chunks "
          "that do not have it.\n\n"
          "This governs every path that adds a layer, not just the brush: Automatic Texturing "
          "and Live Auto Texture had nowhere to write on a full chunk and now have one. It is "
          "never saved to settings, so it is back to Skip on the next start."
        );
      layer_budget_layout->addWidget(_layer_full_mode);

      _nominated_texture = new QLineEdit(_layer_budget_group);
      _nominated_texture->setReadOnly(true);
      _nominated_texture->setPlaceholderText("texture to replace");
      _nominated_texture->setEnabled(false);
      layer_budget_layout->addWidget(_nominated_texture);

      _nominate_selected_btn = new QPushButton("Nominate selected", _layer_budget_group);
      _nominate_selected_btn->setEnabled(false);
      layer_budget_layout->addWidget(_nominate_selected_btn);

      auto layer_manager_btn (new QPushButton("Texture Layers", _layer_budget_group));
      layer_manager_btn->setToolTip
        ("Layer replacement, prepare area, and the duplicate and threshold purges.");
      layer_budget_layout->addWidget(layer_manager_btn);

      tool_layout->addWidget(_layer_budget_group);

      connect ( _layer_full_mode, qOverload<int> (&QComboBox::currentIndexChanged)
              , [this] (int index)
                {
                  bool const nominated
                    = index == static_cast<int>(Noggit::LayerFullPolicy::ReplaceNominated);

                  _nominated_texture->setEnabled(nominated);
                  _nominate_selected_btn->setEnabled(nominated);

                  update_layer_admission();
                }
              );

      connect ( _nominate_selected_btn, &QPushButton::clicked
              , [this]
                {
                  auto const texture = selected_texture::get();

                  if (!texture)
                  {
                    return;
                  }

                  // Normalised on the way in, so the comparison in pickEvictableLayer has to
                  // normalise only the stored side. Both halves have to be normalised or a
                  // path spelled with backslashes never matches one spelled with slashes and
                  // the mode silently does nothing.
                  _nominated_texture->setText
                    ( QString::fromStdString
                        ( BlizzardArchive::ClientData::normalizeFilenameUnix
                            ((*texture)->file_key().filepath())
                        )
                    );

                  update_layer_admission();
                }
              );

      connect ( layer_manager_btn, &QPushButton::clicked
              , [this] { emit textureLayerManagerRequested(); }
              );

      // Pushed once at construction so the process-wide policy and the widget that owns it start
      // in agreement. They would agree anyway on a fresh process -- both default to Skip -- but a
      // second map view opened after the first armed an eviction would otherwise inherit the armed
      // policy behind a combo box reading "Skip full chunks".
      update_layer_admission();

      // spray
      _spray_mode_group = new QGroupBox("Spray", tool_widget);
      _spray_mode_group->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
      _spray_mode_group->setCheckable(true);
      tool_layout->addWidget (_spray_mode_group);

      _spray_content = new QWidget(_spray_mode_group);
      auto spray_layout (new QFormLayout (_spray_content));
      _spray_mode_group->setLayout(spray_layout);

      _inner_radius_cb = new QCheckBox("Inner radius", _spray_content);
      spray_layout->addRow(_inner_radius_cb);

      _spray_size_spin = new QDoubleSpinBox (_spray_content);
      _spray_size_spin->setRange (1.0f, 40.0f);
      _spray_size_spin->setDecimals (2);
      _spray_size_spin->setValue (_spray_size);
      spray_layout->addRow ("Size:", _spray_size_spin);

      _spray_size_slider = new QSlider (Qt::Orientation::Horizontal, _spray_content);
      _spray_size_slider->setRange (100, 40 * 100);
      _spray_size_slider->setSliderPosition (_spray_size * 100);
      spray_layout->addRow (_spray_size_slider);

      _spray_pressure_spin = new QDoubleSpinBox (_spray_content);
      _spray_pressure_spin->setRange (0.0f, 10.0);
      _spray_pressure_spin->setDecimals (2);
      _spray_pressure_spin->setValue (_spray_pressure);
      spray_layout->addRow ("Pressure:", _spray_pressure_spin);

      _spray_pressure_slider = new QSlider (Qt::Orientation::Horizontal, _spray_content);
      _spray_pressure_slider->setRange (0, 10 * 100);
      _spray_pressure_slider->setSliderPosition (std::round(_spray_pressure * 100));
      spray_layout->addRow (_spray_pressure_slider);

      _texture_switcher = new texture_swapper(tool_widget, camera_pos, map_view);
      _texture_switcher->hide();

      _ground_effect_tool = new GroundEffectsTool(this, map_view, this);

      _image_mask_group = new Noggit::Ui::Tools::ImageMaskSelector(map_view, this);
      _image_mask_group->setContinuousActionName("Paint");
      _image_mask_group->setBrushModeVisible(parent == map_view);
      _mask_image = _image_mask_group->getPixmap()->toImage();
      _image_mask_group->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
      // tool_layout->addWidget(_image_mask_group);
      auto* customBrushBox = new ExpanderWidget(this);
      customBrushBox->setExpanderTitle("Custom Brush");
      customBrushBox->addPage(_image_mask_group);
      customBrushBox->setExpanded(false);
      tool_layout->addWidget(customBrushBox);

      tool_layout->setAlignment(_image_mask_group, Qt::AlignTop);

      auto quick_palette_btn (new QPushButton("Quick Palette", this));
      tool_layout->addWidget(quick_palette_btn);
      tool_layout->setAlignment(quick_palette_btn, Qt::AlignTop);

      // Mists HeightMapping, only enable if modern feature setting is on
      bool modern_features = Noggit::Application::NoggitApplication::instance()->getConfiguration()->modern_features;

      // Define UI elements regardless of modern_features being enabled because they're used later on as well.
      _heightmapping_group = new QGroupBox("Height Mapping", tool_widget);
      _heightmapping_group->setVisible(modern_features);

      auto heightmapping_scale_spin = new QDoubleSpinBox(_heightmapping_group);
      heightmapping_scale_spin->setVisible(modern_features);

      auto heightmapping_heightscale_spin = new QDoubleSpinBox(_heightmapping_group);
      heightmapping_heightscale_spin->setVisible(modern_features);

      auto heightmapping_heightoffset_spin = new QDoubleSpinBox(_heightmapping_group);
      heightmapping_heightoffset_spin->setVisible(modern_features);

      QPushButton* _heightmapping_copy_btn = new QPushButton("Copy to JSON", this);
      _heightmapping_copy_btn->setVisible(modern_features);

      if (modern_features)
      {

          auto heightmapping_group_layout(new QFormLayout(_heightmapping_group));

          heightmapping_scale_spin->setRange(0, 512);
          heightmapping_scale_spin->setSingleStep(1);
          heightmapping_scale_spin->setDecimals(0);
          heightmapping_scale_spin->setValue(0);
          heightmapping_group_layout->addRow("Scale:", heightmapping_scale_spin);

          heightmapping_heightscale_spin->setRange(-512, 512);
          heightmapping_heightscale_spin->setSingleStep(0.1);
          heightmapping_heightscale_spin->setDecimals(3);
          heightmapping_heightscale_spin->setValue(0);
          heightmapping_group_layout->addRow("Height Scale:", heightmapping_heightscale_spin);

          heightmapping_heightoffset_spin->setRange(-512, 512);
          heightmapping_heightoffset_spin->setSingleStep(0.1);
          heightmapping_heightoffset_spin->setDecimals(3);
          heightmapping_heightoffset_spin->setValue(1);
          heightmapping_group_layout->addRow("Height Offset:", heightmapping_heightoffset_spin);

          auto heightmapping_btngroup_layout(new QVBoxLayout(_heightmapping_group));
          auto heightmapping_buttons_widget = new QWidget(_heightmapping_group);
          heightmapping_buttons_widget->setLayout(heightmapping_btngroup_layout);

          auto wrap_label = new QLabel("Note: This doesn't save to .cfg, use copy and do it manually.", _heightmapping_group);
          wrap_label->setWordWrap(true);
          heightmapping_group_layout->addRow(wrap_label);

          _heightmapping_apply_global_btn = new QPushButton("Apply (Global)", this);
          _heightmapping_apply_global_btn->setFixedHeight(30);
          heightmapping_btngroup_layout->addWidget(_heightmapping_apply_global_btn);

          _heightmapping_apply_adt_btn = new QPushButton("Apply (Current ADT)", this);
          _heightmapping_apply_adt_btn->setFixedHeight(30);
          heightmapping_btngroup_layout->addWidget(_heightmapping_apply_adt_btn);

          _heightmapping_copy_btn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
          _heightmapping_copy_btn->setFixedHeight(30);
          heightmapping_btngroup_layout->addWidget(_heightmapping_copy_btn);

          heightmapping_group_layout->addRow(heightmapping_buttons_widget);

          tool_layout->addWidget(_heightmapping_group);
      }
      
      auto geffect_tools_btn(new QPushButton("In development", this));
      tool_layout->addWidget(geffect_tools_btn);
      tool_layout->setAlignment(geffect_tools_btn, Qt::AlignTop);

      auto anim_widget (new QWidget (this));
      auto anim_layout (new QFormLayout (anim_widget));

      _anim_group = new QGroupBox("Add anim", anim_widget);
      _anim_group->setCheckable(true);
      _anim_group->setChecked(_anim_prop.get());

      auto anim_group_layout (new QFormLayout (_anim_group));

      auto anim_speed_slider = new QSlider(Qt::Orientation::Horizontal, _anim_group);
      anim_speed_slider->setRange(0, 7);
      anim_speed_slider->setSingleStep(1);
      anim_speed_slider->setTickInterval(1);
      anim_speed_slider->setTickPosition(QSlider::TickPosition::TicksBothSides);
      anim_speed_slider->setValue(_anim_speed_prop.get());
      anim_group_layout->addRow("Speed:", anim_speed_slider);

      anim_group_layout->addRow(new QLabel("Orientation:", _anim_group));

      auto anim_orientation_dial = new QDial(_anim_group);
      anim_orientation_dial->setRange(0, 8);
      anim_orientation_dial->setSingleStep(1);
      anim_orientation_dial->setValue(_anim_rotation_prop.get());
      anim_orientation_dial->setWrapping(true);
      anim_group_layout->addRow(anim_orientation_dial);

      anim_layout->addRow(_anim_group);

      auto overbright_cb = new CheckBox("Overbright", &_overbright_prop, anim_widget);
      anim_layout->addRow(overbright_cb);

      tabs->addTab(tool_widget, "Paint");
      tabs->addTab(_texture_switcher, "Swap");
      tabs->addTab(anim_widget, "Anim");
      tabs->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
      
      layout->addWidget(tabs);

      connect ( _anim_group, &QGroupBox::toggled
              , [&](bool b)
                {
                  _anim_group->setTitle(QString(b ? "Add anim" : "Remove anim"));
                  _anim_prop.set(b);
                }
              );

      connect (anim_speed_slider, &QSlider::valueChanged, &_anim_speed_prop, &Noggit::unsigned_int_property::set);
      connect (anim_orientation_dial, &QDial::valueChanged, &_anim_rotation_prop, &Noggit::unsigned_int_property::set);
      
      if (modern_features)
      {
          connect(heightmapping_scale_spin, qOverload<double>(&QDoubleSpinBox::valueChanged)
              , [&](double v)
              {
                  textureHeightmappingData.uvScale = v;
              }
          );

          connect(heightmapping_heightscale_spin, qOverload<double>(&QDoubleSpinBox::valueChanged)
              , [&](double v)
              {
                  textureHeightmappingData.heightScale = v;
              }
          );
          connect(heightmapping_heightoffset_spin, qOverload<double>(&QDoubleSpinBox::valueChanged)
              , [&](double v)
              {
                  textureHeightmappingData.heightOffset = v;
              }
          );
      }

      connect ( tabs, &QTabWidget::currentChanged
              , [this] (int index)
                {
                  switch (index)
                  {
                    case 0: _texturing_mode = texturing_mode::paint; break;
                    case 1: _texturing_mode = texturing_mode::swap; break;
                    case 2: _texturing_mode = texturing_mode::anim; break;
                  }
                }
              );

      connect ( _brush_level_spin, qOverload<int> (&QSpinBox::valueChanged)
              , [&] (int v)
                {
                  QSignalBlocker const blocker (_brush_level_slider);
                  _brush_level = v;
                  _brush_level_slider->setSliderPosition (v);
                }
              );

      connect ( _brush_level_slider, &QSlider::valueChanged
              , [&] (int v)
                {
                  QSignalBlocker const blocker (_brush_level_spin);
                  _brush_level = v;
                  _brush_level_spin->setValue(v);
                }
              );

      connect(_show_unpaintable_chunks_cb, &QCheckBox::stateChanged
          , [&](int state)
          {
              _show_unpaintable_chunks = state;
          }
      );

      connect ( _spray_size_spin, qOverload<double> (&QDoubleSpinBox::valueChanged)
              , [&] (double v)
                {
                  QSignalBlocker const blocker (_spray_size_slider);
                  _spray_size = v;
                  _spray_size_slider->setSliderPosition ((int)std::round (v * 100.0f));
                  update_spray_brush();
                }
              );

      connect ( _spray_size_slider, &QSlider::valueChanged
              , [&] (int v)
                {
                  QSignalBlocker const blocker (_spray_size_spin);
                  _spray_size = v * 0.01f;
                  _spray_size_spin->setValue (_spray_size);
                  update_spray_brush();
                }
              );

      connect ( _spray_pressure_spin, qOverload<double> (&QDoubleSpinBox::valueChanged)
              , [&] (double v)
                {
                  QSignalBlocker const blocker (_spray_pressure_slider);
                  _spray_pressure = v;
                  _spray_pressure_slider->setSliderPosition ((int)std::round (v * 100.0f));
                }
              );

      connect ( _spray_pressure_slider, &QSlider::valueChanged
              , [&] (int v)
                {
                  QSignalBlocker const blocker (_spray_pressure_spin);
                  _spray_pressure = v * 0.01f;
                  _spray_pressure_spin->setValue(_spray_pressure);
                }
              );

      connect ( _spray_mode_group, &QGroupBox::toggled
              , [&] (bool b)
                {
                  _spray_content->setEnabled(b);
                }
              );

      connect ( quick_palette_btn, &QPushButton::clicked
              , [=] ()
                {
                  emit texturePaletteToggled();
                }
              );

      connect(geffect_tools_btn, &QPushButton::clicked
          , [=]()
          {
              _ground_effect_tool->show();
          }
      );

      connect ( _radius_slider, &Noggit::Ui::Tools::UiCommon::ExtendedSlider::valueChanged
          , [&] (double v)
                {
                    set_radius(static_cast<float>(_radius_slider->value()));
                }
      );


      connect ( _hardness_slider, &Noggit::Ui::Tools::UiCommon::ExtendedSlider::valueChanged
          , [&] (double v)
                {
                    update_brush_hardness();
                }
      );

      connect (_image_mask_group, &Noggit::Ui::Tools::ImageMaskSelector::rotationUpdated, this, &texturing_tool::updateMaskImage);
      connect (_radius_slider, &Noggit::Ui::Tools::UiCommon::ExtendedSlider::valueChanged, this, &texturing_tool::updateMaskImage);
      connect(_image_mask_group, &Noggit::Ui::Tools::ImageMaskSelector::pixmapUpdated, this, &texturing_tool::updateMaskImage);

      // Mists Heightmapping

      if (modern_features)
      {
          connect(_current_texture, &Noggit::Ui::current_texture::texture_updated
              , [=]()
              {
                  auto proj = Noggit::Project::CurrentProject::get();
                  auto foundTexture = proj->ExtraMapData.TextureHeightData_Global.find(_current_texture->filename());
                  if (foundTexture != proj->ExtraMapData.TextureHeightData_Global.end())
                  {
                      heightmapping_scale_spin->setValue(foundTexture->second.uvScale);
                      heightmapping_heightscale_spin->setValue(foundTexture->second.heightScale);
                      heightmapping_heightoffset_spin->setValue(foundTexture->second.heightOffset);
                  }

              }
          );

          connect(_heightmapping_copy_btn, &QPushButton::pressed
              , [=]()
              {
                  std::ostringstream oss;
                  oss << "{\r\n    \"" << _current_texture->filename() << "\": {\r\n"
                      << "    \"Scale\": " << textureHeightmappingData.uvScale << ",\r\n"
                      << "    \"HeightScale\": " << textureHeightmappingData.heightScale << ",\r\n"
                      << "    \"HeightOffset\": " << textureHeightmappingData.heightOffset << "\r\n"
                      << "    }\r\n}";

                  QClipboard* clip = QApplication::clipboard();
                  clip->setText(QString::fromStdString(oss.str()));

                  QMessageBox::information
                  (nullptr
                      , "Copied"
                      , "JSON Copied to Clipboard",
                      QMessageBox::Ok
                  );

              }
          );
      }

      _spray_content->hide();
      update_brush_hardness();
      update_spray_brush();
      set_radius(15.0f);
      toggle_tool(); // to disable

      // The 250px FLOOR is set once, by toolColumn at the top of this constructor. The 250px
      // CEILING that used to sit beside it here is gone: it pinned the most-used tool in the
      // editor to exactly the dock's minimum width, so widening the right-hand dock left a
      // strip of empty panel beside a tool that refused to grow into it. Only two tools carried
      // a ceiling -- this one and ShaderTool -- and neither needed it.
    }

    texturing_tool::~texturing_tool()
    {
        // _ground_effect_tool->delete_renderer();
        // delete _ground_effect_tool;
    }

    // Rebuilds the rotated mask, but only when the mask or the rotation actually changed. The
    // long-form reasoning -- why the radius slider reaches this at all, what the transform costs
    // per mouse-move event, and why the emit stays unguarded -- is on TerrainTool::updateMaskImage,
    // which this is a copy of. (The angle conversion here is spelled differently from the other
    // two and is left exactly as it was; it is not the same rotation for a given dial value, and
    // changing that would move where the mask points.)
    void texturing_tool::updateMaskImage()
    {
      QPixmap* pixmap = _image_mask_group->getPixmap();
      int const rotation = _image_mask_group->getRotation();

      if ( !_mask_image_built
        || _mask_source_key != pixmap->cacheKey()
        || _mask_rotation != rotation
         )
      {
        QTransform matrix;
        matrix.rotateRadians(rotation * M_PI / 180.f);
        _mask_image = pixmap->toImage().transformed(matrix, Qt::SmoothTransformation);

        _mask_source_key = pixmap->cacheKey();
        _mask_rotation = rotation;
        _mask_image_built = true;
      }

      emit _map_view->trySetBrushTexture(&_mask_image, this);
    }

    void texturing_tool::update_brush_hardness()
    {
      _texture_brush.setHardness(static_cast<float>(_hardness_slider->value()));
      _inner_brush.setHardness(static_cast<float>(_hardness_slider->value()));
      _spray_brush.setHardness(static_cast<float>(_hardness_slider->value()));
    }

    void texturing_tool::set_radius(float radius)
    {
      _texture_brush.setRadius(radius);
      _inner_brush.setRadius(radius * static_cast<float>(_hardness_slider->value()));
    }

    void texturing_tool::update_spray_brush()
    {
      if (_texturing_mode == texturing_mode::paint)
      {
        _spray_brush.setRadius(_spray_size * TEXDETAILSIZE / 2.0f);
      }
    }

    void texturing_tool::toggle_tool()
    {
      if (_texturing_mode == texturing_mode::paint)
      {
        _spray_mode_group->setChecked(!_spray_mode_group->isChecked());
      }
      else if (_texturing_mode == texturing_mode::swap)
      {
        _texture_switcher->toggle_brush_mode();
      }
      else if (_texturing_mode == texturing_mode::anim)
      {
        _anim_group->setChecked(!_anim_group->isChecked());
      }
    }

    GroundEffectsTool* texturing_tool::getGroundEffectsTool()
    {
      return _ground_effect_tool;
    }

    void texturing_tool::setRadius(float radius)
    {
      _radius_slider->setValue(radius);
      _texture_switcher->change_radius(radius - _texture_switcher->radius());
      _ground_effect_tool->change_radius(radius);
    }

    void texturing_tool::setHardness(float hardness)
    {
      _hardness_slider->setValue(hardness);
    }

    void texturing_tool::change_radius(float change)
    {
      if (_texturing_mode == texturing_mode::paint)
      {
        _radius_slider->setValue(static_cast<float>(_radius_slider->value()) + change);
      }
      else if (_texturing_mode == texturing_mode::swap)
      {
        _texture_switcher->change_radius(change);
      }
      else if (_texturing_mode == texturing_mode::ground_effect)
      {
        _ground_effect_tool->change_radius(change);
      }
    }

    void texturing_tool::change_hardness(float change)
    {
      if (_texturing_mode == texturing_mode::paint)
      {
        _hardness_slider->setValue(static_cast<float>(_hardness_slider->value()) + change);
      }
    }

    void texturing_tool::change_pressure(float change)
    {
      if (_texturing_mode == texturing_mode::paint)
      {
        _pressure_slider->setValue(static_cast<float>(_pressure_slider->value()) + change);
      }
    }

    void texturing_tool::change_brush_level(float change)
    {
      if (_texturing_mode == texturing_mode::paint)
      {
        _brush_level_spin->setValue(std::ceil(_brush_level + change));
      }
    }

    void texturing_tool::set_brush_level (float level)
    {
      if (_texturing_mode == texturing_mode::paint)
      {
        _brush_level_spin->setValue(level);
      }
    }

	void texturing_tool::toggle_brush_level_min_max()
	{
		if(_brush_level_spin->value() > _brush_level_spin->minimum())
			_brush_level_spin->setValue(_brush_level_spin->minimum());
		else _brush_level_spin->setValue(_brush_level_spin->maximum());
	}

    void texturing_tool::change_spray_size(float change)
    {
      if (_texturing_mode == texturing_mode::paint)
      {
        _spray_size_spin->setValue(_spray_size + change);
      }
    }

    void texturing_tool::change_spray_pressure(float change)
    {
      if (_texturing_mode == texturing_mode::paint)
      {
        _spray_pressure_spin->setValue(_spray_pressure + change);
      }
    }

    Noggit::Ui::Tools::UiCommon::ExtendedSlider* texturing_tool::getRadiusSlider()
    {
      return _radius_slider;
    }

    Noggit::Ui::Tools::UiCommon::ExtendedSlider* texturing_tool::getInnerRadiusSlider()
    {
      return _hardness_slider;
    }

    Noggit::Ui::Tools::UiCommon::ExtendedSlider* texturing_tool::getSpeedSlider()
    {
      return _pressure_slider;
    }

    QDial* texturing_tool::getMaskOrientationDial()
    {
      return _image_mask_group->getMaskOrientationDial();
    }

    void texturing_tool::set_pressure(float pressure)
    {
      if (_texturing_mode == texturing_mode::paint)
      {
        _pressure_slider->setValue(pressure);
      }
    }

    float texturing_tool::brush_radius() const
    {
      // show only a dot when using the anim / swap mode
      switch (getTexturingMode())
      {
        case texturing_mode::paint: return static_cast<float>(_radius_slider->value());
        case texturing_mode::swap: return (_texture_switcher->brush_mode() ? _texture_switcher->radius() : 0.f);
        case texturing_mode::ground_effect: return (_ground_effect_tool->brush_mode() != ground_effect_brush_mode::none ? _ground_effect_tool->radius() : 0.f);
        default: return 0.f;
      }
    }

    float texturing_tool::hardness() const
    { 
      switch (getTexturingMode())
      {
        case texturing_mode::paint: return static_cast<float>(_hardness_slider->value());
        default: return 0.f;
      }
    }

    bool texturing_tool::show_unpaintable_chunks() const
    {
        return _show_unpaintable_chunks && getTexturingMode() == texturing_mode::paint;
    }

    void texturing_tool::paint (World* world, glm::vec3 const& pos, float dt, scoped_blp_texture_reference texture)
    {
      if (TabletManager::instance()->isActive())
      {
        set_radius(static_cast<float>(_radius_slider->value()));
        update_brush_hardness();
      }

      switch(getTexturingMode())
      {
        case (texturing_mode::swap):
          {
              auto to_swap(_texture_switcher->texture_to_swap());
              if (to_swap)
              {
                  if (_texture_switcher->brush_mode())
                  {
                      std::cout << _texture_switcher->radius() << std::endl;
                      world->replaceTexture(pos, _texture_switcher->radius(), to_swap.value(), texture, _texture_switcher->entireChunk(), _texture_switcher->entireTile());
                  }
                  else
                  {
                      world->overwriteTextureAtCurrentChunk(pos, to_swap.value(), texture);
                  }
              }
              break;
          }
        case (texturing_mode::paint):
          {
              float strength = 1.0f - pow(1.0f - _pressure_slider->value(), dt * 10.0f);
              if (_spray_mode_group->isChecked())
              {
                  world->sprayTexture(pos, &_spray_brush, alpha_target(), strength, static_cast<float>(_radius_slider->value()), _spray_pressure, texture);

                  if (_inner_radius_cb->isChecked())
                  {
                      if (!_image_mask_group->isEnabled())
                      {
                          world->paintTexture(pos, &_inner_brush, alpha_target(), strength, texture);
                      }
                      else
                      {
                          world->stampTexture(pos, &_inner_brush, alpha_target(), strength, texture, &_mask_image, _image_mask_group->getBrushMode());
                      }
                  }
              }
              else
              {
                  if (!_image_mask_group->isEnabled())
                  {
                      world->paintTexture(pos, &_texture_brush, alpha_target(), strength, texture);
                  }
                  else
                  {
                      world->stampTexture(pos, &_texture_brush, alpha_target(), strength, texture, &_mask_image, _image_mask_group->getBrushMode());
                  }
              }
              break;
          }
        case (texturing_mode::anim):
          {
              change_tex_flag(world, pos, _anim_prop.get(), texture);
              break;
          }
        case (texturing_mode::ground_effect):
        {
              // handled directly in MapView::tick()

              // if (_ground_effect_tool->brush_mode() == ground_effect_brush_mode::exclusion)
              // {
              //     world->paintGroundEffectExclusion(pos, _ground_effect_tool->radius(), );
              // }
              // else if (_ground_effect_tool->brush_mode() == ground_effect_brush_mode::effect)
              // {
              // 
              // }
        }
        default:
        {

        }
      }
    }

    void texturing_tool::update_layer_admission()
    {
      // THE ONE WRITER of the process-wide admission policy. It is process-wide rather than a
      // parameter because the value has to be readable from inside
      // TextureSet::get_texture_index_or_add, four call levels below the brush, and threading it
      // down would change the signature of World::paintTexture, World::stampTexture,
      // MapChunk::paintTexture and MapChunk::stampTexture for what is a tool setting. Noggit
      // already carries the brush's own texture the same way -- Noggit::Ui::selected_texture is a
      // process-wide holder that MapChunk.cpp reads directly.
      Noggit::TextureLayerAdmission admission;

      admission.policy = static_cast<Noggit::LayerFullPolicy>(_layer_full_mode->currentIndex());
      admission.nominated_texture = _nominated_texture->text().toStdString();

      Noggit::TextureLayerAdmission::setCurrent(std::move(admission));
    }

    Brush const& texturing_tool::texture_brush() const
    {
      return _texture_brush;
    }

    float texturing_tool::alpha_target() const
    {
      return static_cast<float>(_brush_level);
    }

    void texturing_tool::change_tex_flag(World* world, glm::vec3 const& pos, bool add, scoped_blp_texture_reference texture)
    {
      std::uint32_t flag = 0;

      auto flag_view = reinterpret_cast<MCLYFlags*>(&flag);

      flag |= FLAG_ANIMATE;

      // if add == true => flag to add, else it's the flags to remove
      if (add)
      {
        // the qdial in inverted compared to the anim rotation
        flag_view->animation_rotation = (_anim_rotation_prop.get() + 4) % 8;
        flag_view->animation_speed = _anim_speed_prop.get();
      }

      // the texture's flag glow is set if the property is true, removed otherwise
      if (_overbright_prop.get())
      {
        flag |= FLAG_GLOW;
      }

      world->change_texture_flag(pos, texture, flag, add);
    }

    texture_swapper* const texturing_tool::texture_swap_tool()
    {
      return _texture_switcher;
    }

    QSize texturing_tool::sizeHint() const
    {
      return QSize(215, height());
    }

    Noggit::Ui::Tools::ImageMaskSelector* texturing_tool::getImageMaskSelector()
    {
      return _image_mask_group;
    }

    QImage* texturing_tool::getMaskImage()
    {
      return &_mask_image;
    }

    texturing_mode texturing_tool::getTexturingMode() const
    {
      if (_ground_effect_tool->isVisible())
        return texturing_mode::ground_effect;
      else
        return _texturing_mode;
    }

    QJsonObject texturing_tool::toJSON()
    {
      QJsonObject json;

      json["brush_action_type"] = "TEXTURING";

      json["current_texture"] = QString(_current_texture->filename().c_str());
      json["hardness"] = _hardness_slider->rawValue();
      json["pressure"] = _pressure_slider->rawValue();
      json["radius"] = _radius_slider->rawValue();
      json["brush_level"] = _brush_level_spin->value();
      json["texturing_mode"] = static_cast<int>(_texturing_mode);
      json["show_unpaintable_chunks"] = _show_unpaintable_chunks_cb->isChecked();
      json["layer_full_mode"] = _layer_full_mode->currentIndex();
      json["layer_nominated_texture"] = _nominated_texture->text();

      json["anim"] = _anim_prop.get();
      json["anim_speed"] = static_cast<int>(_anim_speed_prop.get());
      json["anim_rot"] = static_cast<int>(_anim_rotation_prop.get());
      json["overbright"] = _overbright_prop.get();

      json["mask_enabled"] = _image_mask_group->isEnabled();
      json["brush_mode"] = _image_mask_group->getBrushMode();
      json["randomize_rot"] = _image_mask_group->getRandomizeRotation();
      json["mask_rot"] = _image_mask_group->getRotation();
      json["mask_image"] = _image_mask_group->getImageMaskPath();

      json["spray"] = _spray_mode_group->isChecked();
      json["inner_radius_cb"] = _inner_radius_cb->isChecked();
      json["spray_size"] = _spray_size_spin->value();
      json["spray_pressure"] = _spray_pressure_spin->value();

      if (_texture_switcher->texture_to_swap().has_value())
          json["texture_to_swap"] = _texture_switcher->texture_to_swap().value()->file_key().filepath().c_str();
      else
        json["texture_to_swap"] = "";

      return json;
    }

    void texturing_tool::fromJSON(QJsonObject const& json)
    {
      _current_texture->set_texture(json["current_texture"].toString().toStdString());
      _hardness_slider->setValue(json["hardness"].toDouble());
      _pressure_slider->setValue(json["pressure"].toDouble());
      _radius_slider->setValue(json["radius"].toDouble());
      _brush_level_spin->setValue(json["brush_level"].toInt());

      tabs->setCurrentIndex(json["texturing_mode"].toInt());
      _show_unpaintable_chunks_cb->setChecked(json["show_unpaintable_chunks"].toBool());

      // A preset saved before Smart Paint existed has neither key. QJsonValue::toInt on a missing
      // key returns 0, which is LayerFullPolicy::Skip, and toString returns an empty string, which
      // makes ReplaceNominated behave like Skip -- so an old preset restores today's behaviour
      // rather than arming an eviction the user never asked for.
      _nominated_texture->setText(json["layer_nominated_texture"].toString());
      _layer_full_mode->setCurrentIndex(json["layer_full_mode"].toInt());
      update_layer_admission();

      _anim_prop.set(json["anim"].toBool());
      _anim_speed_prop.set(json["anim_speed"].toInt());
      _anim_rotation_prop.set(json["anim_rot"].toInt());
      _overbright_prop.set(json["overbright"].toBool());

      _image_mask_group->setEnabled(json["mask_enabled"].toBool());
      _image_mask_group->setBrushMode(json["brush_mode"].toInt());
      _image_mask_group->setRandomizeRotation(json["randomize_rot"].toBool());
      _image_mask_group->setRotationRaw(json["mask_rot"].toInt());
      _image_mask_group->setImageMask(json["mask_image"].toString());

      _spray_mode_group->setChecked(json["spray"].toBool());
      _inner_radius_cb->setChecked(json["inner_radius_cb"].toBool());
      _spray_size_spin->setValue(json["spray_size"].toDouble());
      _spray_pressure_spin->setValue(json["spray_pressure"].toDouble());

      auto tex_to_swap_path = json["texture_to_swap"].toString();

      if (!tex_to_swap_path.isEmpty())
        _texture_switcher->set_texture(tex_to_swap_path.toStdString());

    }

    QPushButton* const texturing_tool::heightmappingApplyGlobalButton()
    {
      return _heightmapping_apply_global_btn;
    }

    QPushButton* const texturing_tool::heightmappingApplyAdtButton()
    {
      return _heightmapping_apply_adt_btn;
    }

    texture_heightmapping_data& texturing_tool::getCurrentHeightMappingSetting()
    {
      return textureHeightmappingData;
    }

    // The width of the ramp, and of the widget it fills. It was a bare 35 written three times
    // in paintEvent below and once here; the four had to agree and nothing said so.
    constexpr int OPACITY_GROOVE_WIDTH = 35;

    OpacitySlider::OpacitySlider(Qt::Orientation orientation, QWidget* parent)
      : QSlider(orientation, parent)
    {
      setFixedWidth(OPACITY_GROOVE_WIDTH);

      // paintEvent now picks the grip colour from underMouse(), so this widget has to be told
      // when the pointer arrives and leaves. QSlider repaints on a hover subcontrol change, but
      // only if it is delivered hover events at all, and that depends on WA_Hover having been
      // set -- normally by QStyleSheetStyle at polish time, which is a dependency on the sheet
      // this class exists precisely because it cannot rely on. Stated here instead.
      //
      // The hover state is the WIDGET's, not the handle's: this is a 35px column and lighting
      // the grip whenever the pointer is anywhere in it is the affordance, since anywhere in it
      // is a valid place to click.
      setAttribute(Qt::WA_Hover, true);
    }

    void OpacitySlider::paintEvent(QPaintEvent* event)
    {
      // QSlider::paintEvent(event);

      // chat-gpt code, can probably be improved...

      QPainter p(this);
      QStyleOptionSlider opt;
      initStyleOption(&opt);
      opt.subControls = QStyle::SC_SliderGroove | QStyle::SC_SliderHandle;
      if (tickPosition() != NoTicks)
        opt.subControls |= QStyle::SC_SliderTickmarks;
      if (isSliderDown()) {
        opt.activeSubControls = QStyle::SC_SliderHandle;
        opt.state |= QStyle::State_Sunken;
      }
      else {
        opt.activeSubControls = QStyle::SC_None;
      }

      // THE ONE GRADIENT IN THE DESIGN SYSTEM, and it is here because this groove is DATA:
      // it is the brush opacity ramp, so the track itself has to show the value the handle is
      // pointing at. Everywhere else a groove is a rail and is flat BG_VOID.
      //
      // The stops were Qt::black and Qt::white, i.e. #000000 and #FFFFFF. The palette forbids
      // both -- pure black and pure white are what stop an interface staying legible beside a
      // bright 3D viewport -- so the ramp runs RAMP_LO to RAMP_HI instead. That is a 1.4% and a
      // 5.1% trim off the two ends; the ramp still spans 17.36:1 and no user can read the
      // difference in the ramp, which is the point of picking the near-extremes rather than
      // visibly grey ones.
      //
      // IT IS NOW A ROUNDED, STROKED WELL rather than a bare fillRect. The theme has always
      // declared `border: 1px solid #565049; border-radius: 5px` on this groove and NONE of it
      // was ever painted -- p.fillRect draws square corners and no pen, so the shipped ramp was
      // a hard-edged rectangle sitting inside a panel where every other control has a 5px
      // radius and a 1px border. Design::RADIUS_CONTROL is 5 and is the same number the sheet
      // names, so the two finally agree on the shape. They no longer agree on the COLOUR, and
      // deliberately -- see the next paragraph. Nothing is lost by that: this paintEvent never
      // asks the style to draw the groove, so the sheet's `border` declaration on it is a
      // statement of intent that has never put a pixel on screen. The pen below is the only
      // thing that does.
      //
      // THE OUTLINE IS Design::EDGE AND IT NOW CLEARS THE FLOOR. It was Design::STROKE, and the
      // note that stood here conceded the failure rather than fixing it: STROKE measures 6.991:1
      // against the pale end of the ramp but only 2.484:1 against the dark end and 1.894:1
      // against the panel behind it, i.e. under 3:1 on two of the three things it has to be seen
      // against. It was written that way because DesignTokens.hpp had no token that did better;
      // theme.qss had already added edge #8A8378 as "the visible edge of an enabled control" and
      // demoted stroke out of that role, and the C++ half of the palette had not followed.
      //
      // EDGE, measured on the three surfaces this one pen touches: 5.269:1 on RAMP_LO, 3.296:1
      // on RAMP_HI, 4.018:1 on BG_PANEL. All three clear 3:1, where STROKE cleared one. So the
      // outline stops being an admitted exception and becomes an ordinary control edge, which is
      // what it always was -- the excuse it used to carry (that the RAMP's own 17.364:1 span is
      // the real identification, so the outline may be under the floor) was true but was doing
      // work no longer needed.
      //
      // The ratio moves in the right direction at both ends at once, which is the point of
      // picking a mid-light neutral: STROKE was strong on the pale end and invisible on the dark
      // one, EDGE is 3.3 to 5.3 across the whole track.
      //
      // The right edge also stops being a pixel wrong. setRight is INCLUSIVE, so the old
      // setLeft(0)/setRight(35) pair described a 36px rect on a 35px widget and the -1 in the
      // fillRect adjust was silently correcting for it.
      QRect grooveRect = style()->subControlRect(QStyle::CC_Slider, &opt, QStyle::SC_SliderGroove, this);
      grooveRect.setLeft((width() - OPACITY_GROOVE_WIDTH) / 2);
      grooveRect.setWidth(OPACITY_GROOVE_WIDTH);

      QLinearGradient gradient(grooveRect.topLeft(), grooveRect.bottomLeft());
      gradient.setColorAt(0, Design::color(Design::RAMP_LO));
      gradient.setColorAt(1, Design::color(Design::RAMP_HI));

      p.setRenderHint(QPainter::Antialiasing, true);
      p.setBrush(gradient);
      p.setPen(QPen(Design::color(Design::EDGE), 1.0));
      // The half-pixel inset centres a 1px cosmetic pen on the boundary rather than straddling
      // two rows, so the stroked box is exactly OPACITY_GROOVE_WIDTH wide.
      p.drawRoundedRect( QRectF(grooveRect).adjusted(0.5, 0.5, -0.5, -0.5)
                       , Design::RADIUS_CONTROL
                       , Design::RADIUS_CONTROL
                       );

      // THE HANDLE, and why it needs a ring where no other handle in the application does.
      //
      // It was a pure-red 35x5 rectangle -- the crimson overload in its purest form, sitting on
      // the one control where red cannot mean anything, since this slider has no error state
      // and nothing here is destructive. It is now ACCENT, which is what the design system uses
      // for "the thing you are acting on" and is exactly what a slider grip is.
      //
      // But a flat accent grip is unreadable at one end of this particular track, and that is
      // measured, not suspected: ACCENT against RAMP_LO is 8.996:1 and against RAMP_HI only
      // 1.930:1. Drag the opacity to full and a borderless gold grip would vanish into the pale
      // end of its own ramp. The INK ring is therefore load-bearing rather than decoration: INK
      // against RAMP_HI is 16.932:1 and against ACCENT 8.772:1, so at the light end the RING
      // carries the grip and at the dark end the CORE does, and nowhere on the range does the
      // grip drop below 5.640:1 against something adjacent to it.
      //
      // WHAT CHANGED, and it is geometry as much as colour.
      //
      // The pill is 33 x 12 with a 2px ring and a 6px radius, replacing 34 x 6 with a 1px ring
      // and a 2px radius. 12px doubles the grab band on a control that is dragged rather than
      // clicked, and a 6px radius is half of 12, so the cap is a full semicircle instead of a
      // barely-rounded corner.
      //
      // THE ARITHMETIC THAT USED TO BE HERE WAS WRONG AND IS CORRECTED RATHER THAN DELETED,
      // because it is exactly the sum that looks like it checks out. It read: "33 + 2 x 1px of
      // ring is exactly the 35px widget, where 34 + 2 was 36 and overhung it." Two errors:
      //
      //   * THE RING IS NOT ADDED TO HANDLE_WIDTH, it is taken out of it. handle_box below is
      //     inset by HANDLE_RING / 2 on every side and shrunk by HANDLE_RING on both axes, so a
      //     pen centred on that path lands its OUTER edge exactly on HANDLE_WIDTH x
      //     HANDLE_HEIGHT. Worked through for a 35px widget: x0 = (35 - 33) / 2 + 1 = 2.0,
      //     width = 33 - 2 = 31, so the path runs x 2.0 -> 33.0 and the 2px pen covers
      //     1.0 -> 34.0. That is 33px of paint with ONE bare pixel of widget either side, not a
      //     flush 35. The same construction makes the pill exactly 12px tall.
      //   * THE OLD PILL DID NOT OVERHANG EITHER. HANDLE_WIDTH 34, a flat 0.5 inset, width
      //     34 - 1 = 33 and a 1px pen put the outer edge at 0.5 -> 34.5: 34px inside 35, half a
      //     pixel of bare widget each side. It fitted. The 36 in the old sentence is theme.qss's
      //     BOX for this handle -- 34 of content plus 1px of border on each side -- which the
      //     sheet states correctly and which never described what this function drew.
      //
      // So the change here is 34x6 -> 33x12: one pixel narrower, twice as tall. The theme's own
      // numbers for this handle described an 8px-tall box while the C++ drew 6, i.e. the band
      // that grabbed and the pill that was drawn disagreed by a pixel top and bottom; the sheet
      // is being brought to these figures in the same pass (see the theme's
      // texturing_brush_level_slider block).
      //
      // The core is now picked by STATE. It was a hardcoded ACCENT, so this slider had no hover
      // and no pressed feedback at all -- paintEvent never read underMouse() or isSliderDown()
      // for colour, even though it already read isSliderDown() for the style option. Measured,
      // every core against the ink ring and against both ends of the ramp it slides over:
      //
      //                        vs ink ring   vs ramp.lo   vs ramp.hi
      //   ACCENT       rest       8.772        8.996        1.930
      //   ACCENT_HI    hover     10.854       11.131        1.560
      //   ACCENT_PRESS press      5.640        5.784        3.002
      //   STROKE       disabled   2.422        2.484        6.991   (exempt, SC 1.4.11)
      //
      // The inset is HANDLE_RING / 2 rather than the old flat 0.5, and that half-of-the-pen is
      // what makes the outer edge land on HANDLE_WIDTH exactly, as derived above. Drop it and
      // the 2px ring straddles the boundary instead: x0 = (35 - 33) / 2 = 1.0 with width 33
      // strokes 0.0 -> 35.0, a pill flush to both edges of the widget with no bare pixel left.
      // The radius loses the same half -- 6 - 1 = 5 on the path, plus 1px of pen outside it --
      // so the OUTER corner keeps HANDLE_RADIUS.
      // FOCUS IS NEW, AND IT IS THE ONE STATE THIS GRIP WAS STILL MISSING. QSlider is
      // Qt::StrongFocus, so this control is tab-reachable like every other slider in the
      // editor, and until now it drew identically focused and unfocused -- the paintEvent read
      // isEnabled, isSliderDown and underMouse and never hasFocus. The theme's grip for the
      // ExtendedSlider rows gained a focus rule in the same pass that gave it hover and pressed
      // ones, so the styled sliders answered the keyboard and the painted one did not.
      //
      // IT IS BUILT THE SAME WAY THE STYLED GRIP'S FOCUS RULE IS: the core inverts to ink and
      // the ring becomes the accent, so the two colours swap roles rather than one of them
      // getting louder. That construction is the only one that survives a groove which is a
      // VALUE RAMP, and this is why -- measured against both ends of the ramp, WCAG 2.1 sRGB,
      // (Lmax + 0.05) / (Lmin + 0.05):
      //
      //                         vs ramp.lo   vs ramp.hi
      //   ink core                  1.026      16.932
      //   accent ring               8.996       1.930
      //
      // Neither colour is legible at both ends and the pair always is: at the dark end the
      // accent ring carries the grip at 8.996:1 and at the pale end the ink core carries it at
      // 16.932:1, with ring against core a constant 8.772:1. Against the UNFOCUSED appearance
      // the core alone moves ink against accent, 8.772:1, far over the 3:1 WCAG 2.1 SC 2.4.13
      // asks of a focus indicator.
      //
      // PRECEDENCE MATCHES THE SHEET'S, which orders its four state rules focus, hover,
      // pressed, disabled and therefore resolves them disabled > pressed > hover > focus. The
      // chain below is in that order, so a focused grip that is also being dragged reads as
      // pressed, which is what the styled ones do.
      //
      // THE RING GROWS BY A PIXEL AND THE PILL DOES NOT MOVE. The whole point of the
      // handle_ring / 2 inset derived below is that the outer edge of the stroke lands on
      // HANDLE_WIDTH x HANDLE_HEIGHT whatever the pen width is, so a 3px focus ring is drawn
      // inside the same 33 x 12 box as the 2px resting one. Worked through for the 35px widget:
      // x0 = (35 - 33) / 2 + 1.5 = 2.5 and width = 33 - 3 = 30, so the path runs 2.5 -> 32.5
      // and a 3px pen covers 1.0 -> 34.0 -- the same 33 painted pixels with the same one bare
      // pixel either side that the 2px ring produces. Vertically, y0 = top + 1.5 and height
      // 12 - 3 = 9 puts the pen's outer edge on top and top + 12 exactly. The radius loses the
      // same half, 6 - 1.5 = 4.5, so the OUTER corner is still HANDLE_RADIUS. Nothing about the
      // grabbable band changes: that comes from the sheet's declared handle box, not from here.
      //
      // ONE DEPENDENCY OUTSIDE THIS FILE, and it is live right now rather than hypothetical.
      // Every colour here is a Design:: token, and Design::ACCENT is what makes this grip agree
      // with the styled ones. If the sheet's accent moves and DesignTokens.hpp does not move
      // with it, this grip keeps the OLD accent while every QSlider next to it takes the new
      // one -- and so do the other painted sites, minimap_widget and SpawnTilePicker. The two
      // lists have no compile-time link; DesignTokens.hpp says so itself at "KEEP THIS BLOCK
      // EQUAL TO theme.qss's palette header".
      constexpr qreal HANDLE_WIDTH = 33.0;
      constexpr qreal HANDLE_HEIGHT = 12.0;
      constexpr qreal HANDLE_RADIUS = 6.0;
      constexpr qreal HANDLE_RING = 2.0;
      constexpr qreal HANDLE_RING_FOCUS = 3.0;

      bool const focused (isEnabled() && !isSliderDown() && !underMouse() && hasFocus());

      char const* const handle_core
        ( !isEnabled()   ? Design::STROKE
        : isSliderDown() ? Design::ACCENT_PRESS
        : underMouse()   ? Design::ACCENT_HI
        : focused        ? Design::INK
                         : Design::ACCENT
        );

      char const* const handle_ring_color (focused ? Design::ACCENT : Design::INK);
      qreal const handle_ring (focused ? HANDLE_RING_FOCUS : HANDLE_RING);

      // CENTRED ON THE STYLE'S HANDLE RECT, NOT ANCHORED TO ITS TOP, and that is the one line
      // here that is defensive rather than cosmetic. The rect comes from QStyleSheetStyle,
      // i.e. from theme.qss, and the pill drawn on top of it comes from this file -- two owners
      // for one control. Anchoring to the top meant the two agreed only while the sheet's
      // declared handle box was exactly HANDLE_HEIGHT, and it was NOT: the shipped sheet
      // described an 8px box while this function drew 6, so the band that grabbed and the pill
      // that was drawn were a pixel out top and bottom.
      //
      // Sharing a CENTRE instead makes the pill point at the right value whatever height the
      // sheet declares, and the clamp keeps it inside the widget if the sheet ever declares a
      // band shorter than the pill -- flush at the ends rather than sliced by the clip.
      QRect handleRect = style()->subControlRect(QStyle::CC_Slider, &opt, QStyle::SC_SliderHandle, this);

      qreal const handle_center (handleRect.top() + handleRect.height() / 2.0);
      qreal const handle_top
        ( qBound ( 0.0
                 , handle_center - HANDLE_HEIGHT / 2.0
                 , qMax (0.0, qreal (height()) - HANDLE_HEIGHT)
                 )
        );

      QRectF const handle_box
        ( (width() - HANDLE_WIDTH) / 2.0 + handle_ring / 2.0
        , handle_top + handle_ring / 2.0
        , HANDLE_WIDTH - handle_ring
        , HANDLE_HEIGHT - handle_ring
        );

      p.setBrush(Design::color(handle_core));
      p.setPen(QPen(Design::color(handle_ring_color), handle_ring));
      p.drawRoundedRect( handle_box
                       , HANDLE_RADIUS - handle_ring / 2.0
                       , HANDLE_RADIUS - handle_ring / 2.0
                       );
      p.setRenderHint(QPainter::Antialiasing, false);

      // Draw the ticks if needed
      if (tickPosition() != NoTicks) {
        opt.subControls = QStyle::SC_SliderTickmarks;
        style()->drawComplexControl(QStyle::CC_Slider, &opt, &p, this);
      }

      // QSlider::paintEvent() source code :
      /*
      Q_D(QSlider);
      QPainter p(this);
      QStyleOptionSlider opt;
      initStyleOption(&opt);
      opt.subControls = QStyle::SC_SliderGroove | QStyle::SC_SliderHandle;
      if (d->tickPosition != NoTicks)
      opt.subControls |= QStyle::SC_SliderTickmarks;
      if (d->pressedControl) {
      opt.activeSubControls = d->pressedControl;
      opt.state |= QStyle::State_Sunken;
      }
      else {
      opt.activeSubControls = d->hoverControl;
      }
      style()->drawComplexControl(QStyle::CC_Slider, &opt, &p, this);
      */
    }
}
}
