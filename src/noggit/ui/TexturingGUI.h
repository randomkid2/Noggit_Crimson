// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once
#include <noggit/TextureManager.h>
#include <noggit/ui/widget.hpp>

#include <QtCore/QStringList>

#include <optional>

class QMouseEvent;
class QListView;
class QCheckBox;
class QFileSystemWatcher;
class QShowEvent;
class QSortFilterProxyModel;
class QStandardItemModel;
class QTimer;

namespace Noggit
{
  namespace Ui
  {
    class current_texture;
    class TextureList;

    struct tileset_chooser : public widget
    {
      Q_OBJECT

    public:
      tileset_chooser (QWidget* parent = nullptr);

      // Rebuilds the tileset list from the listfile and from what is on disk in the
      // project folder right now. This used to be the constructor body, which meant a
      // .blp dropped into <project>/tileset/ was invisible until Noggit was restarted:
      // the chooser is constructed once per tool and kept for the whole session.
      // Callers today are the Rescan button, the project-folder watcher and showEvent.
      void refresh();

    signals:
      void selected (std::string);

    protected:
      // Pays back a rescan the watcher deferred while this palette was hidden, which is
      // the normal state: in the texturing tool the chooser lives inside a dock that
      // starts hidden and in the brush stack it is a toggled tool window.
      void showEvent (QShowEvent* event) override;

    private:
      void setupProjectWatcher();
      void refreshWatchedDirectories();

      // Makes `wanted` the watch set, adding and removing only the difference. Shared by
      // both watch modes so the project-root fallback and the real tileset set cannot
      // drift apart. Refusals are logged here; whether anything at all ended up watched is
      // a separate question the caller asks the watcher directly.
      void setWatchedDirectories (QStringList const& wanted);

      // The only question the project-root watch asks: has a tileset directory appeared at
      // the top of the project folder? One non-recursive listing, no model work -- see the
      // call site for why this gate has to stay cheap.
      bool projectRootHasTilesetDirectory() const;

      // The view and the model chain, kept so refresh() can refill the source model
      // without rebuilding the proxies -- the user's search text and specular filter
      // live in those proxies and would otherwise be reset on every rescan.
      QStandardItemModel* _model = nullptr;
      QSortFilterProxyModel* _specular_filter = nullptr;
      QSortFilterProxyModel* _search_filter = nullptr;
      TextureList* _list = nullptr;
      QCheckBox* _auto_rescan_box = nullptr;

      // Watches the tileset-bearing directories the last scan found, or -- when the scan
      // found none -- the project folder itself, waiting for one to appear. Alongside it a
      // single-shot timer that collapses the burst of events one file copy produces. Both
      // are parented to this widget, so they cannot outlive it and their signals arrive on
      // the GUI thread.
      QFileSystemWatcher* _project_watcher = nullptr;
      QTimer* _rescan_debounce_timer = nullptr;

      // Set when the debounce fired while this palette was hidden; showEvent consumes
      // it, so a deferred rescan is never a lost one.
      bool _rescan_pending_while_hidden = false;

      // Absolute paths of every directory under the project folder whose relative path
      // contains "tileset", filled by the scan in refresh() so the watcher can be
      // re-pointed without a second walk of the disk.
      QStringList _tileset_directories;

      // The project folder itself, absolute and normalised exactly like the entries of
      // _tileset_directories so the two can be diffed against one watch set. Filled by
      // refresh() from the path it already reads, so there is no second lookup.
      QString _project_root;

      // True while the watch set is the project root standing in for a tileset tree that
      // does not exist yet. Gates the directoryChanged handler, which must not rebuild the
      // palette on the ordinary writes that land in the project root.
      bool _watching_project_root = false;

      // One-shot latches so the degradation messages are logged once per palette instead
      // of once per rescan.
      bool _watch_budget_exceeded = false;
      bool _no_tileset_directory_logged = false;
      bool _watch_unavailable_logged = false;

      // Whether the height-texture (_h) or specular (_s) suffix is the one being
      // filtered on. Read once at construction because it also picks the checkbox
      // label, and label and filter must not disagree.
      bool _modern_features = false;
    };

    class selected_texture
    {
    public:
      static void set(scoped_blp_texture_reference t);
      static std::optional<scoped_blp_texture_reference> get();
      static std::optional<scoped_blp_texture_reference> texture;
    };
  }
}
