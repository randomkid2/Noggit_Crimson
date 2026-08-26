// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/ui/WindowChrome.hpp>

#include <noggit/ui/DesignTokens.hpp>

#include <QtCore/QEvent>
#include <QtCore/QObject>
#include <QtGui/QColor>
#include <QtWidgets/QApplication>
#include <QtWidgets/QWidget>

#ifdef Q_OS_WIN
#include <dwmapi.h>
#include <windows.h>
#endif

namespace Noggit::Ui::WindowChrome
{
  namespace
  {
#ifdef Q_OS_WIN
    // These four are documented DWM window attributes, but they are only present in the SDK's
    // dwmapi.h from fairly recent Windows Kits, and this project is built against whichever kit
    // the developer happens to have. Spelling the ordinals out here means the file compiles on an
    // older kit and still does the right thing at run time, where the only thing that matters is
    // the DWM version actually running.
    //
    // DwmSetWindowAttribute returns a failure HRESULT for an attribute the running DWM does not
    // know, which is exactly the "older Windows" case, so an ignored return value IS the fallback
    // path. Nothing here is load-bearing enough to report.
    constexpr DWORD ATTRIBUTE_DARK_MODE = 20;          // Windows 10 build 18985+, and Windows 11
    constexpr DWORD ATTRIBUTE_DARK_MODE_LEGACY = 19;   // Windows 10 builds 17763 - 18984
    constexpr DWORD ATTRIBUTE_BORDER_COLOR = 34;       // Windows 11 build 22000+
    constexpr DWORD ATTRIBUTE_CAPTION_COLOR = 35;      // Windows 11 build 22000+
    constexpr DWORD ATTRIBUTE_TEXT_COLOR = 36;         // Windows 11 build 22000+

    // A COLORREF is 0x00BBGGRR -- byte-reversed against the 0xRRGGBB that every hex colour in this
    // project is written in, which is a good way to ship a blue title bar on a warm brown palette.
    COLORREF colorRef (QColor const& color)
    {
      return RGB (color.red(), color.green(), color.blue());
    }

    void applyToHandle (HWND handle)
    {
      if (!handle)
      {
        return;
      }

      // Asked for first and unconditionally, because on Windows 10 it is the whole effect, and on
      // Windows 11 it still decides how the system draws the minimise, maximise and close glyphs.
      // Setting a dark caption colour without it leaves those glyphs dark-on-dark.
      BOOL const dark (TRUE);

      if (FAILED (::DwmSetWindowAttribute (handle, ATTRIBUTE_DARK_MODE, &dark, sizeof (dark))))
      {
        ::DwmSetWindowAttribute (handle, ATTRIBUTE_DARK_MODE_LEGACY, &dark, sizeof (dark));
      }

      // bg.panel, and that is measured rather than chosen: a screen grab of the launcher samples
      // #292621 immediately below the title bar, so this is the one value that makes the seam
      // vanish instead of merely darkening it. QPalette::Window is the same token, so a caption in
      // any other shade would disagree with the window it belongs to.
      COLORREF const caption (colorRef (Design::color (Design::BG_PANEL)));
      ::DwmSetWindowAttribute (handle, ATTRIBUTE_CAPTION_COLOR, &caption, sizeof (caption));

      // text.hi on bg.panel is 13.24:1. The caption is the window's own title, so it takes the
      // high rank rather than body text -- it is the one string on screen with no competition.
      COLORREF const text (colorRef (Design::color (Design::TEXT_HI)));
      ::DwmSetWindowAttribute (handle, ATTRIBUTE_TEXT_COLOR, &text, sizeof (text));

      // stroke.soft. The border separates the window from whatever is behind it, which is usually
      // a desktop this palette knows nothing about, so it is deliberately the quietest stroke:
      // enough to bound the window against a dark background without drawing a bright rectangle
      // around it on a light one. Against the caption it measures 1.36:1 -- a seam, not an edge.
      COLORREF const border (colorRef (Design::color (Design::STROKE_SOFT)));
      ::DwmSetWindowAttribute (handle, ATTRIBUTE_BORDER_COLOR, &border, sizeof (border));
    }
#endif

    class ChromeFilter : public QObject
    {
    public:
      explicit ChromeFilter (QObject* parent)
        : QObject (parent)
      {
      }

    protected:
      bool eventFilter (QObject* watched, QEvent* event) override
      {
        // Show rather than Create: a widget acquires its native handle lazily, and on the Create
        // event winId() can still be zero. By the time it is shown it always has one.
        if (event->type() == QEvent::Show)
        {
          if (auto* const widget = qobject_cast<QWidget*> (watched))
          {
            if (widget->isWindow())
            {
              applyTo (widget);
            }
          }
        }

        // ALWAYS false. This filter observes; it must never consume a show event, or the widget it
        // was meant to decorate would not appear at all.
        return false;
      }
    };
  }

  void applyTo (QWidget* window)
  {
    if (!window)
    {
      return;
    }

#ifdef Q_OS_WIN
    // Forces the native handle into existence if the widget does not have one yet. Costless when
    // it already does, which is the common case coming from the show filter.
    applyToHandle (reinterpret_cast<HWND> (window->winId()));
#else
    // Every other platform keeps its own title bar. Named rather than left as an unused parameter
    // so the intent is not mistaken for an oversight.
    Q_UNUSED (window);
#endif
  }

  void install (QApplication& application)
  {
    // Parented to the application, so it lives exactly as long as the event loop it filters and
    // nothing has to remember to delete it.
    application.installEventFilter (new ChromeFilter (&application));
  }
}
