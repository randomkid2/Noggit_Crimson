#ifndef NOGGIT_UI_WINDOWS_NOGGITPROJECTSELECTIONWINDOW_HPP
#define NOGGIT_UI_WINDOWS_NOGGITPROJECTSELECTIONWINDOW_HPP

#include <QMainWindow>

QT_BEGIN_NAMESPACE
namespace Ui { class NoggitProjectSelectionWindow; }
QT_END_NAMESPACE

namespace Noggit::Ui
{
  class settings;
}

namespace Noggit::Ui::Component
{
    class RecentProjectsComponent;
    class CreateProjectComponent;
    class LoadProjectComponent;
}

namespace Noggit::Ui::Windows
{
  class NoggitWindow;
}

namespace Noggit::Application
{
    class NoggitApplication;
}

namespace Noggit::Ui::Windows
{
    class NoggitProjectSelectionWindow : public QMainWindow
    {
        Q_OBJECT
    	friend Component::RecentProjectsComponent;
        friend Component::CreateProjectComponent;
        friend Component::LoadProjectComponent;
    public:
        NoggitProjectSelectionWindow(Noggit::Application::NoggitApplication* noggit_app, QWidget* parent = nullptr);
        ~NoggitProjectSelectionWindow();

    private:
        ::Ui::NoggitProjectSelectionWindow* _ui;
        Noggit::Application::NoggitApplication* _noggit_application;
        Noggit::Ui::settings* _settings;
        //Noggit::Ui::CUpdater* _updater;
        //Noggit::Ui::CChangelog* _changelog;

        std::unique_ptr<Noggit::Ui::Windows::NoggitWindow> _project_selection_page;
        std::unique_ptr<Component::LoadProjectComponent> _load_project_component;

        //! Puts the window on the design system's spacing scale and type ranks, names every
        //! surface the theme needs a handle on, and adds the brand band above the two columns.
        //! Appearance only: no control changes what it does, and no colour literal is written
        //! here -- an inline style sheet would outrank the application sheet and pin the window
        //! to one palette.
        void applyVisualDesign();

        void handleContextMenuProjectListItemDelete(std::string const& project_path);
        void handleContextMenuProjectListItemForget(std::string const& project_path);
        void handleContextMenuProjectListItemFavorite(int index);

        void resetFavoriteProject();
    };
}
#endif // NOGGIT_UI_WINDOWS_NOGGITPROJECTSELECTIONWINDOW_HPP
