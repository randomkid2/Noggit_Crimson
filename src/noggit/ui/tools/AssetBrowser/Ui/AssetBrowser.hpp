#ifndef NOGGIT_ASSETBROWSER_HPP
#define NOGGIT_ASSETBROWSER_HPP

#include <QWidget>
#include <QSortFilterProxyModel>
#include <QRegularExpression>
#include <QMainWindow>
#include <QMap>
#include <QStringList>

namespace Ui
{
  class AssetBrowser;
  class AssetBrowserOverlay;
}

class MapView;

class QFileSystemWatcher;
class QStandardItemModel;
class QTimer;

// custom model that makes the searched children expend, credit to https://stackoverflow.com/questions/56781145/expand-specific-items-in-a-treeview-during-filtering
class NoggitExpendableFilterProxyModel : public QSortFilterProxyModel
{
    Q_OBJECT
public:
    QList<QModelIndex> findIndices() const;

    bool rowAccepted(int source_row, const QModelIndex& source_parent) const;
private:
    QList<QModelIndex> recursivelyFindIndices(const QModelIndex& ind) const;
};

namespace Noggit::Ui::Tools::AssetBrowser
{
    enum class asset_browse_mode
    {
        world, // any wmo and m2 in world folder. exclude detail doodads?
        detail_doodads, // "world/nodxt/detail/
        skybox, // environements folder. used by LightSkybox
        creatures,
        characters,
        particles,
        cameras,
        items,
        spells,
        ALL
    };
}

namespace Noggit
{
  namespace Ui::Tools
  {
    class PreviewRenderer;
  namespace AssetBrowser::Ui
  {
    namespace Model
    {
      class TreeManager;
    }

    class AssetBrowserWidget : public QMainWindow
    {
      Q_OBJECT
    public:
      AssetBrowserWidget(MapView* map_view, QWidget* parent = nullptr);
      ~AssetBrowserWidget();
      std::string const& getFilename() const;;

      void set_browse_mode(asset_browse_mode browse_mode);

      asset_browse_mode _browse_mode = asset_browse_mode::world;
    signals:
        void gl_data_unloaded();
        void selectionChanged(std::string const& path);

    private:
      ::Ui::AssetBrowser* ui;
      ::Ui::AssetBrowserOverlay* viewport_overlay_ui;
      QStandardItemModel* _model;
      NoggitExpendableFilterProxyModel* _sort_model;
      PreviewRenderer* _preview_renderer;
      QRegularExpression _wmo_group_and_lod_regex;
      MapView* _map_view;
      std::string _selected_path;

      // Watches the project folder so assets dropped on disk show up without a
      // restart, and a single-shot timer that coalesces the burst of change
      // events a file copy produces. Both are parented to this widget, so they
      // live and die with it and their signals arrive on the GUI thread.
      QFileSystemWatcher* _project_watcher = nullptr;
      QTimer* _rescan_debounce_timer = nullptr;

      // Set when the debounce fired while the browser was hidden, which is the common case: the
      // widget exists for the whole session but is only shown on demand, and Noggit's own writes
      // into the project folder (ADT saves, palette files) keep the watcher busy regardless.
      // showEvent() consumes it, so a deferred rescan is never a lost one.
      bool _rescan_pending_while_hidden = false;

      // Every directory the last project-folder scan walked, in scan order, root
      // first. Filled by recurseDirectory() so the watcher can be re-pointed at
      // the tree that actually exists now -- new subfolders included -- without a
      // second walk of the disk.
      QStringList _scanned_directories;

      // Set once when the project turns out to be too big to watch, so the
      // explanation is logged a single time instead of on every rescan.
      bool _watch_budget_exceeded = false;

      void updateModelData();
      void recurseDirectory(Model::TreeManager& tree_mgr, const QString& s_dir, const QString& project_dir);
      bool validateBrowseMode(const QString& wow_file_path);

      // The one place that decides whether a path passes the WMOs/Models
      // checkboxes. Both the listfile pass and the project-folder pass call it.
      bool isAssetTypeEnabled(const QString& wow_file_path) const;

      void setupProjectWatcher();
      void refreshWatchedDirectories();

      // View state preservation across a model rebuild.
      QStringList collectExpandedPaths() const;
      QModelIndex indexForPath(const QString& path) const;

      // commented objects that shouldn't be placed on the map, still accessible through Show all
      const QMap<QString, asset_browse_mode> brosweModeLabels =  {
      { "World Objects", asset_browse_mode::world },
      { "Detail Doodads", asset_browse_mode::detail_doodads },
      // { "Skybox", asset_browse_mode::skybox },
      // { "Creatures", asset_browse_mode::creatures },
      // { "Characters", asset_browse_mode::characters },
      // {"Particles", asset_browse_mode::particles },
      // {"Cameras", asset_browse_mode::cameras },
      { "Items", asset_browse_mode::items },
      { "Spells", asset_browse_mode::spells },
      { "Show All", asset_browse_mode::ALL },
      };

    protected:
      // Catches up a rescan that was deferred while hidden, so opening the browser always shows
      // what is on disk now rather than what was there when it was last visible.
      void showEvent(QShowEvent* event) override;

      void keyPressEvent(QKeyEvent* event) override;

      void setupConnectsCommon();

    };
  }
  }
}


#endif //NOGGIT_ASSETBROWSER_HPP
