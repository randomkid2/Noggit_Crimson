// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_DATABASE_CONNECTIONCONFIG_HPP
#define NOGGIT_DATABASE_CONNECTIONCONFIG_HPP

#include <string>

namespace Noggit::Database
{
  // Where and how to connect.
  //
  // Deliberately a plain struct with no Qt in sight. Reading these values out of QSettings is
  // the job of a thin adapter at the application boundary, which keeps this whole layer
  // testable without Qt present.
  struct ConnectionConfig
  {
    std::string host = "127.0.0.1";
    unsigned int port = 3306;
    std::string user;
    std::string password;
    std::string schema;
  };

  // What the connection is permitted to do.
  //
  // READ_ONLY is the default posture for anything pointed at a real server. DEV_WRITE is only
  // valid against the single configured disposable schema, and the connection refuses to be
  // constructed otherwise.
  enum class AccessMode
  {
    READ_ONLY,
    DEV_WRITE
  };
}

#endif
