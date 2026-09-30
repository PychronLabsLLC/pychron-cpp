#include "pychron/experiment/model/types.hpp"

#include <algorithm>
#include <cctype>

namespace pychron::experiment {

std::string_view to_string(Unit u) noexcept {
  switch (u) {
    case Unit::Watts: return "watts";
    case Unit::Percent: return "percent";
    case Unit::Temp: return "temp";
    case Unit::Amps: return "amps";
    case Unit::Volts: return "volts";
  }
  return "watts";
}

std::optional<Unit> parse_unit(std::string_view text) {
  std::string s(text);
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
  if (s == "w" || s == "watts" || s == "watt") return Unit::Watts;
  if (s == "%" || s == "percent") return Unit::Percent;
  if (s == "c" || s == "temp" || s == "celsius") return Unit::Temp;
  if (s == "a" || s == "amps" || s == "amp") return Unit::Amps;
  if (s == "v" || s == "volts" || s == "volt") return Unit::Volts;
  return std::nullopt;
}

}  // namespace pychron::experiment
