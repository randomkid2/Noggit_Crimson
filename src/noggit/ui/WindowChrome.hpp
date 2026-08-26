// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_UI_WINDOWCHROME_HPP
#define NOGGIT_UI_WINDOWCHROME_HPP

class QApplication;
class QWidget;

namespace Noggit::Ui::WindowChrome
{
  // Paints the NON-CLIENT area -- the title bar, its caption text and the window border -- in the
  // application's own palette, on the platforms that allow it.
  //
  // Why this exists: everything else in this application is styled by Qt, and Qt does not own the
  // title bar. The window manager draws it. So no theme pass, however thorough, can reach it, and
  // the result was a #F3F3F3 title bar sitting directly on top of #292621 content -- both sampled off
  // a real screen grab of the launcher, a 13.58:1 step at the seam. It was the single brightest
  // surface anywhere in a deliberately dark application, and the first thing anyone sees.
  //
  // On Windows 11 (build 22000 and up) DWM lets an application state the caption colour, the
  // caption text colour and the border colour outright, so the title bar becomes bg.panel and the
  // seam disappears completely. On Windows 10 1809 and up there is no colour control, only a
  // dark-mode flag, which still gets a dark title bar -- just not this exact dark. Older Windows,
  // and every non-Windows platform, get nothing and keep the system title bar; that is a cosmetic
  // shortfall, never a failure, and nothing here reports an error for it.
  //
  // A window must already be native for this to apply, because it works on an HWND. applyTo()
  // handles that itself. Prefer install(), which arms every window the application will ever
  // create, including the dialogs built deep inside tools that no startup path can see.
  void applyTo (QWidget* window);

  // Installs an application-wide filter so that every top-level widget is dressed the first time
  // it is shown. Call once, after the QApplication exists and after the palette is set.
  //
  // A filter rather than a call at each window's construction: this application creates top-level
  // windows in dozens of places -- MapView's warnings, the UID fix window, Help, ModelImport, the
  // texture picker, every tool's pop-out -- and any list of them would be out of date the first
  // time somebody added one. Show is the right hook because a widget has no HWND until then.
  void install (QApplication& application);
}

#endif // NOGGIT_UI_WINDOWCHROME_HPP
