#pragma once

#include <string>
#include <string_view>

#include "pychron/core/error.hpp"
#include "pychron/core/logger.hpp"
#include "pychron/core/signal_bus.hpp"

namespace pychron {

// One error record at error level: "<attempted>: <kind>: <what> (<device>)".
// The parenthesis is left out when the error names no device.
void log_error(const Logger& logger, const Error& error, std::string_view attempted);

// The same record at warn, for an error that was handled (retry, fallback)
// and not shown to anyone.
void log_handled(const Logger& logger, const Error& error, std::string_view attempted);

// A line for the log file and, once, for the dock. `logger` is the process
// logger of `source`. With a process hub the line goes through it (file, and
// the hub's bus); it is also published on `bus` only when that is another bus.
// With no process hub it is published on `bus` alone (when there is one).
void log_and_publish(const Logger& logger, SignalBus* bus, std::string_view source, LogLevel level,
                     std::string message, TimePoint ts);

}  // namespace pychron
