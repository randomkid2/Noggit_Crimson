// This file is part of Noggit3, licensed under GNU General Public License (version 3).

// The two Doctor checks that need a live connection.
//
// Separate from DoctorReport.cpp on purpose. Linking anything that calls into
// WorldDatabaseConnection pulls in MySQL Connector/C++, and the pure half of the Doctor -- the
// schema assertions, the writable-schema warning, the client data path -- has to stay buildable
// and testable with no connector and no database present. Keeping both halves in one
// translation unit would gate all of it behind the connector for the sake of two functions.

#include <noggit/database/DoctorReport.hpp>

#include <noggit/database/SchemaIntrospector.hpp>
#include <noggit/database/SchemaModel.hpp>
#include <noggit/database/WorldDatabaseConnection.hpp>

#include <cctype>
#include <string>

using namespace Noggit::Database;

namespace
{
  constexpr char const* CHECK_CONNECTION = "database connection";
  constexpr char const* CHECK_DATABASE_VERSION = "database version";

  // The 3.3.5 TDB line numbers its db_version as "TDB 335.<cache_id>".
  constexpr char const* WOTLK_VERSION_TOKEN = "335.";

  // True when `version` names the 3.3.5 line rather than some other branch.
  //
  // Matched as a token start rather than as a bare substring: "335" appears inside plenty of
  // unrelated cache ids, and a version check that accepts a master TDB because its cache id
  // happened to contain three digits is worse than no check.
  bool namesWotlkLine(std::string const& version)
  {
    for (std::size_t at (version.find(WOTLK_VERSION_TOKEN))
        ; at != std::string::npos
        ; at = version.find(WOTLK_VERSION_TOKEN, at + 1)
        )
    {
      bool const at_token_start
        ( at == 0
        || std::isdigit(static_cast<unsigned char>(version[at - 1])) == 0
        );

      if (at_token_start)
      {
        return true;
      }
    }

    return false;
  }

  std::string modeDescription(AccessMode mode)
  {
    return mode == AccessMode::DEV_WRITE ? "dev-write" : "read-only";
  }
}

CheckResult Doctor::checkConnection(WorldDatabaseConnection const& connection)
{
  if (!connection.isConnected())
  {
    return CheckResult
      { CHECK_CONNECTION
      , CheckStatus::FAIL
      , "the connection is closed"
      , "Check the host, port and credentials in the database settings, and that the MySQL"
        " server is running. Every other database check is unavailable until this one passes;"
        " the schema assertions can still be run against a captured fixture."
      };
  }

  std::string server_version;

  try
  {
    server_version = connection.serverVersion();
  }
  catch (DatabaseAccessError const& e)
  {
    // The connection is open but will not answer SELECT VERSION(), which needs no privilege at
    // all. That is a broken session rather than a missing grant, so it is a failure and not an
    // unknown.
    return CheckResult
      { CHECK_CONNECTION
      , CheckStatus::FAIL
      , std::string("open but unusable: ") + e.what()
      , "The session is open yet cannot execute the most basic statement there is. Reconnect."
        " If it recurs, suspect a connector/server version mismatch -- see docs/setup.md."
      };
  }

  std::string const where
    ( "schema `" + connection.schema() + "`, " + modeDescription(connection.accessMode())
    );

  if (connection.schema().empty())
  {
    return CheckResult
      { CHECK_CONNECTION
      , CheckStatus::WARN
      , "connected to MySQL "
        + (server_version.empty() ? std::string("(version not reported)") : server_version)
        + " with no default schema selected"
      , "Set a schema in the connection settings. Without one, nothing can be introspected and"
        " every statement has to name its schema explicitly."
      };
  }

  if (server_version.empty())
  {
    return CheckResult
      { CHECK_CONNECTION
      , CheckStatus::WARN
      , "connected (" + where + ") but the server did not report a version"
      , "Harmless for reads. An empty VERSION() usually means a proxy or router sits between"
        " Noggit and the server, which is worth knowing before diagnosing anything else."
      };
  }

  return CheckResult
    { CHECK_CONNECTION
    , CheckStatus::PASS
    , "connected to MySQL " + server_version + " (" + where + ")"
    , {}
    };
}

CheckResult Doctor::checkDatabaseVersion
  ( WorldDatabaseConnection const& connection
  , std::string const& schema
  , SchemaModel const& model
  )
{
  if (!connection.isConnected())
  {
    return CheckResult
      { CHECK_DATABASE_VERSION
      , CheckStatus::SKIPPED
      , "the connection is closed, so no version could be read"
      , "Resolve the connection failure first."
      };
  }

  if (schema.empty())
  {
    return CheckResult
      { CHECK_DATABASE_VERSION
      , CheckStatus::SKIPPED
      , "no schema was given, so there is no version table to read"
      , "Select a schema on the connection."
      };
  }

  std::string version;

  try
  {
    // Probes both version_db_world and version. The reference TDB 335.25101 database has only
    // the latter, which is why reading one name is not enough.
    version = SchemaIntrospector::readVersion(connection, schema, model);
  }
  catch (SchemaCapabilityError const& e)
  {
    return CheckResult
      { CHECK_DATABASE_VERSION
      , CheckStatus::FAIL
      , e.what()
      , "Neither version_db_world nor version exists in `" + schema + "`. A world database with"
        " no version marker is not one this tool should be editing: nothing can be verified"
        " about which core generation produced it."
      };
  }
  catch (DatabaseAccessError const& e)
  {
    // The table is there and the read failed -- a grants or transport problem. That is an
    // unknown, not a verdict on the database.
    return CheckResult
      { CHECK_DATABASE_VERSION
      , CheckStatus::SKIPPED
      , std::string("the version table exists but could not be read: ") + e.what()
      , "Confirm the connecting account has SELECT on `" + schema + "`."
      };
  }

  if (version.empty())
  {
    return CheckResult
      { CHECK_DATABASE_VERSION
      , CheckStatus::WARN
      , "the version table in `" + schema + "` exists but holds no db_version"
      , "Expected for a schema created by copying structure only -- the dev schema built by"
        " tools/dev-db/seed-dev-db.ps1 looks like this. Harmless, but it means the applied TDB"
        " version cannot be confirmed from this database."
      };
  }

  if (!namesWotlkLine(version))
  {
    return CheckResult
      { CHECK_DATABASE_VERSION
      , CheckStatus::WARN
      , "`" + schema + "` reports db_version '" + version + "', which does not name the 3.3.5"
        " line"
      , "This tool targets TrinityCore 3.3.5a only. Reads will still work through the capability"
        " layer, but no assertion in docs/schema-335.md was measured against this generation."
        " Re-run /schema-check before editing."
      };
  }

  return CheckResult
    { CHECK_DATABASE_VERSION
    , CheckStatus::PASS
    , "`" + schema + "` reports db_version '" + version + "'"
    , {}
    };
}
