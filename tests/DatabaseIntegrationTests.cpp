// This file is part of Noggit3, licensed under GNU General Public License (version 3).

// Integration tests against a real MySQL server.
//
// These skip rather than fail when no database is reachable, so the suite still runs on a
// machine with only a compiler. What they cannot do is pass silently when a database IS
// present and wrong -- that distinction is the whole point.
//
// Credentials come from the environment. Dot-source tools/dev-db/.generated/set-dev-env.ps1
// first; nothing here embeds a password.

#include <catch2/catch_test_macros.hpp>

#include <FixtureLoader.hpp>
#include <noggit/database/SchemaIntrospector.hpp>
#include <noggit/database/SchemaModel.hpp>
#include <noggit/database/WorldDatabaseConnection.hpp>

#include <cstdlib>
#include <memory>
#include <string>

using namespace Noggit::Database;
using Noggit::Tests::fixturePath;
using Noggit::Tests::loadSchemaFixture;

namespace
{
  constexpr char const* WRITABLE_SCHEMA = "noggit_dev_world";
  constexpr char const* READ_SCHEMA = "world";
  constexpr char const* GOLDEN_FIXTURE = "schema-tdb335-25101.tsv";

  // MSVC deprecates std::getenv. _dupenv_s allocates, so the result is freed immediately after
  // copying rather than leaked once per lookup.
  std::string envValue(char const* name)
  {
#ifdef _MSC_VER
    char* buffer (nullptr);
    std::size_t size (0);

    if (_dupenv_s(&buffer, &size, name) != 0 || !buffer)
    {
      return {};
    }

    std::string const value (buffer);
    std::free(buffer);
    return value;
#else
    char const* value (std::getenv(name));
    return value ? std::string(value) : std::string();
#endif
  }

  std::string envOr(char const* name, std::string const& fallback)
  {
    std::string const value (envValue(name));
    return value.empty() ? fallback : value;
  }

  bool haveEnv(char const* name)
  {
    return !envValue(name).empty();
  }

  ConnectionConfig readOnlyConfig()
  {
    ConnectionConfig config;
    config.host = envOr("NOGGIT_TEST_DB_HOST", "127.0.0.1");
    config.port = static_cast<unsigned int>(std::stoul(envOr("NOGGIT_TEST_DB_PORT", "3306")));
    config.user = envOr("NOGGIT_TEST_RO_USER", "noggit_ro");
    config.password = envOr("NOGGIT_RO_DB_PWD", "");
    config.schema = envOr("NOGGIT_TEST_READ_SCHEMA", READ_SCHEMA);
    return config;
  }

  ConnectionConfig devWriteConfig()
  {
    ConnectionConfig config;
    config.host = envOr("NOGGIT_TEST_DB_HOST", "127.0.0.1");
    config.port = static_cast<unsigned int>(std::stoul(envOr("NOGGIT_TEST_DB_PORT", "3306")));
    config.user = envOr("NOGGIT_TEST_RW_USER", "noggit_rw");
    config.password = envOr("NOGGIT_DEV_DB_PWD", "");
    config.schema = WRITABLE_SCHEMA;
    return config;
  }

  std::unique_ptr<WorldDatabaseConnection> tryConnect
    (ConnectionConfig const& config, AccessMode mode)
  {
    try
    {
      return std::make_unique<WorldDatabaseConnection>(config, mode, WRITABLE_SCHEMA);
    }
    catch (DatabaseAccessError const&)
    {
      return nullptr;
    }
  }
}

TEST_CASE("write refusal does not need a server to be enforced", "[db][safety]")
{
  // No connection is attempted: the refusal happens before that, so this runs anywhere.
  ConnectionConfig config;
  config.schema = "world";
  config.user = "irrelevant";

  CHECK_THROWS_AS
    ( WorldDatabaseConnection(config, AccessMode::DEV_WRITE, WRITABLE_SCHEMA)
    , DatabaseAccessError
    );

  config.schema = "haste_world";
  CHECK_THROWS_AS
    ( WorldDatabaseConnection(config, AccessMode::DEV_WRITE, WRITABLE_SCHEMA)
    , DatabaseAccessError
    );

  // An empty writable schema must not be treated as "anything goes".
  config.schema = "";
  CHECK_THROWS_AS
    ( WorldDatabaseConnection(config, AccessMode::DEV_WRITE, "")
    , DatabaseAccessError
    );
}

TEST_CASE("introspecting a live schema reproduces the captured fixture", "[db][integration]")
{
  if (!haveEnv("NOGGIT_RO_DB_PWD"))
  {
    SKIP("NOGGIT_RO_DB_PWD not set; skipping live database tests.");
  }

  std::unique_ptr<WorldDatabaseConnection> const connection
    (tryConnect(readOnlyConfig(), AccessMode::READ_ONLY));

  if (!connection)
  {
    SKIP("Could not reach the reference database; skipping.");
  }

  REQUIRE(connection->isConnected());

  SchemaModel const live
    (SchemaIntrospector::readModel(*connection, envOr("NOGGIT_TEST_READ_SCHEMA", READ_SCHEMA)));

  // An account without SELECT sees an empty information_schema rather than an error, so an
  // empty model here means a grants problem, not an empty database.
  REQUIRE(live.tableCount() > 0);

  SchemaModel const captured (loadSchemaFixture(fixturePath(GOLDEN_FIXTURE)));

  // The point of this test: the live introspection query and the fixture format must agree.
  // If they drift, every fixture-based assertion elsewhere is testing something that no longer
  // resembles a real database.
  CHECK(live.wanderDistanceColumn() == captured.wanderDistanceColumn());
  CHECK(live.versionTable() == captured.versionTable());
  CHECK(live.poolMembersTable() == captured.poolMembersTable());
  CHECK(live.waypointPathTable() == captured.waypointPathTable());
  CHECK(live.addonPoseEncoding() == captured.addonPoseEncoding());
  CHECK(live.creatureModelSource() == captured.creatureModelSource());
  CHECK(live.smartScriptsEventParamCount() == captured.smartScriptsEventParamCount());
  CHECK(live.smartScriptsTargetParamCount() == captured.smartScriptsTargetParamCount());

  // Ordinal positions must survive the round trip too, since positional INSERT depends on them.
  CHECK(live.columnPosition("creature", "wander_distance")
        == captured.columnPosition("creature", "wander_distance"));
  CHECK(live.columnPosition("creature", "guid") == 1);
}

TEST_CASE("version is read from whichever version table exists", "[db][integration]")
{
  if (!haveEnv("NOGGIT_RO_DB_PWD"))
  {
    SKIP("NOGGIT_RO_DB_PWD not set; skipping live database tests.");
  }

  std::string const schema (envOr("NOGGIT_TEST_READ_SCHEMA", READ_SCHEMA));

  std::unique_ptr<WorldDatabaseConnection> const connection
    (tryConnect(readOnlyConfig(), AccessMode::READ_ONLY));

  if (!connection)
  {
    SKIP("Could not reach the reference database; skipping.");
  }

  SchemaModel const model (SchemaIntrospector::readModel(*connection, schema));

  // The reference database has `version`, not `version_db_world`. Probing only the latter --
  // as published references advise -- finds nothing here.
  std::string const version (SchemaIntrospector::readVersion(*connection, schema, model));

  CHECK_FALSE(version.empty());
  CHECK(version.find("TDB") != std::string::npos);
}

TEST_CASE("a read-only connection refuses to execute statements", "[db][safety][integration]")
{
  if (!haveEnv("NOGGIT_RO_DB_PWD"))
  {
    SKIP("NOGGIT_RO_DB_PWD not set; skipping live database tests.");
  }

  std::unique_ptr<WorldDatabaseConnection> connection
    (tryConnect(readOnlyConfig(), AccessMode::READ_ONLY));

  if (!connection)
  {
    SKIP("Could not reach the reference database; skipping.");
  }

  // Refused by this class, before anything reaches the server. The server would also refuse it
  // -- that is the grants layer -- but this must not depend on that.
  CHECK_THROWS_AS
    (connection->execute("CREATE TABLE noggit_should_never_exist (x INT)"), DatabaseAccessError);
  CHECK_THROWS_AS
    (connection->execute("DELETE FROM creature WHERE guid = 1"), DatabaseAccessError);

  // Reading the same connection still works.
  CHECK_FALSE(connection->serverVersion().empty());
}

TEST_CASE("a dev-write connection can round-trip a table", "[db][integration]")
{
  if (!haveEnv("NOGGIT_DEV_DB_PWD"))
  {
    SKIP("NOGGIT_DEV_DB_PWD not set; skipping dev-write tests.");
  }

  std::unique_ptr<WorldDatabaseConnection> connection
    (tryConnect(devWriteConfig(), AccessMode::DEV_WRITE));

  if (!connection)
  {
    SKIP("Could not reach the dev database; skipping.");
  }

  REQUIRE(connection->accessMode() == AccessMode::DEV_WRITE);
  REQUIRE(connection->schema() == WRITABLE_SCHEMA);

  connection->execute("DROP TABLE IF EXISTS noggit_roundtrip_test");
  connection->execute
    ("CREATE TABLE noggit_roundtrip_test (id INT PRIMARY KEY, pos FLOAT NOT NULL)");
  connection->execute("INSERT INTO noggit_roundtrip_test (id, pos) VALUES (1, -9512.345)");

  ResultRows const rows (connection->query("SELECT id, pos FROM noggit_roundtrip_test"));

  REQUIRE(rows.size() == 1);
  CHECK(rows[0][0] == "1");

  // FLOAT, not DOUBLE: the stored value is close, not equal. Anything that round-trip-verifies
  // world coordinates by exact equality will report false failures.
  double const stored (std::stod(rows[0][1]));
  CHECK(std::abs(stored - -9512.345) < 0.01);

  connection->execute("DROP TABLE IF EXISTS noggit_roundtrip_test");
}

TEST_CASE("schema names are validated before interpolation", "[db][safety]")
{
  if (!haveEnv("NOGGIT_RO_DB_PWD"))
  {
    SKIP("NOGGIT_RO_DB_PWD not set; skipping live database tests.");
  }

  std::unique_ptr<WorldDatabaseConnection> const connection
    (tryConnect(readOnlyConfig(), AccessMode::READ_ONLY));

  if (!connection)
  {
    SKIP("Could not reach the reference database; skipping.");
  }

  // Schema names are identifiers and cannot be bound as parameters, so they are interpolated.
  // Anything that is not a plain identifier is refused rather than escaped.
  CHECK_THROWS_AS(SchemaIntrospector::readColumns(*connection, "world'; DROP DATABASE x; --")
                 , DatabaseAccessError);
  CHECK_THROWS_AS(SchemaIntrospector::readColumns(*connection, ""), DatabaseAccessError);
  CHECK_THROWS_AS(SchemaIntrospector::readColumns(*connection, "has space"), DatabaseAccessError);
  CHECK_THROWS_AS(SchemaIntrospector::readColumns(*connection, "back`tick"), DatabaseAccessError);
}
