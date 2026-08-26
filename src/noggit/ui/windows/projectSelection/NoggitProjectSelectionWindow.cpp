#include <noggit/application/Configuration/NoggitApplicationConfiguration.hpp>
#include <noggit/application/NoggitApplication.hpp>
#include <noggit/Log.h>
#include <noggit/project/ApplicationProjectReader.h>
#include <noggit/project/CurrentProject.hpp>
#include <noggit/ui/FontAwesome.hpp>
#include <noggit/ui/windows/noggitWindow/NoggitWindow.hpp>
#include <noggit/ui/windows/projectCreation/NoggitProjectCreationDialog.h>
#include <noggit/ui/windows/projectSelection/components/CreateProjectComponent.hpp>
#include <noggit/ui/windows/projectSelection/components/LoadProjectComponent.hpp>
#include <noggit/ui/windows/projectSelection/components/RecentProjectsComponent.hpp>
#include <noggit/ui/windows/projectSelection/NoggitProjectSelectionWindow.hpp>
#include <noggit/ui/windows/projectSelection/widgets/BrandBanner.hpp>
#include <noggit/ui/windows/projectSelection/widgets/LauncherArt.hpp>
#include <noggit/ui/windows/settingsPanel/SettingsPanel.h>
#include <noggit/ui/windows/UiStyle.hpp>


#include <QAbstractItemView>
#include <QBoxLayout>
#include <QColor>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFont>
#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QIcon>
#include <QSizePolicy>
#include <QLabel>
#include <QLayout>
#include <QListWidget>
#include <QPushButton>
#include <QSettings>
#include <QString>
#include <QToolButton>
#include <QVBoxLayout>

#include "ui_NoggitProjectSelectionWindow.h"

#include <filesystem>


using namespace Noggit::Ui::Windows;

namespace
{
  namespace Style = Noggit::Ui::Windows::Style;
  namespace Art = Noggit::Ui::Widget::LauncherArt;

  //! The cog in the corner. 20 in the gear button's 22px content rect, which lays the button out
  //! at 1 + 8 + 22 + 8 + 1 = 40 -- see QToolButton#launcher-gear in theme.qss.
  constexpr int GEAR_GLYPH_EXTENT = 20;

  //! The recent-projects column's floor: two whole cards, so the list can never present itself
  //! as a single row with a scroll bar. A card is 135 tall (the arithmetic is at CARD_MIN_HEIGHT
  //! in ProjectListItem.cpp) and the view's ::item rule adds a 12px bottom margin between them:
  //!   2 x (135 + 12) = 294
  //! This is a FLOOR, not a fixed size -- the column still grows when the window does.
  constexpr int PROJECT_LIST_MIN_HEIGHT = 294;

  //! The Getting Started column. Fixed rather than elastic: a QVBoxLayout stretches its children
  //! across whatever width it is given, so without this the three action cards grow to fill every
  //! pixel the recent-projects list does not want.
  //!
  //! What a card spends before its label starts, all of it arithmetic this file controls:
  //!    1 border + 16 padding-left
  //!  + 52 icon (a 40px tile with 12px of transparency baked on to its right)
  //!  +  4 the icon-to-text gap QCommonStyle::drawControl(CE_PushButtonLabel) hardcodes
  //!  + 20 padding-right + 1 border
  //!  = 94
  //! leaving 206 of the 300 for the longest of the three labels.
  constexpr int ACTION_COLUMN_WIDTH = 300;

  // THE OUTER GLOW ON THE PRIMARY ACTION. Qt style sheets have no box-shadow -- it is not in the
  // Qt 5.15 property list, so check_qss.py would report it as an unknown property and Qt would
  // silently drop it. A QGraphicsDropShadowEffect with a zero offset is a glow rather than a
  // shadow, and it is the only route to one that does not involve a custom paintEvent.
  //
  // The effect is on the BUTTON, with no reserve container around it. The halo grows the widget's
  // effective rect and is painted into the PARENT's backing store, so it is clipped by the
  // central widget rather than by the button, and putting the button in a padded container would
  // have cost 20px of layout on every side and pushed the primary card out of line with the two
  // beneath it.
  constexpr int GLOW_BLUR = 26;

  //! brand.crimson at 40% alpha. The halo's brightest point composites to #65222B over the
  //! bg.void ground, 1.667:1 above it -- present, and nowhere near a light source.
  constexpr int GLOW_ALPHA = 0x66;

  // THE TWO ICON TILES, spelled out here because a QPainter is not reachable from a style sheet.
  //
  //! The primary tile is brand.crimson with an INK mark: 4.790:1, over the 4.5 floor. text.hi on
  //! solid crimson is 3.535:1 and is barred everywhere in this window -- a solid crimson surface
  //! carries ink and nothing else.
  constexpr QRgb TILE_PRIMARY = qRgb (0xE5, 0x40, 0x5C);
  constexpr QRgb TILE_PRIMARY_INK = qRgb (0x10, 0x0E, 0x0B);

  //! The secondary tile is bg.raised with a text.dim mark: 5.922:1. The tile itself is 1.281:1
  //! above the card's bg.panel fill, which is the sheet's own surface step.
  constexpr QRgb TILE_SECONDARY = qRgb (0x3C, 0x37, 0x32);
  constexpr QRgb TILE_SECONDARY_INK = qRgb (0xBF, 0xB7, 0xAA);

  //! Section-heading tracking, in ABSOLUTE pixels. Qt style sheets have no letter-spacing
  //! property and no text-transform either, so the capitals live in the translated string and the
  //! tracking has to be a QFont. See applyHeadingTracking for why it survives the sheet.
  constexpr int HEADING_TRACKING = 2;

  // The narrowest the window may become, from the layout's own numbers:
  //
  //   24 + 24  the column row's SPACE_24 side margins
  //  +420      listView minimumSize from the form -- a card needs
  //            20 + 56 icon + 16 + 16 + 104 art + 20 = 232 before the text column has a single
  //            pixel, so 420 leaves the path 188 to elide in
  //  + 32      SPACE_32 column gutter
  //  +300      ACTION_COLUMN_WIDTH
  //  = 800
  //
  // The banner states its own minimum from real font metrics rather than from a guess at the
  // wordmark's width; it comes out well under this, so this number is what binds.
  constexpr int WINDOW_MIN_WIDTH = 800;

  // And the shortest, which is the taller of the two columns plus the band:
  //
  //   144      the banner
  //  + 24      SPACE_24 between the band and the content
  //  +336      the action column: 20 heading + 72 + 72 + 72 + 0 spacer + 40 gear,
  //            with five SPACE_12 gaps = 336. (The recent-projects column is 20 + 12 + 294
  //            = 326, so the action column is what binds.)
  //  + 24      the bottom margin
  //  = 528
  constexpr int WINDOW_MIN_HEIGHT = 528;

  // What it OPENS at, as opposed to what it may be squeezed to. Wide enough that a card shows a
  // full project path without eliding and the action column still has air around it.
  constexpr int WINDOW_DEFAULT_WIDTH = 1180;
  constexpr int WINDOW_DEFAULT_HEIGHT = 760;

  //! Puts the section-heading tracking on a label, and is safe to call again.
  //!
  //! THE TRACKING SURVIVES THE STYLE SHEET, and the mechanism is worth stating because the same
  //! file records that a font SIZE does not: QStyleSheetStyle::updateStyleSheetFont resolves the
  //! rule's font against the widget's own with QFont::resolve, which merges PER ATTRIBUTE. The
  //! sheet's global rule declares font-family and font-size, so those two are taken from the
  //! rule; it declares no letter spacing, and QFont::LetterSpacingResolved is its own resolve
  //! bit, so the spacing set here is what survives. This is the same mechanism that let
  //! font-weight survive setFont where font-size did not (UiStyle.hpp, the table at the head of
  //! the file).
  //!
  //! The early return is not an optimisation. This is called again from changeEvent on a font
  //! change, and setFont() posts another font change -- without the guard that is an infinite
  //! recursion.
  void applyHeadingTracking (QLabel* label)
  {
    if (!label)
    {
      return;
    }

    QFont font (label->font());

    if (font.letterSpacingType() == QFont::AbsoluteSpacing
        && qRound (font.letterSpacing()) == HEADING_TRACKING)
    {
      return;
    }

    font.setLetterSpacing (QFont::AbsoluteSpacing, HEADING_TRACKING);
    label->setFont (font);
  }

  //! Wraps a column heading in a row: the letterspaced capitals with their crimson tick, then a
  //! hairline running to the far edge of the column.
  //!
  //! The tick is the label's own 3px border-left rather than a separate widget, which is the only
  //! way a style sheet can draw one -- and it is also the one place the mockup cannot be followed
  //! exactly. The mockup's tick is SHORTER than the text; a border-left is always the full height
  //! of the label's box, so the closest honest approximation is a 3px rule beside an 11px label
  //! with 3px of vertical padding, which comes to 19px against the text's 13px line box.
  void buildSectionHeading (QLabel* label, QBoxLayout* owner)
  {
    if (!label || !owner)
    {
      return;
    }

    int const index (owner->indexOf (label));

    QWidget* const row (new QWidget (label->parentWidget()));
    row->setObjectName ("launcher-heading-row");

    QHBoxLayout* const layout (new QHBoxLayout (row));
    layout->setContentsMargins (0, 0, 0, 0);
    layout->setSpacing (Style::SPACE_12);

    owner->removeWidget (label);

    label->setObjectName ("launcher-heading");
    applyHeadingTracking (label);
    layout->addWidget (label, 0, Qt::AlignVCenter);

    QFrame* const rule (new QFrame (row));
    rule->setObjectName ("launcher-heading-rule");
    rule->setFrameShape (QFrame::HLine);
    rule->setFrameShadow (QFrame::Plain);
    rule->setLineWidth (1);
    rule->setFixedHeight (1);
    layout->addWidget (rule, 1, Qt::AlignVCenter);

    owner->insertWidget (index, row);
  }
}

NoggitProjectSelectionWindow::NoggitProjectSelectionWindow(Noggit::Application::NoggitApplication* noggit_app,
                                                           QWidget* parent)
  : QMainWindow(parent)
  , _ui(new ::Ui::NoggitProjectSelectionWindow)
  , _noggit_application(noggit_app)
{
  // Qt::MSWindowsFixedSizeDialogHint IS GONE. On Windows it gives the window a non-resizable
  // frame, which made every comment in applyVisualDesign about the list expanding when the window
  // widens, and about the window's minimum size, describe behaviour no user could reach. The
  // two-column card layout is genuinely elastic and the list is the one thing on this window
  // worth more room, so the window may now be resized.
  setWindowFlags(Qt::Window);

  // BUILT BEFORE THE AUTOLOAD PATH USES IT. The favourite-project branch below calls
  // _load_project_component->loadProject(), and this line used to sit a hundred lines further
  // down -- so that call went through a null unique_ptr. It did not crash only because
  // LoadProjectComponent has no data members and loadProject never touches `this`, which makes it
  // undefined behaviour that happens to work rather than a working program.
  _load_project_component = std::make_unique<Component::LoadProjectComponent>();

  ////////////////////////////
  // auto load favorite project
  QSettings settings;
  int favorite_proj_idx = settings.value("favorite_project", -1).toInt();

  bool load_favorite = settings.value("auto_load_fav_project", true).toBool();

  // if it has client data, it means it already loaded before and we exited through the menu, skip autoloading favorite
  if (noggit_app->hasClientData())
      load_favorite = false;

  if (load_favorite && favorite_proj_idx != -1)
  {
    Log << "Auto loading favorite project index : " << favorite_proj_idx << std::endl;

    int size = settings.beginReadArray("recent_projects");

    QString project_final_path;

    // for (int i = 0; i < size; ++i)
    if (size > favorite_proj_idx)
    {
      settings.setArrayIndex(favorite_proj_idx);
      std::filesystem::path project_path = settings.value("project_path").toString().toStdString().c_str();

      if (std::filesystem::exists(project_path) && std::filesystem::is_directory(project_path))
      {
        auto project_reader = Noggit::Project::ApplicationProjectReader();
        
        auto project = project_reader.readProject(project_path);
        
        if (project.has_value())
        {
          // project->projectVersion;
          // project_directory = QString::fromStdString(project_path.generic_string());
          // auto project_name = QString::fromStdString(project->ProjectName);

          project_final_path = QString(project_path.string().c_str());
        }
      }
    }
    settings.endArray();

    if (!project_final_path.isEmpty())
    {
      auto selected_project = _load_project_component->loadProject(this, project_final_path);

      if (!selected_project)
      {
        LogError << "Selected Project is null, favorite loading failed." << std::endl;
      }
      else
      {
        Noggit::Project::CurrentProject::initialize(selected_project.get());

        _project_selection_page = std::make_unique<Noggit::Ui::Windows::NoggitWindow>(
            _noggit_application->getConfiguration(),
            selected_project);
        _project_selection_page->showMaximized();

        close();
        return;
      }
    }
  }
  ///////////////////////////

  _ui->setupUi(this);

  applyVisualDesign();

  _settings = new Noggit::Ui::settings(this);
  //_changelog = new Noggit::Ui::CChangelog(this);

  _ui->settings_button->setIcon(Noggit::Ui::FontAwesomeIcon(Noggit::Ui::FontAwesome::Icons::cog));
  _ui->settings_button->setIconSize(QSize(GEAR_GLYPH_EXTENT, GEAR_GLYPH_EXTENT));

  _ui->changelog_button->hide();
  //_ui->changelog_button->setIcon(Noggit::Ui::FontAwesomeIcon(Noggit::Ui::FontAwesome::Icons::file));
  //_ui->changelog_button->setIconSize(QSize(20, 20));
  //_ui->changelog_button->setText(tr(" Changelog"));
  //_ui->changelog_button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);

  Component::RecentProjectsComponent::buildRecentProjectsList(this);

  QObject::connect(_ui->settings_button, &QToolButton::clicked, [&]
      {
          _settings->show();
      }
  );

  /*QObject::connect(_ui->changelog_button, &QToolButton::clicked, [&]()
      {
          _changelog->SelectFirst();
          _changelog->show();
      });*/

  QObject::connect(_ui->button_create_new_project, &QPushButton::clicked, [=, this]
                   {
                     ProjectInformation project_reference;
                     NoggitProjectCreationDialog project_creation_dialog(project_reference, this);

                     QObject::connect(&project_creation_dialog,  &QDialog::finished, [&project_reference, this](int result)
                     {
                       if (result != QDialog::Accepted)
                         return;

                       Component::CreateProjectComponent::createProject(this, project_reference);
                       resetFavoriteProject();
                       Component::RecentProjectsComponent::buildRecentProjectsList(this);
                     });

                     project_creation_dialog.exec();
                     project_creation_dialog.setFixedSize(project_creation_dialog.size());

                   }
  );

  QObject::connect(_ui->button_open_existing_project, &QPushButton::clicked, [=]
                   {
                     auto project_reader = Noggit::Project::ApplicationProjectReader();

                     QString proj_file = QFileDialog::getOpenFileName(this, "Open File",
                                                                     "/",
                                                                     "*.noggitproj");

                     if (proj_file.isEmpty())
                     {
                       QMessageBox::critical(this, "Error", "Failed to read project: project file is empty");
                       return;
                     }


                     std::filesystem::path filepath(proj_file.toStdString());

                     auto project = project_reader.readProject(filepath.parent_path());

                     if (!project.has_value())
                     {
                       QMessageBox::critical(this, "Error", "Failed to read project");
                       return;
                     }

                     Component::RecentProjectsComponent::registerProjectChange(filepath.parent_path().string());

                     auto application_configuration = _noggit_application->getConfiguration();
                     auto application_projects_folder_path = std::filesystem::path(application_configuration->ApplicationProjectPath);
                     auto application_project_service = Noggit::Project::ApplicationProject(application_configuration);

                     auto project_to_launch = application_project_service.loadProject(filepath.parent_path());

                     if (!project_to_launch)
                     {
                       return;
                     }

                     Noggit::Application::NoggitApplication::instance()->setClientData(project_to_launch->ClientData);

                     Noggit::Project::CurrentProject::initialize(project_to_launch.get());

                     _project_selection_page = std::make_unique<Noggit::Ui::Windows::NoggitWindow>(
                         _noggit_application->getConfiguration(),
                         project_to_launch);
                     _project_selection_page->showMaximized();

                     close();
                   }
  );

  QObject::connect(_ui->listView, &QListView::doubleClicked, [=]
                   {
                     auto selected_project = _load_project_component->loadProject(this);

                     if (!selected_project)
                     {
                       LogError << "Selected Project is null, loading failed." << std::endl;
                       return;
                     }

                     Noggit::Project::CurrentProject::initialize(selected_project.get());

                     _project_selection_page = std::make_unique<Noggit::Ui::Windows::NoggitWindow>(
                         _noggit_application->getConfiguration(),
                         selected_project);
                         _project_selection_page->showMaximized();

                     close();
                   }
  );

  // !disable-update && !force-changelog
  /*if (!_noggit_application->GetCommand(0) && !_noggit_application->GetCommand(1))
  {
      _updater = new Noggit::Ui::CUpdater(this);

      QObject::connect(_updater, &CUpdater::OpenUpdater, [=]()
          {
              _updater->setModal(true);
              _updater->show();
          });
  }*/

  // auto _set = new QSettings(this);
  //auto first_changelog = _set->value("first_changelog", false);

  // force-changelog
  /*if (_noggit_application->GetCommand(1) || !first_changelog.toBool())
  {
      _changelog->setModal(true);
      _changelog->show();

      if (!first_changelog.toBool())
      {
          _set->setValue("first_changelog", true);
          _set->sync();
      }
  }*/
  show();
}

void NoggitProjectSelectionWindow::applyVisualDesign()
{
  // WHAT THIS FUNCTION IS FOR. Every distance in this window used to be a local decision: uic's
  // default 9px frame, a hand-set 10px top and bottom on the column row, a 5px fixed spacer
  // standing in for a left margin, another 10px on the inside of the right-hand column, and
  // uic's default 6px between the widgets of both columns. Five numbers, none of them from the
  // same scale, which is why nothing in the window lined up with anything else in it. Everything
  // below comes from Style's 4px scale and nowhere else.
  //
  // WHERE COLOUR IS AND IS NOT WRITTEN HERE. Every surface, border and text colour is the
  // theme's: this function sets object names, which are the handles theme.qss selects on, and an
  // inline style sheet is never written because it would outrank the application sheet and pin
  // this window to one palette. There are five literals in this file -- the two icon-tile pairs
  // at the head of it and the glow's crimson -- and they are exceptions for one reason: all five
  // are fed to a QPainter or to a QGraphicsEffect, and neither is reachable from a style sheet.
  // Each one carries its measured ratio where it is declared.

  // ---------------------------------------------------------------- the frame --
  //
  // THE BANNER IS FULL BLEED. The root layout therefore carries no margin whatever and the
  // column row below it carries the window's SPACE_24 instead. A band inset by 24 on three sides
  // is a picture of a banner; a band that runs edge to edge is a header, and the mockup asks for
  // the second one.
  centralWidget()->setObjectName("project-selection-root");

  _ui->verticalLayout_3->setContentsMargins(0, 0, 0, 0);
  _ui->verticalLayout_3->setSpacing(Style::SPACE_24);

  // ----------------------------------------------------------- the brand band --
  //
  // Everything the band draws -- the darkening gradient, the seeded contour field, the glows, the
  // 96px product mark, the two-line wordmark and the rule that closes it -- is in BrandBanner,
  // and the long note at the head of that class says why each piece is painted rather than
  // shipped as a bitmap. The old band was a 28px icon beside a 15px label with no style rule of
  // its own anywhere in the sheet, so it was an unpainted transparent strip.
  _banner = new Noggit::Ui::Widget::BrandBanner(centralWidget());
  _ui->verticalLayout_3->insertWidget(0, _banner);

  // -------------------------------------------------------------- the columns --
  //
  // SPACE_32 is the one step above SPACE_24 and exists for exactly this: the gutter between the
  // two halves of this window.
  _ui->horizontalLayout->setContentsMargins(Style::SPACE_24, 0,
                                            Style::SPACE_24, Style::SPACE_24);
  _ui->horizontalLayout->setSpacing(Style::SPACE_32);

  // THE COLUMN RULE GOES. It was a seam drawn between two regions that are now told apart by the
  // objects in them -- a column of cards against a column of action buttons -- and a rule between
  // them is a third thing to look at that says nothing. Hidden rather than deleted, because the
  // pointer belongs to the generated form; QBoxLayout skips a hidden item AND the spacing that
  // would have gone with it, so the gutter stays a single SPACE_32.
  _ui->line->hide();

  // One gap value down both columns, so the two headings sit the same distance above their
  // content. SPACE_12 rather than SPACE_8: the cards and the action buttons are large objects and
  // the old 8px gap read as crowding against them.
  _ui->verticalLayout_2->setContentsMargins(0, 0, 0, 0);
  _ui->verticalLayout_2->setSpacing(Style::SPACE_12);
  _ui->verticalLayout->setContentsMargins(0, 0, 0, 0);
  _ui->verticalLayout->setSpacing(Style::SPACE_12);

  // WHY THE RIGHT-HAND COLUMN WAS BEING CROPPED. The form gave this nested layout
  // QLayout::SetFixedSize. Qt applies that constraint by calling setFixedSize on the layout's
  // parentWidget() -- and a nested layout's parentWidget() is the widget that owns the TOP-LEVEL
  // layout, i.e. the whole central widget. So one column's size hint was pinning the size of the
  // entire window, and any change to that column's contents moved the window's width with it.
  //
  // SetDefaultConstraint is what a column should have had: the layout raises the widget's
  // MINIMUM to its own minimum and leaves the maximum alone. The window then sizes to the whole
  // of its content instead of to one part of it.
  _ui->verticalLayout->setSizeConstraint(QLayout::SetDefaultConstraint);

  _ui->verticalLayout_4->setContentsMargins(0, 0, 0, 0);
  _ui->verticalLayout_4->setSpacing(Style::SPACE_12);
  _ui->horizontalLayout_2->setContentsMargins(0, 0, 0, 0);
  _ui->horizontalLayout_2->setSpacing(Style::SPACE_8);

  // --------------------------------------------------------------- the ranks ---
  //
  // The two column headings become letterspaced capitals with a crimson tick and a hairline to
  // the column edge. THE CAPITALS ARE IN THE .ui STRING and the tracking is a QFont, because Qt
  // style sheets have neither text-transform nor letter-spacing -- neither appears in the Qt 5.15
  // property list, so writing either would be silently dropped and the heading would simply look
  // wrong with nothing to show for it. The strings were changed in the form rather than with
  // setText() so translators see the shipped form.
  buildSectionHeading(_ui->label, _ui->verticalLayout_2);
  buildSectionHeading(_ui->label_2, _ui->verticalLayout_4);

  // ------------------------------------------------------------------ the list --
  _ui->listView->setAccessibleName("project_list");
  _ui->listView->setMinimumHeight(PROJECT_LIST_MIN_HEIGHT);
  _ui->listView->setFrameShape(QFrame::NoFrame);

  // Expanding, against the form's "Fixed". The form's Fixed policy pins the list to its size
  // hint, so widening the window grew the empty gutter instead of the list, and the list is the
  // one thing on this window that is worth more space. The fixed-width action column is satisfied
  // first regardless; the list takes what is left, down to its 420px floor.
  _ui->listView->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

  // Per-pixel scrolling. QListWidget defaults to ScrollPerItem, which makes a wheel notch jump a
  // whole card; with a persistent editor on every item that reads as the list snapping rather
  // than moving. Appearance and feel only -- the scroll RANGE is unchanged.
  _ui->listView->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
  _ui->listView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

  // Qt was enforcing no minimum at all on this window -- probed by asking it to become 400x300,
  // which succeeded. A window with no minimum does not refuse to shrink, it just paints its
  // content outside itself.
  setMinimumSize(WINDOW_MIN_WIDTH, WINDOW_MIN_HEIGHT);
  resize(WINDOW_DEFAULT_WIDTH, WINDOW_DEFAULT_HEIGHT);

  // ---------------------------------------------------------------- the actions --
  //
  // Three action CARDS, 72px tall and the full width of the column. The height is the theme's:
  // QSS min-/max-height size the CONTENT rect, so the sheet declares 42 and the box arrives at
  // 1 + 14 + 42 + 14 + 1 = 72. Both min and max, because the form gives these buttons Preferred
  // vertical policy and the column carries an Expanding spacer that would otherwise stretch them.
  //
  // They share one object name on purpose: they are one control family, and the primary is told
  // apart by its dynamic `state` property rather than by a name of its own. The names uic gave
  // them are not used as selectors anywhere and every connection in this file is by pointer.
  for (QPushButton* const button : {_ui->button_create_new_project,
                                    _ui->button_open_existing_project,
                                    _ui->button_convert_project})
  {
    button->setObjectName("launcher-action");
    button->setMinimumWidth(ACTION_COLUMN_WIDTH);
    button->setMaximumWidth(ACTION_COLUMN_WIDTH);
    button->setIconSize(QSize(Art::TILE_EXTENT + Art::TILE_GAP, Art::TILE_EXTENT));
    button->setCursor(Qt::PointingHandCursor);
  }

  // ONE primary per window: the brand fill is reserved for the action the window exists to
  // perform, and every other button stays neutral so that the fill still means something.
  Style::markPrimary(_ui->button_create_new_project);

  // THE PRIMARY BUTTON GETS A GLYPH BACK, and the reason it could not have one before is the
  // reason it can now. Both Font Awesome icon engines resolve their pen from one of exactly two
  // theme-owned slots -- an unchecked icon is text.dim #BFB7AA and a checked one is the accent --
  // and neither of them is ink, so on the old gold fill the plus sign measured 1.106:1 and was a
  // pale smear. The tile is painted here instead, through FontAwesome::ThemeIcons::pixmap, which
  // TAKES a pen colour: ink #100E0B on brand.crimson #E5405C is 4.790:1.
  //
  // Convert Project takes filearchive rather than exchangealt, which is the glyph that reads
  // closest to "convert" AND has artwork in the shipped icon set. exchangealt has none, and the
  // Font Awesome font is not redistributed with this fork, so on a clean install its tile would
  // have been empty -- a hole in one of three otherwise identical cards.
  struct ActionTile
  {
    QPushButton* button;
    Noggit::Ui::FontAwesome::Icons glyph;
    QRgb tile;
    QRgb ink;
  };

  // THE APPLICATION'S RATIO, NOT THIS WIDGET'S, and the difference is not academic. A QPushButton
  // fetches its icon with QIcon::pixmap(iconSize), which forwards to the QWindow overload with a
  // null window -- and that resolves the ratio as qApp->devicePixelRatio(), because it has no
  // window to ask. QPixmapIconEngine only ever scales a stored bitmap DOWN, so a tile built at a
  // lower ratio than the one QIcon asks for is returned at its own size and then stretched, which
  // is exactly the softness this file already fixed once for the product mark.
  qreal const ratio(qApp->devicePixelRatio());

  for (ActionTile const& action : {
         ActionTile{_ui->button_create_new_project, Noggit::Ui::FontAwesome::Icons::plus,
                    TILE_PRIMARY, TILE_PRIMARY_INK},
         ActionTile{_ui->button_open_existing_project, Noggit::Ui::FontAwesome::Icons::folderopen,
                    TILE_SECONDARY, TILE_SECONDARY_INK},
         ActionTile{_ui->button_convert_project, Noggit::Ui::FontAwesome::Icons::filearchive,
                    TILE_SECONDARY, TILE_SECONDARY_INK}})
  {
    QPixmap const tile(Art::actionTile(static_cast<char32_t>(action.glyph),
                                       QColor(action.tile), QColor(action.ink), ratio));

    if (!tile.isNull())
    {
      action.button->setIcon(QIcon(tile));
    }
  }

  // The halo. See GLOW_BLUR above for why this is an effect rather than a style-sheet
  // declaration, and why there is no container around the button.
  auto* const glow(new QGraphicsDropShadowEffect(_ui->button_create_new_project));
  glow->setBlurRadius(GLOW_BLUR);
  glow->setOffset(0.0, 0.0);
  glow->setColor(QColor(0xE5, 0x40, 0x5C, GLOW_ALPHA));
  _ui->button_create_new_project->setGraphicsEffect(glow);

  _ui->button_convert_project->setToolTip(tr("Not available yet"));

  // The cog is a rounded square in the corner rather than a bare glyph. It had no accessible name
  // and no tip either, so it was an unlabelled mark that could have been anything.
  _ui->settings_button->setObjectName("launcher-gear");
  _ui->settings_button->setAccessibleName("project_settings_button");
  _ui->settings_button->setToolTip(tr("Settings"));
  _ui->settings_button->setCursor(Qt::PointingHandCursor);
}

void NoggitProjectSelectionWindow::changeEvent(QEvent* event)
{
  QMainWindow::changeEvent(event);

  // Qt's font resolution accumulates across a theme switch, and an attribute the new sheet does
  // not mention can be resolved away with the ones it does. The headings' tracking is exactly
  // that kind of attribute, so it is re-armed whenever the style or the font moves underneath
  // them -- the same guarantee ProjectListItem::changeEvent gives its height floor.
  // _banner is null until applyVisualDesign has run, which is the only point after which
  // _ui->label and _ui->label_2 are pointers to real widgets rather than the uninitialised
  // members uic leaves in the generated form. A style change delivered to a half-built window
  // would otherwise dereference garbage.
  if (_banner
      && (event->type() == QEvent::StyleChange || event->type() == QEvent::FontChange))
  {
    applyHeadingTracking(_ui->label);
    applyHeadingTracking(_ui->label_2);
  }
}

void NoggitProjectSelectionWindow::handleContextMenuProjectListItemDelete(std::string const& project_path)
{
  QMessageBox prompt;
  prompt.setWindowIcon(QIcon(":/icon"));
  prompt.setWindowTitle("Delete Project");
  prompt.setIcon(QMessageBox::Warning);
  prompt.setWindowFlags(Qt::WindowStaysOnTopHint);
  prompt.setText("Deleting a project will remove all saved data. Do you want to continue?");
  prompt.addButton("Accept", QMessageBox::AcceptRole);
  prompt.setDefaultButton(prompt.addButton("Cancel", QMessageBox::RejectRole));
  prompt.setWindowFlags(Qt::CustomizeWindowHint | Qt::WindowTitleHint);

  prompt.exec();

  switch (prompt.buttonRole(prompt.clickedButton()))
  {
    case QMessageBox::AcceptRole:
    {
      Component::RecentProjectsComponent::registerProjectRemove(project_path);
      QFile folder(project_path.c_str());
      folder.moveToTrash();
      break;
    }
    case QMessageBox::DestructiveRole:
    default:
      break;
  }
  resetFavoriteProject();

  Component::RecentProjectsComponent::buildRecentProjectsList(this);
}

void NoggitProjectSelectionWindow::handleContextMenuProjectListItemForget(std::string const& project_path)
{
  QMessageBox prompt;
  prompt.setWindowIcon(QIcon(":/icon"));
  prompt.setWindowTitle("Forget Project");
  prompt.setIcon(QMessageBox::Warning);
  prompt.setWindowFlags(Qt::WindowStaysOnTopHint);
  prompt.setText("Data on the disk will not be removed, this action will only hide the project. Continue?.");
  prompt.addButton("Accept", QMessageBox::AcceptRole);
  prompt.setDefaultButton(prompt.addButton("Cancel", QMessageBox::RejectRole));
  prompt.setWindowFlags(Qt::CustomizeWindowHint | Qt::WindowTitleHint);

  prompt.exec();

  switch (prompt.buttonRole(prompt.clickedButton()))
  {
    case QMessageBox::AcceptRole:
      Component::RecentProjectsComponent::registerProjectRemove(project_path);
      break;
    case QMessageBox::DestructiveRole:
    default:
      break;
  }

  resetFavoriteProject();
  Component::RecentProjectsComponent::buildRecentProjectsList(this);
}

void Noggit::Ui::Windows::NoggitProjectSelectionWindow::handleContextMenuProjectListItemFavorite(int index)
{
  QSettings settings;
  settings.sync();
  settings.setValue("favorite_project", index);
  Component::RecentProjectsComponent::buildRecentProjectsList(this);
}

void Noggit::Ui::Windows::NoggitProjectSelectionWindow::resetFavoriteProject()
{
    QSettings settings;
    settings.sync();
    settings.setValue("favorite_project", -1);
}

NoggitProjectSelectionWindow::~NoggitProjectSelectionWindow()
{
  delete _ui;
}

