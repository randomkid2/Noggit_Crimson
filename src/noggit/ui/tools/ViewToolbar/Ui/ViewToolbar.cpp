// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/BoolToggleProperty.hpp>
#include <noggit/MapView.h>
#include <noggit/ui/DesignTokens.hpp>
#include <noggit/ui/FontAwesome.hpp>
#include <noggit/ui/tools/ActionHistoryNavigator/ActionHistoryNavigator.hpp>
#include <noggit/ui/tools/ViewToolbar/Ui/ViewToolbar.hpp>
#include <noggit/World.h>

#include <QCheckBox>
#include <QDialog>
#include <QFontMetrics>
#include <QSize>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QtCore/QSettings>

#include <algorithm>

using namespace Noggit::Ui;
using namespace Noggit::Ui::Tools::ViewToolbar::Ui;

class SliderAction : public QWidgetAction
{
public:
  template <typename T>
  SliderAction(const QString& title, int min, int max, int value, const QString& prec,
    std::function<T(T v)> func)
    : QWidgetAction(NULL)
  {
    static_assert (std::is_arithmetic<T>::value, "<T>SliderAction - T must be numeric");

    QWidget* _widget = new QWidget(NULL);
    QHBoxLayout* _layout = new QHBoxLayout();
    // S1 is the design system's "inside a unit" step; the 3 that was here is off the 4px
    // scale. setMargin has been deprecated since Qt 5.13.
    _layout->setSpacing(Noggit::Ui::Design::S1);
    _layout->setContentsMargins(0, 0, 0, 0);
    QLabel* _label = new QLabel(title);

    // The read-out text was built twice, here and again in the valueChanged handler, and the
    // two spellings had to stay identical by hand. One formatter now, so the initial value and
    // every later one cannot drift. Nothing about what it produces changed: same func, same
    // 'f'/2 formatting, same leading space before the unit, same tr() call on the same literal.
    auto const format
      ( [prec, func] (int slider_value) -> QString
        {
          return QString::number (func (static_cast<T> (slider_value)), 'f', 2)
               + tr (" %1").arg (prec);
        }
      );

    QLabel* _display = new QLabel (format (value));

    // Right-aligned and pinned to the widest reading the formatter can produce.
    //
    // The row is label / slider / read-out inside a QHBoxLayout, and the SLIDER is the only
    // expanding item in it -- so every digit the number gained or lost was paid for by the
    // slider, which resized under the pointer in the middle of a drag. On the climb bar that
    // is a one-character swing (0.00 -> 89.00 degrees) and therefore a visible twitch every
    // time the value crosses 10 or 100. Reserving the space up front removes the reflow
    // without touching the range, the step or the emitted value.
    //
    // The widest reading is MEASURED across the range rather than assumed to sit at an
    // endpoint: func is caller-supplied and need not be monotonic in string length -- the
    // climb one converts radians to degrees and rounds to int, so where its text is widest is
    // not something this constructor can know. Eleven samples, once, at construction.
    _display->setAlignment (Qt::AlignRight | Qt::AlignVCenter);

    {
      QFontMetrics const metrics (_display->fontMetrics());
      int widest (0);

      for (int i (0); i <= 10; ++i)
      {
        widest = std::max
          (widest, metrics.horizontalAdvance (format (min + (max - min) * i / 10)));
      }

      _display->setMinimumWidth (widest);
    }

    _slider = new QSlider(NULL);
    _slider->setOrientation(Qt::Horizontal);
    _slider->setMinimum(min);
    _slider->setMaximum(max);
    _slider->setValue(value);

    // A floor, not a fixed width -- the slider still expands to fill whatever the popup gives
    // it. Without one it fell back to the style's size hint, which is around 84px, and the
    // climb slider spans 1571 steps: 19 units of travel per pixel, on a control whose whole
    // job is to be aimed. 160px halves that. Range, step and page step are untouched; only how
    // much screen the same travel is spread over changes.
    _slider->setMinimumWidth (160);

    connect(_slider, &QSlider::valueChanged, [_display, format](int value)
      {
        _display->setText (format (value));
      });

    _layout->addWidget(_label);
    _layout->addWidget(_slider);
    _layout->addWidget(_display);
    _widget->setLayout(_layout);

    setDefaultWidget(_widget);
  }

  QSlider* slider() { return _slider; }

private:
  QSlider* _slider;
};

// The glyph size for every button on the three floating bars this class builds.
//
// NOTHING SET THIS BEFORE -- not this file, not MapView, and not the theme, which sets
// qproperty-iconSize on the frameless window controls and the dock buttons but never on a tool
// bar. So the size came from QStyle::PM_ToolBarIconSize, a per-style platform default that has
// no idea what these buttons are.
//
// MEASURED with a standalone Qt 5.15.2 probe rather than assumed: on this machine the style is
// windowsvista, PM_ToolBarIconSize is 36, and QToolBar::iconSize() reports 36x36 both with and
// without the CrimsonSlate sheet applied. The design system specifies a 20px icon inside a
// 34x34 tool button and the sheet's rule leaves a content box of about 22px after its 1px
// border and 3-5px padding, so every one of the twenty toggle glyphs was rasterised at 36px and
// then resampled down to roughly 22 -- a 61% reduction, which for a FONT GLYPH is the whole
// difference between a crisp stem and a soft one. These are FontNoggitIconEngine icons and the
// engine rasterises at exactly rect.height(), so this number is the glyph's real pixel size and
// not a hint.
//
// Declared once and applied in all three constructors rather than refactored into a shared
// init, because the three differ in orientation and size policy and merging them would change
// more than appearance.
namespace
{
  constexpr int TOOLBAR_ICON_EXTENT = 20;
}

ViewToolbar::ViewToolbar(MapView* mapView)
  : _tool_group(this)
{
  setContextMenuPolicy(Qt::PreventContextMenu);
  setAllowedAreas(Qt::TopToolBarArea | Qt::BottomToolBarArea);
  setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Maximum);
  setIconSize(QSize(TOOLBAR_ICON_EXTENT, TOOLBAR_ICON_EXTENT));

  IconAction* climb_icon = new IconAction(FontNoggitIcon{FontNoggit::VISIBILITY_CLIMB });

  CheckBoxAction* climb_use_output_color_angle = new CheckBoxAction(tr("Display all angle color"));
  climb_use_output_color_angle->checkbox()->setChecked(false);
  connect(climb_use_output_color_angle->checkbox(), &QCheckBox::toggled, [mapView](bool checked)
          {
              mapView->getWorld()->renderer()->getTerrainParamsUniformBlock()->climb_use_output_angle = checked;
              mapView->getWorld()->renderer()->markTerrainParamsUniformBlockDirty();
          });

  CheckBoxAction* climb_use_smooth_interpolation = new CheckBoxAction(tr("Smooth"));
  climb_use_smooth_interpolation->setChecked(false);
  connect(climb_use_smooth_interpolation->checkbox(), &QCheckBox::toggled, [mapView](bool checked)
          {
              mapView->getWorld()->renderer()->getTerrainParamsUniformBlock()->climb_use_smooth_interpolation = checked;
              mapView->getWorld()->renderer()->markTerrainParamsUniformBlockDirty();
          });

  SliderAction* climb_value = new SliderAction(tr("Climb maximum value"), 0, 1570, 856, tr("degrees"),
      std::function<int(int v)>() = [&](int v) {
          float radian = float(v) / 1000.f;
          float degrees = radian * (180.0 / 3.141592653589793238463);
          return int(degrees);
      });

  connect(climb_value->slider(), &QSlider::valueChanged, [mapView](int value)
          {
              float radian = float(value) / 1000.0f;
              mapView->getWorld()->renderer()->getTerrainParamsUniformBlock()->climb_value = radian;
              mapView->getWorld()->renderer()->markTerrainParamsUniformBlockDirty();
          });

  PushButtonAction* climb_reset_slider = new PushButtonAction(tr("Reset"));
  connect(climb_reset_slider->pushbutton(), &QPushButton::clicked, [climb_value]()
          {
              climb_value->slider()->setValue(856);
          });

  _climb_secondary_tool.push_back(climb_icon);
  _climb_secondary_tool.push_back(climb_use_smooth_interpolation);
  _climb_secondary_tool.push_back(climb_use_output_color_angle);
  _climb_secondary_tool.push_back(climb_value);
  _climb_secondary_tool.push_back(climb_reset_slider);


  // Time toolbar
  IconAction* time_icon = new IconAction(FontNoggitIcon{ FontNoggit::TIME_PAUSE });

  PushButtonAction* pause_time = new PushButtonAction(tr("Pause Time"));
  connect(pause_time->pushbutton(), &QPushButton::clicked, [pause_time]()
      {

      });

  PushButtonAction* speed_up_time = new PushButtonAction(tr("Increase Time speed"));
  connect(climb_reset_slider->pushbutton(), &QPushButton::clicked, [pause_time]()
      {

      });

  _time_secondary_tool.push_back(time_icon);
  /*
  ADD_ACTION(view_menu, "Increase time speed", Qt::Key_N, [this] { mTimespeed += 90.0f; });
  ADD_ACTION(view_menu, "Decrease time speed", Qt::Key_B, [this] { mTimespeed = std::max(0.0f, mTimespeed - 90.0f); });
  ADD_ACTION(view_menu, "Pause time", Qt::Key_J, [this] { mTimespeed = 0.0f; });
  */

}

ViewToolbar::ViewToolbar(MapView *mapView, ViewToolbar *tb)
    : _tool_group(this)
{
    setContextMenuPolicy(Qt::PreventContextMenu);
    setAllowedAreas(Qt::TopToolBarArea | Qt::BottomToolBarArea);
    setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Maximum);
    setIconSize(QSize(TOOLBAR_ICON_EXTENT, TOOLBAR_ICON_EXTENT));

    add_tool_icon(mapView, &mapView->_draw_models, tr("Doodads"),
                  tr("Draw M2 models placed on the terrain."), tr("F1"), FontNoggit::VISIBILITY_DOODADS, tb);
    add_tool_icon(mapView, &mapView->_draw_wmo, tr("WMOs"),
                  tr("Draw world model objects -- buildings, caves, bridges."), tr("F6"), FontNoggit::VISIBILITY_WMO, tb);
    add_tool_icon(mapView, &mapView->_draw_wmo_doodads, tr("WMO doodads"),
                  tr("Draw the M2 models a WMO carries inside it."), tr("F2"), FontNoggit::VISIBILITY_WMO_DOODADS, tb);
    add_tool_icon(mapView, &mapView->_draw_wmo_exterior, tr("WMO exterior"),
                  tr("Draw the outside faces of world model objects."), QString(), FontNoggit::UI_TOGGLE, tb);
    add_tool_icon(mapView, &mapView->_draw_terrain, tr("Terrain"),
                  tr("Draw the ADT terrain mesh."), tr("F3"), FontNoggit::VISIBILITY_TERRAIN, tb);
    add_tool_icon(mapView, &mapView->_draw_water, tr("Water"),
                  tr("Draw water, lava and slime surfaces."), tr("F4"), FontNoggit::VISIBILITY_WATER, tb);

    addSeparator();

    add_tool_icon(mapView, &mapView->_draw_lines, tr("Lines"),
                  tr("Draw the ADT tile and chunk grid over the terrain."), tr("F7"), FontNoggit::VISIBILITY_LINES, tb);
    add_tool_icon(mapView, &mapView->_draw_hole_lines, tr("Hole lines"),
                  tr("Outline the holes cut in the terrain."), tr("Shift+F1"), FontNoggit::VISIBILITY_HOLE_LINES, tb);
    add_tool_icon(mapView, &mapView->_draw_wireframe, tr("Wireframe"),
                  tr("Draw the terrain mesh edges over the surface."), tr("F10"), FontNoggit::VISIBILITY_WIREFRAME, tb);
    add_tool_icon(mapView, &mapView->_draw_contour, tr("Contours"),
                  tr("Shade the terrain by height with contour bands."), tr("F9"), FontNoggit::VISIBILITY_CONTOURS, tb);
    add_tool_icon(mapView, &mapView->_draw_climb, tr("Climb"),
                  tr("Colour the terrain by slope, showing what is walkable."), tr("Shift+F2"), FontNoggit::VISIBILITY_CLIMB, tb, tb->_climb_secondary_tool);
    add_tool_icon(mapView, &mapView->_draw_vertex_color, tr("Vertex Color"),
                  tr("Apply the per-vertex colour layer (MCCV) to the terrain."), tr("Shift+F3"), FontNoggit::VISIBILITY_VERTEX_PAINTER, tb);
    add_tool_icon(mapView, &mapView->_draw_baked_shadows, tr("Baked Shadows"),
                  tr("Apply the shadow map baked into each chunk."), tr("Shift+F4"), FontNoggit::VISIBILITY_BAKED_SHADOWS, tb); // TODO : better icon

    addSeparator();

    add_tool_icon(mapView, &mapView->_draw_model_animations, tr("Animations"),
                  tr("Play model animations in the viewport."), tr("F11"), FontNoggit::VISIBILITY_ANIMATION, tb);
    add_tool_icon(mapView, &mapView->_draw_fog, tr("Fog"),
                  tr("Apply distance fog as the client would."), tr("F12"), FontNoggit::VISIBILITY_FOG, tb);
    // The "currently doesn't work" note used to sit in the ACTION TEXT, so it went into the
    // button's accessible name and its fallback tooltip, and the newline inside it made the
    // primary bar advertise a broken feature over two lines. It is a caveat about the feature,
    // so it belongs in the description.
    add_tool_icon(mapView, &mapView->_draw_mfbo, tr("Flight bounds"),
                  tr("Draw the MFBO flight ceiling and floor. Not currently working."), QString(), FontNoggit::VISIBILITY_FLIGHT_BOUNDS, tb);
    // add_tool_icon(mapView, &mapView->_draw_lights_zones, tr("Light zones"), FontNoggit::VISIBILITY_LIGHT, tb);
    addSeparator();

    // Hole lines always on
    add_tool_icon(mapView, &mapView->_draw_models_with_box, tr("Models with box"),
                  tr("Draw each model's bounding box alongside it."), QString(), FontNoggit::VISIBILITY_WITH_BOX, tb);
    add_tool_icon(mapView, &mapView->_draw_hidden_models, tr("Hidden models"),
                  tr("Draw models that have been hidden from the viewport."), QString(), FontNoggit::VISIBILITY_HIDDEN_MODELS, tb);
    addSeparator();
    /*
    auto tablet_sensitivity = new QSlider(this);
    tablet_sensitivity->setOrientation(Qt::Horizontal);
    addWidget(tablet_sensitivity);
   */

    // some unused icons : 
    // VISIBILITY_LIGHT VISIBILITY_GROUNDEFFECTS CAMERA_TURN CAMERA_SPEED_FASTER.. INFO TIME_NORMAL VIEW_AXIS VISIBILITY_UNUSED SETTINGS

    // normal view mode icon, and make them only 1 at a time out of the 3 view modes? 
    // add_tool_icon(mapView, &mapView->_game_mode_camera, tr("Normal view"), FontNoggit::VIEW_AXIS, tb);
    // add_tool_icon(mapView, &mapView->_game_mode_camera, tr("Game view"), FontNoggit::VIEW_MODE_GAME, tb);
    // add_tool_icon(mapView, &mapView->_game_mode_camera, tr("Tile view"), FontNoggit::VIEW_MODE_2D, tb);
    // addSeparator();

    add_tool_icon(mapView, &mapView->_show_minimap_window, tr("Show Minimap"),
                  tr("Show the minimap dock and its tile overview."), tr("M"), FontNoggit::TOOL_MINIMAP_EDITOR, tb);
    add_tool_icon(mapView, &mapView->_show_detail_info_window, tr("Details info"),
                  tr("Show the detail panel for whatever is selected."), tr("F8"), FontNoggit::INFO, tb);

    // TODO : will open a panel with time controls, or use 2n toolbar
    //add_tool_icon(mapView, &mapView->_game_mode_camera, tr("Time speed"), FontNoggit::TIME_NORMAL, tb, _time_secondary_tool);

    /*
    auto tile_view_btn = new QPushButton(this);
    tile_view_btn->setIcon(FontNoggitIcon{ FontNoggit::VIEW_MODE_2D });
    tile_view_btn->setToolTip("2D View");
    addWidget(tile_view_btn);
    */

    // Was a QPushButton handed to QToolBar::addWidget, which meant the twenty-first control on
    // this bar was the one widget on it that no QToolButton rule could reach: different
    // padding, different minimum height, different hover plate, visibly out of line with its
    // twenty neighbours. As an action it is a QToolButton like the rest and takes the bar's own
    // rules with no style sheet needed.
    auto undo_stack_action = addAction(FontAwesomeIcon(FontAwesome::undo), tr("History"));
    undo_stack_action->setToolTip(QString("<b>%1</b><br/>%2")
                                    .arg(tr("History").toHtmlEscaped(),
                                         tr("Show the undo stack for this session.").toHtmlEscaped()));

    auto undo_stack_popup = new QDialog(this);
    undo_stack_popup->setMinimumWidth(160);
    undo_stack_popup->setMinimumHeight(300);
    undo_stack_popup->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    auto layout = new QVBoxLayout(undo_stack_popup);
    layout->setContentsMargins(0, 0, 0, 0);
    auto action_navigator = new Noggit::Ui::Tools::ActionHistoryNavigator(undo_stack_popup);
    action_navigator->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    action_navigator->setMinimumWidth(160);
    action_navigator->setMinimumHeight(300);
    // layout->addWidget(undo_stack_popup);
    layout->addWidget(action_navigator);

    // Set ONCE, here, rather than on every click. Changing a widget's window flags destroys and
    // recreates its native window; doing that from the click handler threw away the platform
    // window and rebuilt it every single time the popup was opened. The flag never changes, so
    // it belongs at construction -- and the forced repaint() that used to follow adjustSize()
    // on a widget that is not yet visible painted nothing and is gone with it.
    undo_stack_popup->setWindowFlags(Qt::Popup);
    undo_stack_popup->updateGeometry();
    undo_stack_popup->adjustSize();
    undo_stack_popup->setVisible(false);

    connect(undo_stack_action, &QAction::triggered,
            [=]()
            {
                // Resolved at click time rather than captured: QToolBar owns the button it
                // builds for an action and may rebuild it, so asking each time is the only way
                // to be sure the anchor is the widget actually on screen.
                QWidget* const button (widgetForAction(undo_stack_action));

                if (!button)
                    return;

                QPoint const new_pos (mapToGlobal(QPoint(button->pos().x(),
                                                         button->pos().y() + button->height())));

                undo_stack_popup->setGeometry(new_pos.x(),
                                              new_pos.y(),
                                              undo_stack_popup->width(),
                                              undo_stack_popup->height());

                undo_stack_popup->show();
            });
}

ViewToolbar::ViewToolbar(MapView* mapView, editing_mode mode)
    : _tool_group(this)
    , current_mode(mode)
{
    setContextMenuPolicy(Qt::PreventContextMenu);
    setAllowedAreas(Qt::TopToolBarArea | Qt::BottomToolBarArea);
    setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Minimum);
    setIconSize(QSize(TOOLBAR_ICON_EXTENT, TOOLBAR_ICON_EXTENT));
    setOrientation(Qt::Vertical);
    mapView->getLeftSecondaryToolbar()->hide();

    {
        /*
         * FLATTEN/BLUE SECONDARY TOOL 
         */

        SubToolBarAction* _toolbar = new SubToolBarAction();

        {
            IconAction* _icon = new IconAction(FontNoggitIcon{ FontNoggit::TOOL_FLATTEN_BLUR });

            CheckBoxAction* _raise = new CheckBoxAction(tr("Raise"), true);
            connect(_raise->checkbox(), &QCheckBox::stateChanged, [this, mapView](int state)
                {
                    emit updateStateRaise(state != 0);
                });

            CheckBoxAction* _lower = new CheckBoxAction(tr("Lower"), true);
            connect(_lower->checkbox(), &QCheckBox::stateChanged, [this, mapView](int state)
                {
                    emit updateStateLower(state != 0);
                });


            _toolbar->ADD_ACTION(_icon);
            _toolbar->ADD_ACTION(_raise); raise_index = 1;
            _toolbar->ADD_ACTION(_lower); lower_index = 2;
            _toolbar->SETUP_WIDGET(false);
        }

        _flatten_secondary_tool.push_back(_toolbar);
    }

    {
        /*
         * TEXTURE PAINTER SECONDARY TOOL
         */

        SubToolBarAction* _toolbar = new SubToolBarAction();

        {
            IconAction* _icon = new IconAction(FontNoggitIcon{ FontNoggit::TOOL_TEXTURE_PAINT });

            CheckBoxAction* _unpaintable_chunk = new CheckBoxAction(tr("Unpaintable chunk"));
            connect(_unpaintable_chunk->checkbox(), &QCheckBox::toggled, [mapView](bool checked)
                    {
                        mapView->getWorld()->renderer()->getTerrainParamsUniformBlock()->draw_paintability_overlay = checked;
                        mapView->getWorld()->renderer()->markTerrainParamsUniformBlockDirty();
                    });

            _toolbar->ADD_ACTION(_icon);
            _toolbar->ADD_ACTION(_unpaintable_chunk); unpaintable_chunk_index = 1;
            _toolbar->SETUP_WIDGET(false);
        }

        _texture_secondary_tool.push_back(_toolbar);
    }

    {
        /*
         * OBJECT SECONDARY TOOL 
         */

        SubToolBarAction* _up_toolbar = new SubToolBarAction();

        {
            QVector<QWidgetAction*> _up_temp;

            IconAction* _icon = new IconAction(FontNoggitIcon{ FontNoggit::TOOL_OBJECT_EDITOR });
            CheckBoxAction* _rotate_follow_cursor = new CheckBoxAction(tr("Rotate following cursor"), true);
            CheckBoxAction* _smooth_follow_rotation = new CheckBoxAction(tr("Smooth follow rotation"), true);
            CheckBoxAction* _random_all_on_rotation = new CheckBoxAction(tr("Random Rotation/Tilt/Scale on Rotation"));
            CheckBoxAction* _magnetic_to_ground = new CheckBoxAction(tr("Magnetic to ground when dragging"));

            _up_toolbar->ADD_ACTION(_icon);
            _up_toolbar->ADD_ACTION(_rotate_follow_cursor);
            _up_toolbar->ADD_ACTION(_smooth_follow_rotation);
            _up_toolbar->ADD_ACTION(_random_all_on_rotation);
            _up_toolbar->ADD_ACTION(_magnetic_to_ground);
            _up_toolbar->SETUP_WIDGET(false);
        }

        SubToolBarAction* _down_toolbar = new SubToolBarAction();

        {
            QVector<QWidgetAction*> _down_temp;

            CheckBoxAction* _magnetic_to_ground = new CheckBoxAction(tr("Magnetic to ground when dragging"));
            CheckBoxAction* _rotation_around_pivot = new CheckBoxAction(tr("Rotate around pivot"), true);

            _down_toolbar->ADD_ACTION(_magnetic_to_ground);
            _down_toolbar->ADD_ACTION(_rotation_around_pivot);
            _down_toolbar->SETUP_WIDGET(true);
        }


        _object_secondary_tool.push_back(_up_toolbar);
        _object_secondary_tool.push_back(_down_toolbar);
    }

    {
        /*
         * LIGHT SECONDARY TOOL 
         */

        SubToolBarAction* _toolbar = new SubToolBarAction();

        {
            IconAction* _icon = new IconAction(FontNoggitIcon{ FontNoggit::TOOL_STAMP });
            CheckBoxAction* _draw_only_inside = new CheckBoxAction(tr("Draw current only"));
            CheckBoxAction* _draw_wireframe = new CheckBoxAction(tr("Draw wireframe"));
            SliderAction* _alpha_value = new SliderAction(tr("Alpha"), 0, 100, 30, "",
                std::function<float(float v)>() = [&](float v) {
                    return v / 100.f;
                });

            _toolbar->ADD_ACTION(_icon);
            _toolbar->ADD_ACTION(_draw_only_inside); sphere_light_inside_index = 1;
            _toolbar->ADD_ACTION(_draw_wireframe); sphere_light_wireframe_index = 2;
            _toolbar->ADD_ACTION(_alpha_value); sphere_light_alpha_index = 3;
            _toolbar->SETUP_WIDGET(false);
        }

        _light_secondary_tool.push_back(_toolbar);
    }
}

void ViewToolbar::setCurrentMode(MapView* mapView, editing_mode mode)
{
    mapView->getLeftSecondaryToolbar()->hide();
    current_mode = mode;

    QSettings settings;
    bool use_classic_ui = settings.value("classicUI", false).toBool();

    switch (current_mode)
    {
    case editing_mode::ground:
        break;
    case editing_mode::flatten_blur:
        if (_flatten_secondary_tool.size() > 0)
        {
            setupWidget(_flatten_secondary_tool);
            if (!use_classic_ui)
                mapView->getLeftSecondaryToolbar()->show();
            else
                mapView->getLeftSecondaryToolbar()->hide();
        }
        break;
    case editing_mode::paint:
        if (_texture_secondary_tool.size() > 0)
        {
            setupWidget(_texture_secondary_tool);
            if (!use_classic_ui)
                mapView->getLeftSecondaryToolbar()->show();
            else
                mapView->getLeftSecondaryToolbar()->hide();
        }
        break;
    case editing_mode::object:
        if (_object_secondary_tool.size() > 0)
        {
            //setupWidget(_object_secondary_tool, true);
            //mapView->getLeftSecondaryToolbar()->show();
        }
        break;
    case editing_mode::light:
        if (_light_secondary_tool.size() > 0)
        {
            setupWidget(_light_secondary_tool, true);
            mapView->getLeftSecondaryToolbar()->show();
        }
        break;
    default:
        break;
    }
}

editing_mode ViewToolbar::getCurrentMode() const
{
  return current_mode;
}

void ViewToolbar::add_tool_icon(MapView* mapView,
                                Noggit::BoolToggleProperty* view_state,
                                const QString& name,
                                const QString& description,
                                const QString& shortcut,
                                const FontNoggit::Icons& icon,
                                ViewToolbar* sec_tool_bar,
                                QVector<QWidgetAction*> sec_action_bar)
{
    auto action = addAction(FontNoggitIcon{icon}, name);

    // Twenty icon-only toggles with no explicit tooltip: Qt falls back to QAction::text(), so
    // hovering one used to show its bare name and nothing about what it does or which key
    // flips it. Every key named below is a REAL binding, taken off the ADD_TOGGLE and
    // ADD_TOGGLE_POST lines in MapView::setupMainMenu -- those QActions keep sole ownership of
    // the shortcut and nothing here calls setShortcut, so the hint cannot become a second
    // handler for the same key. The worst a stale entry can do is display a wrong hint.
    //
    // Rich text so the name reads as a heading and the rest as subordinate. No colour is
    // hardcoded; Qt applies the tooltip palette and the theme stays in charge. Everything that
    // passes through tr() is escaped, because a translator may legitimately return a character
    // that means something in markup.
    QString tooltip (QString("<b>%1</b>").arg(name.toHtmlEscaped()));

    if (!description.isEmpty())
      tooltip += QString("<br/>%1").arg(description.toHtmlEscaped());

    if (!shortcut.isEmpty())
      tooltip += QString("<br/><small>Shortcut: %1</small>").arg(shortcut.toHtmlEscaped());

    action->setToolTip(tooltip);
    connect (action, &QAction::triggered, [action, view_state] () {
        action->setChecked(!view_state->get());
        view_state->set(!view_state->get());
    });

    // Hover is a high-frequency signal: sweeping the pointer across this bar fires once per
    // button, nineteen times, and only one action on it (Climb) actually has a secondary bar.
    // Every one of the other eighteen used to run clear() + hide() unconditionally, and hide()
    // on the holder is a visibility change inside the viewport overlay layout, i.e. a relayout
    // of the overlay for a bar that was not showing in the first place.
    //
    // The guard below is deliberately on "is anything actually up" rather than on "does this
    // action have a bar". Those are not the same: moving off Climb onto a neighbour must still
    // take Climb's bar down, and an early return keyed on the action would have left it
    // stranded. This does exactly what the old code did, and skips only the no-op case.
    connect (action, &QAction::hovered, [mapView, sec_tool_bar, sec_action_bar] () {
        QWidget* const holder (mapView->getSecondaryToolBar());

        if (sec_action_bar.isEmpty())
        {
            if (holder->isVisible())
            {
                sec_tool_bar->clear();
                holder->hide();
            }

            return;
        }

        sec_tool_bar->clear();
        holder->hide();
        sec_tool_bar->setupWidget(sec_action_bar);
        holder->show();
    });

    connect (view_state, &Noggit::BoolToggleProperty::changed, [action, view_state, mapView] () {
      
      /* it has been removed from the bar
        if (action->text() == "Game view" && view_state->get())
        {
            // hack, manually update camera when switch to game_view
            mapView->setCameraDirty();
            auto ground_pos = mapView->getWorld()->get_ground_height(mapView->getCamera()->position);
            mapView->getCamera()->position.y = ground_pos.y + 2;
        }*/

        action->setChecked(view_state->get());
    });

    action->setCheckable(true);
    action->setChecked(view_state->get());
}

void ViewToolbar::setupWidget(QVector<QWidgetAction *> _to_setup, bool ignoreSeparator)
{
    clear();
    for (int i = 0; i < _to_setup.size(); ++i)
    {
        addAction(_to_setup[i]);
        (i == _to_setup.size() - 1) ? NULL : (ignoreSeparator) ? NULL : addSeparator();
    }
}

bool ViewToolbar::showUnpaintableChunk()
{
    return static_cast<SubToolBarAction*>(_texture_secondary_tool[0])->GET<CheckBoxAction*>(unpaintable_chunk_index)->checkbox()->isChecked() && current_mode == editing_mode::paint;
}

void ViewToolbar::nextFlattenMode()
{
    CheckBoxAction* _raise_option = static_cast<SubToolBarAction*>(_flatten_secondary_tool[0])->GET<CheckBoxAction*>(raise_index);
    CheckBoxAction* _lower_option = static_cast<SubToolBarAction*>(_flatten_secondary_tool[0])->GET<CheckBoxAction*>(lower_index);

    QSignalBlocker const raise_lock(_raise_option);
    QSignalBlocker const lower_lock(_lower_option);

    _raise_option->setChecked(true);
    _lower_option->setChecked(true);
}

bool ViewToolbar::drawOnlyInsideSphereLight()
{
    return static_cast<SubToolBarAction*>(_light_secondary_tool[0])->GET<CheckBoxAction*>(sphere_light_inside_index)->checkbox()->isChecked() && current_mode == editing_mode::light;
}

bool ViewToolbar::drawWireframeSphereLight()
{
    return static_cast<SubToolBarAction*>(_light_secondary_tool[0])->GET<CheckBoxAction*>(sphere_light_wireframe_index)->checkbox()->isChecked() && current_mode == editing_mode::light;
}

float ViewToolbar::getAlphaSphereLight()
{
    auto toolbar = static_cast<SubToolBarAction*>(_light_secondary_tool[0]);
    auto slider = toolbar->GET<SliderAction*>(sphere_light_alpha_index)->slider();

    return float(slider->value()) / 100.f;
}

PushButtonAction::PushButtonAction(const QString& text)
  : QWidgetAction(NULL)
{
  QWidget* _widget = new QWidget(NULL);
  QHBoxLayout* _layout = new QHBoxLayout();
  _layout->setContentsMargins(0, 0, 0, 0);

  _push = new QPushButton(text);

  _layout->addWidget(_push);
  _widget->setLayout(_layout);

  setDefaultWidget(_widget);
}

QPushButton* PushButtonAction::pushbutton()
{
  return _push;
}

CheckBoxAction::CheckBoxAction(const QString& text, bool checked)
  : QWidgetAction(NULL)
{
  QWidget* _widget = new QWidget(NULL);
  QHBoxLayout* _layout = new QHBoxLayout();
  _layout->setContentsMargins(0, 0, 0, 0);

  _checkbox = new QCheckBox(text);
  _checkbox->setChecked(checked);

  _layout->addWidget(_checkbox);
  _widget->setLayout(_layout);

  setDefaultWidget(_widget);
}

QCheckBox* CheckBoxAction::checkbox()
{
  return _checkbox;
}

IconAction::IconAction(const QIcon& icon)
  : QWidgetAction(NULL)
{
  QWidget* _widget = new QWidget(NULL);
  QHBoxLayout* _layout = new QHBoxLayout();
  _layout->setContentsMargins(0, 0, 0, 0);

  _icon = new QLabel();
  _icon->setPixmap(icon.pixmap(QSize(22, 22)));

  _layout->addWidget(_icon);
  _widget->setLayout(_layout);
  setDefaultWidget(_widget);
}

QLabel* IconAction::icon()
{
  return _icon;
}

SpacerAction::SpacerAction(Qt::Orientation orientation)
  : QWidgetAction(NULL)
{
  QWidget* _widget = new QWidget(NULL);
  QHBoxLayout* _layout = new QHBoxLayout();
  _layout->setContentsMargins(0, 0, 0, 0);

  if (orientation == Qt::Vertical)
    _layout->addSpacerItem(new QSpacerItem(0, 0, QSizePolicy::Minimum, QSizePolicy::Expanding));

  if (orientation == Qt::Horizontal)
    _layout->addSpacerItem(new QSpacerItem(0, 0, QSizePolicy::Expanding, QSizePolicy::Minimum));

  _widget->setLayout(_layout);
  setDefaultWidget(_widget);
}

SubToolBarAction::SubToolBarAction()
  : QWidgetAction(NULL)
{
  QWidget* _widget = new QWidget(NULL);
  QHBoxLayout* _layout = new QHBoxLayout();
  _layout->setSpacing(Noggit::Ui::Design::S1);
  // setMargin has been deprecated since Qt 5.13 and is the same call for all four sides.
  _layout->setContentsMargins(0, 0, 0, 0);

  _toolbar = new QToolBar();
  _toolbar->setContextMenuPolicy(Qt::PreventContextMenu);
  // THE SUB-BARS WERE THE ONE SET OF TOOL BARS THE TOOLBAR_ICON_EXTENT NOTE ABOVE NEVER
  // REACHED. Five of them are built -- the secondary bars for the texture, flatten, light and
  // climb modes -- and every one inherited QStyle::PM_ToolBarIconSize, measured 36 on
  // windowsvista here, into a content box of roughly 22px. That is the identical 61 percent
  // resample the note above describes as "a large part of why the chrome read as muddy",
  // surviving on the bars a user sees every time they pick a sub-mode.
  //
  // The three OUTER ViewToolbar constructors and toolbar::toolbar all set this already; this
  // one did not, and TOOLBAR_ICON_EXTENT is in an anonymous namespace in this same translation
  // unit, so it was in scope the whole time.
  _toolbar->setIconSize(QSize(TOOLBAR_ICON_EXTENT, TOOLBAR_ICON_EXTENT));
  _toolbar->setAllowedAreas(Qt::TopToolBarArea | Qt::BottomToolBarArea);
  _toolbar->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Maximum);
  _toolbar->setOrientation(Qt::Horizontal);

  _layout->addWidget(_toolbar);
  _widget->setLayout(_layout);

  setDefaultWidget(_widget);
}

QToolBar* SubToolBarAction::toolbar()
{
  return _toolbar;
}

void SubToolBarAction::ADD_ACTION(QWidgetAction* _act)
{
  _actions.push_back(_act);
}

void SubToolBarAction::SETUP_WIDGET(bool forceSpacer, Qt::Orientation orientation)
{
  _toolbar->clear();
  for (int i = 0; i < _actions.size(); ++i)
  {
    _toolbar->addAction(_actions[i]);
    if (i == _actions.size() - 1)
    {
      if (forceSpacer)
      {
        /* TODO: fix this spacer */

        _toolbar->addAction(new SpacerAction(orientation));
      }
    }
    else
    {
      _toolbar->addSeparator();
    }
  }
}
