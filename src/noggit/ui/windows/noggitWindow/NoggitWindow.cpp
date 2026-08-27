#include <noggit/application/Configuration/NoggitApplicationConfiguration.hpp>
#include <noggit/application/NoggitApplication.hpp>
#include <noggit/ContextObject.hpp>
#include <noggit/DBC.h>
#include <noggit/DBCFile.h>
#include <noggit/Log.h>
#include <noggit/MapView.h>
#include <noggit/PatchAssetPacker.hpp>
#include <noggit/project/ApplicationProject.h>
#include <noggit/ui/FontAwesome.hpp>
#include <noggit/ui/FramelessWindow.hpp>
#include <noggit/ui/minimap_widget.hpp>
#include <noggit/ui/tools/MapCreationWizard/Ui/MapCreationWizard.hpp>
#include <noggit/ui/tools/UiCommon/StackedWidget.hpp>
#include <noggit/ui/UidFixWindow.hpp>
#include <noggit/ui/WaitCursor.hpp>
#include <noggit/ui/windows/about/About.h>
#include <noggit/ui/windows/noggitWindow/components/BuildMapListComponent.hpp>
#include <noggit/ui/windows/noggitWindow/NoggitWindow.hpp>
#include <noggit/ui/windows/noggitWindow/widgets/MapBookmarkListItem.hpp>
#include <noggit/ui/windows/noggitWindow/widgets/MapListItem.hpp>
#include <noggit/ui/windows/noggitWindow/widgets/MapSelectionArt.hpp>
#include <noggit/ui/windows/projectSelection/NoggitProjectSelectionWindow.hpp>
#include <noggit/ui/windows/projectSelection/widgets/BrandBanner.hpp>
#include <noggit/ui/windows/settingsPanel/SettingsPanel.h>
#include <noggit/ui/windows/UiStyle.hpp>
#include <noggit/uid_storage.hpp>
#include <noggit/World.h>

#include <string>
#include <blizzard-archive-library/include/Exception.hpp>

#include <QCheckBox>
#include <QColor>
#include <QFileInfo>
#include <QFont>
#include <QFormLayout>
#include <QFrame>
#include <QIcon>
#include <QLineEdit>
#include <QPixmap>
#include <QProcess>
#include <QScrollArea>
#include <QStackedLayout>
#include <QToolButton>
#include <QtCore/QChildEvent>
#include <QtCore/QEvent>
#include <QtGui/QCloseEvent>
#include <QtWidgets/QAbstractItemView>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QMenuBar>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QProgressDialog>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QStatusBar>
#include <QtWidgets/QTabWidget>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QWidget>

#include <algorithm>
#include <chrono>
#include <sstream>

#ifdef USE_MYSQL_UID_STORAGE
#include <mysql/mysql.h>

#include <QtCore/QSettings>
#endif

#include "ui_TitleBar.h"
#include <noggit/ui/tools/ViewportManager/ViewportManager.hpp>

namespace
{
  namespace Style = Noggit::Ui::Windows::Style;

  //! The left column of the map-selection screen. 420 rather than the 310 it carried before,
  //! and the number is what the CARD needs rather than what the old strip survived on. Every
  //! term is one this file or MapListItem.cpp decides:
  //!
  //!    24  the tab page's SPACE_12 margins, one either side
  //!  + 18  the view's ::item padding (8 either side) and its 2px selection reserve
  //!  + 12  the vertical scroll bar
  //!  + 32  the card's own chrome: 1px border and 14px padding, one either side
  //!  + 48  the 36px emblem and the 12px column gap after it
  //!  + 12  the gap before the trailing group
  //!  + 22  the 14px chevron and the 8px that separates it from the badge
  //!  = 168 before the type badge or the map name has a single pixel.
  //!
  //! 420 leaves those two 252 to share. At 310 they had 142 between them, and since the badge
  //! takes its share first, almost every map name elided.
  constexpr int MAP_COLUMN_WIDTH = 420;

  //! The glyph inside the header strip's gear button, in its 18px content rect:
  //!   1 border + 7 padding + 18 content + 7 padding + 1 border = 34
  //! which is 34 of the strip's 64, leaving 15 of air above and below.
  constexpr int HEADER_GEAR_GLYPH_EXTENT = 18;

  //! The tab glyphs. 14 against the tab label's 12px text, so the mark reads as a mark and not
  //! as a second word.
  constexpr int TAB_GLYPH_EXTENT = 14;

  //! The magnifier beside the SEARCH heading. 12 against the heading's 11px capitals, so the
  //! mark and the word sit on the same optical line.
  constexpr int SECTION_GLYPH_EXTENT = 12;

  //! The plus on the add-map button, and the info mark on the hint row.
  constexpr int ADD_BUTTON_GLYPH_EXTENT = 14;
  constexpr int HINT_GLYPH_EXTENT = 14;

  //! The two pages of the right pane's preview panel, in the order they are added. Named
  //! because setCurrentIndex(0) at one end of the file and addWidget at the other is exactly the
  //! kind of pairing that survives one edit and not two.
  constexpr int PREVIEW_PAGE_EMPTY = 0;
  constexpr int PREVIEW_PAGE_MINIMAP = 1;

  //! The pin that fills the right pane's empty state. Large enough to be an illustration rather
  //! than an icon -- it is the only object in a pane that is otherwise several hundred pixels of
  //! nothing.
  constexpr int EMPTY_PIN_EXTENT = 64;

  //! Tracking for the letterspaced capitals, in ABSOLUTE pixels. Qt style sheets have no
  //! letter-spacing property, so this cannot be a sheet declaration; see applyTracking.
  constexpr int LABEL_TRACKING = 2;

  //! Theme colours that have to be named from C++ because they are handed to a QPainter, and a
  //! QPainter is the one place in this application a style sheet cannot reach. Hand-carried
  //! copies: they have to be moved by hand when the theme moves.
  //!
  //!   text.hi  #F3F0E9 on the add button's brand.fill #2E1516 -- 14.940:1, which is the whole
  //!            reason that button is TINTED rather than filled: text.hi on solid brand.crimson
  //!            is 3.535:1 and fails the body floor.
  //!   text.dim #BFB7AA for the hint mark and for the SEARCH heading's magnifier, both on the
  //!            bg.void #100E0B tab page -- 9.699:1.
  //!   text.off #7F786A for the empty-state pin, on the bg.alt #1D1916 preview panel --
  //!            3.988:1. Over the 3:1 graphical floor and deliberately under the 4.5 body floor:
  //!            the pin is a decoration behind a caption, not a second caption, and the caption
  //!            itself is text.dim at 8.787:1 on the same panel.
  constexpr QRgb TEXT_HI = qRgb(0xF3, 0xF0, 0xE9);
  constexpr QRgb TEXT_DIM = qRgb(0xBF, 0xB7, 0xAA);
  constexpr QRgb TEXT_OFF = qRgb(0x7F, 0x78, 0x6A);

  //! The seven readouts MapView adds to the status bar, in the order it adds them. See
  //! NoggitWindow::eventFilter for why the order is what identifies them.
  constexpr int STATUS_READOUT_COUNT = 7;

  char const* const STATUS_READOUT_NAMES[STATUS_READOUT_COUNT] =
    { "status-position"
    , "status-selection"
    , "status-area"
    , "status-time"
    , "status-fps"
    , "status-culling"
    , "status-database"
    };

  //! Which of the seven carry a number the user is actually watching, and therefore take the
  //! value rank -- 12px semibold at the highest text rank -- instead of the body rank the rest
  //! of the bar uses. Position, frame rate and the loaded/rendered counts; the zone name, the
  //! selection description, the in-game clock and the database line are context, not data.
  bool const STATUS_READOUT_IS_VALUE[STATUS_READOUT_COUNT] =
    { true, false, false, false, true, true, false };

  //! Puts letterspacing on a widget's font, and is safe to call again.
  //!
  //! THE TRACKING SURVIVES THE STYLE SHEET, and the mechanism is worth stating because UiStyle's
  //! own measurements record that a font SIZE does not: QStyleSheetStyle::updateStyleSheetFont
  //! resolves the rule's font against the widget's own with QFont::resolve, which merges PER
  //! ATTRIBUTE. The sheet's global rule declares font-family and font-size, so those two come
  //! from the rule; it declares no letter spacing, and QFont::LetterSpacingResolved is its own
  //! resolve bit, so the spacing set here is what survives. The capitals cannot be done this way
  //! at all -- Qt style sheets have no text-transform either -- so they live in the string.
  //!
  //! The early return is not an optimisation: setFont() posts a font change, and anything that
  //! re-applied the tracking from a font-change handler would recurse without it.
  void applyTracking(QWidget* widget, int tracking)
  {
    if (!widget)
      return;

    QFont font(widget->font());

    if (font.letterSpacingType() == QFont::AbsoluteSpacing
        && qRound(font.letterSpacing()) == tracking)
      return;

    font.setLetterSpacing(QFont::AbsoluteSpacing, tracking);
    widget->setFont(font);
  }

  //! One section heading for the left column: a magnifier, a letterspaced capital, and a
  //! hairline running to the far edge of the column.
  //!
  //! THIS REPLACES A QGroupBox. A group box draws a titled frame around whatever it holds, which
  //! put a second box inside a column that is already a panel inside a tab -- three nested
  //! outlines around one form. A heading over a rule says the same thing with no box at all,
  //! and it is what the mockup draws.
  //!
  //! The magnifier is PAINTED. It is not in the shipped icon set -- the set has map, bookmark,
  //! cog, info and plus but no magnifier at all -- and the only other route to one is the Font
  //! Awesome glyph font, which this fork deliberately does not redistribute. A search heading
  //! whose mark is missing on every clean install is worse than no mark.
  QWidget* buildSectionHeading(QWidget* parent, QString const& text)
  {
    QWidget* const row = new QWidget(parent);
    row->setObjectName("map-section-row");

    QHBoxLayout* const layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(Style::SPACE_8);

    QLabel* const glyph = new QLabel(row);
    glyph->setObjectName("map-section-glyph");
    glyph->setFixedSize(SECTION_GLYPH_EXTENT, SECTION_GLYPH_EXTENT);
    glyph->setPixmap(Noggit::Ui::Widget::MapSelectionArt::magnifierGlyph(
        SECTION_GLYPH_EXTENT, QColor(TEXT_DIM), row->devicePixelRatioF()));
    layout->addWidget(glyph, 0, Qt::AlignVCenter);

    QLabel* const label = new QLabel(text, row);
    label->setObjectName("map-section-heading");
    applyTracking(label, LABEL_TRACKING);
    layout->addWidget(label, 0, Qt::AlignVCenter);

    QFrame* const rule = new QFrame(row);
    rule->setObjectName("map-section-rule");
    rule->setFrameShape(QFrame::HLine);
    rule->setFrameShadow(QFrame::Plain);
    rule->setLineWidth(1);
    rule->setFixedHeight(1);
    layout->addWidget(rule, 1, Qt::AlignVCenter);

    return row;
  }
}

namespace Noggit::Ui::Windows
{
  NoggitWindow::NoggitWindow(std::shared_ptr<Noggit::Application::NoggitApplicationConfiguration> application,
                             std::shared_ptr<Noggit::Project::NoggitProject> project)
      : QMainWindow(nullptr)
      , _null_widget(new QWidget(this))
      , _applicationConfiguration(application)
      , _project(project)
  {

    // The product name alone. A window title names this window in the taskbar and in Alt-Tab,
    // where it is truncated first and read a hundred times a day; a commit hash there is noise.
    // The build lives in About and in the log. FramelessWindow copies this string into the
    // in-app title band once at construction, so it shows up twice under the Crimson chrome --
    // one more reason to keep it short.
    setWindowTitle(QStringLiteral("Noggit Crimson"));
    setWindowIcon(QIcon(":/icon"));

    Log << "Project version : " << Noggit::Project::ClientVersionFactory::MapToStringVersion(project->projectVersion).c_str() << std::endl;

    if (project->projectVersion == Project::ProjectVersion::WOTLK)
    {
      OpenDBs(project->ClientData);
    }
    else
    {
        LogError << "NoggitWindow() : Unsupported project version, skipping loading DBCs." << std::endl;
    }

    setCentralWidget(_null_widget);

    // The default value is AnimatedDocks | AllowTabbedDocks.
    setDockOptions(AnimatedDocks | AllowNestedDocks | AllowTabbedDocks | GroupedDragging);

    _about = new about(this);
    _settings = new settings(this);

    _menuBar = menuBar();

    QSettings settings;

    if (!settings.value("systemWindowFrame", true).toBool())
    {
      QWidget* widget = new QWidget(this);
      ::Ui::TitleBar* titleBarWidget = setupFramelessWindow(widget, this, minimumSize(), maximumSize(), true);

      // The helper returns nullptr when it decides the window keeps its system frame. Reaching
      // here at all means this branch already read the key as false and the helper now reads it
      // the same way, so it cannot be null -- but this line used to dereference the result
      // unconditionally while the two readers disagreed about the default, and a null check is
      // cheaper than trusting that they never drift apart again.
      if (titleBarWidget)
      {
        titleBarWidget->horizontalLayout->insertWidget(2, _menuBar);
        setMenuWidget(widget);
      }
    }

    _menuBar->setNativeMenuBar(settings.value("nativeMenubar", true).toBool());

    // Named so the sheet can reach the editor's own bar without also selecting the one the
    // frameless title bar embeds, and so it stops inheriting whatever QMainWindow's default
    // QMenuBar rule happens to say.
    _menuBar->setObjectName("editor-menu-bar");

    auto file_menu(_menuBar->addMenu("&Noggit"));
    file_menu->setObjectName("editor-menu-noggit");

    // The four entries were one undifferentiated run of text. They are two groups -- "open
    // something else" and "leave" -- and a separator plus a leading glyph is the whole of what
    // QSS can do to say so, since a menu item's text rank is fixed by the sheet.
    auto settings_action(file_menu->addAction("Settings"));
    settings_action->setIcon(Noggit::Ui::FontAwesomeIcon(Noggit::Ui::FontAwesome::Icons::cog));
    QObject::connect(settings_action, &QAction::triggered, [&]
                     {
                       _settings->show();
                     }
    );

    auto about_action(file_menu->addAction("About"));
    about_action->setIcon(Noggit::Ui::FontAwesomeIcon(Noggit::Ui::FontAwesome::Icons::infocircle));
    QObject::connect(about_action, &QAction::triggered, [&]
                     {
                       _about->show();
                     }
    );

    file_menu->addSeparator();

    auto proj_selec_action(file_menu->addAction("Exit to Project Selection"));
    proj_selec_action->setIcon(
        Noggit::Ui::FontAwesomeIcon(Noggit::Ui::FontAwesome::Icons::signoutalt));
    QObject::connect(proj_selec_action, &QAction::triggered, [this]
        {
            // auto noggit = Noggit::Application::NoggitApplication::instance();
            // auto project_selection = new Noggit::Ui::Windows::NoggitProjectSelectionWindow(noggit);
            // project_selection->show();

            exit_to_project_selection = true;
            close();
        }
    );

    auto mapmenu_action(file_menu->addAction("Exit"));
    mapmenu_action->setIcon(Noggit::Ui::FontAwesomeIcon(Noggit::Ui::FontAwesome::Icons::poweroff));
    QObject::connect(mapmenu_action, &QAction::triggered, [this]
                     {
                       close();
                     }
    );

    _menuBar->adjustSize();

    prepareStatusBar();

    _buildMapListComponent = std::make_unique<Component::BuildMapListComponent>();

    _map_creation_wizard = new Noggit::Ui::Tools::MapCreationWizard::Ui::MapCreationWizard(_project, this);

    buildMenu();
  }

  void NoggitWindow::prepareStatusBar()
  {
    // THE PROBLEM. MapView adds seven QLabels to this window's status bar -- position,
    // selection, zone, in-game time, frame rate, loaded/rendered counts and the database line --
    // and gives none of them an object name or a rank. The sheet can therefore only reach them
    // as `QStatusBar QLabel`, which paints all seven identically, and the bar reads as one long
    // sentence of small grey text with no way in.
    //
    // THE MECHANISM, and why it is an event filter rather than seven lines at the call site.
    // Those labels are constructed in MapView, which this pass does not own. What this window
    // does own is the QStatusBar itself, and QStatusBar::addWidget reparents the label onto it,
    // which posts QEvent::ChildAdded here. Measured with a standalone Qt 5.15.2 probe: the burst
    // arrives as exactly seven ChildAdded events carrying a fully constructed QLabel, in
    // addWidget order, interleaved with plain QObject children the filter ignores.
    //
    // THE HONEST LIMIT. Order is the only identity available, so the mapping is positional and
    // is taken modulo seven -- MapView's labels are not destroyed when a map is left (they are
    // reparented onto the bar and only hidden, also measured), so a second map entry produces a
    // second burst of seven and the counter has to wrap. If some future code adds an eighth
    // QLabel to this bar the names shift by one. The consequence of being wrong is a readout
    // wearing the wrong rank; nothing reads these names back, so nothing can break.
    QStatusBar* const bar = statusBar();
    bar->setObjectName("editor-status-bar");
    bar->installEventFilter(this);
  }

  void NoggitWindow::dressStatusReadout(QLabel* readout)
  {
    int const slot = _status_readouts_seen % STATUS_READOUT_COUNT;
    ++_status_readouts_seen;

    readout->setObjectName(QString::fromLatin1(STATUS_READOUT_NAMES[slot]));

    if (STATUS_READOUT_IS_VALUE[slot])
    {
      // QLabel[state="value"] has existed in the shipped sheet since the theme was written and
      // has never once fired, because nothing in src/ ever set the property. This is the wiring.
      Style::markValue(readout);
    }
  }

  bool NoggitWindow::eventFilter(QObject* watched, QEvent* event)
  {
    if (event->type() == QEvent::ChildAdded && watched == statusBar())
    {
      if (QLabel* const readout =
              qobject_cast<QLabel*>(static_cast<QChildEvent*>(event)->child()))
      {
        dressStatusReadout(readout);
      }
    }

    return QMainWindow::eventFilter(watched, event);
  }

  void NoggitWindow::check_uid_then_enter_map
      (glm::vec3 pos, math::degrees camera_pitch, math::degrees camera_yaw, bool from_bookmark
      )
  {
    QSettings settings;
    assert(getWorld());

    unsigned int world_map_id = getWorld()->getMapID();

#ifdef USE_MYSQL_UID_STORAGE
    bool use_mysql = settings.value("project/mysql/enabled", false).toBool();

    bool valid_conn = false;
    if (use_mysql)
    {
        valid_conn = mysql::testConnection(true);
    }

    if ((valid_conn && mysql::hasMaxUIDStoredDB(world_map_id))
      || uid_storage::hasMaxUIDStored(world_map_id)
       )
    {

      getWorld()->mapIndex.loadMaxUID();
      enterMapAt(pos, camera_pitch, camera_yaw, uid_fix_mode::none, from_bookmark);
    }
#else
    if (uid_storage::hasMaxUIDStored(world_map_id))
    {
      if (settings.value("uid_startup_check", true).toBool())
      {
        enterMapAt(pos, camera_pitch, camera_yaw, uid_fix_mode::max_uid, from_bookmark);
      } else
      {
        getWorld()->mapIndex.loadMaxUID();
        enterMapAt(pos, camera_pitch, camera_yaw, uid_fix_mode::none, from_bookmark);
      }
    }
#endif
    else
    {
      auto uid_fix_window(new UidFixWindow(pos, camera_pitch, camera_yaw));
      uid_fix_window->show();

      connect(uid_fix_window, &Noggit::Ui::UidFixWindow::fix_uid, [this, from_bookmark]
                  (glm::vec3 pos, math::degrees camera_pitch, math::degrees camera_yaw, uid_fix_mode uid_fix
                  )
              {
                enterMapAt(pos, camera_pitch, camera_yaw, uid_fix, from_bookmark);
              }
      );
    }
  }

  void
  NoggitWindow::enterMapAt(glm::vec3 pos, math::degrees camera_pitch, math::degrees camera_yaw, uid_fix_mode uid_fix,
                           bool from_bookmark
  )
  {
    World* world = getWorld();

    if (world->mapIndex.hasAGlobalWMO())
    {
        // enter at mdoel's position
        // pos = glm::vec3(_world->mWmoEntry[0], _world->mWmoEntry.pos[1], _world->mWmoEntry.pos[2]);

        // better, enter at model's max extent, facing toward min extent
        auto min_extent = glm::vec3(world->mWmoEntry.extents[0][0], world->mWmoEntry.extents[0][1], world->mWmoEntry.extents[0][2]);
        auto max_extent = glm::vec3(world->mWmoEntry.extents[1][0], world->mWmoEntry.extents[1][1] * 2, world->mWmoEntry.extents[1][2]);
        float dx = min_extent.x - max_extent.x;
        float dy = min_extent.z - max_extent.z; // flipping z and y works better for some reason
        float dz = min_extent.y - max_extent.y;

        pos = { world->mWmoEntry.pos[0], world->mWmoEntry.pos[1], world->mWmoEntry.pos[2] };

        camera_yaw = math::degrees(math::radians(std::atan2(dx, dy)));

        float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
        camera_pitch = -math::degrees(math::radians(std::asin(dz / distance)));

    }


    // _map_creation_wizard->destroyFakeWorld();
    _map_view = (new MapView(camera_yaw, camera_pitch, pos, this, _project, std::move(_map_creation_wizard->_world), uid_fix, from_bookmark));
    connect(_map_view, &MapView::uid_fix_failed, [this]()
    { promptUidFixFailure(); });
    connect(_settings, &settings::saved, [this]()
    { if (_map_view) _map_view->onSettingsSave(); });

    // _app_toolbar = new QToolBar("Application", this);
    // _app_toolbar->setOrientation(Qt::Horizontal);
    // addToolBar(_app_toolbar);

    _stack_widget->addWidget(_map_view);
    _stack_widget->setCurrentIndex(1);

    map_loaded = true;

  }

  void NoggitWindow::applyFilterSearch(const QString &name, int type, int expansion, bool wmo_maps)
  {
      for (int i = 0; i < _continents_table->count(); ++i)
      {
          auto item_widget = _continents_table->item(i);
          auto widget = qobject_cast<Noggit::Ui::Widget::MapListItem*>(_continents_table->itemWidget(item_widget));

          if (!widget)
              continue;

          item_widget->setHidden(false);

          if (!widget->name().contains(name, Qt::CaseInsensitive))
          {
              item_widget->setHidden(true);
              continue;
          }

          if (!(widget->type() == (type - 2)) && type != 0)
          {
              item_widget->setHidden(true);
              continue;
          }

          if (!(widget->expansion() == (expansion - 1)) && expansion != 0)
          {
              item_widget->setHidden(true);
          }

          if (!(widget->wmo_map() == wmo_maps))
          {
              item_widget->setHidden(true);
          }
      }
  }

  void NoggitWindow::loadMap(int map_id)
  {
    // This is not a cheap notification. mapSelected lands in MapCreationWizard::selectMap, which
    // constructs a World: it reads the map's WDT and builds the horizon minimap, on the UI
    // thread, before this function returns. It is wired to the map list's itemSelectionChanged,
    // so it also runs once per arrow-key press through the list. Nothing told the user the
    // application was busy -- the first screen of the editor simply stopped responding for as
    // long as the read took.
    //
    // The cursor is the whole change. A debounce in front of this call was considered and
    // rejected: getWorld() is read straight afterwards for the minimap, and again by the
    // enter-map path, where line 161's assert compiles to nothing under NDEBUG and line 163
    // dereferences the result. Deferring the construction while leaving those readers where
    // they are turns a stall into a null dereference, which is a far worse trade.
    Noggit::Ui::WaitCursor const busy;

    // _minimap->world(nullptr);

    // World is now created only here in
    // void MapCreationWizard::selectMap(int map_id)
    emit mapSelected(map_id);

    /*
    _world.reset();

    auto table = _project->ClientDatabase->LoadTable("Map", readFileAsIMemStream);
    auto record = table.Record(map_id);

    _world = std::make_unique<World>(record.Columns["Directory"].Value, map_id, Noggit::NoggitRenderContext::MAP_VIEW);
    */

    _minimap->world(getWorld());

    updateMapDetail();

    //_project->ClientDatabase->UnloadTable("Map");
  }

  void NoggitWindow::buildMenu()
  {
    _stack_widget = new StackedWidget(this);
    _stack_widget->setAutoResize(true);

    setCentralWidget(_stack_widget);

    auto widget(new QWidget(_stack_widget));
    widget->setObjectName("map-selection-root");
    _stack_widget->addWidget(widget);

    // THE WINDOW IS A HEADER OVER A BODY NOW. The root used to be one QHBoxLayout holding the
    // two panes, with nothing above them at all -- this window carried no product mark of any
    // kind, so the first screen after the launcher looked like a different application.
    //
    // The root layout carries NO margin, because the header strip is FULL BLEED: a band inset by
    // the body margin is a picture of a header, a band that runs edge to edge is one. The body
    // widget below it carries the window's SPACE_16 instead.
    auto root_layout(new QVBoxLayout(widget));
    root_layout->setContentsMargins(0, 0, 0, 0);
    root_layout->setSpacing(0);

    // ------------------------------------------------------------- the header strip --
    //
    // The same widget the launcher's band is, at its compact scale: the 32px product mark, the
    // two-line wordmark, the backdrop artwork under its veil and scrim, and the rule that closes
    // the strip. See BrandBanner for the two metric tables and for why the wordmark is painted.
    _header_banner = new Noggit::Ui::Widget::BrandBanner(
        Noggit::Ui::Widget::BrandBanner::Scale::Compact, widget);

    // The banner paints its own ground, so a child laid out on top of it needs no surface of its
    // own -- which is the whole reason the gear can live inside the band rather than in a row
    // underneath it. The leading stretch is what pins it to the far edge; the trailing margin is
    // the mirror of the mark's own inset on the left.
    auto header_row(new QHBoxLayout(_header_banner));
    header_row->setContentsMargins(0, 0, Style::SPACE_16, 0);
    header_row->setSpacing(0);
    header_row->addStretch(1);

    auto settings_button(new QToolButton(_header_banner));
    settings_button->setObjectName("map-header-gear");
    settings_button->setAccessibleName("map_settings_button");
    settings_button->setToolTip(tr("Settings"));
    settings_button->setCursor(Qt::PointingHandCursor);
    settings_button->setIcon(Noggit::Ui::FontAwesomeIcon(Noggit::Ui::FontAwesome::Icons::cog));
    settings_button->setIconSize(QSize(HEADER_GEAR_GLYPH_EXTENT, HEADER_GEAR_GLYPH_EXTENT));

    // The SAME action the Noggit -> Settings menu item runs, and deliberately not a second one:
    // a gear that opened a different dialogue from the menu entry beside it would be a new
    // control wearing a familiar mark.
    QObject::connect(settings_button, &QToolButton::clicked, [this] { _settings->show(); });

    header_row->addWidget(settings_button, 0, Qt::AlignVCenter);

    root_layout->addWidget(_header_banner);

    // -------------------------------------------------------------------- the body --

    auto body(new QWidget(widget));
    body->setObjectName("map-selection-body");
    root_layout->addWidget(body, 1);

    auto layout(new QHBoxLayout(body));

    // SPACE_16 all round is the dialog content margin, SPACE_24 between the two major regions.
    // The screen used to run on uic's default 9px frame and 6px gap, which put the map column
    // hard against the window edge and left the two halves closer to each other than either was
    // to its own contents.
    layout->setContentsMargins(Style::SPACE_16, Style::SPACE_16, Style::SPACE_16, Style::SPACE_16);
    layout->setSpacing(Style::SPACE_24);

    QListWidget* bookmarks_table(new QListWidget(body));
    _continents_table = new QListWidget(body);
    _continents_table->setSelectionMode(QAbstractItemView::SelectionMode::SingleSelection);
    _continents_table->setSelectionBehavior(QAbstractItemView::SelectItems);

    // The two lists become WELLS: the frame goes so the sheet's own border is the only edge, and
    // both scroll per pixel instead of per item, so a wheel notch moves the list rather than
    // snapping it a whole row. Scroll RANGE and selection behaviour are untouched.
    for (QListWidget* list : {_continents_table, bookmarks_table})
    {
      list->setFrameShape(QFrame::NoFrame);
      list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
      list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    }

    // Object names, so the sheet can give these two lists the card treatment -- a transparent
    // well and a thin crimson thumb -- without reaching every other list in the application.
    // The accessible names stay: they are what assistive technology reads, and the sheet's
    // matching is a separate question from the view's identity.
    _continents_table->setObjectName("map-list");
    bookmarks_table->setObjectName("bookmark-list");

    _continents_table->setAccessibleName("map_list");
    bookmarks_table->setAccessibleName("bookmark_list");

    // WHY THE SELECTED CARD NEEDS ITS OWN STATE. The view paints selection as a wash on the
    // ::item rect, UNDER the widget setItemWidget installed on it -- and a card is opaque, so
    // that wash is invisible and the selected map looked exactly like every other map. The state
    // therefore has to move onto the card, which is the same move the launcher's project rows
    // made when they became cards.
    //
    // currentItemChanged rather than itemSelectionChanged because it hands over BOTH ends of the
    // change: the row that lost the state and the row that gained it. Walking the whole list to
    // find the previous one would be O(n) on every arrow-key press through a list that holds
    // every map in the client.
    QObject::connect(_continents_table, &QListWidget::currentItemChanged,
      [this](QListWidgetItem* current, QListWidgetItem* previous)
      {
        auto const mark = [this](QListWidgetItem* item, bool selected)
        {
          if (!item)
            return;

          if (auto* const card = qobject_cast<Noggit::Ui::Widget::MapListItem*>(
                  _continents_table->itemWidget(item)))
          {
            // An EMPTY string rather than "unselected": Style::applyState only repolishes when
            // the value actually changes, and [state="selected"] simply fails to match an empty
            // property, so there is no second rule to keep in step with this one.
            Style::applyState(card, selected ? QStringLiteral("selected") : QString());
          }
        };

        mark(previous, false);
        mark(current, true);
      }
    );

    // in some situations like when returning to menu an item is selected and itemSelectionChanged won't fire when reclicking it
    // so might need to also connect itemClicked but then they both trigger at the same time.
    QObject::connect(_continents_table, &QListWidget::itemSelectionChanged, [this]()
      {
        QSignalBlocker const blocker(_continents_table);
        QListWidgetItem* const item = _continents_table->currentItem();
        if (item)
        {
          loadMap(item->data(Qt::UserRole).toInt());
        }
      }
    );


    QTabWidget* entry_points_tabs(new QTabWidget(body));
    entry_points_tabs->setObjectName("map-entry-tabs");

     auto add_btn = new QPushButton(tr("ADD NEW MAP"), this);
     add_btn->setObjectName("map-add-button");

     // THE PLUS IS PAINTED. plus.png is in the shipped icon set, but both icon engines resolve
     // their pen from one of exactly two theme-owned slots -- text.dim unchecked, the accent
     // checked -- and this button's label is text.hi #F3F0E9 on brand.fill #2E1516 at 14.940:1.
     // A text.dim plus at 8.558:1 beside a text.hi label is a two-tone control, and the engine
     // cannot be told otherwise. MapSelectionArt::plusGlyph takes the pen.
     add_btn->setIcon(QIcon(Noggit::Ui::Widget::MapSelectionArt::plusGlyph(
         ADD_BUTTON_GLYPH_EXTENT, QColor(TEXT_HI), devicePixelRatioF())));
     add_btn->setIconSize(QSize(ADD_BUTTON_GLYPH_EXTENT, ADD_BUTTON_GLYPH_EXTENT));
     add_btn->setAccessibleName("map_wizard_add_button");
     add_btn->setCursor(Qt::PointingHandCursor);

     // Letterspaced capitals, and both halves have to be done by hand: Qt style sheets have
     // neither text-transform nor letter-spacing, so the capitals live in the string above and
     // the tracking is a QFont here. It survives the sheet's font resolution because
     // QFont::resolve merges PER ATTRIBUTE and the sheet declares no letter spacing -- the same
     // mechanism the launcher's headings rely on.
     applyTracking(add_btn, LABEL_TRACKING);

    /* set-up widget for seaching etc... through _continents_table */
    {
        QWidget* _first_tab = new QWidget(this);
        _first_tab->setObjectName("map-maps-page");
        QVBoxLayout* _first_tab_layout = new QVBoxLayout();

        // Panel side margin is SPACE_12 on every edge, and one SPACE_8 gap between the search
        // section, the map list and the add button. The column previously ran on 8px gutters
        // beside a 9px one and a hand-placed 5px spacer -- three distances down one column.
        _first_tab_layout->setContentsMargins(Style::SPACE_12, Style::SPACE_12,
                                              Style::SPACE_12, Style::SPACE_12);
        _first_tab_layout->setSpacing(Style::SPACE_8);

        _first_tab->setLayout(_first_tab_layout);

        QLineEdit* _line_edit_search = new QLineEdit(this);
        QComboBox* _combo_search = new QComboBox(this);
        _combo_search->addItems(QStringList() <<
                                tr("All") <<
                                tr("Unknown") <<
                                tr("Continent") <<
                                tr("Dungeon") <<
                                tr("Raid") <<
                                tr("Battleground") <<
                                tr("Arena") <<
                                tr("Scenario"));
        QSettings settings;
        _combo_search->setCurrentIndex(settings.value("mapComboSearch", 2).toInt());

        QComboBox* _combo_exp_search = new QComboBox(this);
        _combo_exp_search->addItem(tr("All"));
        _combo_exp_search->addItem(QIcon(":/icon-classic"), tr("Classic"));
        _combo_exp_search->addItem(QIcon(":/icon-burning"), tr("Burning Cursade"));
        _combo_exp_search->addItem(QIcon(":/icon-wrath"), tr("Wrath of the Lich King"));
        _combo_exp_search->addItem(QIcon(":/icon-cata"), tr("Cataclism"));
        _combo_exp_search->addItem(QIcon(":/icon-panda"), tr("Mist of Pandaria"));
        _combo_exp_search->addItem(QIcon(":/icon-warlords"), tr("Warlords of Draenor"));
        _combo_exp_search->addItem(QIcon(":/icon-legion"), tr("Legion"));
        _combo_exp_search->addItem(QIcon(":/icon-battle"), tr("Battle for Azeroth"));
        _combo_exp_search->addItem(QIcon(":/icon-shadow"), tr("Shadowlands"));
        _combo_exp_search->setCurrentIndex(0);

        QCheckBox* _wmo_maps_search = new QCheckBox("Display WMO maps (No terrain)", this);

        QObject::connect(_line_edit_search, QOverload<const QString&>::of(&QLineEdit::textChanged), [this, _combo_search, _combo_exp_search, _wmo_maps_search](const QString &name)
                         {
                             applyFilterSearch(name, _combo_search->currentIndex(), _combo_exp_search->currentIndex(), _wmo_maps_search->isChecked());
                         });

        QObject::connect(_combo_search, QOverload<int>::of(&QComboBox::currentIndexChanged), [this, _line_edit_search, _combo_exp_search, _wmo_maps_search](int index)
                         {
                             applyFilterSearch(_line_edit_search->text(), index, _combo_exp_search->currentIndex(), _wmo_maps_search->isChecked());
                             QSettings settings;
                             settings.setValue("mapComboSearch", index);
                             settings.sync();
                         });

        QObject::connect(_combo_exp_search, QOverload<int>::of(&QComboBox::currentIndexChanged), [this, _line_edit_search, _combo_search, _wmo_maps_search](int index)
                         {
                             applyFilterSearch(_line_edit_search->text(), _combo_search->currentIndex(), index, _wmo_maps_search->isChecked());
                         });

        QObject::connect(_wmo_maps_search, &QCheckBox::stateChanged, [this, _line_edit_search, _combo_search, _combo_exp_search](bool b)
                         {
                             applyFilterSearch(_line_edit_search->text(), _combo_search->currentIndex(), _combo_exp_search->currentIndex(), b);
                         });

        // A placeholder, not a clear button: a clear button would ADD a control to the window.
        // Measured against the running application, the placeholder renders at text.dim #BFB7AA
        // where the combo boxes beside it render their real content at text.hi #F3F0E9, so an
        // empty filter cannot be mistaken for a set one.
        _line_edit_search->setObjectName("map-search-name");
        _line_edit_search->setPlaceholderText(tr("Filter by name"));

        // THE GROUP BOX IS GONE. A QGroupBox draws a titled frame around its contents, which put
        // a second box inside a column that is already a panel inside a tab; the mockup asks for
        // a SECTION HEADING instead -- a small letterspaced capital, a magnifier, and a hairline
        // running to the far edge of the column. That is three widgets and no frame.
        QWidget* const search_heading = buildSectionHeading(_first_tab, tr("SEARCH"));

        QFormLayout* _group_layout = new QFormLayout();

        // The trailing " : " on each label was doing the work a layout gap should do, and it put
        // three different amounts of whitespace between a label and its field depending on how
        // wide the label was. The colon goes; the gap is now one value from the scale.
        _group_layout->setContentsMargins(0, 0, 0, 0);
        _group_layout->setHorizontalSpacing(Style::SPACE_8);
        _group_layout->setVerticalSpacing(Style::SPACE_8);
        _group_layout->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        _group_layout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

        _group_layout->addRow(tr("Name"), _line_edit_search);
        _group_layout->addRow(tr("Type"), _combo_search);
        _group_layout->addRow(tr("Expansion"), _combo_exp_search);
        _group_layout->addRow( _wmo_maps_search);

        _first_tab_layout->addWidget(search_heading);
        _first_tab_layout->addLayout(_group_layout);
        _first_tab_layout->addWidget(_continents_table, 1);
        _first_tab_layout->addWidget(add_btn);

        entry_points_tabs->addTab(
            _first_tab,
            Noggit::Ui::FontAwesomeIcon(Noggit::Ui::FontAwesome::Icons::map),
            tr("Maps"));

        // The bookmarks list used to BE the tab page, so it sat flush against the pane on all
        // four sides while the Maps tab beside it had a 12px gutter -- switching tabs moved the
        // whole column. It gets the same frame, and an empty state instead of a blank rectangle.
        QWidget* const bookmarks_page = new QWidget(this);
        bookmarks_page->setObjectName("map-bookmarks-page");
        QVBoxLayout* const bookmarks_layout = new QVBoxLayout(bookmarks_page);
        bookmarks_layout->setContentsMargins(Style::SPACE_12, Style::SPACE_12,
                                             Style::SPACE_12, Style::SPACE_12);
        bookmarks_layout->setSpacing(Style::SPACE_8);
        bookmarks_layout->addWidget(bookmarks_table, 1);

        if (_project->Bookmarks.empty())
        {
          QLabel* const empty = new QLabel(
              tr("No bookmarks yet.\nSave one from the editor to return to a spot."),
              bookmarks_page);
          empty->setObjectName(Style::NAME_SECONDARY);
          empty->setAlignment(Qt::AlignCenter);
          empty->setWordWrap(true);
          bookmarks_layout->insertWidget(0, empty);
        }

        entry_points_tabs->addTab(
            bookmarks_page,
            Noggit::Ui::FontAwesomeIcon(Noggit::Ui::FontAwesome::Icons::bookmark),
            tr("Bookmarks"));

        entry_points_tabs->setIconSize(QSize(TAB_GLYPH_EXTENT, TAB_GLYPH_EXTENT));
        entry_points_tabs->setFixedWidth(MAP_COLUMN_WIDTH);
        layout->addWidget(entry_points_tabs);

        _buildMapListComponent->buildMapList(this);
        applyFilterSearch(_line_edit_search->text(), _combo_search->currentIndex(), _combo_exp_search->currentIndex(), _wmo_maps_search->isChecked());
    }

    qulonglong bookmark_index(0);
    for (auto entry: _project->Bookmarks)
    {
      auto item = new QListWidgetItem(bookmarks_table);

      auto bookmark_data = Widget::MapListBookmarkData();
      bookmark_data.MapName = QString::fromStdString(entry.name);
      bookmark_data.Position = entry.position;

      auto map_bookmark_item = new Widget::MapListBookmarkItem(bookmark_data, bookmarks_table);

      item->setData(Qt::UserRole, QVariant(bookmark_index++));
      item->setSizeHint(map_bookmark_item->minimumSizeHint());
      bookmarks_table->setItemWidget(item, map_bookmark_item);
    }

    QObject::connect(bookmarks_table, &QListWidget::itemDoubleClicked, [this](QListWidgetItem* item)
                     {

                       auto& entry(_project->Bookmarks.at(item->data(Qt::UserRole).toInt()));

                       _map_creation_wizard->_world.reset();

                       for (DBCFile::Iterator it = gMapDB.begin(); it != gMapDB.end(); ++it)
                       {
                         if (it->getInt(MapDB::MapID) == entry.map_id)
                         {
                           //     emit mapSelected(map_id); to update UI
                           _map_creation_wizard->_world = std::make_unique<World>(it->getString(MapDB::InternalName),
                                                            entry.map_id, Noggit::NoggitRenderContext::MAP_VIEW);
                           check_uid_then_enter_map(entry.position, math::degrees(entry.camera_pitch), math::degrees(entry.camera_yaw),
                                                    true
                           );
                           return;
                         }
                       }
                     }
    );


    _minimap = new minimap_widget(this);
    _minimap->draw_boundaries(true);
    //_minimap->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);

    QObject::connect(_minimap, &minimap_widget::map_clicked, [this](::glm::vec3 const& pos)
                     {
                        if (getWorld()->mapIndex.hasAGlobalWMO()) // skip uid check
                            enterMapAt(pos, math::degrees(30.f), math::degrees(90.f), uid_fix_mode::none, false);
                        else
                            check_uid_then_enter_map(pos, math::degrees(30.f), math::degrees(90.f));
                     }
    );

    _right_side = new QTabWidget(this);
    _right_side->setObjectName("map-detail-tabs");

    auto minimap_holder = new QScrollArea(this);
    minimap_holder->setWidgetResizable(true);
    minimap_holder->setAlignment(Qt::AlignCenter);
    minimap_holder->setWidget(_minimap);
    minimap_holder->setFrameShape(QFrame::NoFrame);
    minimap_holder->setAccessibleName("main_menu_minimap_holder");

    // THE VIEWPORT IS A SEPARATE WIDGET and it has to be named too. The scroll area used to be
    // the framed object on this pane -- a bg.void box with an edge border, which is why the
    // sheet had a rule for its accessible name -- and now it sits INSIDE the preview panel,
    // which carries that frame instead. Left alone, the viewport would match the sheet's global
    // QWidget rule and paint a bg.panel #292621 rectangle inside a bg.alt #1D1916 panel, a
    // 1.158:1 patch with no meaning at all. The sheet turns both transparent by name.
    minimap_holder->viewport()->setObjectName("map-preview-viewport");

    // WHY THE RIGHT PANE LOOKED EMPTY. It held one widget: a scroll area with a square minimap
    // centred in it. Maximised, that is a 1200px-wide region containing a picture and no words
    // at all -- the name of the map the user just clicked was only ever visible back in the list
    // on the far side of the window, and there was nothing anywhere saying what to do next.
    //
    // The pane becomes a page with a head, a body and a foot: which map is selected and what it
    // is, at the two top ranks; a panel holding either the minimap or an empty state; and one
    // line of instruction at the secondary rank. Every string comes from the row already
    // selected in the list, so nothing new is read from the client and no lookup can fail.
    QWidget* const map_detail_page = new QWidget(this);
    map_detail_page->setObjectName("map-detail-page");

    QVBoxLayout* const map_detail_layout = new QVBoxLayout(map_detail_page);
    map_detail_layout->setContentsMargins(Style::SPACE_12, Style::SPACE_12,
                                          Style::SPACE_12, Style::SPACE_12);
    map_detail_layout->setSpacing(Style::SPACE_12);

    QVBoxLayout* const map_detail_head = new QVBoxLayout();
    map_detail_head->setContentsMargins(0, 0, 0, 0);
    // SPACE_2: the title and its metadata are one unit, not two siblings.
    map_detail_head->setSpacing(Style::SPACE_2);

    // The pane's head is a LIVE VALUE -- it is the name of whichever map is selected -- so it
    // takes the value rank's colour (text.hi) from the sheet by property, and its size from the
    // window-title rank here. That composition is measured: the font-only widget sheet supplies
    // 15px/600 and the sheet's QLabel[state="value"] rule still supplies #F3F0E9.
    _map_detail_title = new QLabel(map_detail_page);
    _map_detail_title->setObjectName("map-detail-title");
    Style::markValue(_map_detail_title);
    Style::applyRank(_map_detail_title, Style::RANK_WINDOW_TITLE_PIXELS,
                     Style::RANK_WINDOW_TITLE_WEIGHT);

    _map_detail_meta = new QLabel(map_detail_page);
    _map_detail_meta->setObjectName(Style::NAME_SECONDARY);

    map_detail_head->addWidget(_map_detail_title);
    map_detail_head->addWidget(_map_detail_meta);

    // ------------------------------------------------------------- the preview panel --
    //
    // ONE PANEL, TWO PAGES, and a QStackedLayout rather than show/hide because the two pages
    // must not be able to disagree about the panel's size: a stacked layout sizes itself to the
    // largest of its pages and keeps that size whichever is current, so switching from the empty
    // state to the minimap cannot make the pane jump.
    //
    // The minimap keeps its own scroll area and its own double-click-to-enter connection
    // untouched. All that changed is what it is parented into.
    QWidget* const preview_panel = new QWidget(map_detail_page);
    preview_panel->setObjectName("map-preview-panel");
    preview_panel->setAttribute(Qt::WA_StyledBackground, true);

    _map_preview_stack = new QStackedLayout(preview_panel);
    _map_preview_stack->setContentsMargins(Style::SPACE_12, Style::SPACE_12,
                                           Style::SPACE_12, Style::SPACE_12);

    QWidget* const preview_empty = new QWidget(preview_panel);
    preview_empty->setObjectName("map-preview-empty");

    QVBoxLayout* const preview_empty_layout = new QVBoxLayout(preview_empty);
    preview_empty_layout->setContentsMargins(0, 0, 0, 0);
    preview_empty_layout->setSpacing(Style::SPACE_16);
    preview_empty_layout->addStretch(1);

    QLabel* const preview_pin = new QLabel(preview_empty);
    preview_pin->setObjectName("map-preview-pin");
    preview_pin->setAlignment(Qt::AlignCenter);
    preview_pin->setPixmap(Noggit::Ui::Widget::MapSelectionArt::mapPinGlyph(
        EMPTY_PIN_EXTENT, QColor(TEXT_OFF), devicePixelRatioF()));
    preview_empty_layout->addWidget(preview_pin, 0, Qt::AlignHCenter);

    QLabel* const preview_caption = new QLabel(tr("Select a map"), preview_empty);
    preview_caption->setObjectName("map-preview-caption");
    preview_caption->setAlignment(Qt::AlignCenter);
    preview_empty_layout->addWidget(preview_caption, 0, Qt::AlignHCenter);

    preview_empty_layout->addStretch(1);

    _map_preview_stack->addWidget(preview_empty);
    _map_preview_stack->addWidget(minimap_holder);

    // ----------------------------------------------------------------- the hint row --

    QWidget* const hint_row = new QWidget(map_detail_page);
    hint_row->setObjectName("map-detail-hint-row");

    QHBoxLayout* const hint_layout = new QHBoxLayout(hint_row);
    hint_layout->setContentsMargins(0, 0, 0, 0);
    hint_layout->setSpacing(Style::SPACE_8);

    // The info mark DOES come from the shipped icon set -- info.png is there, and the set's
    // first tier is a PNG beside the executable, so it resolves without the Font Awesome font
    // this fork does not redistribute. ThemeIcons::pixmap rather than the icon engine because it
    // is the only entry point that takes both a pen colour and a device pixel ratio; the engine
    // resolves its pen from one of two theme slots and neither is text.dim by request.
    //
    // A null pixmap is a legitimate answer, for a theme that ships no artwork under that name.
    // The label is simply left empty in that case and the hint reads as a plain line of text,
    // which is a worse hint and not a broken one.
    QPixmap const hint_glyph
      ( Noggit::Ui::ThemeIcons::pixmap
          ( Noggit::Ui::IconSet::FontAwesome
          , static_cast<char32_t>(Noggit::Ui::FontAwesome::Icons::info)
          , QIcon::Off
          , QSize(HINT_GLYPH_EXTENT, HINT_GLYPH_EXTENT)
          , devicePixelRatioF()
          , QColor(TEXT_DIM)
          )
      );

    QLabel* const hint_mark = new QLabel(hint_row);
    hint_mark->setObjectName("map-detail-hint-glyph");
    hint_mark->setFixedSize(HINT_GLYPH_EXTENT, HINT_GLYPH_EXTENT);

    if (!hint_glyph.isNull())
      hint_mark->setPixmap(hint_glyph);

    QLabel* const map_detail_hint = new QLabel(
        tr("Double-click a tile on the minimap to enter the map there."), hint_row);
    map_detail_hint->setObjectName(Style::NAME_SECONDARY);
    map_detail_hint->setWordWrap(true);

    hint_layout->addWidget(hint_mark, 0, Qt::AlignTop);
    hint_layout->addWidget(map_detail_hint, 1);

    map_detail_layout->addLayout(map_detail_head);
    map_detail_layout->addWidget(preview_panel, 1);
    map_detail_layout->addWidget(hint_row);

    _right_side->addTab(map_detail_page, tr("Enter map"));

    _map_wizard_connection = connect(_map_creation_wizard,
                                     &Noggit::Ui::Tools::MapCreationWizard::Ui::MapCreationWizard::map_dbc_updated,
                                [this](int map_id = -1)
                                     {
                                       _buildMapListComponent->buildMapList(this);

                                       // if a new map was added select it
                                       if (map_id >= 0)
                                       {
                                           for (int i = 0; i < _continents_table->count(); ++i)
                                           {
                                               QListWidgetItem* item = _continents_table->item(i);
                                               if (item && item->data(Qt::UserRole).toInt() == map_id)
                                               {
                                                   _continents_table->setCurrentItem(item); // calls itemSelectionChanged -> loadmap
                                                   _continents_table->scrollToItem(item);
                                                   _right_side->setCurrentIndex(0); // swap to "enter map" tab
                                                   // loadMap(map_id);
                                                   break;
                                               }
                                           }
                                       }
                                     });

    _right_side->addTab(_map_creation_wizard, tr("Edit map"));

    layout->addWidget(_right_side);

    connect(add_btn, &QPushButton::clicked
        , [&]()
        {
            _right_side->setCurrentIndex(1);
            _map_creation_wizard->addNewMap();
        });

    //setCentralWidget (_stack_widget);

    _minimap->adjustSize();

    updateMapDetail();
  }

  void NoggitWindow::updateMapDetail()
  {
    if (!_map_detail_title || !_map_detail_meta)
      return;

    QListWidgetItem* const item = _continents_table ? _continents_table->currentItem() : nullptr;
    auto* const row = item
        ? qobject_cast<Noggit::Ui::Widget::MapListItem*>(_continents_table->itemWidget(item))
        : nullptr;

    if (!row)
    {
      // The empty state is a state, not a blank. Before this the pane simply showed the
      // minimap's own placeholder and nothing else; now the preview panel swaps to a page that
      // says what the pane is for.
      _map_detail_title->setText(tr("No map selected"));
      _map_detail_meta->setText(tr("Pick a map from the list to preview it."));

      if (_map_preview_stack)
        _map_preview_stack->setCurrentIndex(PREVIEW_PAGE_EMPTY);

      return;
    }

    if (_map_preview_stack)
      _map_preview_stack->setCurrentIndex(PREVIEW_PAGE_MINIMAP);

    _map_detail_title->setText(row->name());

    // The same two facts the list row carries, spelled out: a bare "530" in a list is a hint, a
    // header has room to say what the number is. The instance-type wording is deliberately the
    // same string the Type filter above the list uses, so the two cannot disagree.
    QString instance_type(tr("Unknown"));
    switch (row->type())
    {
      case 0: instance_type = tr("Continent"); break;
      case 1: instance_type = tr("Dungeon"); break;
      case 2: instance_type = tr("Raid"); break;
      case 3: instance_type = tr("Battleground"); break;
      case 4: instance_type = tr("Arena"); break;
      case 5: instance_type = tr("Scenario"); break;
      default: break;
    }

    _map_detail_meta->setText(tr("Map %1  ·  %2%3")
                                  .arg(row->id())
                                  .arg(instance_type)
                                  .arg(row->wmo_map() ? tr("  ·  WMO only, no terrain")
                                                      : QString()));
  }

  void NoggitWindow::closeEvent(QCloseEvent* event)
  {
    if (map_loaded)
    {
      event->ignore();
      promptExit(event);
    } else
    {
      if (exit_to_project_selection)
      {
        auto noggit = Noggit::Application::NoggitApplication::instance();
        auto project_selection = new Noggit::Ui::Windows::NoggitProjectSelectionWindow(noggit);
        project_selection->show();
      }
      else
        event->accept();
    }
    exit_to_project_selection = false;
  }

  void NoggitWindow::handleEventMapListContextMenuPinMap(int mapId, std::string MapName)
  {
    _project->pinMap(mapId, MapName);
    _buildMapListComponent->buildMapList(this);
  }

  void NoggitWindow::handleEventMapListContextMenuUnpinMap(int mapId)
  {
    _project->unpinMap(mapId);
    _buildMapListComponent->buildMapList(this);
  }

  World* NoggitWindow::getWorld()
  {
    return _map_creation_wizard->getWorld();
  }


  void NoggitWindow::promptExit(QCloseEvent* event)
  {
    emit exitPromptOpened();

    QMessageBox prompt;
    prompt.setModal(true);
    prompt.setIcon(QMessageBox::Warning);
    prompt.setText("Exit?");
    prompt.setInformativeText("Any unsaved changes will be lost.");
    prompt.addButton("Exit", QMessageBox::DestructiveRole);
    if (!exit_to_project_selection)
        prompt.addButton("Return to menu", QMessageBox::AcceptRole);
    prompt.setDefaultButton(prompt.addButton("Cancel", QMessageBox::RejectRole));
    prompt.setWindowFlags(Qt::CustomizeWindowHint | Qt::WindowTitleHint | Qt::WindowStaysOnTopHint);

    prompt.exec();

    switch (prompt.buttonRole(prompt.clickedButton()))
    {
      case QMessageBox::AcceptRole:
        _stack_widget->setCurrentIndex(0);
        _stack_widget->removeLast();
        delete _map_view;
        _map_view = nullptr;
        _minimap->world(nullptr);

        map_loaded = false;
        break;
      case QMessageBox::DestructiveRole:
        Noggit::Ui::Tools::ViewportManager::ViewportManager::unloadAll();
        setCentralWidget(_null_widget = new QWidget(this));
        if (exit_to_project_selection)
        {
            auto noggit = Noggit::Application::NoggitApplication::instance();
            auto project_selection = new Noggit::Ui::Windows::NoggitProjectSelectionWindow(noggit);
            project_selection->show();
        }
        event->accept();
        break;
      default:
        event->ignore();
        break;
    }
  }

  void NoggitWindow::promptUidFixFailure()
  {
    _stack_widget->setCurrentIndex(0);

    QMessageBox::critical
        (nullptr, "UID fix failed", "The UID fix couldn't be done because some models were missing or fucked up.\n"
                                    "The models are listed in the log file.", QMessageBox::Ok
        );
  }
  void NoggitWindow::startWowClient()
  {
      // TODO auto login

      // TODO attach to process to allow memory edits

      // std::filesystem::path WoW_path = std::filesystem::path(Noggit::Project::CurrentProject::get()->ClientPath) / "Wow.exe";
      std::filesystem::path WoW_path = std::filesystem::path(_project->ClientPath) / "Wow.exe";

      QString program_path = WoW_path.string().c_str();
      QFileInfo checkFile(program_path);

      QStringList arguments;
      // arguments << "-console"; // deosn't seem to work
      arguments << "-windowed";
      // auto login using https://github.com/FrostAtom/awesome_wotlk, requires patched Wow.exe
      // arguments << "-login \"LOGIN\" -password \"PASSWORD\" -realmlist \"REALMLIST\" -realmname \"REALMNAME\"";


      if (checkFile.exists() && checkFile.isFile())
      {
          QProcess* process = new QProcess(this); // this parent?
          process->start(program_path, arguments);

          //Noggit::Application::NoggitApplication::instance().
          if (!process->waitForStarted()) {
              // qWarning("Failed to start process");
              QMessageBox::information(this, "Error", "Failed to start process Wow.exe");
          }
      }
      else
      {
          // qWarning("File does not exist");
          QMessageBox::information(this, "Error", std::format("File {} does not exist", WoW_path.string()).c_str());
      }
  }

  QString NoggitWindow::packReferencedAssets( BlizzardArchive::ClientData* client_data
                                            , BlizzardArchive::Archive::MPQArchive* archive
                                            , bool include_base_client_assets
                                            , bool compress
                                            , bool compact
                                            , QString& detail_out
                                            )
  {
      if (!client_data || !archive)
      {
          return "Referenced assets: no client data, nothing collected.";
      }

      Noggit::PatchAssetPackerOptions options;
      options.include_base_client_assets = include_base_client_assets;
      options.compress = compress;
      options.compact = compact;

      Noggit::PatchAssetPacker packer (client_data, archive, options);

      // APPLICATION MODAL, and that is the safety property, not a nicety.
      //
      // QProgressDialog::setValue pumps the event loop, and a use-after-free from exactly that has
      // already happened in this codebase (the ambient occlusion bake held MapTile pointers across
      // it while MapView::tick unloaded them). Modality means the only event the user can generate
      // while this runs is Cancel. Nothing that could unload the project, close the window or
      // destroy the archive is reachable.
      //
      // Nothing raw is held across the pump either: the packer keeps only the ClientData and
      // MPQArchive pointers, both owned by the application singleton for its whole lifetime, and
      // the lambda captures only this stack frame, which cannot go away while run() is on it.
      QProgressDialog progress ("Collecting referenced assets...", "Cancel", 0, 1, this);
      progress.setWindowTitle("Patch Client");
      progress.setWindowModality(Qt::ApplicationModal);
      progress.setMinimumDuration(0);
      progress.setAutoClose(false);
      progress.setAutoReset(false);
      progress.setValue(0);

      char const* last_phase = nullptr;
      std::size_t last_shown = 0;

      Noggit::PatchAssetPackerResult const result = packer.run
        ( [&] (char const* phase, std::size_t done, std::size_t total, std::string const& current) -> bool
          {
              // Throttled, because every setValue is a full trip through the event loop and this is
              // called once per asset -- tens of thousands of times on a real project. Repainting
              // that often is slower than the work being reported on.
              bool const interesting
                (phase != last_phase || done >= last_shown + 64 || done >= total);

              if (interesting)
              {
                  last_phase = phase;
                  last_shown = done;

                  progress.setLabelText
                    (QString("%1\n%2").arg(QString::fromLatin1(phase), QString::fromStdString(current)));
                  progress.setMaximum(static_cast<int>(std::min<std::size_t>(total, 1000000)));
                  progress.setValue(static_cast<int>(std::min<std::size_t>(done, 1000000)));
              }

              return !progress.wasCanceled();
          }
        );

      progress.close();

      detail_out = QString::fromStdString(result.toReport());

      Log << "Patch dependencies: " << result.summary() << std::endl;

      QString summary (QString::fromStdString(result.summary()));

      if (result.cancelled)
      {
          summary += "\n\nCancelled: the archive contains whatever had already been written.";
      }

      if (!result.parse_failures.empty())
      {
          // ABOVE the resolved/unresolved counts, because it is the paragraph that says those
          // counts are incomplete. A file whose contents could not be parsed took every reference
          // it makes out of the walk, so none of those names appear anywhere else in this report or
          // in the numbers next to it -- they were never looked up at all.
          summary += QString("\n\n%1 file(s) were read but could NOT be fully parsed. Some or all"
                             " of what they reference was never followed, so it is not in this"
                             " patch and no other line here can name it."
                             "\nOpen \"Show Details\" for each file, what it recovered, and where"
                             " it stopped.")
                       .arg(result.parse_failures.size());
      }

      if (result.scan.hasFailures())
      {
          // Named, never merely counted. A dependency pack that omits something silently is the
          // failure mode that wastes the user's evening -- they find out in game, with no idea
          // which file it was.
          summary += QString("\n\n%1 reference(s) are NOT in the patch: %2 could not be resolved"
                             " anywhere, %3 resolved but could not be read."
                             "\nOpen \"Show Details\" for every name.")
                       .arg(result.scan.distinctMissing() + result.scan.distinctUnreadable())
                       .arg(result.scan.distinctMissing())
                       .arg(result.scan.distinctUnreadable());
      }

      return summary;
  }

  void NoggitWindow::patchWowClient()
  {
      // Option to make folder patch ?

      QDialog* mpq_patch_params = new QDialog(this);
      mpq_patch_params->setWindowFlags(Qt::Dialog);
      mpq_patch_params->setWindowTitle("Patch Export Settings");
      QVBoxLayout* mpq_patch_params_layout = new QVBoxLayout(mpq_patch_params);

      mpq_patch_params_layout->addWidget(new QLabel("<font color=orange><h4> Warning :\
          \nErrors can cause MPQ Corruption, make sure to have backup.</h4></font>\
          \nWhile Noggit is writting the archive, files are not accessible by the client, please wait.", mpq_patch_params));

      mpq_patch_params_layout->addWidget(new QLabel("MPQ Name:", mpq_patch_params));

      QLineEdit* mpq_patch_params_ledit = new QLineEdit("patch-A.MPQ", mpq_patch_params);
      QSettings settings;
      // saved last set patch name
      mpq_patch_params_ledit->setText(settings.value("noggit_window/mpq_name", "patch-A.MPQ").toString());
      mpq_patch_params_layout->addWidget(mpq_patch_params_ledit);


      QCheckBox* mpq_patch_params_locale_chk = new QCheckBox("Save DBC to Locale:", mpq_patch_params);
      mpq_patch_params_locale_chk->setToolTip("This is recommended for non English clients");
      //auto set locale mode if client isn't english.
      bool non_english = false;
      BlizzardArchive::Locale locale_mode = Noggit::Application::NoggitApplication::instance()->clientData()->locale_mode();
      if (!(locale_mode != BlizzardArchive::Locale::AUTO) && !(locale_mode != BlizzardArchive::Locale::enGB)
          && !(locale_mode != BlizzardArchive::Locale::enUS))
      {
          non_english = true;
      }
      mpq_patch_params_locale_chk->setChecked(non_english);
      // Unimplemented
      mpq_patch_params_locale_chk->setHidden(true);
      mpq_patch_params_locale_chk->setDisabled(true);
      mpq_patch_params_layout->addWidget(mpq_patch_params_locale_chk);


      // mode choice between replace archive and add files to archive
      QCheckBox* mpq_overwrite_chk = new QCheckBox("Overwrite Archive", mpq_patch_params);
      mpq_overwrite_chk->setToolTip("If there is already an archive with this name, it will be deleted and replaced by a new one.");
      mpq_overwrite_chk->setChecked(false);
      // Unimplemented
      mpq_overwrite_chk->setHidden(true);
      mpq_overwrite_chk->setDisabled(true);
      mpq_patch_params_layout->addWidget(mpq_overwrite_chk);


      QCheckBox* mpq_compact_chk = new QCheckBox("Compact Archive", mpq_patch_params);
      mpq_compact_chk->setToolTip("Adding and removing files creates empty space in the MPQ, making it grow in size.\
          \nCompacting takes a few seconds depending on patch size, it is recommended to compact when many files have been added.");
      mpq_compact_chk->setChecked(false);
      mpq_patch_params_layout->addWidget(mpq_compact_chk);

      QCheckBox* mpq_compress_files_chk = new QCheckBox("Compress Files (slow)", mpq_patch_params);
      mpq_compress_files_chk->setToolTip("Files will be compressed in the MPQ, but writting becomes much slower(about 3x slower).\
          \nRecommended for distribution.");
      mpq_compress_files_chk->setChecked(true);
      mpq_patch_params_layout->addWidget(mpq_compress_files_chk);

      // Dependency packing. The project folder holds the terrain; the models and textures that
      // terrain REFERENCES usually live somewhere else entirely, and a patch containing an ADT
      // whose WMO is missing renders as a hole in the world.
      QCheckBox* mpq_dependencies_chk = new QCheckBox("Include referenced models and textures", mpq_patch_params);
      mpq_dependencies_chk->setToolTip("Follows every reference out of the project's ADTs -- models, world models and\
          \ntheir group files, textures, .skin and .anim files -- and adds the ones the\
          \nplayer does not already have.\
          \n\nWithout this, a model you placed from another patch is named by the terrain\
          \nbut absent from the archive, and is invisible in game.");
      mpq_dependencies_chk->setChecked(settings.value("noggit_window/mpq_pack_dependencies", true).toBool());
      mpq_patch_params_layout->addWidget(mpq_dependencies_chk);

      QCheckBox* mpq_base_assets_chk = new QCheckBox("    ...including files already in the base client", mpq_patch_params);
      mpq_base_assets_chk->setToolTip("Off: only assets that are NOT in a stock 3.3.5a archive are added, which is the\
          \nuseful behaviour -- a stock building references stock textures every player\
          \nalready has, and copying them would multiply the size of the patch for no effect.\
          \n\nOn: every referenced file is copied in, whatever it came from. Use this only to\
          \nbuild a self-contained archive for someone with no base client.");
      // Default OFF, the conservative option: it cannot make the archive enormous, and a file the
      // player already has is not a file that can be missing.
      mpq_base_assets_chk->setChecked(settings.value("noggit_window/mpq_pack_base_assets", false).toBool());
      mpq_base_assets_chk->setEnabled(mpq_dependencies_chk->isChecked());
      mpq_patch_params_layout->addWidget(mpq_base_assets_chk);

      connect(mpq_dependencies_chk, &QCheckBox::toggled, mpq_base_assets_chk, &QCheckBox::setEnabled);


      QPushButton* mpq_patch_params_okay = new QPushButton("Save Project to Client MPQ", mpq_patch_params);
      mpq_patch_params_layout->addWidget(mpq_patch_params_okay);

      connect(mpq_patch_params_okay, &QPushButton::clicked
          , [=]()
          {
              // check if mpq name is allowed
              if (!Noggit::Application::NoggitApplication::instance()->clientData()->isMPQNameValid(mpq_patch_params_ledit->text().toLower().toStdString(), true))
              {
                  QMessageBox::warning(this, "Name Error", "MPQ Name is not allowed.\This name is already used by base client patches, or the client can't load it.\
                     \nYour patch must be named \"patch-[4-9].MPQ\" or \"patch-[A-Z].MPQ\".");
              }
              else
              {
                QSettings settings;
                settings.setValue("noggit_window/mpq_name", mpq_patch_params_ledit->text());
                settings.setValue("noggit_window/mpq_pack_dependencies", mpq_dependencies_chk->isChecked());
                settings.setValue("noggit_window/mpq_pack_base_assets", mpq_base_assets_chk->isChecked());
                settings.sync();

                mpq_patch_params->accept();
              }
          });

      // execute dialog, and run code when Mpq_patch_params_okay calls Mpq_patch_params->accept();
      if (mpq_patch_params->exec() == QDialog::Accepted)
      {
          // OK button was pressed, do stuff.

          auto archive_name = mpq_patch_params_ledit->text().toStdString();
          auto clientData = Noggit::Application::NoggitApplication::instance()->clientData();

          namespace fs = std::filesystem;
          // progress bar, count maximum files
          // ignore directories, only count files
          auto regular_file = [](const fs::path& path) {
              std::string const extension = path.extension().string();
              // filter noggit files
              if (extension == ".noggitproj" || extension == ".json" || extension == ".ini")
                  return false;
              return fs::is_regular_file(path);
              };

          int totalItems = std::count_if(fs::recursive_directory_iterator(clientData->projectPath()), {}, regular_file);

          if (!totalItems)
          {
              QMessageBox::warning(this, "Error", std::format("No files found in project {}", clientData->projectPath()).c_str());
              return;
          }

          if (!clientData->mpqArchiveExistsOnDisk(archive_name))
          {
              try
              {
                  auto archive = clientData->tryCreateMPQArchive(archive_name);
              }
              catch (BlizzardArchive::Exceptions::Archive::ArchiveOpenError& e)
              {
                  QMessageBox::critical(nullptr, "Error", e.what());
              }
              catch (...)
              {
                  QMessageBox::critical(nullptr, "Error", "Failed to create MPQ Archive. Unhandled exception.");
              }
          }
          {
              auto archive = clientData->getMPQArchive(archive_name);
              if (archive.has_value())
              {
                  auto progress_box = new QMessageBox(QMessageBox::Information, "Working...", std::format("Saving {} files to patch {}...\
                      \nClosing the program now can corrupt the MPQ.", std::to_string(totalItems), archive_name ).c_str(), QMessageBox::StandardButton::NoButton, this);
                  progress_box->setStandardButtons(QMessageBox::NoButton);
                  progress_box->setWindowFlags(progress_box->windowFlags() & ~Qt::WindowCloseButtonHint);
                  // progress_box->exec(); // this stops code execution
                  progress_box->repaint();
                  qApp->processEvents();

                  bool const pack_dependencies = mpq_dependencies_chk->isChecked();
                  bool const compress_files = mpq_compress_files_chk->isChecked();
                  bool const compact_archive = mpq_compact_chk->isChecked();

                  try
                  {
                      auto start = std::chrono::high_resolution_clock::now();

                      // Compaction is DEFERRED to the dependency pass when there is one.
                      // SFileCompactArchive rebuilds the whole archive, so compacting here and then
                      // adding several thousand more files would pay for the rebuild and then undo
                      // its benefit.
                      std::array<int, 2> result = clientData->saveLocalFilesToArchive(archive.value(), compress_files, compact_archive && !pack_dependencies);
                      int processed_files = result[0];
                      int files_failed = processed_files - result[1];

                      auto end = std::chrono::high_resolution_clock::now();
                      std::chrono::duration<double> duration = end - start;
                      std::ostringstream oss; // duration in seconds with 1 digit
                      oss << std::fixed << std::setprecision(1) << duration.count();

                      // if no file was processed, archive was most likely opened and not accessible
                      // TODO : we can throw an error message in saveLocalFilesToArchive if (!archive->openForWritting()) instead
                      if (!processed_files)
                      {
                          QMessageBox::warning(this, "Error", "Project Folder is not a valid directory or client MPQ is not accessible.\
                              \nMake sure it isn't opened by Wow or MPQ editor");
                      }
                      else
                      {
                          QString message
                            ( QString::fromStdString
                              ( std::format("Added {} project files to archive {} in {} seconds.\n{} files failed."
                                           , std::to_string(processed_files), archive_name, oss.str()
                                           , std::to_string(files_failed))
                              )
                            );

                          QString detail;

                          if (pack_dependencies)
                          {
                              message += "\n\n" + packReferencedAssets
                                ( clientData
                                , archive.value()
                                , mpq_base_assets_chk->isChecked()
                                , compress_files
                                , compact_archive
                                , detail
                                );
                          }

                          QMessageBox report (QMessageBox::Information, "Archive Updated", message, QMessageBox::Ok, this);

                          if (!detail.isEmpty())
                          {
                              report.setDetailedText(detail);
                          }

                          report.exec();
                      }
                  }
                  catch (...)
                  {
                      QMessageBox::critical(nullptr, "Error", "unhandled exception");
                  }

                  progress_box->close();
              }
              else
                  QMessageBox::warning(this, "Error", std::format("Error accessing archive {}", archive_name).c_str());
          }
      }
  }
}
