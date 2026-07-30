// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/database/SchemaIntrospector.hpp>

#include <noggit/database/WorldDatabaseConnection.hpp>

#include <algorithm>
#include <cctype>
#include <stdexcept>

using namespace Noggit::Database;

namespace
{
  // Schema names are identifiers, not values, so they cannot be passed as bound parameters and
  // have to be interpolated. Rather than escape, refuse anything that is not a plain
  // identifier: it is a smaller rule to get right, and no legitimate schema name needs more.
  void requirePlainIdentifier(std::string const& name)
  {
    if (name.empty() || name.size() > 64)
    {
      throw DatabaseAccessError
        ("Invalid schema name '" + name + "': must be 1-64 characters.");
    }

    bool const ok
      ( std::all_of
          ( name.begin(), name.end()
          , [] (unsigned char c)
            {
              return std::isalnum(c) || c == '_' || c == '$';
            }
          )
      );

    if (!ok)
    {
      throw DatabaseAccessError
        ("Invalid schema name '" + name + "': only letters, digits, underscore and $ are"
         " accepted. Refusing to interpolate it into a query.");
    }
  }

  std::string fieldOr(ResultRow const& row, std::size_t index)
  {
    return index < row.size() ? row[index] : std::string();
  }

  int toIntOrZero(std::string const& s)
  {
    try
    {
      return s.empty() ? 0 : std::stoi(s);
    }
    catch (std::exception const&)
    {
      return 0;
    }
  }
}

std::vector<ColumnInfo> SchemaIntrospector::readColumns
  (WorldDatabaseConnection const& connection, std::string const& schema)
{
  requirePlainIdentifier(schema);

  // `column_default IS NULL` is selected separately because the result reader renders SQL NULL
  // as an empty string, and "no default" and "defaults to the empty string" are different
  // facts about a column.
  std::string const sql
    ( "SELECT table_name, ordinal_position, column_name, column_type, is_nullable,"
      " column_default, (column_default IS NULL), extra"
      " FROM information_schema.columns"
      " WHERE table_schema = '" + schema + "'"
      " ORDER BY table_name, ordinal_position"
    );

  ResultRows const rows (connection.query(sql));

  std::vector<ColumnInfo> columns;
  columns.reserve(rows.size());

  for (ResultRow const& row : rows)
  {
    if (row.size() < 3)
    {
      continue;
    }

    ColumnInfo column;
    column.table_name = row[0];
    column.ordinal_position = toIntOrZero(row[1]);
    column.column_name = row[2];
    column.column_type = fieldOr(row, 3);
    column.is_nullable = fieldOr(row, 4) == "YES";

    bool const default_is_null (fieldOr(row, 6) != "0");
    column.has_default = !default_is_null;
    column.column_default = column.has_default ? fieldOr(row, 5) : std::string();

    column.extra = fieldOr(row, 7);

    columns.push_back(std::move(column));
  }

  return columns;
}

SchemaModel SchemaIntrospector::readModel
  (WorldDatabaseConnection const& connection, std::string const& schema)
{
  return SchemaModel(readColumns(connection, schema));
}

std::string SchemaIntrospector::readVersion
  ( WorldDatabaseConnection const& connection
  , std::string const& schema
  , SchemaModel const& model
  )
{
  requirePlainIdentifier(schema);

  // Throws SchemaCapabilityError when neither version table exists, which is the correct
  // outcome: a world database with no version marker is not one to be editing.
  std::string const table (model.versionTable());

  ResultRows const rows
    (connection.query("SELECT db_version FROM `" + schema + "`.`" + table + "` LIMIT 1"));

  if (rows.empty() || rows.front().empty())
  {
    return {};
  }

  return rows.front().front();
}
