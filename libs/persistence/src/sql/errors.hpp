#pragma once

#include <string>

#include "pychron/core/error.hpp"
#include "pychron/persistence/store.hpp"

namespace pychron::persistence::detail {

// Maps a driver error to the core ErrorKind (DVC schema spec, section 12.2):
//   NotConnected  server unreachable or connection lost        (retryable)
//   Timeout       serialization failure, deadlock, busy, lack  (retryable)
//                 of resources, admin shutdown
//   Protocol      constraint, data or trigger violation        (permanent)
//   Io            anything else (disk, SQLite I/O)
// `native_code` is the SQLSTATE on PostgreSQL and the (extended) result code
// on SQLite.
Error sql_error(Dialect dialect, const std::string& native_code, bool connection_error, const std::string& what);

}  // namespace pychron::persistence::detail
