#pragma once

#include <string>
#include <vector>

#include "pychron/core/config/system_config.hpp"
#include "pychron/core/error.hpp"

namespace pychron::config {

// One problem found while loading a config file.
struct Diagnostic {
  SourceLoc loc;
  std::string field;  // dotted path, e.g. "valves[0].actuator"
  std::string message;
};

// "file:line:field: message"
std::string to_string(const Diagnostic& d);

// Folds diagnostics into one ErrorKind::Config error, one line per diagnostic.
Error to_error(const std::vector<Diagnostic>& diagnostics);

}  // namespace pychron::config
