#pragma once

#include <string_view>

#include "pychron/core/error.hpp"
#include "pychron/core/logger.hpp"

namespace pychron {

// One error record at error level: "<attempted>: <kind>: <what> (<device>)".
// The parenthesis is left out when the error names no device.
void log_error(const Logger& logger, const Error& error, std::string_view attempted);

// The same record at warn, for an error that was handled (retry, fallback)
// and not shown to anyone.
void log_handled(const Logger& logger, const Error& error, std::string_view attempted);

}  // namespace pychron
