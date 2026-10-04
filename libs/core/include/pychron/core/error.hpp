#pragma once

#include <string>
#include <string_view>

#include "pychron/core/expected.hpp"

namespace pychron {

enum class ErrorKind { Timeout, Io, Protocol, Config, NotConnected, Interlock, Cancelled };

struct Error {
  ErrorKind kind = ErrorKind::Io;
  std::string what;
  std::string device;
  // A stable word a producer may add so that a caller can tell this failure
  // from others of its kind without reading `what`; it survives a caller that
  // rewrites `what`. Empty for most errors. Each library documents its codes.
  std::string code = {};

  friend bool operator==(const Error&, const Error&) = default;
};

// No exceptions cross library boundaries: fallible calls return Result<T>.
template <class T>
using Result = Expected<T, Error>;

// Stable lower-case name, e.g. "not_connected".
std::string_view to_string(ErrorKind kind) noexcept;

// "<kind>: <what>" or "<device>: <kind>: <what>".
std::string to_string(const Error& error);

// `return fail(ErrorKind::Timeout, "no reply", "ig1");`
inline Unexpected<Error> fail(ErrorKind kind, std::string what, std::string device = {}) {
  return Unexpected<Error>(Error{kind, std::move(what), std::move(device)});
}

inline Unexpected<Error> fail(Error error) { return Unexpected<Error>(std::move(error)); }

}  // namespace pychron
