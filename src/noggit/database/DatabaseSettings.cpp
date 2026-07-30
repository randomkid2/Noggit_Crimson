// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/database/DatabaseSettings.hpp>

#include <QtCore/QSettings>
#include <QtCore/QString>
#include <QtCore/QVariant>

using namespace Noggit::Database;

namespace
{
  // Matches the keys the existing UID-storage feature already uses, so an existing
  // configuration keeps working unchanged.
  constexpr char const* KEY_ENABLED = "project/mysql/enabled";
  constexpr char const* KEY_SERVER = "project/mysql/server";
  constexpr char const* KEY_PORT = "project/mysql/port";
  constexpr char const* KEY_USER = "project/mysql/user";
  constexpr char const* KEY_PASSWORD = "project/mysql/pwd";
  constexpr char const* KEY_SCHEMA = "project/mysql/db";

  // Added by this fork.
  constexpr char const* KEY_DEV_SCHEMA = "project/mysql/dev_schema";

  constexpr char const* DEFAULT_HOST = "127.0.0.1";
  constexpr unsigned int DEFAULT_PORT = 3306;
  constexpr char const* DEFAULT_DEV_SCHEMA = "noggit_dev_world";

  std::string toStdString(QVariant const& value, std::string const& fallback)
  {
    QString const text (value.toString());
    return text.isEmpty() ? fallback : text.toStdString();
  }
}

ConnectionConfig DatabaseSettings::readConnectionConfig()
{
  QSettings settings;

  ConnectionConfig config;
  config.host = toStdString(settings.value(KEY_SERVER), DEFAULT_HOST);
  config.user = toStdString(settings.value(KEY_USER), std::string());
  config.password = toStdString(settings.value(KEY_PASSWORD), std::string());
  config.schema = toStdString(settings.value(KEY_SCHEMA), std::string());

  // Upstream stores the port as a string in some versions and an int in others, and its
  // SettingsPanel defaults several of these keys to "127.0.0.1" through what looks like a
  // copy-paste slip. Convert defensively and fall back rather than trusting the stored type.
  bool port_ok (false);
  unsigned int const stored_port
    (settings.value(KEY_PORT, DEFAULT_PORT).toString().toUInt(&port_ok));

  config.port = (port_ok && stored_port > 0 && stored_port <= 65535)
              ? stored_port
              : DEFAULT_PORT;

  return config;
}

std::string DatabaseSettings::readWritableSchema()
{
  QSettings settings;
  return toStdString(settings.value(KEY_DEV_SCHEMA), DEFAULT_DEV_SCHEMA);
}

bool DatabaseSettings::isEnabled()
{
  QSettings settings;
  return settings.value(KEY_ENABLED, false).toBool();
}

void DatabaseSettings::writeConnectionConfig(ConnectionConfig const& config)
{
  QSettings settings;

  settings.setValue(KEY_SERVER, QString::fromStdString(config.host));
  settings.setValue(KEY_PORT, config.port);
  settings.setValue(KEY_USER, QString::fromStdString(config.user));
  settings.setValue(KEY_SCHEMA, QString::fromStdString(config.schema));

  // An empty password means "leave the stored one alone", so a settings dialog can save
  // without having to read the secret back out and write it again.
  if (!config.password.empty())
  {
    settings.setValue(KEY_PASSWORD, QString::fromStdString(config.password));
  }
}
