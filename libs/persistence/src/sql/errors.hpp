#pragma once

#include <string>
#include <string_view>

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
// on SQLite. An integrity-constraint violation also carries the code
// kCodeConstraint, which tells it from the other Protocol failures (no
// permission, a missing column, a trigger that raises, SQL that does not run).
// Error::code of an integrity-constraint violation (unique, check, not null,
// foreign key, exclusion): SQLSTATE class 23; on SQLite the primary result
// code SQLITE_CONSTRAINT (19), whatever the extended code. sql_error sets it.
inline constexpr std::string_view kCodeConstraint = "constraint";
bool is_constraint_violation(Dialect dialect, const std::string& native_code);

Error sql_error(Dialect dialect, const std::string& native_code, bool connection_error, const std::string& what);

}  // namespace pychron::persistence::detail
