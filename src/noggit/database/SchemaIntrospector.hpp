// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_DATABASE_SCHEMAINTROSPECTOR_HPP
#define NOGGIT_DATABASE_SCHEMAINTROSPECTOR_HPP

#include <noggit/database/ColumnInfo.hpp>
#include <noggit/database/SchemaModel.hpp>

#include <string>
#include <vector>

namespace Noggit::Database
{
  class WorldDatabaseConnection;

  // Reads column metadata out of information_schema.
  //
  // This is the live counterpart to the fixture loader used by the tests: both produce
  // std::vector<ColumnInfo>, and neither needs to know the other exists. That symmetry is what
  // makes it possible to assert that introspecting a real database yields the same model as
  // the captured fixture -- which is the test that proves the query and the fixture format
  // actually agree.
  //
  // Reads only. Uses no privilege beyond SELECT, so it works through the read-only account.
  namespace SchemaIntrospector
  {
    // Every column of every table in `schema`, ordered by table then ordinal position.
    //
    // Note that information_schema only reports objects the connected user has some privilege
    // on. An account missing SELECT on the target schema sees an empty result rather than an
    // error, so an unexpectedly empty return means "check the grants", not "the schema is
    // empty".
    std::vector<ColumnInfo> readColumns
      (WorldDatabaseConnection const& connection, std::string const& schema);

    // Convenience: read and wrap in a SchemaModel.
    SchemaModel readModel
      (WorldDatabaseConnection const& connection, std::string const& schema);

    // Reads the applied database version, probing whichever version table exists.
    //
    // Returns an empty string when the table exists but holds no rows. Throws
    // SchemaCapabilityError via SchemaModel when neither version table is present, because a
    // database with no version marker is not one this tool should be editing.
    std::string readVersion
      ( WorldDatabaseConnection const& connection
      , std::string const& schema
      , SchemaModel const& model
      );
  }
}

#endif
