#ifndef NOGGIT_FRAMELESSWINDOW_HPP
#define NOGGIT_FRAMELESSWINDOW_HPP

#include <external/framelesshelper/framelesswindowsmanager.h>
#include <noggit/ui/FontAwesome.hpp>
#include <ui_TitleBar.h>

#include <QSettings>
#include <QWidget>


namespace Noggit::Ui
{
  template <typename T>
  ::Ui::TitleBar* setupFramelessWindow(QWidget* titlebar_target, T* window,  QSize minimum_size, QSize maximum_size, bool is_resizeable = true)
  {

    QSettings settings;

    // THREE readers of one key, and this one used to disagree with the other two about what an
    // unset key means. NoggitWindow and the settings panel both default systemWindowFrame to
    // true; this defaulted it to false. On a fresh profile the result was one application
    // wearing two different window chromes at once: the main window kept the OS frame, because
    // its own reader said true and it skipped the frameless branch entirely, while the settings
    // window called in here unconditionally, got false, and built the custom Crimson title bar
    // -- underneath a radio button that said "system frame". Aligned to the other two, so an
    // unset key means the OS frame everywhere and the checkbox describes what is on screen.
    if (settings.value("systemWindowFrame", true).toBool())
    {
      return nullptr;
    }

    auto titleBarWidget = new ::Ui::TitleBar;
    titleBarWidget->setupUi(titlebar_target);
    titleBarWidget->windowTitle->setText(window->windowTitle());

    titleBarWidget->iconButton->setAccessibleName("titlebar_icon");
    titleBarWidget->iconButton->setMinimumWidth(32);
    titleBarWidget->minimizeButton->setIcon(FontAwesomeIcon(FontAwesome::windowminimize));
    titleBarWidget->minimizeButton->setIconSize(QSize(16, 16));
    titleBarWidget->minimizeButton->setAccessibleName("titlebar_minimize");
    titleBarWidget->maximizeButton->setIcon(FontAwesomeIcon(FontAwesome::windowmaximize));
    titleBarWidget->maximizeButton->setAccessibleName("titlebar_maximize");
    titleBarWidget->maximizeButton->setIconSize(QSize(14, 14));
    titleBarWidget->closeButton->setIcon(FontAwesomeIcon(FontAwesome::times));
    titleBarWidget->closeButton->setAccessibleName("titlebar_close");
    titleBarWidget->closeButton->setIconSize(QSize(18, 18));

    QObject::connect(titleBarWidget->closeButton,
                     &QPushButton::clicked,
                     window,
                     &T::close);

    QObject::connect(titleBarWidget->minimizeButton,
                     &QPushButton::clicked,
                     window,
                     &T::showMinimized);

    QObject::connect(titleBarWidget->maximizeButton,
                     &QPushButton::clicked,
                     [window, titleBarWidget]() {
                       if (window->isMaximized()) {
                         window->showNormal();
                         titleBarWidget->maximizeButton->setToolTip(QObject::tr("Maximize"));
                       } else {
                         window->showMaximized();
                         titleBarWidget->maximizeButton->setToolTip(QObject::tr("Restore"));
                       }
                     });

    FramelessWindowsManager::addWindow(window);

    FramelessWindowsManager::addIgnoreObject(window, titleBarWidget->minimizeButton);
    FramelessWindowsManager::addIgnoreObject(window, titleBarWidget->closeButton);

    FramelessWindowsManager::setResizable(window, is_resizeable);
    FramelessWindowsManager::setMinimumSize(window, minimum_size);
    FramelessWindowsManager::setMaximumSize(window, maximum_size);

    return titleBarWidget;

  }

}

#endif //NOGGIT_FRAMELESSWINDOW_HPP
