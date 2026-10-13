#pragma once

// Reading the rendered text in tests: a series is found by everything before
// its value, e.g. `pychron_pressure{gauge="IG1",unit="torr"}`.

#include <cstdlib>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace metrics_text {

inline std::vector<std::string> lines(const std::string& text) {
  std::vector<std::string> out;
  std::istringstream in(text);
  for (std::string line; std::getline(in, line);) out.push_back(line);
  return out;
}

// The text after `series` and one space, on the line that starts with it.
inline std::optional<std::string> raw(const std::string& text, std::string_view series) {
  for (const std::string& line : lines(text)) {
    if (line.size() > series.size() + 1 && line.starts_with(series) && line[series.size()] == ' ') {
      return line.substr(series.size() + 1);
    }
  }
  return std::nullopt;
}

inline bool has(const std::string& text, std::string_view series) { return raw(text, series).has_value(); }

// The series' value; -1e300 when it is absent, so a comparison fails loudly.
inline double value(const std::string& text, std::string_view series) {
  const auto r = raw(text, series);
  return r ? std::strtod(r->c_str(), nullptr) : -1e300;
}

// The text without the registry's and the server's own families.
inline std::string without_own(const std::string& text) {
  std::string out;
  for (const std::string& line : lines(text)) {
    if (line.find("pychron_metrics_") != std::string::npos) continue;
    out += line + "\n";
  }
  return out;
}

}  // namespace metrics_text
