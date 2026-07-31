// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/DevBridge.hpp>

#include <noggit/Log.h>
#include <noggit/MapView.h>

#include <QtCore/QByteArray>
#include <QtCore/QString>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>

#include <cstdlib>
#include <string>

using namespace Noggit;

namespace
{
  // The environment variable that arms the bridge, and the only way to arm it.
  //
  // An environment variable rather than a setting, on purpose: a QSettings key persists, so a
  // build left with the bridge enabled once would keep listening on every later run without
  // anything on screen saying so. An environment variable is scoped to the process that was
  // deliberately started with it.
  constexpr char const* PORT_ENVIRONMENT_VARIABLE = "NOGGIT_BRIDGE_PORT";

  // Refuse a line long enough to be an accident or an attack rather than a command. The longest
  // real command is a screenshot path, which cannot exceed MAX_PATH by much.
  constexpr int MAX_COMMAND_LENGTH = 4096;
}

std::unique_ptr<DevBridge> DevBridge::createIfEnabled(MapView* view)
{
  char const* const port_text = std::getenv(PORT_ENVIRONMENT_VARIABLE);

  if (!port_text || !*port_text)
  {
    return nullptr;
  }

  bool parsed = false;
  unsigned int const port (QString(port_text).toUInt(&parsed));

  // Port 0 would make QTcpServer pick an arbitrary free port, which is useless to a caller that
  // has to know where to connect, so it is refused rather than silently honoured.
  if (!parsed || port == 0 || port > 65535)
  {
    LogError << "Dev bridge: " << PORT_ENVIRONMENT_VARIABLE << " is \"" << port_text
             << "\", which is not a usable port. The bridge is not listening." << std::endl;
    return nullptr;
  }

  std::unique_ptr<DevBridge> bridge
    (new DevBridge(view, static_cast<unsigned short>(port)));

  if (!bridge->_server->isListening())
  {
    return nullptr;
  }

  return bridge;
}

DevBridge::DevBridge(MapView* view, unsigned short port)
  : _view(view)
  , _server(new QTcpServer())
{
  // LocalHost, never Any. This is the difference between a development aid and an open command
  // channel on whatever network the machine is attached to.
  if (!_server->listen(QHostAddress::LocalHost, port))
  {
    LogError << "Dev bridge: could not listen on 127.0.0.1:" << port << " - "
             << _server->errorString().toStdString() << std::endl;
    return;
  }

  QObject::connect(_server, &QTcpServer::newConnection, _server, [this] { onNewConnection(); });

  Log << "Dev bridge listening on 127.0.0.1:" << port
      << ". This build accepts remote commands; it is not a release configuration." << std::endl;
}

DevBridge::~DevBridge()
{
  // The server owns the sockets it parented, so closing it tears the connections down with it.
  _server->close();
  delete _server;
}

void DevBridge::onNewConnection()
{
  while (QTcpSocket* socket = _server->nextPendingConnection())
  {
    // Parented to the server so a client that disappears without a clean close is still cleaned
    // up, and so ~DevBridge does not have to track them.
    socket->setParent(_server);

    QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket] { onReadyRead(socket); });
    QObject::connect(socket, &QTcpSocket::disconnected, socket, &QTcpSocket::deleteLater);
  }
}

void DevBridge::onReadyRead(QTcpSocket* socket)
{
  while (socket->canReadLine())
  {
    QByteArray const raw (socket->readLine(MAX_COMMAND_LENGTH));
    std::string line (QString::fromUtf8(raw).trimmed().toStdString());

    if (line.empty())
    {
      continue;
    }

    std::string reply;

    // Nothing may escape into the Qt event loop. A command that threw would unwind through
    // QTcpSocket's signal emission and terminate the editor, which would make the bridge a
    // liability rather than a tool -- and it would do so while a human was watching the window
    // rather than the socket.
    try
    {
      reply = _view->handleBridgeCommand(line);
    }
    catch (std::exception const& e)
    {
      reply = std::string("ERR unhandled exception: ") + e.what();
    }
    catch (...)
    {
      reply = "ERR unhandled exception";
    }

    reply += '\n';
    socket->write(reply.data(), static_cast<qint64>(reply.size()));
    socket->flush();
  }
}
