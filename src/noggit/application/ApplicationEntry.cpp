// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#include <noggit/application/Configuration/NoggitApplicationConfiguration.hpp>
#include <noggit/application/NoggitApplication.hpp>
#include <noggit/errorHandling.h>
#include <noggit/ui/windows/projectSelection/NoggitProjectSelectionWindow.hpp>

#include <qcommandlineoption.h>
#include <qcommandlineparser.h>
#include <QStyleFactory>
#include <QtWidgets/QApplication>
#include <QtWidgets/QFileDialog>

#ifdef NOGGIT_DEV_BRIDGE_ENABLED
#include <noggit/database/SpawnPlacement.hpp>
#include <noggit/project/ApplicationProject.h>
#include <noggit/project/CurrentProject.hpp>
#include <noggit/ui/windows/noggitWindow/NoggitWindow.hpp>
#include <noggit/Log.h>

#include <filesystem>
#endif

QCommandLineParser* ProcessCommandLine()
{
    QCommandLineParser* parser = new QCommandLineParser();
    parser->setApplicationDescription("Help");
    parser->addHelpOption();
    parser->addVersionOption();
    parser->addOptions({
        {"disable-update", QApplication::translate("main", "Disable the check for update.")},
        {"force-changelog", QApplication::translate("main", "Force displaying the changelog popup.")}
        });

#ifdef NOGGIT_DEV_BRIDGE_ENABLED
    // Unattended startup, for the dev bridge. Compiled only into a -DNOGGIT_DEV_BRIDGE=ON build,
    // because it skips the UID fix -- see NoggitWindow::openMapUnattended.
    parser->addOptions({
        {"project", QApplication::translate("main", "Open this project folder without the selection window (dev bridge builds only)."), "path"},
        {"map", QApplication::translate("main", "Open this map id immediately. Requires --project."), "id"},
        {"goto", QApplication::translate("main", "Enter at these TrinityCore server coordinates, as x,y,z."), "coords"}
        });
#endif

    return parser;
}

int main(int argc, char *argv[])
{
  Noggit::RegisterErrorHandlers();
  std::set_terminate(Noggit::Application::NoggitApplication::terminationHandler);

  QApplication::setStyle(QStyleFactory::create("Fusion"));
  QCoreApplication::setAttribute(Qt::AA_UseDesktopOpenGL);
  QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
  QApplication q_application (argc, argv);
  q_application.setApplicationName ("Noggit");
  q_application.setOrganizationName ("Noggit");

  auto parser = ProcessCommandLine();
  parser->process(q_application);

  std::vector<bool> Command;
  Command.push_back(parser->isSet("disable-update"));
  Command.push_back(parser->isSet("force-changelog"));

  auto noggit = Noggit::Application::NoggitApplication::instance();
  bool initialized = noggit->initalize(argc, argv, Command);

  if (!initialized) [[unlikely]]
  {
    return EXIT_FAILURE;
  }

#ifdef NOGGIT_DEV_BRIDGE_ENABLED
  if (parser->isSet("project"))
  {
    // Mirrors what NoggitProjectSelectionWindow does when a project is opened from disk, minus the
    // window. Kept in step with that path by hand; if project loading grows a step there, it needs
    // one here too.
    std::filesystem::path const project_path (parser->value("project").toStdString());

    auto const configuration = noggit->getConfiguration();
    auto project_service = Noggit::Project::ApplicationProject(configuration);
    auto project = project_service.loadProject(project_path);

    if (!project)
    {
      LogError << "Could not load project at " << project_path.string() << std::endl;
      return EXIT_FAILURE;
    }

    noggit->setClientData(project->ClientData);
    Noggit::Project::CurrentProject::initialize(project.get());

    // Leaked on purpose: it lives for the life of the application, exactly as the window the
    // selection page would otherwise have owned does.
    auto window = new Noggit::Ui::Windows::NoggitWindow(configuration, project);
    window->showMaximized();

    if (parser->isSet("map"))
    {
      glm::vec3 position (0.0f, 0.0f, 0.0f);

      if (parser->isSet("goto"))
      {
        auto const parts = parser->value("goto").split(',');

        if (parts.size() != 3)
        {
          LogError << "--goto expects three comma-separated server coordinates." << std::endl;
          return EXIT_FAILURE;
        }

        // Through the tested conversion, never by hand. Same seam the bridge's `goto` uses.
        Noggit::Database::NoggitPlacement const placement
          ( Noggit::Database::SpawnPlacement::positionFor
            ( Noggit::Database::WorldPosition
              {parts[0].toDouble(), parts[1].toDouble(), parts[2].toDouble()}
            )
          );

        position = glm::vec3( static_cast<float>(placement.x)
                            , static_cast<float>(placement.y)
                            , static_cast<float>(placement.z)
                            );
      }

      if (!window->openMapUnattended(parser->value("map").toInt(), position))
      {
        return EXIT_FAILURE;
      }
    }

    return q_application.exec();
  }
#endif

  auto project_selection = new Noggit::Ui::Windows::NoggitProjectSelectionWindow(noggit);
  // project_selection->show();

  return q_application.exec();
}
