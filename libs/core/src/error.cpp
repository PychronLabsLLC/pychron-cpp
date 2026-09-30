#include "pychron/core/error.hpp"

namespace pychron {

std::string_view to_string(ErrorKind kind) noexcept {
  switch (kind) {
    case ErrorKind::Timeout: return "timeout";
    case ErrorKind::Io: return "io";
    case ErrorKind::Protocol: return "protocol";
    case ErrorKind::Config: return "config";
    case ErrorKind::NotConnected: return "not_connected";
    case ErrorKind::Interlock: return "interlock";
    case ErrorKind::Cancelled: return "cancelled";
  }
  return "unknown";
}

std::string to_string(const Error& error) {
  std::string out;
  if (!error.device.empty()) {
    out += error.device;
    out += ": ";
  }
  out += to_string(error.kind);
  out += ": ";
  out += error.what;
  return out;
}

}  // namespace pychron
