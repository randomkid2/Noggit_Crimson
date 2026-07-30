// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_DATABASE_COLUMNINFO_HPP
#define NOGGIT_DATABASE_COLUMNINFO_HPP

#include <string>

namespace Noggit::Database
{
  // One row of information_schema.columns.
  //
  // Deliberately a plain aggregate with no behaviour: it is produced both by querying a live
  // database and by loading a captured fixture, and neither producer should need to know
  // anything about the other.
  struct ColumnInfo
  {
    std::string table_name;
    int ordinal_position = 0;
    std::string column_name;
    std::string column_type;
    bool is_nullable = false;

    // column_default is only meaningful when has_default is true. SQL NULL and the empty
    // string are different things and conflating them loses information.
    bool has_default = false;
    std::string column_default;

    std::string extra;
  };
}

#endif
