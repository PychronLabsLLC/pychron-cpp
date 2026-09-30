#pragma once

// Script identity, the `#! pychron:` metadata header and the per-run context.
//
// Metadata that pychron kept in a docstring (`eqtime: 20`) moves to header
// lines anywhere in the script:
//
//   #! pychron: eqtime=20, duration=5
//   #! pychron: label="two words"
//
// Pairs are separated by commas and/or whitespace; a value may be quoted.
// A later key overrides an earlier one.

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "pychron/core/error.hpp"

namespace pychron::scripting {

// Which run step a script serves. Extraction, PostEquilibration and
// PostMeasurement scripts define main(); measurement hooks define any of
// before_main/after_main/on_whiff_result.
enum class ScriptKind { Extraction, PostEquilibration, PostMeasurement, MeasurementHook };

// Stable name, also the scripts/ subdirectory: "extraction",
// "post_equilibration", "post_measurement", "measurement_hooks".
std::string_view to_string(ScriptKind kind) noexcept;

struct Script {
  std::string name;  // e.g. "extraction/laser_default.py"; used in diagnostics
  std::string text;
  ScriptKind kind = ScriptKind::Extraction;
};

// Lower-case hex SHA-256 of `text` (recorded with the script in the
// AnalysisRecord).
std::string sha256_hex(std::string_view text);

class ScriptHeader {
 public:
  const std::map<std::string, std::string, std::less<>>& values() const noexcept { return values_; }
  std::optional<std::string> get(std::string_view key) const;
  // Config error if present but not a number.
  Result<std::optional<double>> number(std::string_view key) const;
  void set(std::string key, std::string value) { values_[std::move(key)] = std::move(value); }

 private:
  std::map<std::string, std::string, std::less<>> values_;
};

// Config error naming the line for a malformed header line.
Result<ScriptHeader> parse_header(std::string_view text);

// A context/option value: None, bool, int, float or str in Python.
using Value = std::variant<std::monostate, bool, std::int64_t, double, std::string>;
using ValueMap = std::map<std::string, Value, std::less<>>;

// Injected read-only into every script of a run.
struct ScriptContext {
  // Module globals (pychron's set_default_context names and any extras).
  ValueMap globals;
  // `script_options`, reached as opt.<key>.
  ValueMap options;
};

// pychron's set_default_context names with their defaults: analysis_type,
// run_identifier, position, extract_value, extract_units, duration, cleanup,
// pre_cleanup, post_cleanup, tray, pattern, beam_diameter, light_value,
// ramp_rate, ramp_duration, cryo_temperature, load_identifier,
// extract_device, disable_between_positions.
ValueMap default_context();

// default_context() overlaid with `overrides`.
ScriptContext make_context(ValueMap overrides = {}, ValueMap options = {});

}  // namespace pychron::scripting
