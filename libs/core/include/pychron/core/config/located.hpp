#pragma once

#include <cstdint>
#include <map>
#include <string>

namespace pychron::config {

// Where a value came from, for `file:line:field` diagnostics.
struct SourceLoc {
  std::string file;
  std::uint32_t line = 0;
  std::uint32_t column = 0;
};

// Every configured entity records its own location, its dotted field path
// (e.g. "valves[2]" or "transports.valve_bus") and the location of each key it
// read, so later validation can point at the exact offending line.
struct Located {
  SourceLoc loc;
  std::string path;
  std::map<std::string, SourceLoc> field_locs;

  // Location of `field` (e.g. "actuator", "interlocks[1]"), or the entity's own.
  const SourceLoc& where(const std::string& field) const {
    auto it = field_locs.find(field);
    return it == field_locs.end() ? loc : it->second;
  }
};

}  // namespace pychron::config
