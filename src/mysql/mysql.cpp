// This file is part of Noggit3, licensed under GNU General Public License (version 3).

// Upstream's UID-storage seam, re-pointed at this fork's write policy.
//
// What it used to do, on EVERY call, against whatever host project/mysql/server named:
//
//     CREATE DATABASE IF NOT EXISTS <project/mysql/db>
//     CREATE TABLE IF NOT EXISTS `UIDs` (...)
//
// That is the project's central safety claim -- "never write to a live database" -- falsified by
// the application itself. Point Noggit at a production world DB with the feature enabled and it
// would create a schema and a table there before doing anything else.
//
// Two changes fix it, in this order:
//
//   1. CREATE DATABASE is gone entirely. The schema must already exist; if it does not, the
//      connection fails and is reported, rather than being conjured into existence.
//   2. Nothing here opens a connection unless the configured schema (project/mysql/db) is
//      exactly the nominated writable schema (project/mysql/dev_schema). The remaining
//      CREATE TABLE therefore cannot reach any other schema, and neither can the INSERT or the
//      UPDATE -- and neither can the two SELECTs, which are restricted for uniformity rather
//      than from need.
//
// The policy is not reimplemented here. The schema comes from Database::DatabaseSettings and
// enforcement from Database::WorldDatabaseConnection's DEV_WRITE guard, which is the same code
// path the fork's own database layer is bound by. A second, parallel policy would be a second
// thing to get wrong. The explicit check below the connection call is redundant against that
// guard on purpose: it costs nothing, it fires before a socket is opened, and it produces a
// message about UID storage rather than about changesets.
//
// Still true, and deliberately not restructured: a fresh connection is opened per call. That is
// upstream's design and it is tolerable here only because these five functions run a handful of
// times per session, at map load and map save. It is why the fork's spawn queries use
// WorldDatabaseConnection directly instead of extending this seam -- per-tile queries through a
// connect-per-call path would be unusable.

#include <mysql/mysql.h>

#include <noggit/Log.h>
#include <noggit/database/ConnectionConfig.hpp>
#include <noggit/database/DatabaseSettings.hpp>
#include <noggit/database/WorldDatabaseConnection.hpp>

#include <QtCore/Qt>
// QMessageBox is used throughout testConnection but was never included by upstream; it lives in
// QtWidgets, not QtCore. std::stringstream and std::unique_ptr were likewise relied on
// transitively. Note that the connector's own headers are no longer needed at all: every
// statement now goes through WorldDatabaseConnection, whose header forward-declares
// sql::Connection. That also retires upstream's `#include <driver.h>`, which could never
// resolve against a stock Connector/C++ layout -- driver.h lives in cppconn/, not beside it.
#include <QtWidgets/QMessageBox>

#include <exception>
#include <memory>
#include <sstream>
#include <string>

namespace
{
  using Noggit::Database::AccessMode;
  using Noggit::Database::ConnectionConfig;
  using Noggit::Database::DatabaseAccessError;
  using Noggit::Database::WorldDatabaseConnection;

  namespace DatabaseSettings = Noggit::Database::DatabaseSettings;

  // Unchanged from upstream's src/sql. The only DDL that survives, and it is reachable only
  // after the schema has been established as the sanctioned one.
  constexpr char const* UIDS_TABLE_DDL
    = "CREATE TABLE IF NOT EXISTS `UIDs` ("
      "`_map_id` int(11) NOT NULL,"
      "`UID` int(11) NOT NULL,"
      "PRIMARY KEY(`_map_id`)"
      ") ENGINE = InnoDB DEFAULT CHARSET = latin1;";

  // Empty when UID storage is permitted; otherwise the reason it is not, phrased for a user.
  std::string uidStorageRefusalReason
    (ConnectionConfig const& config, std::string const& writable_schema)
  {
    if (writable_schema.empty())
    {
      return "no writable schema is configured (project/mysql/dev_schema is empty)";
    }

    if (config.schema.empty())
    {
      return "no schema is configured (project/mysql/db is empty)";
    }

    if (config.schema != writable_schema)
    {
      return "the configured schema '" + config.schema + "' is not the writable schema '"
           + writable_schema + "'";
    }

    return {};
  }

  // A write-capable connection to the writable schema, or nullptr with the reason logged.
  //
  // Returning nullptr rather than throwing keeps every caller's existing shape: upstream's
  // callers already test for it, and a UID bookkeeping failure must not abort a map load.
  std::unique_ptr<WorldDatabaseConnection> connect()
  {
    ConnectionConfig const config (DatabaseSettings::readConnectionConfig());
    std::string const writable_schema (DatabaseSettings::readWritableSchema());

    std::string const refusal (uidStorageRefusalReason(config, writable_schema));

    if (!refusal.empty())
    {
      LogError << "MySQL UID storage refused: " << refusal
               << ". Nothing was sent to the server. UID storage may only touch the schema "
                  "nominated as writable; set project/mysql/db to it, or disable the feature in "
                  "Noggit -> Settings -> MySQL." << std::endl;
      return nullptr;
    }

    try
    {
      // DEV_WRITE re-checks schema == writable_schema in the constructor and again in every
      // execute(), which is the guard that actually stands between this seam and a live server.
      auto connection
        (std::make_unique<WorldDatabaseConnection>(config, AccessMode::DEV_WRITE, writable_schema));

      // The schema is never created. If it does not exist the constructor above has already
      // thrown, so reaching this line means the user made the schema deliberately.
      connection->execute(UIDS_TABLE_DDL);

      return connection;
    }
    catch (DatabaseAccessError const& e)
    {
      LogError << "MySQL UID storage unavailable: " << e.what() << std::endl;
      return nullptr;
    }
  }
}

namespace mysql
{
  bool testConnection(bool report_only_err)
  {
    ConnectionConfig const config (DatabaseSettings::readConnectionConfig());
    std::string const writable_schema (DatabaseSettings::readWritableSchema());

    QMessageBox prompt;
    prompt.setWindowFlag(Qt::WindowStaysOnTopHint);

    try
    {
      // READ_ONLY on purpose. This is a reachability test, and a test button must not be the
      // thing that writes to a server. It also means the test still succeeds -- correctly --
      // against a live world database that UID storage itself will refuse.
      WorldDatabaseConnection connection (config, AccessMode::READ_ONLY, writable_schema);

      std::string const refusal (uidStorageRefusalReason(config, writable_schema));

      std::stringstream promptText;
      promptText << "Server version: " << connection.serverVersion() << "\n";

      if (refusal.empty())
      {
        promptText << "\nUID storage is enabled and will use schema '" << config.schema
                   << "', which is the schema nominated as writable.";
      }
      else
      {
        promptText << "\nUID storage is REFUSED because " << refusal
                   << ".\nThe connection works and reads are fine; nothing will be written."
                   << "\nNoggit will not create a schema or a table on this server.";
      }

      prompt.setIcon(QMessageBox::Information);
      prompt.setText("Successfully connected to the MySQL server (read-only).");
      prompt.setInformativeText(promptText.str().c_str());
      prompt.setWindowTitle("Success");

      if (!report_only_err)
        prompt.exec();

      return true;
    }
    catch (DatabaseAccessError const& e)
    {
      prompt.setIcon(QMessageBox::Warning);
      prompt.setText("Failed to reach the MySQL database, check your settings. \nIf you did not "
                     "intend to use this feature, disable it in Noggit->settings->MySQL");
      prompt.setWindowTitle("Noggit Database Error");

      std::stringstream promptText;
      promptText << "\n# ERR: " << e.what();

      prompt.setInformativeText(promptText.str().c_str());
      prompt.exec();

      return false;
    }
  }

  bool hasMaxUIDStoredDB(std::size_t mapID)
  {
    auto connection (connect());

    if (connection == nullptr)
      return false;

    try
    {
      // Concatenated rather than bound, because WorldDatabaseConnection deliberately exposes no
      // prepared-statement API. Injection-free regardless: mapID is std::size_t, so to_string
      // can only ever produce digits.
      return !connection->query
        ("SELECT `UID` FROM `UIDs` WHERE `_map_id`=" + std::to_string(mapID)).empty();
    }
    catch (DatabaseAccessError const& e)
    {
      LogError << "MySQL UID lookup failed: " << e.what() << std::endl;
      return false;
    }
  }

  std::uint32_t getGUIDFromDB(std::size_t mapID)
  {
    auto connection (connect());

    if (connection == nullptr)
      return 0;

    try
    {
      auto const rows
        (connection->query("SELECT `UID` FROM `UIDs` WHERE `_map_id`=" + std::to_string(mapID)));

      if (rows.empty() || rows.back().empty())
        return 0;

      // Last row, matching upstream, which looped to the end of the result set. `_map_id` is the
      // primary key so there is at most one row anyway.
      return static_cast<std::uint32_t>(std::stoul(rows.back().front()));
    }
    catch (std::exception const& e)
    {
      // Also catches the stoul failure on a malformed or empty value, which must not propagate
      // into map loading.
      LogError << "MySQL UID read failed: " << e.what() << std::endl;
      return 0;
    }
  }

  void insertUIDinDB(std::size_t mapID, std::uint32_t NewUID)
  {
    auto connection (connect());

    if (connection == nullptr)
      return;

    try
    {
      connection->execute
        ("INSERT INTO `UIDs` SET `_map_id`=" + std::to_string(mapID)
         + ", `UID`=" + std::to_string(NewUID));
    }
    catch (DatabaseAccessError const& e)
    {
      LogError << "MySQL UID insert failed: " << e.what() << std::endl;
    }
  }

  void updateUIDinDB (std::size_t mapID, std::uint32_t NewUID)
  {
    auto connection (connect());

    if (connection == nullptr)
      return;

    try
    {
      connection->execute
        ("UPDATE `UIDs` SET `UID`=" + std::to_string(NewUID)
         + " WHERE `_map_id`=" + std::to_string(mapID));
    }
    catch (DatabaseAccessError const& e)
    {
      LogError << "MySQL UID update failed: " << e.what() << std::endl;
    }
  }
}
