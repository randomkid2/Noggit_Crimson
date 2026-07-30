// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <FixtureLoader.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>

#ifndef NOGGIT_TEST_FIXTURE_DIR
  #error "NOGGIT_TEST_FIXTURE_DIR must be defined by the build. See tests/CMakeLists.txt."
#endif

using namespace Noggit;

namespace
{
  // MySQL's batch output writes SQL NULL as this.
  constexpr char const* SQL_NULL_MARKER = "\\N";

  std::vector<std::string> splitTabs(std::string const& line)
  {
    std::vector<std::string> fields;
    std::string current;
    std::istringstream stream (line);

    while (std::getline(stream, current, '\t'))
    {
      fields.push_back(current);
    }

    return fields;
  }

  std::string trimCarriageReturn(std::string s)
  {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n'))
    {
      s.pop_back();
    }

    return s;
  }

  // Fixtures reach this loader from two producers that disagree about encoding: a captured
  // dump written by PowerShell (UTF-8 *with* BOM) and a hand-authored file (no BOM). Left in
  // place the BOM becomes part of the first field, so the header is not recognised and gets
  // parsed as data.
  std::string stripUtf8Bom(std::string s)
  {
    if (s.size() >= 3
        && static_cast<unsigned char>(s[0]) == 0xEF
        && static_cast<unsigned char>(s[1]) == 0xBB
        && static_cast<unsigned char>(s[2]) == 0xBF)
    {
      s.erase(0, 3);
    }

    return s;
  }

  // Trailing empty fields are dropped by some producers, so read defensively rather than
  // indexing blind.
  std::string fieldOr(std::vector<std::string> const& fields, std::size_t index)
  {
    return index < fields.size() ? fields[index] : std::string();
  }

  bool startsWithIgnoringCase(std::string const& haystack, std::string const& prefix)
  {
    if (haystack.size() < prefix.size())
    {
      return false;
    }

    return std::equal
      ( prefix.begin(), prefix.end(), haystack.begin()
      , [] (char a, char b)
        {
          return std::tolower(static_cast<unsigned char>(a))
              == std::tolower(static_cast<unsigned char>(b));
        }
      );
  }
}

std::vector<Database::ColumnInfo> Tests::loadSchemaFixture(std::string const& path)
{
  std::ifstream file (path);

  if (!file)
  {
    throw std::runtime_error("Cannot open schema fixture: " + path);
  }

  std::vector<Database::ColumnInfo> columns;
  std::string line;
  bool header_skipped (false);
  std::size_t line_number (0);

  while (std::getline(file, line))
  {
    ++line_number;
    line = trimCarriageReturn(line);

    if (line_number == 1)
    {
      line = stripUtf8Bom(std::move(line));
    }

    if (line.empty())
    {
      continue;
    }

    if (!header_skipped)
    {
      header_skipped = true;

      // Case-insensitively: MySQL returns information_schema column names in upper case, so a
      // captured fixture has TABLE_NAME while a hand-written one has table_name.
      if (startsWithIgnoringCase(line, "table_name"))
      {
        continue;
      }
      // No header present: fall through and treat this as data.
    }

    std::vector<std::string> const fields (splitTabs(line));

    if (fields.size() < 3)
    {
      throw std::runtime_error
        ("Malformed schema fixture " + path + " at line " + std::to_string(line_number)
         + ": expected at least 3 tab-separated fields, got " + std::to_string(fields.size()));
    }

    Database::ColumnInfo column;
    column.table_name = fields[0];

    try
    {
      column.ordinal_position = std::stoi(fields[1]);
    }
    catch (std::exception const&)
    {
      throw std::runtime_error
        ("Malformed schema fixture " + path + " at line " + std::to_string(line_number)
         + ": ordinal_position '" + fields[1] + "' is not an integer");
    }

    column.column_name = fields[2];
    column.column_type = fieldOr(fields, 3);
    column.is_nullable = fieldOr(fields, 4) == "YES";

    std::string const default_field (fieldOr(fields, 5));
    column.has_default = !default_field.empty() && default_field != SQL_NULL_MARKER;
    column.column_default = column.has_default ? default_field : std::string();

    column.extra = fieldOr(fields, 6);

    columns.push_back(std::move(column));
  }

  if (columns.empty())
  {
    throw std::runtime_error("Schema fixture contained no rows: " + path);
  }

  return columns;
}

std::string Tests::fixturePath(std::string const& file_name)
{
  return std::string(NOGGIT_TEST_FIXTURE_DIR) + "/" + file_name;
}
