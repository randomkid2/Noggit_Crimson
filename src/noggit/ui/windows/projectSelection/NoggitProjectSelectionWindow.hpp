#ifndef NOGGIT_UI_WINDOWS_NOGGITPROJECTSELECTIONWINDOW_HPP
#define NOGGIT_UI_WINDOWS_NOGGITPROJECTSELECTIONWINDOW_HPP

#include <QMainWindow>

#include <memory>
#include <string>

class QEvent;

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

namespace Noggit::Ui::Widget
{
    class BrandBanner;
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

        //! The painted band across the head of the window: the product mark, the wordmark, the
        //! contour texture and the rule that closes it. Parented to the central widget, so the
        //! window does not own it beyond holding this handle.
        Noggit::Ui::Widget::BrandBanner* _banner = nullptr;

        //! Puts the window on the design system's spacing scale and type ranks, names every
        //! surface the theme needs a handle on, and adds the brand band above the two columns.
        //! Appearance only: no control changes what it does. The only colours written here are
        //! the two icon-tile pairs, which exist because a QPainter cannot be reached from a
        //! style sheet; everything else is a theme handle.
        void applyVisualDesign();

    protected:
        //! Re-arms the section headings' letter spacing when the theme or the font changes
        //! underneath them. Qt style sheets carry no letter-spacing property, so the tracking is
        //! a QFont attribute and font resolution can otherwise take it away on a theme switch.
        void changeEvent(QEvent* event) override;

    private:
        void handleContextMenuProjectListItemDelete(std::string const& project_path);
        void handleContextMenuProjectListItemForget(std::string const& project_path);
        void handleContextMenuProjectListItemFavorite(int index);

        void resetFavoriteProject();
    };
}
#endif // NOGGIT_UI_WINDOWS_NOGGITPROJECTSELECTIONWINDOW_HPP
