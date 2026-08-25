#ifndef NOGGIT_FRAMELESSWINDOW_HPP
#define NOGGIT_FRAMELESSWINDOW_HPP

#include <external/framelesshelper/framelesswindowsmanager.h>
#include <noggit/ui/FontAwesome.hpp>
#include <ui_TitleBar.h>

#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QStyle>
#include <QWidget>
#include <QWindow>


namespace Noggit::Ui
{
  // WHAT THE CAPTION BAND ACTUALLY IS, measured on this machine with a standalone Qt 5.15.2
  // probe rather than reasoned about, because the two numbers do not agree and the difference
  // is the whole of this file's remaining problem.
  //
  // framelesshelper decides "is this point on the title bar" in WM_NCHITTEST as
  //
  //     isTitleBar = (localMouse.y <= (tbh + bh)) && !isInIgnoreObjects
  //
  // (winnativeeventfilter.cpp, the getHTResult lambda). tbh is the window's titleBarHeight,
  // which defaults to SM_CYCAPTION, and bh is SM_CYSIZEFRAME + SM_CXPADDEDBORDER. Both come
  // back from getSystemMetric in DEVICE pixels. On this machine -- Windows 11, the desktop at
  // 144 dpi, i.e. 150% scaling, GetSystemMetricsForDpi read from a per-monitor-aware probe:
  //
  //     SM_CYCAPTION 34 + SM_CYSIZEFRAME 5 + SM_CXPADDEDBORDER 6  =  45 device px band
  //     TitleBar holder, laid out                                 =  29 Qt px
  //     TitleBar holder with NoggitWindow's menu bar inserted      =  34 Qt px
  //
  // Every point in that band that is not inside a registered ignore object returns HTCAPTION,
  // which means Windows takes the press and the Qt widget underneath never sees it: content
  // that falls inside it -- the top of the toolbar on the main window, the top of the tab strip
  // on the settings window -- drags the window instead of hitting the control.
  //
  // WHAT setTitleBarHeight BELOW DOES AND DOES NOT DO. It replaces tbh, and nothing else, so
  // the band ends up at the bar's own height PLUS bh: the `+ bh` term above stays there
  // whatever we pass. It narrows the overhang, it does not close it, and bh device px of
  // content below the bar keep reporting HTCAPTION. Closing it outright would mean passing
  // (bar height - bh), which is only correct while bh is what it is today and goes wrong on any
  // machine where the two differ; the deliberate choice here is the safe direction, so this
  // stays a REDUCTION and the comment says so.
  //
  // The units do not line up either, and that is measured, not suspected. framelesshelper turns
  // the value we pass into device pixels with its OWN ratio, GetDpiForWindow/96 rounded to a
  // preferred number (GetDevicePixelRatioForWindow) -- 1.5 here -- while Qt lays the bar out at
  // QWidget::devicePixelRatioF, which a probe with AA_EnableHighDpiScaling reports as 2.0 on
  // this same monitor, because Qt 5.14+ defaults its scale-factor rounding policy to Round and
  // rounds 1.5 up. So a 29 Qt-px bar occupies 58 device px while tbh is set to round(29 * 1.5)
  // = 44, and the 45-vs-29 arithmetic above is comparing two different units. The two ratios
  // agree only at 100% scaling. This is the reason the frameless frame is still not the
  // default, and it is not fixable from this file: framelesshelper would have to be told Qt's
  // ratio.
  //
  // It is called AFTER addWindow deliberately: the only caller of updateQtFrame_internal, which
  // is what turns tbh into the DWM frame margin, is addFramelessWindow itself, so setting the
  // height afterwards changes the hit test and nothing else. Setting it before would move the
  // frame margins too.
  //
  // WHAT IS STILL NOT FIXED HERE, because it is not in this file: NoggitWindow.cpp inserts the
  // whole application menu bar into this title bar (insertWidget(2, _menuBar)) and does not
  // register it, so with the frameless frame turned on every top level menu sits inside the
  // caption band and cannot be clicked. makeTitleBarWidgetClickable below exists for exactly
  // that call site and NOTHING CALLS IT YET -- see the note on the function itself for the one
  // line that has to be added, and do not read its presence as the defect being fixed.
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

    // setupUi names the holder "TitleBar", which is what the theme selects on. A plain QWidget
    // ignores a style sheet background unless it is told to draw one, so without this the band
    // takes the default panel colour and reads as a strip of content rather than as chrome.
    titlebar_target->setAttribute(Qt::WA_StyledBackground, true);

    // The title is the one piece of text in this bar and it was inheriting plain 12/400 body
    // colour, i.e. the same rank as a check box caption three hundred pixels below it. Named so
    // the sheet can give it the window-title rank its own type scale already reserves.
    titleBarWidget->windowTitle->setAccessibleName("titlebar_title");

    titleBarWidget->iconButton->setAccessibleName("titlebar_icon");

    // setMinimumWidth(32) used to sit here. iconButton is a QLabel pinned to 28x28 by the form
    // with scaledContents true, and QWidget::setMinimumWidth raises the MAXIMUM to match when
    // the two cross -- so the pin became 32x28 and the application icon was drawn stretched 14%
    // horizontally. The horizontal breathing room it was reaching for is margin, and the theme
    // already sets it (margin-left 8px, margin-right 6px on titlebar_icon).

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
                     [window]() {
                       if (window->isMaximized()) {
                         window->showNormal();
                       } else {
                         window->showMaximized();
                       }
                     });

    FramelessWindowsManager::addWindow(window);

    FramelessWindowsManager::addIgnoreObject(window, titleBarWidget->minimizeButton);
    FramelessWindowsManager::addIgnoreObject(window, titleBarWidget->closeButton);

    // The third button was missing from this list, and the omission is invisible until you try
    // to use it: minimize and close escape the caption band because they are registered here,
    // maximize did not, so WM_NCHITTEST returned HTCAPTION over it, Windows took the press as
    // the start of a window drag and the clicked() above never fired. The button looked live,
    // hovered like the other two and did nothing. (Double-clicking it still maximised, but by
    // way of DefWindowProc's caption handling, not by way of the button.)
    FramelessWindowsManager::addIgnoreObject(window, titleBarWidget->maximizeButton);

    FramelessWindowsManager::setResizable(window, is_resizeable);
    FramelessWindowsManager::setMinimumSize(window, minimum_size);
    FramelessWindowsManager::setMaximumSize(window, maximum_size);

    // sizeHint rather than height: nothing has been laid out yet at this point, so the widget's
    // own height is still zero. It is a FLOOR and is meant to be -- a band shorter than the bar
    // costs a few pixels of drag area at the bottom of the bar, a band taller than the bar eats
    // clicks from the content below it, and only one of those two failures is recoverable by
    // the user. Callers that make the bar taller afterwards re-set it through the helper below.
    //
    // This sets tbh only. The hit test is `localMouse.y <= (tbh + bh)`, so the caption band
    // still runs bh device pixels past the bottom of the bar however tall the bar is -- the
    // header comment has the numbers. Reduction, not closure.
    FramelessWindowsManager::setTitleBarHeight(window, titlebar_target->sizeHint().height());

    // Keep the maximise control describing the state it is actually in. The window state can
    // change without this button: Aero Snap, a double click on the caption, Win+Up, or the
    // system menu. QWindow::windowStateChanged is the one notification that catches all of
    // them, and the handle exists by now because addWindow above called winId().
    //
    // The icon is swapped by renaming the control rather than by setIcon, because the icon is
    // the theme's -- qproperty-icon on the accessibleName -- and a setIcon here would be undone
    // the next time the widget is polished. Renaming plus an explicit repolish is the same
    // idiom the destructive-button state property uses elsewhere in this code base.
    if (QWindow* window_handle = window->windowHandle())
    {
      QPushButton* maximize_button = titleBarWidget->maximizeButton;

      QObject::connect(window_handle,
                       &QWindow::windowStateChanged,
                       maximize_button,
                       [window, maximize_button](Qt::WindowState)
                       {
                         bool const maximized (window->isMaximized());

                         maximize_button->setAccessibleName(maximized ? "titlebar_restore"
                                                                      : "titlebar_maximize");
                         maximize_button->setToolTip(maximized ? QObject::tr("Restore")
                                                               : QObject::tr("Maximize"));

                         maximize_button->style()->unpolish(maximize_button);
                         maximize_button->style()->polish(maximize_button);
                       });
    }

    // The main window retitles itself -- the project name is appended once a project opens --
    // and the label was a one-off copy taken before any of that happened.
    QObject::connect(window,
                     &T::windowTitleChanged,
                     titleBarWidget->windowTitle,
                     &QLabel::setText);

    return titleBarWidget;

  }

  // Register a widget a caller has added to the title bar AFTER setupFramelessWindow returned.
  // Without this the widget is inside the caption band, Windows keeps every press on it, and the
  // widget is dead to the mouse while still hovering and painting normally -- which is the
  // failure mode that is hardest to recognise as a window-frame problem rather than a broken
  // control. Re-sets the band to the bar's new height at the same time, since inserting into
  // the bar is the thing most likely to change it.
  //
  // NOTHING CALLS THIS. It was written for one call site -- the menu-bar insertion in
  // NoggitWindow.cpp, `titleBarWidget->horizontalLayout->insertWidget(2, _menuBar);`, currently
  // line 115 -- and that file was not in the same change, so the menu bar is still unregistered
  // and the defect it addresses is still live whenever the frameless frame is switched on. The
  // whole fix is one line immediately after that insertWidget:
  //
  //     Noggit::Ui::makeTitleBarWidgetClickable(this, widget, _menuBar);
  //
  // (`widget` is the QWidget declared three lines above it and handed to setupFramelessWindow
  // as titlebar_target; the qualification is redundant inside Noggit::Ui::Windows but harmless.)
  // Delete this function instead if that edit is not going to be made -- an unreferenced helper
  // is a worse record of a known bug than no helper at all.
  inline void makeTitleBarWidgetClickable(QWidget* window, QWidget* titlebar_target,
                                          QWidget* child)
  {
    FramelessWindowsManager::addIgnoreObject(window, child);
    FramelessWindowsManager::setTitleBarHeight(window, titlebar_target->sizeHint().height());
  }

}

#endif //NOGGIT_FRAMELESSWINDOW_HPP
