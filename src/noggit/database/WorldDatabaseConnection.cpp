// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/database/WorldDatabaseConnection.hpp>

#include <cppconn/driver.h>
#include <cppconn/exception.h>
#include <cppconn/metadata.h>
#include <cppconn/resultset.h>
#include <cppconn/statement.h>

#include <sstream>
#include <string>
#include <utility>
#include <vector>

using namespace Noggit::Database;

namespace
{
  // Connector/C++ wants a URI, not a bare host. Building the string rather than passing a
  // host/port pair also sidesteps a known crash when release connector binaries are used from
  // a debug build (MySQL bug 91238).
  std::string connectionUri(ConnectionConfig const& config)
  {
    return "tcp://" + config.host + ":" + std::to_string(config.port);
  }
}

WorldDatabaseConnection::WorldDatabaseConnection
  ( ConnectionConfig config
  , AccessMode mode
  , std::string writable_schema
  )
  : _config(std::move(config))
  , _mode(mode)
  , _writable_schema(std::move(writable_schema))
{
  if (_mode == AccessMode::DEV_WRITE)
  {
    if (_writable_schema.empty())
    {
      throw DatabaseAccessError
        ("Refusing a write-capable connection: no writable schema was configured.");
    }

    if (_config.schema != _writable_schema)
    {
      throw DatabaseAccessError
        ("Refusing a write-capable connection to schema '" + _config.schema
         + "'. The only writable schema is '" + _writable_schema
         + "'. Emit a reviewable .sql changeset instead of writing to a live database.");
    }
  }

  try
  {
    sql::Driver* driver (get_driver_instance());

    _connection.reset
      (driver->connect(connectionUri(_config), _config.user, _config.password));

    if (!_config.schema.empty())
    {
      _connection->setSchema(_config.schema);
    }

    // Ask the server to refuse writes too, where it supports doing so. Belt as well as braces:
    // if this connection is ever handed to code that forgets the mode check, the session is
    // still read-only. Not every server or version honours it, so this supplements the checks
    // in execute() rather than replacing them.
    if (_mode == AccessMode::READ_ONLY)
    {
      try
      {
        std::unique_ptr<sql::Statement> statement (_connection->createStatement());
        statement->execute("SET SESSION TRANSACTION READ ONLY");
      }
      catch (sql::SQLException const&)
      {
        // Older servers reject this. The mode check in execute() remains authoritative.
      }
    }
  }
  catch (sql::SQLException const& e)
  {
    throw DatabaseAccessError
      ("Could not connect to " + connectionUri(_config) + " as '" + _config.user
       + "': " + e.what() + " (MySQL error " + std::to_string(e.getErrorCode()) + ")");
  }
}

// Defined here, not in the header, because sql::Connection is incomplete there.
WorldDatabaseConnection::~WorldDatabaseConnection() = default;

bool WorldDatabaseConnection::isConnected() const
{
  return _connection && !_connection->isClosed();
}

ResultRows WorldDatabaseConnection::query(std::string const& sql) const
{
  if (!isConnected())
  {
    throw DatabaseAccessError("Query attempted on a closed connection.");
  }

  try
  {
    std::unique_ptr<sql::Statement> statement (_connection->createStatement());
    std::unique_ptr<sql::ResultSet> result (statement->executeQuery(sql));

    unsigned int const column_count (result->getMetaData()->getColumnCount());

    ResultRows rows;

    while (result->next())
    {
      ResultRow row;
      row.reserve(column_count);

      for (unsigned int i = 1; i <= column_count; ++i)
      {
        // isNull must be checked first: getString on a NULL column yields an empty string,
        // which is a different thing and would erase the distinction.
        row.push_back(result->isNull(i) ? std::string() : std::string(result->getString(i)));
      }

      rows.push_back(std::move(row));
    }

    return rows;
  }
  catch (sql::SQLException const& e)
  {
    throw DatabaseAccessError
      (std::string("Query failed: ") + e.what()
       + " (MySQL error " + std::to_string(e.getErrorCode()) + ")");
  }
}

std::size_t WorldDatabaseConnection::executeScript(std::string const& sql)
{
  // Split first, then let execute() enforce the write guard on each statement. Doing it that way
  // round means the guard cannot be bypassed by this entry point -- there is exactly one place a
  // statement reaches the server.
  std::vector<std::string> statements;
  std::string current;

  std::istringstream lines (sql);
  std::string line;

  while (std::getline(lines, line))
  {
    // Strip a trailing carriage return so a CRLF file does not leave one inside a statement.
    if (!line.empty() && line.back() == '\r')
    {
      line.pop_back();
    }

    // Whole-line SQL comments only. Deliberately not a general comment parser: these files are
    // emitted by ChangesetBuilder, which never puts `--` anywhere but the start of a line, and a
    // half-correct parser would be worse than a narrow one that matches the format exactly.
    std::string::size_type const first (line.find_first_not_of(" \t"));

    if (first == std::string::npos || line.compare(first, 2, "--") == 0)
    {
      continue;
    }

    current += line;
    current += '\n';

    // Statement boundary. Safe for this format because no emitted literal contains a semicolon:
    // every value is a number, and the only strings are identifiers in backticks.
    if (line.find_last_not_of(" \t") != std::string::npos
     && line[line.find_last_not_of(" \t")] == ';')
    {
      statements.push_back(current);
      current.clear();
    }
  }

  if (current.find_first_not_of(" \t\n") != std::string::npos)
  {
    statements.push_back(current);
  }

  for (auto const& statement : statements)
  {
    execute(statement);
  }

  return statements.size();
}

void WorldDatabaseConnection::execute(std::string const& sql)
{
  if (_mode != AccessMode::DEV_WRITE)
  {
    throw DatabaseAccessError
      ("Refusing to execute a statement on a read-only connection to schema '"
       + _config.schema + "'. Writes are only permitted against '" + _writable_schema + "'.");
  }

  // Re-checked rather than trusted from construction: setSchema could have been called since,
  // and this is the last line of defence before a statement reaches a server.
  if (_config.schema != _writable_schema)
  {
    throw DatabaseAccessError
      ("Refusing to execute a statement: connection schema '" + _config.schema
       + "' is not the writable schema '" + _writable_schema + "'.");
  }

  if (!isConnected())
  {
    throw DatabaseAccessError("Statement attempted on a closed connection.");
  }

  try
  {
    std::unique_ptr<sql::Statement> statement (_connection->createStatement());
    statement->execute(sql);
  }
  catch (sql::SQLException const& e)
  {
    throw DatabaseAccessError
      (std::string("Statement failed: ") + e.what()
       + " (MySQL error " + std::to_string(e.getErrorCode()) + ")");
  }
}

std::string WorldDatabaseConnection::serverVersion() const
{
  ResultRows const rows (query("SELECT VERSION()"));

  if (rows.empty() || rows.front().empty())
  {
    return {};
  }

  return rows.front().front();
}
