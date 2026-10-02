#include "sql/errors.hpp"

#include <cstdlib>

namespace pychron::persistence::detail {
namespace {

ErrorKind classify_sqlstate(const std::string& s) {
  if (s.size() != 5) return ErrorKind::Io;
  const std::string cls = s.substr(0, 2);
  if (cls == "08") return ErrorKind::NotConnected;                          // connection exception
  if (s == "57P01" || s == "57P02" || s == "57P03") return ErrorKind::NotConnected;  // shutdown, cannot connect now
  if (s == "40001" || s == "40P01" || s == "57014" || cls == "53") return ErrorKind::Timeout;
  if (cls == "23" || cls == "22" || cls == "42" || cls == "P0" || cls == "0A") return ErrorKind::Protocol;
  return ErrorKind::Io;
}

ErrorKind classify_sqlite(const std::string& code) {
  char* end = nullptr;
  const long value = std::strtol(code.c_str(), &end, 10);
  if (code.empty() || *end != '\0') return ErrorKind::Io;
  switch (value & 0xff) {
    case 5:   // SQLITE_BUSY
    case 6:   // SQLITE_LOCKED
      return ErrorKind::Timeout;
    case 19:  // SQLITE_CONSTRAINT (includes RAISE(ABORT) from the immutability triggers)
    case 20:  // SQLITE_MISMATCH (STRICT type check)
    case 1:   // SQLITE_ERROR (SQL error, missing table)
      return ErrorKind::Protocol;
    default:
      return ErrorKind::Io;
  }
}

}  // namespace

Error sql_error(Dialect dialect, const std::string& native_code, bool connection_error, const std::string& what) {
  ErrorKind kind = dialect == Dialect::PostgreSql ? classify_sqlstate(native_code) : classify_sqlite(native_code);
  if (connection_error && kind == ErrorKind::Io) kind = ErrorKind::NotConnected;
  std::string message = what;
  if (!native_code.empty()) message = "[" + native_code + "] " + message;
  return Error{kind, std::move(message), "persistence"};
}

}  // namespace pychron::persistence::detail
