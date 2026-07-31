#include "AssetBrowser.hpp"

#include <ui_AssetBrowser.h>
#include <ui_AssetBrowserOverlay.h>

#include <noggit/application/NoggitApplication.hpp>
#include <noggit/ContextObject.hpp>
#include <noggit/Log.h>
#include <noggit/MapView.h>
#include <noggit/project/CurrentProject.hpp>
#include <noggit/ui/FontNoggit.hpp>
#include <noggit/ui/FramelessWindow.hpp>
#include <noggit/ui/GroundEffectsTool.hpp>
#include <noggit/ui/texturing_tool.hpp>
#include <noggit/ui/tools/AssetBrowser/Ui/Model/TreeManager.hpp>
#include <noggit/ui/tools/PreviewRenderer/PreviewRenderer.hpp>

#include <QDial>
#include <QDialog>
#include <QDir>
#include <QFileSystemWatcher>
#include <QIcon>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QPixmap>
#include <QScrollBar>
#include <QSet>
#include <QSettings>
#include <QSignalBlocker>
#include <QSlider>
#include <QStandardItemModel>
#include <QTimer>

#include <functional>

using namespace Noggit::Ui::Tools::AssetBrowser::Ui;
using namespace Noggit::Ui;

namespace
{
  // A single file copy raises several directory-change notifications (the entry
  // appears, its size grows, the handle closes), and dropping a folder of models
  // raises one such burst per file. Rebuilding the tree on each of them would be
  // absurd, so every event restarts this timer and only the quiet period at the
  // end of the copy actually triggers a rescan. One second is long enough to
  // swallow a multi-file copy over a local disk, short enough that the asset is
  // there by the time the user alt-tabs back to Noggit to look for it.
  constexpr int RESCAN_DEBOUNCE_MS = 1000;

  // QFileSystemWatcher costs one OS handle per watched directory, and Qt's
  // Windows backend spawns a worker thread for every 60 or so handles because
  // WaitForMultipleObjects tops out at MAXIMUM_WAIT_OBJECTS. A project folder
  // holding a fully extracted client mirror has tens of thousands of
  // directories, which would mean hundreds of threads: pathological, and the
  // kind of thing that makes a watcher worse than no watcher. Past this budget
  // we do not watch at all, say so in the log, and leave the user with the
  // Rescan button.
  constexpr int MAX_WATCHED_DIRECTORIES = 2048;

  char const* const AUTO_RESCAN_SETTING = "assetBrowser/auto_rescan";
}

AssetBrowserWidget::AssetBrowserWidget(MapView* map_view, QWidget *parent)
: QMainWindow(parent, Qt::Window)
, _map_view(map_view)
{
  setWindowTitle("Asset Browser");

  auto body = new QWidget(this);
  ui = new ::Ui::AssetBrowser;
  ui->setupUi(body);
  setCentralWidget(body);

  setWindowFlags(windowFlags() | Qt::Tool | Qt::WindowStaysOnTopHint);

  ui->checkBox_M2s->setChecked(true);
  ui->checkBox_WMOs->setChecked(true);
  connect(ui->checkBox_M2s, &QCheckBox::stateChanged, [&](int state)
      {
          updateModelData();
      });

  connect(ui->checkBox_WMOs, &QCheckBox::stateChanged, [&](int state)
      {
          updateModelData();
      });

  ui->comboBox_BrowseMode->addItems(brosweModeLabels.keys());

  // set combobox browse mode to World
  for (auto it = brosweModeLabels.constBegin(); it != brosweModeLabels.constEnd(); ++it) {
      if (it.value() == asset_browse_mode::world) {
          ui->comboBox_BrowseMode->setCurrentText(it.key());
          break;
      }
  }

  connect(ui->comboBox_BrowseMode, qOverload<int>(&QComboBox::currentIndexChanged)
      , [this](int index)
      {
          asset_browse_mode mode = brosweModeLabels[ui->comboBox_BrowseMode->currentText()];

          set_browse_mode(mode);
      }
  );

  _model = new QStandardItemModel(this);
  // _sort_model = new QSortFilterProxyModel(this);
  _sort_model = new NoggitExpendableFilterProxyModel;
  _sort_model->setFilterCaseSensitivity(Qt::CaseInsensitive);
  _sort_model->setFilterRole(Qt::UserRole);
  _sort_model->setRecursiveFilteringEnabled(true);

  auto overlay = new QWidget(ui->viewport);
  viewport_overlay_ui = new ::Ui::AssetBrowserOverlay();
  viewport_overlay_ui->setupUi(overlay);
  overlay->setAttribute(Qt::WA_TranslucentBackground);
  overlay->setMouseTracking(true);
  overlay->setGeometry(0,0,ui->viewport->width(),ui->viewport->height());

  connect(ui->viewport, &ModelViewer::resized
      ,[this, overlay]()
      {
        overlay->setGeometry(0,0,ui->viewport->width(),ui->viewport->height());

        if (ui->viewport->width() < 700  && !overlay->isHidden())
          overlay->hide();
        else if (ui->viewport->width() > 700 && overlay->isHidden())
          overlay->setVisible(true);
      }
  );

  viewport_overlay_ui->toggleAnimationButton->setIcon(FontNoggitIcon(FontNoggit::Icons::VISIBILITY_ANIMATION));
  viewport_overlay_ui->toggleModelsButton->setIcon(FontNoggitIcon(FontNoggit::Icons::VISIBILITY_DOODADS));
  viewport_overlay_ui->toggleParticlesButton->setIcon(FontNoggitIcon(FontNoggit::Icons::VISIBILITY_PARTICLES));
  viewport_overlay_ui->toggleBoundingBoxButton->setIcon(FontNoggitIcon(FontNoggit::Icons::VISIBILITY_WITH_BOX));
  viewport_overlay_ui->toggleWMOButton->setIcon(FontNoggitIcon(FontNoggit::Icons::VISIBILITY_WMO));
  viewport_overlay_ui->toggleGridButton->setIcon(FontNoggitIcon(FontNoggit::Icons::VISIBILITY_LINES));

  ui->viewport->installEventFilter(overlay);
  overlay->show();

  ui->viewport->setLightDirection(120.0f, 60.0f);

  // drag'n'drop
  ui->listfileTree->setDragEnabled(true);
  ui->listfileTree->setDragDropMode(QAbstractItemView::DragOnly);

  ui->listfileTree->setIconSize(QSize(90, 90));

  _sort_model->setSourceModel(_model);
  ui->listfileTree->setModel(_sort_model);

  _preview_renderer = new PreviewRenderer(90, 90,
      Noggit::NoggitRenderContext::ASSET_BROWSER_PREVIEW, this);
  _preview_renderer->setVisible(false);

  // just to initialize context, ugly-ish
  _preview_renderer->setModelOffscreen("world/wmo/azeroth/buildings/human_farm/farm.wmo");
  _preview_renderer->renderToPixmap();

  connect(ui->listfileTree->selectionModel(), &QItemSelectionModel::selectionChanged
      ,[=] (const QItemSelection& selected, const QItemSelection& deselected)
        {
            for (auto const& index : selected.indexes())
            {
              auto path = index.data(Qt::UserRole).toString();
              if (path.endsWith(".m2") || path.endsWith(".wmo"))
              {
                auto str_path = path.toStdString();

                ui->viewport->setModel(str_path);
                _selected_path = str_path;

                emit selectionChanged(str_path);
              }
            }

        }

  );

  connect(ui->viewport, &ModelViewer::model_set
      , [=](const std::string& filename)
      {
        viewport_overlay_ui->doodadSetSelector->clear();
        viewport_overlay_ui->doodadSetSelector->insertItems(0, ui->viewport->getDoodadSetNames(filename));

        bool is_wmo = QString::fromStdString(filename).endsWith(".wmo");
        viewport_overlay_ui->doodadSetSelector->setVisible(is_wmo);
        viewport_overlay_ui->doodadSetLabel->setVisible(is_wmo);
      }
  );

  connect(ui->viewport, &ModelViewer::gl_data_unloaded,[=] () { emit gl_data_unloaded(); });

  // Handle preview rendering and drag
  connect(ui->listfileTree, &QTreeView::expanded
      ,[this] (const QModelIndex& index)
      {
        QSettings settings;
        bool render_preview = settings.value("assetBrowser/render_asset_preview").toBool();

        if (!render_preview)
          return;

        for (int i = 0; i != _sort_model->rowCount(index); ++i)
        {
          auto child = _sort_model->index(i, 0, index);
          // auto child = index.child(i, 0);
          auto path = child.data(Qt::UserRole).toString();
          if (path.endsWith(".wmo") || path.endsWith(".m2"))
          {
            _preview_renderer->setModelOffscreen(path.toStdString());

            auto preview_pixmap = _preview_renderer->renderToPixmap();
            auto item = _model->itemFromIndex(_sort_model->mapToSource(child));
            item->setIcon(QIcon(*preview_pixmap));
            item->setDragEnabled(true);
            item->setFlags(item->flags() | Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled);
          }
        }
      }

  );

  setupConnectsCommon();

  _wmo_group_and_lod_regex = QRegularExpression(".+_\\d{3}(_lod.+)*.wmo");

  // Must exist before the first updateModelData(): that call is what discovers
  // the project's directory layout, and it hands the result straight to the
  // watcher on its way out.
  setupProjectWatcher();

  updateModelData();


}

void AssetBrowserWidget::setupConnectsCommon()
{
  connect(ui->searchButton, &QPushButton::clicked
      ,[this]()
          {
              _sort_model->setFilterFixedString(ui->searchField->text());
          }

  );

  // The manual escape hatch. Before this existed the only way to pick up a model
  // dropped into the project folder mid-session was to toggle one of the type
  // checkboxes twice, which nobody would guess, so people restarted Noggit.
  connect(ui->rescanButton, &QPushButton::clicked
      ,[this]()
          {
              // A deliberate rescan supersedes whatever the watcher had queued.
              if (_rescan_debounce_timer)
                _rescan_debounce_timer->stop();

              updateModelData();
          }

  );

  connect(viewport_overlay_ui->lightDirY, &QDial::valueChanged
      ,[this]()
          {
              ui->viewport->setLightDirection(viewport_overlay_ui->lightDirY->value(),
                                              viewport_overlay_ui->lightDirZ->value());
          }
  );

  connect(viewport_overlay_ui->lightDirZ, &QSlider::valueChanged
      ,[this]()
          {
              ui->viewport->setLightDirection(viewport_overlay_ui->lightDirY->value(),
                                              viewport_overlay_ui->lightDirZ->value());
          }
  );

  connect(viewport_overlay_ui->moveSensitivitySlider, &QSlider::valueChanged
      ,[this]()
          {
              ui->viewport->setMoveSensitivity(static_cast<float>(viewport_overlay_ui->moveSensitivitySlider->value()));
          }
  );

  connect(ui->viewport, &ModelViewer::sensitivity_changed
      ,[this]()
          {
              viewport_overlay_ui->moveSensitivitySlider->setValue(ui->viewport->getMoveSensitivity() * 30.0f);
          }
  );

  connect(viewport_overlay_ui->cameraXButton, &QPushButton::clicked
      ,[this]()
          {
              ui->viewport->resetCamera(0.f, 0.f, 0.f, 0.f, -90.f, 0.f);
          }
  );

  connect(viewport_overlay_ui->cameraYButton, &QPushButton::clicked
      ,[this]()
          {
              ui->viewport->resetCamera(0.f, 0.f, 0.f, 0.f, 0, 90.f);
          }
  );

  connect(viewport_overlay_ui->cameraZButton, &QPushButton::clicked
      ,[this]()
          {
              ui->viewport->resetCamera(0.f, 0.f, 0.f, 0.f, 0.f, 0.f);
          }
  );

  connect(viewport_overlay_ui->cameraResetButton, &QPushButton::clicked
      ,[this]()
          {
              ui->viewport->resetCamera();
          }
  );

  connect(viewport_overlay_ui->doodadSetSelector, qOverload<int>(&QComboBox::currentIndexChanged)
      ,[this](int index)
          {
              ui->viewport->setActiveDoodadSet(ui->viewport->getLastSelectedModel(),
                                               viewport_overlay_ui->doodadSetSelector->currentText().toStdString());
          }
  );

  // Render toggles
  connect(viewport_overlay_ui->toggleWMOButton, &QPushButton::clicked,
          [this]() {ui->viewport->_draw_wmo.toggle();});
  connect(viewport_overlay_ui->toggleBoundingBoxButton, &QPushButton::clicked,
          [this]() {ui->viewport->_draw_boxes.toggle();});
  connect(viewport_overlay_ui->toggleParticlesButton, &QPushButton::clicked,
          [this]() {ui->viewport->_draw_particles.toggle();});
  connect(viewport_overlay_ui->toggleModelsButton, &QPushButton::clicked,
          [this]() {ui->viewport->_draw_models.toggle();});
  connect(viewport_overlay_ui->toggleAnimationButton, &QPushButton::clicked,
          [this]() {ui->viewport->_draw_animated.toggle();});
  connect(viewport_overlay_ui->toggleGridButton, &QPushButton::clicked,
          [this]() {ui->viewport->_draw_grid.toggle();});

  connect(&ui->viewport->_draw_wmo, &BoolToggleProperty::changed,
          [this](bool state) {ui->viewport->_draw_wmo.set(state);});
  connect(&ui->viewport->_draw_boxes, &BoolToggleProperty::changed,
          [this](bool state) {ui->viewport->_draw_boxes.set(state);});
  connect(&ui->viewport->_draw_particles, &BoolToggleProperty::changed,
          [this](bool state) {ui->viewport->_draw_particles.set(state);});
  connect(&ui->viewport->_draw_models, &BoolToggleProperty::changed,
          [this](bool state) {ui->viewport->_draw_models.set(state);});
  connect(&ui->viewport->_draw_animated, &BoolToggleProperty::changed,
          [this](bool state) {ui->viewport->_draw_animated.set(state);});
  connect(&ui->viewport->_draw_grid, &BoolToggleProperty::changed,
          [this](bool state) {ui->viewport->_draw_grid.set(state);});
}

bool AssetBrowserWidget::validateBrowseMode(const QString& wow_file_path)
{
    // if (_browse_mode == asset_browse_mode::detail_doodads)
    // {
    //     _sort_model->setFilterFixedString("world/nodxt/detail");
    // }


    // TODO : do it in sort model instead?
    switch (_browse_mode)
    {
    case asset_browse_mode::ALL:
        return true;
    case asset_browse_mode::world:
    {
        if (wow_file_path.startsWith("World", Qt::CaseInsensitive) /*&& !wow_file_path.startsWith("world/nodxt/detail/", Qt::CaseInsensitive)*/)
            return true;
        return false;
    }
    case asset_browse_mode::detail_doodads:
    {
        if (wow_file_path.startsWith("world/nodxt/detail/", Qt::CaseInsensitive) 
            && wow_file_path.endsWith(".m2", Qt::CaseInsensitive))
            return true;
        return false;
    }
    case asset_browse_mode::skybox:
    {
        if (wow_file_path.startsWith("environments/stars", Qt::CaseInsensitive)
            && wow_file_path.endsWith(".m2", Qt::CaseInsensitive))
            return true;
        return false;
    }
    case asset_browse_mode::creatures:
    {
        if (wow_file_path.startsWith("creature", Qt::CaseInsensitive)
            && wow_file_path.endsWith(".m2", Qt::CaseInsensitive))
            return true;
        return false;
    }
    case asset_browse_mode::characters:
    {
        if (wow_file_path.startsWith("character", Qt::CaseInsensitive)
            && wow_file_path.endsWith(".m2", Qt::CaseInsensitive))
            return true;
        return false;
    }
    case asset_browse_mode::particles:
    {
        if (wow_file_path.startsWith("particles", Qt::CaseInsensitive)
            && wow_file_path.endsWith(".m2", Qt::CaseInsensitive))
            return true;
        return false;
    }
    case asset_browse_mode::cameras:
    {
        if (wow_file_path.startsWith("cameras", Qt::CaseInsensitive)
            && wow_file_path.endsWith(".m2", Qt::CaseInsensitive))
            return true;
        return false;
    }
    case asset_browse_mode::items:
    {
        if (wow_file_path.startsWith("item", Qt::CaseInsensitive)
            && wow_file_path.endsWith(".m2", Qt::CaseInsensitive))
            return true;
        return false;
    }
    case asset_browse_mode::spells:
    {
        if (wow_file_path.startsWith("SPELLS", Qt::CaseInsensitive)
            && wow_file_path.endsWith(".m2", Qt::CaseInsensitive))
            return true;
        return false;
    }
    default:
        return false; // ?
    }
}

// Single source of truth for the WMOs/Models checkboxes. The listfile pass and
// the project-folder pass used to spell this condition out separately, and the
// project-folder one simply forgot the checkboxes: unticking "Models (*.m2)"
// hid every m2 from the archives but left the ones sitting in the project
// folder on screen.
bool AssetBrowserWidget::isAssetTypeEnabled(const QString& wow_file_path) const
{
  if (wow_file_path.endsWith(".wmo", Qt::CaseInsensitive))
  {
    // Group files (foo_000.wmo) and their LODs are pieces of a root WMO, not
    // things you would ever place, so they never belong in the tree.
    return ui->checkBox_WMOs->isChecked()
        && !_wmo_group_and_lod_regex.match(wow_file_path).hasMatch();
  }

  if (wow_file_path.endsWith(".m2", Qt::CaseInsensitive))
    return ui->checkBox_M2s->isChecked();

  return false;
}

// Add WMOs and M2s from project directory recursively
void AssetBrowserWidget::recurseDirectory(Model::TreeManager& tree_mgr, const QString& s_dir, const QString& project_dir)
{
  QDir dir(s_dir);

  if (!dir.exists())
    return;

  // Recorded on the way in, so the list comes out root-first and holds every
  // directory that exists right now. refreshWatchedDirectories() consumes it.
  _scanned_directories.append(dir.absolutePath());

  QFileInfoList list = dir.entryInfoList();
  for (int i = 0; i < list.count(); ++i)
  {
    QFileInfo info = list[i];

    QString q_path = info.filePath();
    if (info.isDir())
    {
      if (info.fileName() != ".." && info.fileName() != ".")
      {
        recurseDirectory(tree_mgr, q_path, project_dir);
      }
    }
    else
    {
      QString rel_path = QDir(project_dir).relativeFilePath(q_path);

      if (!isAssetTypeEnabled(rel_path))
        continue;

      if (!validateBrowseMode(rel_path))
          continue;

      tree_mgr.addItem(rel_path);
    }
  }
}

void AssetBrowserWidget::updateModelData()
{
  // Everything below is string enumeration and QStandardItem construction: the
  // listfile map, a QDir walk, and TreeManager. No model or texture is loaded,
  // no scoped_model_reference is touched, and no GL call is made -- which is
  // what makes it safe to run off a file-system event as well as off a click.
  // The one GL-adjacent thing in this widget is the preview render hung off
  // QTreeView::expanded, and the expansion restore below is careful to keep it
  // from firing.

  // Rebuilding the model throws away the selection, the scroll offset and every
  // expanded folder. That is merely annoying when the user asked for a rescan;
  // it is unacceptable when the watcher fires unannounced under their cursor.
  QStringList const previously_expanded = collectExpandedPaths();
  int const previous_scroll = ui->listfileTree->verticalScrollBar()->value();
  QString previously_selected;
  {
    QModelIndex const current = ui->listfileTree->selectionModel()->currentIndex();
    if (current.isValid())
      previously_selected = current.data(Qt::UserRole).toString();
  }

  _model->clear();
  _scanned_directories.clear();

  Model::TreeManager tree_mgr =  Model::TreeManager(_model);
  for (auto const& key_pair : Noggit::Application::NoggitApplication::instance()->clientData()->listfile()->pathToFileDataIDMap())
  {
    // std::string const& filename = key_pair.first;

    QString const q_path = QString(key_pair.first.c_str());

    if (!isAssetTypeEnabled(q_path))
      continue;

    if (!validateBrowseMode(q_path))
        continue;

    tree_mgr.addItem(q_path);
  }


  QString project_dir = QString(Noggit::Project::CurrentProject::get()->ProjectPath.c_str());
  recurseDirectory(tree_mgr, project_dir, project_dir);

  _sort_model->setSortRole(Qt::UserRole);
  _sort_model->sort(0, Qt::AscendingOrder);

  // TODO : set default layout(base folder) for the modes
  if (_browse_mode != asset_browse_mode::ALL)
  {
      // expend base directories
      // ui->listfileTree->expand();
  }

  // those modes don't have subfolders, can auto expend without it becoming a mess
  if (_browse_mode == asset_browse_mode::detail_doodads
      || _browse_mode == asset_browse_mode::spells
      || _browse_mode == asset_browse_mode::skybox
      || _browse_mode == asset_browse_mode::cameras
      || _browse_mode == asset_browse_mode::particles)
  {
      ui->listfileTree->expandAll();
  }

  {
    // Signals stay blocked while we put the view back the way we found it.
    // QTreeView::expanded is wired to the preview renderer, which loads the
    // model behind each child row; letting a rescan re-trigger that would mean
    // constructing model references off a file-system event, which is exactly
    // what the async refcounting rules forbid. The consequence is that preview
    // icons under a restored folder come back blank until the user collapses
    // and re-expands it -- a fair trade for not touching the loader.
    // The selection model is blocked for the same class of reason: re-emitting
    // selectionChanged would make the viewport reload a model it already has.
    QSignalBlocker const tree_blocker(ui->listfileTree);
    QSignalBlocker const selection_blocker(ui->listfileTree->selectionModel());

    for (QString const& path : previously_expanded)
    {
      QModelIndex const index = indexForPath(path);
      if (index.isValid())
        ui->listfileTree->setExpanded(index, true);
    }

    if (!previously_selected.isEmpty())
    {
      QModelIndex const index = indexForPath(previously_selected);
      if (index.isValid())
      {
        ui->listfileTree->selectionModel()->setCurrentIndex(
            index, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
      }
    }
  }

  // After the expansion restore, because expanding rows changes the scrollable
  // range and the bar would otherwise clamp to the collapsed maximum.
  ui->listfileTree->verticalScrollBar()->setValue(previous_scroll);

  // The view learns which rows to paint as selected from the selection model's
  // signals, which were blocked above; the state is right but nothing asked for
  // a repaint.
  ui->listfileTree->viewport()->update();

  // New subdirectories only exist once the scan has seen them, so the watcher
  // is re-pointed here rather than at setup time.
  refreshWatchedDirectories();
}

QStringList AssetBrowserWidget::collectExpandedPaths() const
{
  // Only descends into rows that are themselves expanded. QTreeView remembers
  // the state of rows hidden under a collapsed parent, so this is not the
  // complete set -- but it is exactly the set the user can see, which is the
  // set whose loss they would notice.
  QStringList expanded;

  std::function<void(QModelIndex const&)> walk = [&] (QModelIndex const& parent)
  {
    for (int i = 0; i < _sort_model->rowCount(parent); ++i)
    {
      QModelIndex const child = _sort_model->index(i, 0, parent);

      if (!ui->listfileTree->isExpanded(child))
        continue;

      expanded << child.data(Qt::UserRole).toString();
      walk(child);
    }
  };

  walk(QModelIndex());

  return expanded;
}

QModelIndex AssetBrowserWidget::indexForPath(const QString& path) const
{
  // TreeManager stores the cumulative lower-cased path on every node under
  // Qt::UserRole, so a node can be found again by walking one path component at
  // a time and comparing that role. Cost is depth * siblings, paid once per
  // remembered folder -- there are a handful of those, not thousands.
  QModelIndex parent;
  QString accumulated;

  for (QString const& part : path.split(QLatin1Char('/'), Qt::SkipEmptyParts))
  {
    accumulated = accumulated.isEmpty() ? part : accumulated + QLatin1Char('/') + part;

    QModelIndex found;
    for (int i = 0; i < _sort_model->rowCount(parent); ++i)
    {
      QModelIndex const child = _sort_model->index(i, 0, parent);
      if (child.data(Qt::UserRole).toString() == accumulated)
      {
        found = child;
        break;
      }
    }

    if (!found.isValid())
      return QModelIndex();

    parent = found;
  }

  return parent;
}

void AssetBrowserWidget::setupProjectWatcher()
{
  // QFileSystemWatcher emits on the thread that owns it and this widget lives on
  // the GUI thread, so the debounce timer and the tree rebuild it eventually
  // triggers are all main-thread work -- unlike AsyncLoader::finishLoading,
  // which is not. Parenting both objects to `this` also guarantees they cannot
  // outlive the browser.
  _project_watcher = new QFileSystemWatcher(this);

  _rescan_debounce_timer = new QTimer(this);
  _rescan_debounce_timer->setSingleShot(true);
  _rescan_debounce_timer->setInterval(RESCAN_DEBOUNCE_MS);

  {
    // Restoring the persisted preference must not look like a user click, or it
    // would kick off a watcher refresh before the first scan has run.
    QSettings settings;
    QSignalBlocker const blocker(ui->checkBox_AutoRescan);
    ui->checkBox_AutoRescan->setChecked(settings.value(AUTO_RESCAN_SETTING, true).toBool());
  }

  connect(_project_watcher, &QFileSystemWatcher::directoryChanged, this
      ,[this](const QString&)
          {
              if (!ui->checkBox_AutoRescan->isChecked())
                return;

              // start() on a running single-shot timer restarts it, so a burst
              // of events collapses into one rescan after the burst ends.
              _rescan_debounce_timer->start();
          }
  );

  connect(_rescan_debounce_timer, &QTimer::timeout, this
      ,[this]()
          {
              // Rebuilding a hidden tree is pure waste, and it is not a rare case: the widget is
              // constructed for every MapView and immediately hidden, so the watcher is armed for
              // the whole session whether or not the browser is ever opened. Worse, Noggit writes
              // into the very tree being watched during ordinary editing -- every ADT save, and
              // noggit_palettes.json on every palette change -- so an unguarded handler turns a
              // normal texturing session into a rescan every second.
              //
              // The event is not dropped, only deferred: the flag below makes showEvent catch up,
              // so opening the browser always shows what is on disk now.
              if (!isVisible())
              {
                _rescan_pending_while_hidden = true;
                return;
              }

              updateModelData();
          }
  );

  connect(ui->checkBox_AutoRescan, &QCheckBox::toggled, this
      ,[this](bool on)
          {
              QSettings settings;
              settings.setValue(AUTO_RESCAN_SETTING, on);

              if (!on)
                _rescan_debounce_timer->stop();

              refreshWatchedDirectories();
          }
  );
}

void AssetBrowserWidget::refreshWatchedDirectories()
{
  if (!_project_watcher)
    return;

  auto const unwatch_everything = [this] ()
  {
    QStringList const watched = _project_watcher->directories();
    if (!watched.isEmpty())
      _project_watcher->removePaths(watched);
  };

  if (!ui->checkBox_AutoRescan->isChecked())
  {
    unwatch_everything();
    return;
  }

  // Watching only the project root would be useless: QFileSystemWatcher does not
  // recurse, and assets live at arbitrary depth under a folder that mirrors the
  // whole client path space (world/wmo/..., creature/..., item/...). There is no
  // "asset-bearing root" to narrow down to. So we watch every directory the scan
  // just walked -- which is also how a folder created since the last scan gets
  // covered: its parent's change event causes the rescan that finds it.
  if (_scanned_directories.size() > MAX_WATCHED_DIRECTORIES)
  {
    unwatch_everything();

    if (!_watch_budget_exceeded)
    {
      _watch_budget_exceeded = true;

      LogError << "Asset Browser: project folder contains " << _scanned_directories.size()
               << " directories, over the watch budget of " << MAX_WATCHED_DIRECTORIES
               << ". Automatic asset detection is disabled for this project because"
                  " watching that many directories costs one OS handle each. Use the"
                  " Rescan button after adding files on disk." << std::endl;

      ui->checkBox_AutoRescan->setEnabled(false);
      ui->checkBox_AutoRescan->setToolTip
          ( "Disabled: this project folder has too many directories to watch"
            " cheaply. Use the Rescan button after adding model files on disk."
          );
    }

    return;
  }

  QStringList const currently_watched = _project_watcher->directories();

  QSet<QString> const wanted_set(_scanned_directories.begin(), _scanned_directories.end());
  QSet<QString> const watched_set(currently_watched.begin(), currently_watched.end());

  QStringList to_remove;
  for (QString const& path : currently_watched)
  {
    if (!wanted_set.contains(path))
      to_remove << path;
  }

  QStringList to_add;
  for (QString const& path : _scanned_directories)
  {
    if (!watched_set.contains(path))
      to_add << path;
  }

  if (!to_remove.isEmpty())
    _project_watcher->removePaths(to_remove);

  if (!to_add.isEmpty())
  {
    QStringList const failed = _project_watcher->addPaths(to_add);

    if (!failed.isEmpty())
    {
      LogError << "Asset Browser: failed to watch " << failed.size() << " of "
               << to_add.size() << " project directories (first: "
               << failed.first().toStdString()
               << "). Changes under them will only be picked up by the Rescan button."
               << std::endl;
    }
  }
}

void AssetBrowserWidget::showEvent(QShowEvent* event)
{
  QMainWindow::showEvent(event);

  // A rescan the debounce timer deferred while this was hidden is paid now, once, instead of
  // having been paid repeatedly against a tree nobody was looking at.
  if (_rescan_pending_while_hidden)
  {
    _rescan_pending_while_hidden = false;
    updateModelData();
  }
}

AssetBrowserWidget::~AssetBrowserWidget()
{
  // Torn down before `ui` goes away: both handlers dereference ui->, and a timer
  // that fired between the two deletes would read a freed form object.
  if (_rescan_debounce_timer)
    _rescan_debounce_timer->stop();

  if (_project_watcher)
  {
    QStringList const watched = _project_watcher->directories();
    if (!watched.isEmpty())
      _project_watcher->removePaths(watched);
  }

  delete ui;
  delete _preview_renderer;
}

std::string const& Noggit::Ui::Tools::AssetBrowser::Ui::AssetBrowserWidget::getFilename() const
{
  return _selected_path;
}

void AssetBrowserWidget::set_browse_mode(asset_browse_mode browse_mode)
{    
    if (_browse_mode == browse_mode)
        return;

    {
        QSignalBlocker const combobox_blocker(ui->comboBox_BrowseMode);

        QString key_text = "";

        for (auto it = brosweModeLabels.constBegin(); it != brosweModeLabels.constEnd(); ++it) {
            if (it.value() == browse_mode) {
                key_text = it.key();
                break; 
            }
        }
        // if (key_text.isEmpty())

        ui->comboBox_BrowseMode->setCurrentText(key_text);
    }

    _browse_mode = browse_mode;
    updateModelData();
}

void AssetBrowserWidget::keyPressEvent(QKeyEvent *event)
{
  if (event->key() == Qt::Key_Enter || event->key() == Qt::Key_Return)
  {
    QString text = ui->searchField->text();
    _sort_model->setFilterFixedString(ui->searchField->text());
        ui->listfileTree->collapseAll();
    if (text.isEmpty() || text.length() < 3) // too few characters is too performance expensive, require 3
    {
        return;
    }

    // todo : crashes when typics something like creature

    ui->listfileTree->expandAll();

    // QList<QModelIndex> acceptedIndices = _sort_model->findIndices();
    // for (auto& index : acceptedIndices)
    // {
    //     if (!index.isValid())
    //         continue;
    //     ui->listfileTree->expand(index);
    // 
    //     auto expanderIndex = index.parent();
    //     while (expanderIndex.isValid())
    //     {
    //         if (!ui->listfileTree->isExpanded(expanderIndex))
    //         {
    //             ui->listfileTree->expand(expanderIndex);
    //             expanderIndex = expanderIndex.parent();
    //         }
    //         else
    //             break;
    //     }
    // }

  }
}

QList<QModelIndex> NoggitExpendableFilterProxyModel::findIndices() const
{
  QList<QModelIndex> ret;
  for (auto iter = 0; iter < rowCount(); iter++)
  {
    auto childIndex = index(iter, 0, QModelIndex());
    ret << recursivelyFindIndices(childIndex);
  }
  return ret;
}

bool NoggitExpendableFilterProxyModel::rowAccepted(int source_row, const QModelIndex& source_parent) const
{
  return filterAcceptsRow(source_row, source_parent);
}

QList<QModelIndex> NoggitExpendableFilterProxyModel::recursivelyFindIndices(const QModelIndex& ind) const
{
  QList<QModelIndex> ret;
  if (rowAccepted(ind.row(), ind.parent()))
  {
    ret << ind;
  }
  else
  {
    for (auto iter = 0; iter < rowCount(ind); iter++)
    {
      ret << recursivelyFindIndices(index(iter, 0, ind));
    }
  }
  return ret;
}
