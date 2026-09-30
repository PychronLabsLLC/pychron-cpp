#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace pychron::experiment {

// Seconds. Every duration in the model is a plain seconds value.
using Duration = std::chrono::duration<double>;

enum class Unit { Watts, Percent, Temp, Amps, Volts };

std::string_view to_string(Unit u) noexcept;
std::optional<Unit> parse_unit(std::string_view text);  // "w", "watts", "%", "c", ...

// Ordered list of tray holes, already range-expanded.
struct Position {
  std::vector<int> holes;
  friend bool operator==(const Position&, const Position&) = default;
};

using Pattern = std::string;
using ScriptOptions = std::string;

using ParamValue = std::variant<bool, std::int64_t, double, std::string>;
using ParamOverrides = std::map<std::string, ParamValue>;

// Reference to a conditionals file entry; `kind` is "action", "truncate", "cancel", ...
struct ConditionalRef {
  std::string name;
  std::string kind = "action";
  friend bool operator==(const ConditionalRef&, const ConditionalRef&) = default;
};

struct Overlap {
  Duration duration{0};   // how long before the previous run ends the next may start
  Duration min_delay{0};
  friend bool operator==(const Overlap&, const Overlap&) = default;
};

struct Delays {
  Duration before_analyses{15};
  Duration between_analyses{15};
  Duration after_blank{15};
  Duration extract_delay{0};
  friend bool operator==(const Delays&, const Delays&) = default;
};

struct SampleInfo {
  std::string sample, material, project, irradiation, level;
  std::optional<int> irradiation_position;
  friend bool operator==(const SampleInfo&, const SampleInfo&) = default;
};

}  // namespace pychron::experiment
