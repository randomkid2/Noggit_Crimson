// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_TESTS_FIXTURELOADER_HPP
#define NOGGIT_TESTS_FIXTURELOADER_HPP

#include <noggit/database/ColumnInfo.hpp>

#include <string>
#include <vector>

namespace Noggit::Tests
{
  // Loads a captured information_schema dump from the tab-separated format described in
  // tests/fixtures/README.md.
  //
  // This lives in tests/ rather than src/ so no test-only code ships inside the application.
  // It is the second producer of ColumnInfo, alongside the live introspector, which is what
  // lets the schema model be exercised with no database present.
  //
  // Throws std::runtime_error if the file cannot be read or a row is malformed. A fixture that
  // silently loads as empty would make every assertion against it vacuously pass.
  std::vector<Database::ColumnInfo> loadSchemaFixture(std::string const& path);

  // Absolute path to a fixture by file name, using the directory baked in at configure time.
  std::string fixturePath(std::string const& file_name);
}

#endif
