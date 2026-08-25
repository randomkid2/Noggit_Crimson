// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_UI_WAIT_CURSOR_HPP
#define NOGGIT_UI_WAIT_CURSOR_HPP

#include <QtGui/QGuiApplication>

namespace Noggit
{
  namespace Ui
  {
    // Scope guard for the hourglass. Qt keeps override cursors on a STACK, so every
    // setOverrideCursor has to be paired with exactly one restoreOverrideCursor -- miss one and
    // the application is stuck showing a busy pointer until it exits, and call restore once too
    // often and a nested guard's cursor disappears early. Writing it as a destructor makes the
    // pairing structural, so an early return or a thrown exception in the middle of a long
    // operation cannot leave the stack unbalanced.
    //
    // This was already written once, in the alpha integrity report, where it wraps the two scans
    // that walk every loaded ADT. It lives here so that the other places that block the UI
    // thread for a visible length of time -- building a World from a WDT, waiting on an MPQ read
    // -- do not each grow their own copy.
    struct WaitCursor
    {
      WaitCursor() { QGuiApplication::setOverrideCursor (Qt::WaitCursor); }
      ~WaitCursor() { QGuiApplication::restoreOverrideCursor(); }

      WaitCursor (WaitCursor const&) = delete;
      WaitCursor (WaitCursor&&) = delete;
      WaitCursor& operator= (WaitCursor const&) = delete;
      WaitCursor& operator= (WaitCursor&&) = delete;
    };
  }
}

#endif // NOGGIT_UI_WAIT_CURSOR_HPP
