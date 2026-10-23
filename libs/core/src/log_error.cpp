#include "pychron/core/log_error.hpp"

#include <string>

#include "pychron/core/events.hpp"
#include "pychron/core/process_log.hpp"

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

void log_and_publish(const Logger& logger, SignalBus* bus, std::string_view source, LogLevel level,
                     std::string message, TimePoint ts) {
  if (const auto hub = process_log_hub()) {
    if (!logger.enabled(level)) return;
    logger.log(level, message);  // publishes on the hub's bus
    if (bus == nullptr || bus == hub->bus()) return;
  }
  if (bus != nullptr) bus->publish(Log{level, std::string(source), std::move(message), ts});
}

}  // namespace pychron
