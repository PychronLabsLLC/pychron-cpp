#include "pychron/core/log_error.hpp"

#include <string>

namespace pychron {
namespace {

void write(const Logger& logger, LogLevel level, const Error& error, std::string_view attempted) {
  if (!logger.enabled(level)) return;
  std::string message;
  message.reserve(attempted.size() + error.what.size() + error.device.size() + 24);
  message.append(attempted).append(": ").append(to_string(error.kind)).append(": ").append(error.what);
  if (!error.device.empty()) message.append(" (").append(error.device).push_back(')');
  logger.log(level, message);
}

}  // namespace

void log_error(const Logger& logger, const Error& error, std::string_view attempted) {
  write(logger, LogLevel::Error, error, attempted);
}

void log_handled(const Logger& logger, const Error& error, std::string_view attempted) {
  write(logger, LogLevel::Warn, error, attempted);
}

}  // namespace pychron
