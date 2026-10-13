#pragma once

// Typed model of the optional `[metrics]` table of `extraction_line.toml`:
// the HTTP endpoint a Prometheus server scrapes. Off unless enabled, since it
// opens a port.

#include <cstdint>
#include <string>

#include "pychron/core/config/located.hpp"

namespace pychron::config {

struct MetricsConfig : Located {
  bool enabled = false;
  std::string bind = "0.0.0.0";  // an interface's IP address; "127.0.0.1" for this computer only
  std::int64_t port = 9464;
};

}  // namespace pychron::config
