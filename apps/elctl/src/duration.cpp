#include "duration.hpp"

#include <chrono>
#include <string>

namespace elctl {

using pychron::Duration;
using pychron::ErrorKind;
using pychron::fail;

pychron::Result<Duration> parse_duration(std::string_view text) {
  const auto bad = [&] { return fail(ErrorKind::Config, "invalid duration '" + std::string(text) + "'"); };

  std::size_t digits = 0;
  bool dot = false;
  for (; digits < text.size(); ++digits) {
    const char c = text[digits];
    if (c == '.' && !dot) {
      dot = true;
    } else if (c < '0' || c > '9') {
      break;
    }
  }
  const std::string_view number = text.substr(0, digits);
  const std::string_view unit = text.substr(digits);
  if (number.empty() || number == ".") return bad();

  double scale = 0;  // seconds per unit
  if (unit.empty() || unit == "s") {
    scale = 1;
  } else if (unit == "ms") {
    scale = 1e-3;
  } else if (unit == "m") {
    scale = 60;
  } else if (unit == "h") {
    scale = 3600;
  } else {
    return bad();
  }

  const double seconds = std::stod(std::string(number)) * scale;
  return std::chrono::duration_cast<Duration>(std::chrono::duration<double>(seconds));
}

}  // namespace elctl
