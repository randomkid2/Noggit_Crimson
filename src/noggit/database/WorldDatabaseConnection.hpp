// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_DATABASE_WORLDDATABASECONNECTION_HPP
#define NOGGIT_DATABASE_WORLDDATABASECONNECTION_HPP

#include <noggit/database/ConnectionConfig.hpp>

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

// Forward declared so consumers of this header do not need MySQL Connector/C++ headers.
namespace sql
{
  class Connection;
}

namespace Noggit::Database
{
  // Raised when a connection cannot be opened, a statement fails, or a write is refused.
  class DatabaseAccessError : public std::runtime_error
  {
    public:
      explicit DatabaseAccessError(std::string const& message)
        : std::runtime_error(message) {}
  };

  // All values as strings. Sufficient for schema introspection, and it avoids leaking
  // connector types through this header.
  using ResultRow = std::vector<std::string>;
  using ResultRows = std::vector<ResultRow>;

  // One held connection for its whole lifetime, with writes refused unless explicitly and
  // narrowly permitted.
  //
  // This replaces the pattern in src/mysql/mysql.cpp, which opens a fresh connection on every
  // call and issues CREATE DATABASE IF NOT EXISTS each time. That is both far too slow for
  // per-tile spawn queries and a live-database write hazard in its own right. The old path is
  // not extended, because leaving it alive leaves it reachable.
  //
  // Write protection here is the third of three independent layers, after database grants and
  // the tooling's own assertions. It assumes the other two may be misconfigured.
  class WorldDatabaseConnection
  {
    public:
      // Opens the connection immediately.
      //
      // Throws DatabaseAccessError when mode is DEV_WRITE and config.schema is not exactly
      // writable_schema. A write-capable connection to anything else is refused at
      // construction rather than at first write, so the mistake cannot be made at all.
      WorldDatabaseConnection
        ( ConnectionConfig config
        , AccessMode mode
        , std::string writable_schema
        );

      ~WorldDatabaseConnection();

      WorldDatabaseConnection(WorldDatabaseConnection const&) = delete;
      WorldDatabaseConnection& operator=(WorldDatabaseConnection const&) = delete;

      AccessMode accessMode() const { return _mode; }
      std::string const& schema() const { return _config.schema; }
      bool isConnected() const;

      // Always permitted. Reading a live database is fine; only writing is not.
      ResultRows query(std::string const& sql) const;

      // Throws unless the connection is DEV_WRITE and still pointed at the writable schema.
      void execute(std::string const& sql);

      // Server version string, for the environment Doctor.
      std::string serverVersion() const;

    private:
      ConnectionConfig _config;
      AccessMode _mode;
      std::string _writable_schema;
      std::unique_ptr<sql::Connection> _connection;
  };
}

#endif
