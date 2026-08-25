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
#include <noggit/ui/windows/settingsPanel/SettingsPanel.h>
#include <noggit/ui/windows/UiStyle.hpp>


#include <QAbstractItemView>
#include <QFile>
#include <QFileDialog>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLayout>
#include <QListWidget>
#include <QPushButton>
#include <QSettings>
#include <QString>
#include <QToolButton>
#include <QVBoxLayout>

#include "revision.h"
#include "ui_NoggitProjectSelectionWindow.h"

#include <filesystem>


using namespace Noggit::Ui::Windows;

namespace
{
  namespace Style = Noggit::Ui::Windows::Style;

  //! The product mark at the head of the window. Large enough to read as an identity, small
  //! enough that it does not compete with "Recent Projects" for the first fixation.
  constexpr int BRAND_MARK_EXTENT = 28;

  //! The cog, and the leading glyph on each Getting Started button. One value for all four, so
  //! the utility row and the action column agree. The form used to ask for a 64px icon slot on
  //! every button, which reserved space no icon ever occupied.
  constexpr int ICON_UTILITY_EXTENT = 16;

  //! The recent-project well. Four rows of ProjectListItem come to roughly 300px, and a well
  //! shorter than its content reads as a scrap of list rather than as the point of the window.
  //! This is a FLOOR, not a fixed size -- the column still grows if the other one is taller.
  constexpr int PROJECT_LIST_MIN_HEIGHT = 300;

  //! The Getting Started column. Fixed rather than elastic: a QVBoxLayout stretches its children
  //! across whatever width it is given, so without this the three buttons grow to fill every
  //! pixel the recent-projects well does not want, which is how you get a 500px-wide
  //! "Convert Project".
  constexpr int ACTION_COLUMN_WIDTH = 220;
}

NoggitProjectSelectionWindow::NoggitProjectSelectionWindow(Noggit::Application::NoggitApplication* noggit_app,
                                                           QWidget* parent)
  : QMainWindow(parent)
  , _ui(new ::Ui::NoggitProjectSelectionWindow)
  , _noggit_application(noggit_app)
{
  setWindowFlags(Qt::Window | Qt::MSWindowsFixedSizeDialogHint);

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

  _load_project_component = std::make_unique<Component::LoadProjectComponent>();

  _ui->settings_button->setIcon(Noggit::Ui::FontAwesomeIcon(Noggit::Ui::FontAwesome::Icons::cog));
  _ui->settings_button->setIconSize(QSize(ICON_UTILITY_EXTENT, ICON_UTILITY_EXTENT));

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
  // Colour is deliberately absent. The object names set here are the theme's handles; not one
  // literal colour is written, because an inline style sheet outranks the application sheet and
  // would pin this window to one palette no matter which theme the user picked.

  // ---------------------------------------------------------------- the frame --
  //
  // Dialog outer margin is SPACE_24 on three sides. The top is SPACE_16 because the brand band
  // that follows carries its own optical weight and a full 24 above it leaves the window
  // top-heavy.
  centralWidget()->setObjectName("project-selection-root");

  _ui->verticalLayout_3->setContentsMargins(Style::SPACE_24, Style::SPACE_16,
                                            Style::SPACE_24, Style::SPACE_24);
  _ui->verticalLayout_3->setSpacing(Style::SPACE_16);

  // ----------------------------------------------------------- the brand band --
  //
  // The window opened straight onto two headings with nothing above them, so it read as a panel
  // that had lost its window. A mark, the product name at the window-title rank and the build at
  // the secondary rank give the first thing the eye lands on somewhere to land, and they are the
  // one place in the application that states which build is running without opening About.
  QWidget* const banner = new QWidget(centralWidget());
  banner->setObjectName("project-banner");

  QHBoxLayout* const banner_layout = new QHBoxLayout(banner);
  banner_layout->setContentsMargins(0, 0, 0, 0);
  banner_layout->setSpacing(Style::SPACE_8);

  QLabel* const banner_mark = new QLabel(banner);
  banner_mark->setObjectName("project-banner-mark");
  banner_mark->setPixmap(QIcon(":/icon").pixmap(QSize(BRAND_MARK_EXTENT, BRAND_MARK_EXTENT)));
  banner_mark->setFixedSize(BRAND_MARK_EXTENT, BRAND_MARK_EXTENT);

  // The window-title rank, 15px/600. It is one step BELOW the column headings below it, and that
  // is the design system's own ordering: a column heading names a whole region of the window and
  // out-ranks the window's identity line, which only has to be found once.
  QLabel* const banner_title = new QLabel(tr("Noggit Crimson"), banner);
  banner_title->setObjectName("project-banner-title");
  Style::applyRank(banner_title, Style::RANK_WINDOW_TITLE_PIXELS, Style::RANK_WINDOW_TITLE_WEIGHT);

  // The secondary rank, taken from the sheet by name rather than written here, so the theme keeps
  // both the 11px and the text.dim that go with it.
  QLabel* const banner_version = new QLabel(QString::fromLatin1(STRPRODUCTVER), banner);
  banner_version->setObjectName(Style::NAME_SECONDARY);
  banner_version->setToolTip(tr("Build in use"));

  banner_layout->addWidget(banner_mark, 0, Qt::AlignVCenter);
  banner_layout->addWidget(banner_title, 0, Qt::AlignVCenter);
  banner_layout->addWidget(banner_version, 0, Qt::AlignBottom);
  banner_layout->addStretch(1);

  // One rule under the band, drawn Plain so it is the 1px hairline the design asks for rather
  // than the two-tone sunken bevel QFrame defaults to.
  QFrame* const banner_rule = new QFrame(centralWidget());
  banner_rule->setObjectName("section-rule");
  banner_rule->setFrameShape(QFrame::HLine);
  banner_rule->setFrameShadow(QFrame::Plain);
  banner_rule->setLineWidth(1);
  banner_rule->setFixedHeight(1);

  _ui->verticalLayout_3->insertWidget(0, banner);
  _ui->verticalLayout_3->insertWidget(1, banner_rule);

  // -------------------------------------------------------------- the columns --
  //
  // SPACE_32 is the one step above SPACE_24 and exists for exactly this: the gutter between the
  // two halves of this window. The column rule sits in the middle of it.
  _ui->horizontalLayout->setContentsMargins(0, 0, 0, 0);
  _ui->horizontalLayout->setSpacing(Style::SPACE_32);

  _ui->line->setObjectName("column-rule");
  _ui->line->setFrameShadow(QFrame::Plain);
  _ui->line->setLineWidth(1);
  _ui->line->setFixedWidth(1);

  // One gap value down both columns, so "Recent Projects" sits the same distance above its list
  // as "Getting Started" does above its buttons.
  _ui->verticalLayout_2->setContentsMargins(0, 0, 0, 0);
  _ui->verticalLayout_2->setSpacing(Style::SPACE_8);
  _ui->verticalLayout->setContentsMargins(0, 0, 0, 0);
  _ui->verticalLayout->setSpacing(Style::SPACE_8);

  // WHY THE RIGHT-HAND COLUMN WAS BEING CROPPED. The form gave this nested layout
  // QLayout::SetFixedSize. Qt applies that constraint by calling setFixedSize on the layout's
  // parentWidget() -- and a nested layout's parentWidget() is the widget that owns the TOP-LEVEL
  // layout, i.e. the whole central widget. So one column's size hint was pinning the size of the
  // entire window, and any change to that column's contents moved the window's width with it.
  // Captured with PrintWindow against a real run: the window came up 1071x621 physical with
  // "Getting Started" and all three buttons running off the right edge.
  //
  // SetDefaultConstraint is what a column should have had: the layout raises the widget's
  // MINIMUM to its own minimum and leaves the maximum alone. The window then sizes to the whole
  // of its content instead of to one part of it.
  _ui->verticalLayout->setSizeConstraint(QLayout::SetDefaultConstraint);

  // With the fixed-size constraint gone the column needs a width of its own, or a QVBoxLayout
  // stretches its buttons across every pixel the window has. This is the column's width, stated
  // once: wide enough for "Open an existing project" with its glyph, narrow enough that the
  // recent-projects well stays the subject of the window.
  for (QPushButton* const button : {_ui->button_create_new_project,
                                    _ui->button_open_existing_project,
                                    _ui->button_convert_project})
  {
    button->setMinimumWidth(ACTION_COLUMN_WIDTH);
    button->setMaximumWidth(ACTION_COLUMN_WIDTH);
  }
  _ui->verticalLayout_4->setContentsMargins(0, 0, 0, 0);
  _ui->verticalLayout_4->setSpacing(Style::SPACE_8);
  _ui->horizontalLayout_2->setContentsMargins(0, 0, 0, 0);
  _ui->horizontalLayout_2->setSpacing(Style::SPACE_8);

  // --------------------------------------------------------------- the ranks ---
  //
  // The two section headings and the recent-project list are NAMED rather than styled. They used
  // to carry an inline style sheet, which outranks the application sheet and so pinned them to
  // one size no matter which theme was loaded.
  _ui->label->setObjectName(Style::NAME_SECTION_TITLE);
  _ui->label_2->setObjectName(Style::NAME_SECTION_TITLE);

  // ------------------------------------------------------------------ the well --
  _ui->listView->setAccessibleName("project_list");
  _ui->listView->setMinimumHeight(PROJECT_LIST_MIN_HEIGHT);
  _ui->listView->setFrameShape(QFrame::NoFrame);

  // Per-pixel scrolling. QListWidget defaults to ScrollPerItem, which makes a wheel notch jump a
  // whole 74px project row; with a persistent editor on every item that reads as the list
  // snapping rather than moving. Appearance and feel only -- the scroll RANGE is unchanged.
  _ui->listView->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
  _ui->listView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

  // ---------------------------------------------------------------- the actions --
  //
  // ONE primary per window: the accent fill is reserved for the action the window exists to
  // perform, and every other button stays neutral so that the fill still means something. The
  // theme paints it; this only says which one it is.
  Style::markPrimary(_ui->button_create_new_project);

  // THE PRIMARY BUTTON GETS NO GLYPH, and the reason is measured rather than stylistic. Both
  // Font Awesome icon engines resolve their pen from one of exactly two theme-owned slots -- an
  // unchecked icon is text.dim #BFB7AA and a checked one is the accent -- and neither of them is
  // ink. On the primary button's accent fill #DFA52E, text.dim measures 1.106:1: sampled off a
  // real capture, the plus sign was a pale smear on gold, well under the 3:1 floor a graphical
  // mark has to clear. There is no third slot to reach for, so the glyph goes and the label,
  // which is ink at 8.77:1, carries the button on its own.
  struct ActionIcon
  {
    QPushButton* button;
    Noggit::Ui::FontAwesome::Icons glyph;
  };

  for (ActionIcon const& action : {
         ActionIcon{_ui->button_open_existing_project, Noggit::Ui::FontAwesome::Icons::folderopen},
         ActionIcon{_ui->button_convert_project, Noggit::Ui::FontAwesome::Icons::exchangealt}})
  {
    action.button->setIcon(Noggit::Ui::FontAwesomeIcon(action.glyph));
    action.button->setIconSize(QSize(ICON_UTILITY_EXTENT, ICON_UTILITY_EXTENT));
  }

  for (QPushButton* const button : {_ui->button_create_new_project,
                                    _ui->button_open_existing_project,
                                    _ui->button_convert_project})
  {
    button->setCursor(Qt::PointingHandCursor);
  }

  _ui->button_convert_project->setToolTip(tr("Not available yet"));

  // The cog had no accessible name and no tip, so it was an unlabelled square in the corner.
  _ui->settings_button->setAccessibleName("project_settings_button");
  _ui->settings_button->setToolTip(tr("Settings"));
  _ui->settings_button->setCursor(Qt::PointingHandCursor);
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

