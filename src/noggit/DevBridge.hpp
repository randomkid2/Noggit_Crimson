// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_DEVBRIDGE_HPP
#define NOGGIT_DEVBRIDGE_HPP

#include <memory>

class MapView;
class QTcpServer;
class QTcpSocket;

namespace Noggit
{
  // A loopback command socket for driving the editor from a script.
  //
  // This exists because the interesting half of this fork cannot be tested any other way. The
  // logic layer has 220 test cases, but "do the spawns appear in the right place, at the right
  // size, with the right texture" is a question about pixels in an OpenGL window, and answering it
  // has meant a human opening a map and looking. Every rendering defect found in this branch so
  // far -- the 300-yard cull, the context crash on the second load, the black skins -- cost a
  // round trip through a person that way.
  //
  // > [!warning] This is a remote control surface. It is gated twice, deliberately.
  // > 1. Compiled only when CMake is configured with `-DNOGGIT_DEV_BRIDGE=ON`, which defaults to
  // >    OFF. A released build does not contain this code at all.
  // > 2. Even then it listens only when the environment variable `NOGGIT_BRIDGE_PORT` names a
  // >    port, and it binds `QHostAddress::LocalHost` -- never a routable interface.
  // >
  // > Both gates matter for a fork intended for public release. A build shipping an
  // > always-listening command socket would let anything running on the machine drive the editor,
  // > and what the editor can do includes writing files.
  //
  // The protocol is one line of text in, one line of text out, so it can be driven with nothing
  // but a TCP client. Replies begin with `OK` or `ERR`. See `docs/dev-bridge.md`.
  //
  // This class is transport only: it accepts connections, splits lines, and hands each one to
  // MapView::handleBridgeCommand. The commands themselves live there because they need the
  // camera, the world and the spawn cache, all of which are MapView's private business -- and
  // making this class a friend to reach them would trade a clean seam for a shortcut.
  //
  // Everything runs on the GUI thread. QTcpServer delivers signals on the thread its object lives
  // on, and MapView constructs this, so handlers may touch the GL context directly.
  class DevBridge
  {
    public:
      // Returns nullptr when the bridge is not enabled, which is the normal case. The caller
      // stores the result unconditionally and needs no #ifdef of its own.
      static std::unique_ptr<DevBridge> createIfEnabled(MapView* view);

      ~DevBridge();

      DevBridge(DevBridge const&) = delete;
      DevBridge& operator=(DevBridge const&) = delete;

    private:
      DevBridge(MapView* view, unsigned short port);

      void onNewConnection();
      void onReadyRead(QTcpSocket* socket);

      MapView* _view;
      QTcpServer* _server;
  };
}

#endif
