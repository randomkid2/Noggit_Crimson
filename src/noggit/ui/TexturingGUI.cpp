// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/ui/TexturingGUI.h>
#include <noggit/application/NoggitApplication.hpp>
#include <noggit/application/Configuration/NoggitApplicationConfiguration.hpp>
#include <noggit/Log.h>
#include <noggit/project/CurrentProject.hpp>
#include <noggit/TextureManager.h> // TextureManager, Texture
#include <noggit/ui/TextureList.hpp>

#include <ClientData.hpp>

#include <QtCore/QDir>
#include <QtCore/QFileSystemWatcher>
#include <QtCore/QSet>
#include <QtCore/QSettings>
#include <QtCore/QSignalBlocker>
#include <QtCore/QSortFilterProxyModel>
#include <QtCore/QTimer>
#include <QtGui/QIcon>
#include <QtGui/QStandardItemModel>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QVBoxLayout>

#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <unordered_set>
#include <vector>

namespace
{
  // A single file copy raises several directory-change notifications and dropping a
  // folder of textures raises one burst per file, so every event restarts this timer
  // and only the quiet period at the end of the copy triggers a rescan. Same value and
  // same reasoning as the asset browser's RESCAN_DEBOUNCE_MS: long enough to swallow a
  // multi-file copy over a local disk, short enough that the texture is there by the
  // time the user alt-tabs back into Noggit to look for it.
  constexpr int RESCAN_DEBOUNCE_MS = 1000;

  // QFileSystemWatcher costs one OS handle per directory, and Qt's Windows backend
  // spawns a worker thread per ~60 handles. This budget is deliberately far below the
  // asset browser's 2048 for two reasons: the palette watches only tileset-bearing
  // directories, of which any real project has a handful, and unlike the asset browser
  // the palette is instantiated more than once -- once for the texturing tool's dock
  // and once per texturing brush in the brush stack -- so the cost is paid N times.
  // A project past this number is not a normal project and gets the Rescan button.
  constexpr int MAX_WATCHED_DIRECTORIES = 256;

  char const* const AUTO_RESCAN_SETTING = "texturePalette/auto_rescan";

  // Distinguishes tilesets that have the companion variant from those that do not, so
  // the "only with ..." checkbox can filter on it. Qt::UserRole rather than a custom
  // role because the proxy filters on exactly one role and nothing else stores here.
  constexpr int const HAS_SPECULAR_ROLE = Qt::UserRole;
}

namespace Noggit
{
  namespace Ui
  {
    struct model_item : QStandardItem
    {
      model_item (QString const& display_role)
        : QStandardItem (display_role)
      {}

      virtual QVariant data (int role) const
      {
        if (role == Qt::DecorationRole)
        {
          if (!_rendered)
          {
            //! \note The one time Qt is const correct and we don't want that.
            auto that (const_cast<model_item*> (this));
            that->_rendered = true;

            // render_blp_to_pixmap throws on a BLP it cannot decode, and this runs from
            // paintEvent, where an escaping exception unwinds through Qt's event loop
            // and takes the process with it. That was survivable while the list could
            // only ever hold files present at startup; now that a rescan can pick up a
            // file the moment it lands on disk -- truncated, mid-copy, or simply not a
            // real BLP -- the throw is reachable in normal use. A blank icon and a log
            // line are the correct outcome: the entry stays selectable, and the user
            // finds out why in the log rather than by losing their session.
            try
            {
              that->_pixmap = *BLPRenderer::getInstance().render_blp_to_pixmap (data (Qt::DisplayRole).toString().prepend ("tileset/").toStdString(), 256, 256);
            }
            catch (std::exception const& e)
            {
              LogError << "Texture palette: cannot render "
                       << data (Qt::DisplayRole).toString().toStdString()
                       << ": " << e.what() << std::endl;
            }
            catch (...)
            {
              // The archive layer does not exclusively throw std::exception, and
              // "unknown exception" beats "process gone" in a paint handler.
              LogError << "Texture palette: cannot render "
                       << data (Qt::DisplayRole).toString().toStdString()
                       << ": unknown exception" << std::endl;
            }

            // Built once, here, rather than on every data() call. A QIcon is not a handle to the
            // pixmap: constructing one allocates a QIconPrivate and a QPixmapIconEngine and adds
            // the pixmap to it, and QIcon::pixmap() then looks its scaled results up in
            // QPixmapCache under a key derived from the icon's own serial number -- so a fresh
            // QIcon per call meant a fresh serial per call, a guaranteed cache miss, and a
            // rescale of a 256x256 pixmap on every repaint of every visible row. data() is called
            // by QStyledItemDelegate::initStyleOption during painting and again for size hints.
            //
            // The exception paths above leave _pixmap null on purpose; QIcon::addPixmap ignores a
            // null pixmap, so this stays a null QIcon in exactly the cases QIcon(_pixmap) did.
            that->_icon = QIcon (that->_pixmap);
          }
          return _icon;
        }

        return QStandardItem::data (role);
      }

      bool _rendered = false;
      QPixmap _pixmap;
      QIcon _icon;
    };

    tileset_chooser::tileset_chooser (QWidget* parent)
      : widget (parent, Qt::Window)
    {
      setWindowTitle ("Texture palette");
      setWindowIcon (QIcon (":/icon"));
      setMinimumHeight(490);

      // If modern features are enabled, set filtering to height textures (_h), otherwise specular (_s).
      _modern_features = Noggit::Application::NoggitApplication::instance()->getConfiguration()->modern_features;

      // The model chain is built empty and never rebuilt. refresh() refills only the
      // source model, so the two proxies -- and with them the user's search text and
      // the state of the "only with ..." checkbox -- survive every rescan.
      _model = new QStandardItemModel (this);

      _specular_filter = new QSortFilterProxyModel (this);
      _specular_filter->setSourceModel (_model);
      _specular_filter->setFilterRole (HAS_SPECULAR_ROLE);

      _search_filter = new QSortFilterProxyModel (this);
      _search_filter->setSourceModel (_specular_filter);
      _search_filter->sort (0, Qt::AscendingOrder);

      // Locals for the lambdas below. Capturing these by value keeps the connections
      // free of an implicit `this` capture (deprecated under C++20); they alias members
      // that outlive every connection, because both are parented to this widget.
      auto texture_filter (_specular_filter);
      auto search_filter (_search_filter);

      auto filter (new QComboBox);
      filter->setEditable (true);
      filter->addItems ( { "", "base", "brick", "brush", "bush", "cobblestone"
                         , "crack", "creep", "crop", "crystal", "dark", "dead"
                         , "dirt", "fern", "floor", "flower", "footprints"
                         , "grass", "ice", "ivy", "lava", "leaf", "light"
                         , "mineral", "moss", "mud", "needle", "pebbl", "road"
                         , "rock", "root", "rubble", "sand", "shore", "slime"
                         , "smooth", "snow", "water", "waves", "web", "weed"
                         }
                       );
      connect ( filter, &QComboBox::currentTextChanged
              , [=] (QString text)
                {
                  search_filter->setFilterRegExp (text);
                }
              );

      auto texture_filter_box(new QCheckBox("only with specular texture variant"));

      if (_modern_features)
          texture_filter_box->setText("only with height texture variant");

      connect(texture_filter_box, &QCheckBox::toggled
          , [=](bool on)
          {
              texture_filter->setFilterRegExp(on ? "true" : "");
          }
      );

      texture_filter_box->setChecked(true);

      _list = new TextureList(this);
      auto list (_list);
      list->setEditTriggers (QAbstractItemView::NoEditTriggers);
      list->setViewMode (QListView::IconMode);
      list->setMovement (QListView::Static);
      list->setResizeMode (QListView::Adjust);
      list->setUniformItemSizes (true);
      list->setIconSize ({128, 128});
      list->setWrapping (true);
      list->setModel (search_filter);

      connect(list->selectionModel(), &QItemSelectionModel::selectionChanged,
        [=]()
        {
          QModelIndexList selectedIndexes = list->selectionModel()->selectedIndexes();
          if (!selectedIndexes.isEmpty())
          {
            QModelIndex index = selectedIndexes.first();
            emit selected("tileset/" + index.data().toString().toStdString());
          }
        });

      auto size_slider (new QSlider (Qt::Horizontal));
      size_slider->setRange (64, 256);
      size_slider->setValue (128);
      connect ( size_slider, &QSlider::valueChanged
              , [=] (int size)
                {
                  list->setIconSize ({size, size});
                }
              );

      // The manual escape hatch, and the only way to pick up a texture at all when the
      // watcher had to stand down (see refreshWatchedDirectories: over the handle budget,
      // or unable to register a single path). It is no longer the only way to notice a
      // first-ever <project>/tileset -- the watcher follows the project root until that
      // directory exists -- but it is still what a user reaches for when in doubt.
      auto rescan_button (new QPushButton ("Rescan"));
      rescan_button->setToolTip
          ( "Re-read the project folder, picking up .blp files added to"
            " <project>/tileset since Noggit started."
          );
      connect ( rescan_button, &QPushButton::clicked
              , this
              , [this]
                {
                  // A deliberate rescan supersedes whatever the watcher had queued.
                  if (_rescan_debounce_timer)
                    _rescan_debounce_timer->stop();

                  _rescan_pending_while_hidden = false;

                  refresh();
                }
              );

      _auto_rescan_box = new QCheckBox ("auto");
      _auto_rescan_box->setToolTip
          ( "Watch the project's tileset folders and rescan automatically a moment"
            " after files are added or removed."
          );

      auto layout (new QVBoxLayout (this));
      auto top_bar (new QHBoxLayout);
      layout->addLayout (top_bar);
      top_bar->addWidget (size_slider);
      top_bar->addStretch();
      top_bar->addWidget (texture_filter_box);
      top_bar->addWidget (filter);
      top_bar->addWidget (rescan_button);
      top_bar->addWidget (_auto_rescan_box);
      layout->addWidget (list);

      // Must come before the first refresh(): the watcher setup restores the persisted
      // auto-rescan preference, and refresh() hands the directories it just walked
      // straight to the watcher on its way out.
      setupProjectWatcher();

      refresh();

      resize(155 * 5 + 35, height());
    }

    // Was the constructor body. Everything here is listfile enumeration, a std::filesystem
    // walk and QStandardItem construction: no texture is loaded, no scoped_blp_texture_reference
    // is constructed or destroyed, and no GL call is made. That is what makes it safe to run
    // off a file-system event and not only off a click. The model_items dropped below own a
    // plain QPixmap produced by QPixmap::fromImage -- a raster copy with no GL resource behind
    // it and no reference into TextureManager -- so discarding them cannot drop the last
    // reference to a blp_texture on a thread with no current context. The blp_texture that
    // produced each pixmap was a stack local inside BLPRenderer::render_blp_to_pixmap and died
    // there, under the renderer's own offscreen context. Re-rendering after a rebuild is
    // usually free as well: BLPRenderer keeps a process-wide pixmap cache keyed by name and
    // size that this rebuild does not touch.
    void tileset_chooser::refresh()
    {
      // A rescan the user did not ask for must not move the palette under their cursor or
      // silently drop what they had picked, so the selected texture and the scroll offset
      // are taken now and put back at the end.
      QString previously_selected;
      {
        QModelIndex const current (_list->selectionModel()->currentIndex());
        if (current.isValid())
          previously_selected = current.data().toString();
      }
      int const previous_scroll (_list->verticalScrollBar()->value());

      std::vector<std::string> tilesets;
      std::unordered_set<std::string> tilesets_with_specular_variant;

      _tileset_directories.clear();

      for (auto const& entry_pair : Application::NoggitApplication::instance()->clientData()->listfile()->pathToFileDataIDMap())
      {
        std::string const& filepath = entry_pair.first;

        if ( filepath.find ("tileset") != std::string::npos
          && filepath.find (".blp") != std::string::npos
           )
        {
          auto suffix_pos (filepath.find (_modern_features ? "_h.blp" : "_s.blp"));
          if (suffix_pos == std::string::npos)
          {
            tilesets.emplace_back (filepath);
          }
          else
          {
            std::string specular (filepath);
            specular.erase (suffix_pos, strlen (_modern_features ? "_h" : "_s"));
            tilesets_with_specular_variant.emplace (specular);
          }
        }
      }

      {

        auto const prefix (std::filesystem::path ( Noggit::Project::CurrentProject::get()->ProjectPath ));
        auto const prefix_size (prefix.string().length());

        // Taken here rather than in setupProjectWatcher() so there is exactly one place
        // that reads the project path, and so the watcher follows it if it ever changes.
        // QDir gives the same absolute, forward-slash form the tileset entries below get,
        // which is what lets both feed one watch set. The empty guard is not academic:
        // QDir("").absolutePath() answers with the process working directory, and watching
        // that would be worse than watching nothing.
        _project_root = prefix.empty()
                      ? QString()
                      : QDir (QString::fromStdString (prefix.string())).absolutePath();

        if (std::filesystem::exists (prefix))
        {
          // skip_permission_denied and the catch below exist because this walk is no
          // longer a once-per-session affair. A directory that vanishes or refuses
          // access between two iterator steps used to be a startup crash nobody hit;
          // from a timer it would be an exception escaping a Qt slot, which is a
          // std::terminate. A partial list plus a log line is the survivable outcome.
          try
          {
            for ( auto const& entry_abs : std::filesystem::recursive_directory_iterator
                    (prefix, std::filesystem::directory_options::skip_permission_denied))
            {
              auto entry ( BlizzardArchive::ClientData::normalizeFilenameInternal(
                  entry_abs.path().string().substr(prefix_size))
                         );

              std::error_code ec;
              if (entry_abs.is_directory (ec))
              {
                // The watch set, collected here rather than by a second walk. Only
                // directories whose *relative* path contains "tileset" qualify, which is
                // exactly the set of paths the loop below is willing to list -- testing
                // the relative path also stops a project folder that happens to be named
                // ...\tileset\... from matching everything.
                if (entry.find ("tileset") != std::string::npos)
                {
                  _tileset_directories.append
                      (QDir (QString::fromStdString (entry_abs.path().string())).absolutePath());
                }

                continue;
              }

              if ( entry.find ("tileset") != std::string::npos
                && entry.find (".blp") != std::string::npos
                && entry.find("_h.blp") == std::string::npos // skip _h textures
                 )
              {
                auto suffix_pos (entry.find (_modern_features ? "_h.blp" : "_s.blp"));
                if (suffix_pos == std::string::npos)
                {
                  tilesets.emplace_back (entry);
                }
                else
                {
                  std::string specular (entry);
                  specular.erase (suffix_pos, strlen (_modern_features ? "_h" : "_s"));
                  tilesets_with_specular_variant.emplace (specular);
                }
              }
            }
          }
          catch (std::filesystem::filesystem_error const& e)
          {
            LogError << "Texture palette: project folder scan stopped early: "
                     << e.what() << ". The list may be incomplete; use Rescan."
                     << std::endl;
          }
        }
      }

      {
        // The selection model stays blocked across the clear, the refill and the restore.
        // Its selectionChanged is wired to the `selected` signal, and in the texturing tool
        // that signal's handler calls makeCurrent() and builds a scoped_blp_texture_reference
        // -- real GL work. Letting a file-system event re-emit it would run that work at an
        // arbitrary moment, which is the whole class of bug this widget must not introduce.
        QSignalBlocker const selection_blocker (_list->selectionModel());

        _model->clear();

        for (auto const& texture : tilesets)
        {
          auto item ( new model_item
                        (QString::fromStdString (texture).remove ("tileset/"))
                    );
          item->setData ( tilesets_with_specular_variant.count (texture) ? "true" : "false"
                        , HAS_SPECULAR_ROLE
                        );
          _model->appendRow (item);
        }

        _search_filter->sort (0, Qt::AscendingOrder);

        if (!previously_selected.isEmpty())
        {
          // Flat list, so a linear scan of the proxy is the whole search. Paid once per
          // rescan against a few thousand rows at worst.
          for (int i = 0; i < _search_filter->rowCount(); ++i)
          {
            QModelIndex const index (_search_filter->index (i, 0));

            if (index.data().toString() != previously_selected)
              continue;

            _list->selectionModel()->setCurrentIndex
                (index, QItemSelectionModel::ClearAndSelect);
            break;
          }
        }
      }

      // After the refill, because the scrollable range only exists once the rows do. The
      // bar clamps to the new maximum on its own if the texture count shrank.
      _list->verticalScrollBar()->setValue (previous_scroll);

      // The view learns which rows to paint as selected from the selection model's signals,
      // which were blocked above: the state is correct but nothing asked for a repaint.
      _list->viewport()->update();

      // Directories created since the last scan only exist as far as we are concerned now,
      // so the watch set is refreshed here rather than at setup time.
      refreshWatchedDirectories();
    }

    void tileset_chooser::setupProjectWatcher()
    {
      // QFileSystemWatcher emits on the thread that owns it, and this widget lives on the
      // GUI thread, so the debounce and the rebuild it eventually triggers are main-thread
      // work. Parenting both to `this` also guarantees they cannot outlive the palette,
      // which matters because their handlers dereference members.
      _project_watcher = new QFileSystemWatcher (this);

      _rescan_debounce_timer = new QTimer (this);
      _rescan_debounce_timer->setSingleShot (true);
      _rescan_debounce_timer->setInterval (RESCAN_DEBOUNCE_MS);

      {
        // Restoring the persisted preference must not look like a user click: the toggled
        // handler refreshes the watch set, and there is nothing to watch until the first
        // scan has run.
        QSettings settings;
        QSignalBlocker const blocker (_auto_rescan_box);
        _auto_rescan_box->setChecked (settings.value (AUTO_RESCAN_SETTING, true).toBool());
      }

      connect ( _project_watcher, &QFileSystemWatcher::directoryChanged, this
              , [this] (QString const&)
                {
                  if (!_auto_rescan_box->isChecked())
                    return;

                  // While the project root is standing in for a tileset tree that does not
                  // exist yet, the only event worth acting on is one that created it. The
                  // root is written by ordinary editing -- ApplicationProjectWriter rewrites
                  // noggit_palettes.json there on every palette edit -- so an ungated root
                  // watch would restart the debounce, and walk the whole project folder,
                  // every time the user touched a palette. One non-recursive listing of the
                  // root settles it, and costs nothing next to the rebuild it prevents.
                  //
                  // Not folded into the debounce handler: the point is to leave the timer
                  // alone entirely, so a project that never grows a tileset folder never
                  // schedules anything at all.
                  if (_watching_project_root && !projectRootHasTilesetDirectory())
                    return;

                  // start() on a running single-shot timer restarts it, so a burst of
                  // events collapses into one rescan after the burst ends.
                  _rescan_debounce_timer->start();
                }
              );

      connect ( _rescan_debounce_timer, &QTimer::timeout, this
              , [this]
                {
                  // Rebuilding a hidden palette is pure waste, and hidden is its normal
                  // state: the texturing tool's copy sits in a dock that starts hidden and
                  // the brush stack's copy is a tool window the user pops open. Noggit also
                  // writes into the project folder throughout an ordinary session (ADT
                  // saves, noggit_palettes.json on every palette change), so an unguarded
                  // handler would walk the project folder on a timer for the whole session.
                  //
                  // Narrowing the watch to tileset directories keeps most of that churn out,
                  // and in the one mode that does watch the write-heavy project root the
                  // handler above refuses to start this timer until a tileset directory
                  // actually exists. This guard is the third line anyway: it is the
                  // difference between "usually idle" and "provably idle while nobody is
                  // looking", and it is what bounds the cost if either of the other two is
                  // ever loosened.
                  //
                  // The event is deferred, not dropped -- showEvent pays it back.
                  if (!isVisible())
                  {
                    _rescan_pending_while_hidden = true;
                    return;
                  }

                  refresh();
                }
              );

      connect ( _auto_rescan_box, &QCheckBox::toggled, this
              , [this] (bool on)
                {
                  QSettings settings;
                  settings.setValue (AUTO_RESCAN_SETTING, on);

                  if (!on)
                    _rescan_debounce_timer->stop();

                  refreshWatchedDirectories();

                  // Ticking the box does not replay the events that happened while it was
                  // unticked, so a tileset directory created during that time would leave
                  // the palette watching the project root for something that is already
                  // there -- and waiting for the next unrelated write to the root to notice.
                  // The stale case is worth one directory listing on a user click.
                  if (on && _watching_project_root && projectRootHasTilesetDirectory())
                    _rescan_debounce_timer->start();
                }
              );
    }

    void tileset_chooser::refreshWatchedDirectories()
    {
      if (!_project_watcher)
        return;

      if (!_auto_rescan_box->isChecked())
      {
        setWatchedDirectories ({});
        _watching_project_root = false;
        return;
      }

      // Deliberately narrower than the asset browser, which has to watch every directory it
      // walked because placeable assets live at arbitrary depth. Tilesets do not: this widget
      // only ever lists a .blp whose relative path contains "tileset", so a change anywhere
      // else cannot alter what the palette shows and a handle spent there is wasted. Skipping
      // the rest also keeps the two write-heavy paths out of the watch set entirely -- ADT
      // saves under world/maps, and noggit_palettes.json in the project root, which is
      // rewritten on every single palette edit.
      //
      // Narrowing used to mean giving up entirely on a project with no tileset directory
      // yet, on the grounds that this cost one Rescan click in a project with no textures.
      // That reasoning had the case backwards: a project with no <project>/tileset is
      // precisely the project where the user is about to create one, which is the single
      // scenario the auto checkbox exists to serve. Leaving the box ticked and watching
      // nothing made it a no-op exactly when it was needed. So instead of watching nothing
      // we watch the nearest directory that does exist -- the project folder itself -- and
      // re-point onto the real tree the moment it appears. The extra churn that buys is
      // paid for by the gate in the directoryChanged handler, not by rescanning.
      if (_tileset_directories.size() > MAX_WATCHED_DIRECTORIES)
      {
        setWatchedDirectories ({});
        _watching_project_root = false;

        if (!_watch_budget_exceeded)
        {
          _watch_budget_exceeded = true;

          LogError << "Texture palette: project folder contains " << _tileset_directories.size()
                   << " tileset directories, over the watch budget of " << MAX_WATCHED_DIRECTORIES
                   << ". Automatic texture detection is disabled for this project because"
                      " watching that many directories costs one OS handle each, per open"
                      " palette. Use the Rescan button after adding files on disk."
                   << std::endl;

          _auto_rescan_box->setEnabled (false);
          _auto_rescan_box->setToolTip
              ( "Disabled: this project folder has too many tileset directories to watch"
                " cheaply. Use the Rescan button after adding .blp files on disk."
              );
        }

        return;
      }

      QStringList wanted;

      if (_tileset_directories.isEmpty())
      {
        _watching_project_root = !_project_root.isEmpty() && QDir (_project_root).exists();

        if (_watching_project_root)
        {
          wanted << _project_root;

          if (!_no_tileset_directory_logged)
          {
            _no_tileset_directory_logged = true;

            Log << "Texture palette: no tileset directory under the project folder yet,"
                   " so the project folder itself is being watched until one appears."
                   " <project>/tileset and its contents will be picked up automatically."
                << std::endl;
          }
        }
      }
      else
      {
        _watching_project_root = false;
        wanted = _tileset_directories;
      }

      setWatchedDirectories (wanted);

      // Everything above this point decided what *should* be watched; this decides what the
      // checkbox is allowed to claim. Ticked-and-watching-nothing is the state this whole
      // change exists to abolish, so if not one directory could be registered -- no project
      // folder on disk, a path the OS refuses, the watcher out of handles -- the box says so
      // instead of sitting there ticked and inert.
      if (_project_watcher->directories().isEmpty())
      {
        if (!_watch_unavailable_logged)
        {
          _watch_unavailable_logged = true;

          LogError << "Texture palette: nothing under the project folder could be watched,"
                      " so automatic rescanning is unavailable. Use the Rescan button after"
                      " adding .blp files on disk." << std::endl;
        }

        _auto_rescan_box->setEnabled (false);
        _auto_rescan_box->setToolTip
            ( "Disabled: this project folder cannot be watched. Use the Rescan button"
              " after adding .blp files on disk."
            );

        return;
      }

      // Recovered, or never broken. Guarded by the budget latch because that one is a
      // deliberate give-up for the rest of the session: if the directory count later drops
      // back under the budget the code above stops taking the early return, and without
      // this guard the palette would quietly re-enable a checkbox it already told the user,
      // in the log and in the tooltip, that it had switched off.
      if (!_watch_budget_exceeded)
      {
        _auto_rescan_box->setEnabled (true);
        _auto_rescan_box->setToolTip
            ( _watching_project_root
            ? "This project has no tileset folder yet, so the project folder is being"
              " watched instead. The palette will pick up <project>/tileset as soon as"
              " it appears, and follow it from then on."
            : "Watch the project's tileset folders and rescan automatically a moment"
              " after files are added or removed."
            );
      }
    }

    // Split out of refreshWatchedDirectories so the project-root fallback and the real
    // tileset set go through identical add/remove bookkeeping -- the fallback watches a
    // single path, which is exactly the case where a bespoke "just add it" would have been
    // tempting and would have quietly stopped removing the paths it replaced.
    void tileset_chooser::setWatchedDirectories (QStringList const& wanted)
    {
      QStringList const currently_watched (_project_watcher->directories());

      QSet<QString> const wanted_set (wanted.begin(), wanted.end());
      QSet<QString> const watched_set (currently_watched.begin(), currently_watched.end());

      QStringList to_remove;
      for (QString const& path : currently_watched)
      {
        if (!wanted_set.contains (path))
          to_remove << path;
      }

      QStringList to_add;
      for (QString const& path : wanted)
      {
        if (!watched_set.contains (path))
          to_add << path;
      }

      if (!to_remove.isEmpty())
        _project_watcher->removePaths (to_remove);

      if (to_add.isEmpty())
        return;

      // addPaths reports back a path it was already watching as a failure too, which is why
      // the already-watched ones were filtered out above: without that, a steady state
      // would log itself as a total failure on every rescan.
      QStringList const failed (_project_watcher->addPaths (to_add));

      if (!failed.isEmpty())
      {
        LogError << "Texture palette: failed to watch " << failed.size() << " of "
                 << to_add.size() << " directories (first: "
                 << failed.first().toStdString()
                 << "). Changes under them will only be picked up by the Rescan button."
                 << std::endl;
      }
    }

    // Deliberately not a recursive walk: this runs on every directoryChanged event the
    // project root raises, and the project root is written by ordinary editing. It answers
    // the one question the fallback watch is asking -- is there now a top-level directory
    // the scan in refresh() would collect? -- with a single directory listing.
    //
    // The predicate has to be the scan's own, or the two disagree and the palette either
    // rescans for nothing or never rescans at all. For a directory directly under the
    // project folder the scan's relative path is just the directory name, and it tests that
    // name lowercased for "tileset", so that is what is tested here.
    bool tileset_chooser::projectRootHasTilesetDirectory() const
    {
      if (_project_root.isEmpty())
        return false;

      // The non-throwing overloads throughout. This is called from a Qt slot, where an
      // escaping exception unwinds through the event loop and ends the process, and a
      // directory being renamed out from under the listing is exactly the situation that
      // raised the event in the first place.
      std::error_code ec;
      std::filesystem::directory_iterator entry
          ( std::filesystem::path (_project_root.toStdString())
          , std::filesystem::directory_options::skip_permission_denied
          , ec
          );

      if (ec)
        return false;

      std::filesystem::directory_iterator const end;

      for (; entry != end; entry.increment (ec))
      {
        if (ec)
          return false;

        std::error_code is_dir_ec;
        if (!entry->is_directory (is_dir_ec) || is_dir_ec)
          continue;

        auto const name ( BlizzardArchive::ClientData::normalizeFilenameInternal
                            (entry->path().filename().string())
                        );

        if (name.find ("tileset") != std::string::npos)
          return true;
      }

      return false;
    }

    void tileset_chooser::showEvent (QShowEvent* event)
    {
      // widget::showEvent emits visibilityChanged, which callers rely on; go through it.
      widget::showEvent (event);

      // A rescan the debounce timer deferred while this was hidden is paid now, once,
      // instead of having been paid repeatedly against a list nobody was looking at.
      if (_rescan_pending_while_hidden)
      {
        _rescan_pending_while_hidden = false;
        refresh();
      }
    }

    // selected_texture:
    std::optional<scoped_blp_texture_reference> selected_texture::texture = std::nullopt;

    std::optional<scoped_blp_texture_reference> selected_texture::get()
    {
      return selected_texture::texture; // TODO: something performance-hungry is going on here
    }

    void selected_texture::set (scoped_blp_texture_reference t)
    {
      selected_texture::texture = std::move (t);
    }
  }
}
