// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_DATABASE_DATABASESETTINGS_HPP
#define NOGGIT_DATABASE_DATABASESETTINGS_HPP

#include <noggit/database/ConnectionConfig.hpp>

#include <string>

namespace Noggit::Database
{
  // The only Qt-dependent file in this layer.
  //
  // Everything else -- the capability model, the connection, the introspector, the changeset
  // builder -- takes plain structs, which is what allows the whole layer to be built and
  // tested with Qt absent. Keep it that way: if this file's includes leak upward, that
  // property is lost and nothing below can be tested without a full Qt toolchain.
  //
  // Reads the same QSettings keys the existing UID-storage feature uses, so an existing
  // configuration keeps working:
  //
  //   project/mysql/server, project/mysql/port, project/mysql/user,
  //   project/mysql/pwd, project/mysql/db
  //
  // plus one this fork adds:
  //
  //   project/mysql/dev_schema   the single writable schema (default "noggit_dev_world")
  //
  // Note the credentials are stored in QSettings, which is the Windows registry, in plain
  // text. That is inherited from upstream rather than introduced here, but it is worth knowing
  // before pointing this at anything that matters.
  namespace DatabaseSettings
  {
    // Reads connection details from QSettings. Never returns a DEV_WRITE-capable config --
    // the access mode is chosen by the caller and validated by WorldDatabaseConnection.
    ConnectionConfig readConnectionConfig();

    // The one schema this application may write to.
    std::string readWritableSchema();

    // Whether the user enabled the database feature at all (project/mysql/enabled).
    bool isEnabled();

    // Overwrites the connection settings. Does not touch the stored password when
    // `password` is empty, so a settings dialog can save without round-tripping the secret.
    void writeConnectionConfig(ConnectionConfig const& config);
  }
}

#endif
