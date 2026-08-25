#ifndef NOGGIT_WINDOW_NOGGIT_HPP
#define NOGGIT_WINDOW_NOGGIT_HPP

#include <noggit/ui/uid_fix_mode.hpp>

#include <math/trig.hpp>

#include <QtWidgets/QMainWindow>
#include <QWidget>
#include <QMetaObject>

#include <memory>
#include <string>
#include <unordered_set>

class MapView;
class StackedWidget;
class World;

class QChildEvent;
class QEvent;
class QLabel;
class QListWidget;
class QObject;

namespace BlizzardArchive
{
  class ClientData;

  namespace Archive
  {
    class MPQArchive;
  }
}

namespace Noggit::Application
{
  struct NoggitApplicationConfiguration;
}

namespace Noggit::Ui::Component
{
  class BuildMapListComponent;
}

namespace Noggit::Project
{
  class NoggitProject;
}

namespace Noggit::Ui::Tools::MapCreationWizard::Ui
{
  class MapCreationWizard;
}

namespace Noggit::Ui
{
    class minimap_widget;
    class settings;
    class about;

    namespace Tools::MapCreationWizard::Ui
    {
      class MapCreationWizard;
    }
}

namespace Noggit::Ui::Windows
{
    class NoggitWindow : public QMainWindow
    {
      Q_OBJECT

      friend class Noggit::Ui::Component::BuildMapListComponent;

    public:
      NoggitWindow(std::shared_ptr<Noggit::Application::NoggitApplicationConfiguration> application,
          std::shared_ptr<Noggit::Project::NoggitProject> project);

      void promptExit(QCloseEvent* event);
      void promptUidFixFailure();

      // TODO, better location for those utils that need to be above mapview?
      void startWowClient();

      void patchWowClient();


      QMenuBar* _menuBar;

      QToolBar* _app_toolbar;

      // std::unique_ptr<World> _world;

      std::unordered_set<QWidget*> displayed_widgets;
      void buildMenu();

    protected:
      //! Watches the status bar for the seven readouts MapView reparents onto it, and gives each
      //! one an object name and a rank. See NoggitWindow::prepareStatusBar for why identifying
      //! them by arrival order is the only handle this window has on labels it does not build.
      bool eventFilter(QObject* watched, QEvent* event) override;

    signals:
      void exitPromptOpened();
      void mapSelected(int map_id);

    private:
    	std::unique_ptr<Component::BuildMapListComponent> _buildMapListComponent;
      std::shared_ptr<Application::NoggitApplicationConfiguration> _applicationConfiguration;
      std::shared_ptr<Project::NoggitProject> _project;


      // Second half of patchWowClient: adds the models and textures the packed terrain references
      // but the project folder does not contain. Returns the one line summary and fills
      // `detail_out` with the full report, which is what NAMES every reference that resolved
      // nowhere -- a dependency pack that omits something silently is worse than one that does not
      // run at all.
      //
      // Split out of patchWowClient rather than inlined because it owns a modal QProgressDialog and
      // therefore pumps the event loop, and everything it holds across that pump has to be visible
      // in one place.
      QString packReferencedAssets( BlizzardArchive::ClientData* client_data
                                  , BlizzardArchive::Archive::MPQArchive* archive
                                  , bool include_base_client_assets
                                  , bool compress
                                  , bool compact
                                  , QString& detail_out
                                  );

      void handleEventMapListContextMenuPinMap(int mapId, std::string MapName);
      void handleEventMapListContextMenuUnpinMap(int mapId);

      World* getWorld();

      void loadMap (int map_id);

      //! Names the status bar and arms the readout filter. Appearance only.
      void prepareStatusBar();

      //! Gives one status-bar readout its object name and, for the three that carry a number the
      //! user watches, the value rank.
      void dressStatusReadout(QLabel* readout);

      //! Re-states the map-detail header on the right-hand pane from whichever row the map list
      //! currently has selected. Reads nothing from the client; every string comes from the row.
      void updateMapDetail();

      void check_uid_then_enter_map ( glm::vec3 pos
                                    , math::degrees camera_pitch
                                    , math::degrees camera_yaw
                                    , bool from_bookmark = false
                                    );

      void enterMapAt ( glm::vec3 pos
                      , math::degrees camera_pitch
                      , math::degrees camera_yaw
                      , uid_fix_mode uid_fix = uid_fix_mode::none
                      , bool from_bookmark = false
                      );

      minimap_widget* _minimap;
      settings* _settings;
      about* _about;
      QWidget* _null_widget;
      MapView* _map_view;
      StackedWidget* _stack_widget;

      Noggit::Ui::Tools::MapCreationWizard::Ui::MapCreationWizard* _map_creation_wizard;
      QMetaObject::Connection _map_wizard_connection;

      QListWidget* _continents_table;
      QString _filter_name;
      QTabWidget* _right_side;

      //! The head of the right-hand pane: which map is selected, and what it is.
      QLabel* _map_detail_title = nullptr;
      QLabel* _map_detail_meta = nullptr;

      //! How many status-bar readouts have been dressed. Positional, taken modulo the seven
      //! MapView adds, because a second map entry produces a second burst.
      int _status_readouts_seen = 0;

      void applyFilterSearch(const QString& name, int type, int expansion, bool wmo_maps);

      bool map_loaded = false;
      bool exit_to_project_selection = false;

      virtual void closeEvent (QCloseEvent*) override;
    };
}
#endif // NOGGIT_WINDOW_NOGGIT_HPP
