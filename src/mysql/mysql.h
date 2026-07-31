// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_MYSQL_MYSQL_H
#define NOGGIT_MYSQL_MYSQL_H

#include <cinttypes>
#include <cstddef>

// Upstream's UID-storage seam: five free functions persisting one UIDs(_map_id, UID) table.
// It is not a database layer -- that is src/noggit/database, which goes through
// WorldDatabaseConnection and emits reviewable changesets.
//
// Every function here refuses to do anything at all unless the configured schema
// (project/mysql/db) is exactly the nominated writable schema (project/mysql/dev_schema).
// See the header comment in mysql.cpp for why, and for what is and is not guaranteed.
namespace mysql
{
  // Opens a read-only connection and reports the outcome in a dialog. Also reports whether
  // UID storage is permitted, since a connection can succeed while storage stays refused.
  bool testConnection(bool report_only_err = false);

  bool hasMaxUIDStoredDB(std::size_t mapID);
  std::uint32_t getGUIDFromDB(std::size_t mapID);
  void insertUIDinDB(std::size_t mapID, std::uint32_t NewUID);
  void updateUIDinDB (std::size_t mapID, std::uint32_t NewUID);
}

#endif
