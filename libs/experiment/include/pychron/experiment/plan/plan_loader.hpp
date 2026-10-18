#pragma once

// Plan template -> effective plan -> resolved, validated MeasurementPlan.
//
//   parse_plan_template(text)                      syntax + parameters.expose shape/paths
//   render_effective_plan(template, overrides)     expose enforcement, TOML text for the record
//   resolve_plan(effective_toml, resolvers)        '@' aliases, typed parse, every schema rule
//   load_plan(...)                                 all three
//
// Errors are ErrorKind::Config; `what` lists every problem found, one per line,
// each prefixed "<source>: <path>: ".

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pychron/core/error.hpp"
#include "pychron/experiment/model/types.hpp"
#include "pychron/experiment/plan/plan.hpp"

namespace pychron::experiment::plan {

// extraction_line.toml [aliases]: "valves.inlet" -> "V12", "extraction.eqtime" -> 20.
class IAliasResolver {
 public:
  virtual ~IAliasResolver() = default;
  // `key` has no leading '@'. nullopt = not an alias this resolver knows.
  virtual std::optional<ParamValue> resolve_alias(std::string_view key) const = 0;
};

// spectrometer.toml: detector names plus optional aliases of its own.
class ISpectrometerCatalog {
 public:
  virtual ~ISpectrometerCatalog() = default;
  virtual bool has_detector(std::string_view name) const = 0;
  // The detector's configured isotope (spectrometer.toml `isotope`), nullopt
  // when the detector is unknown or has none. Labels the inactive entry the v1
  // desugar adds for a protected detector a hop does not collect.
  virtual std::optional<std::string> isotope_of(std::string_view detector) const = 0;
  // Mass of a collect label (isotope, molecule, pseudo-isotope such as PHHCl
  // or Ar40++) from the spectrometer's molecular weights; nullopt: none.
  virtual std::optional<double> mass_of(std::string_view label) const = 0;
  // The detector's deflection can be set (spectrometer.toml `deflection.control`).
  virtual bool supports_deflection(std::string_view detector) const = 0;
  // A named optics preset the driver applies (none before the optics phase).
  virtual bool has_optics(std::string_view name) const = 0;
  // A named field table loaded with the spectrometer.
  virtual bool has_table(std::string_view name) const = 0;
  // The magnet can be set by a raw DAC value (`at = { dac }`), and the
  // source's HV can position a beam (`at = { hv, detector }`).
  virtual bool positions_by_dac() const = 0;
  virtual bool positions_by_hv() const = 0;
  virtual std::optional<ParamValue> resolve_alias(std::string_view /*key*/) const { return std::nullopt; }
};

// '@x.y' resolves, in order: to plan key x.y (self reference, e.g.
// "@equilibration.time_s"), then the extraction line, then the spectrometer.
// A null spectrometer skips every check that needs the spectrometer
// (detector names, label masses, deflection, optics, tables, positioning).
// conditionals.include entries are references to conditional sets and are
// kept verbatim.
struct PlanResolvers {
  const IAliasResolver* extraction_line = nullptr;
  const ISpectrometerCatalog* spectrometer = nullptr;
  // Whether a run of this analysis type has an extraction (a
  // during_extraction segment needs one). Null: every type has one.
  std::function<bool(std::string_view analysis_type)> has_extraction = nullptr;
};

struct PlanTemplate {
  std::string source;  // file name or template id, for messages
  std::string text;    // template TOML as authored
  std::string name;    // [plan].name
  std::vector<ExposeEntry> expose;
};

struct LoadOptions {
  bool advanced = false;  // advanced runs may override any existing key
};

struct LoadedPlan {
  MeasurementPlan plan;
  std::string effective_toml;  // template + overrides, aliases unresolved
};

Result<PlanTemplate> parse_plan_template(std::string_view text, std::string_view source = "plan");

// Overrides are keyed by dotted/indexed path. Only exposed paths (or children
// of an exposed table) are accepted unless `advanced`. Values must match the
// template's type (an int may set a float; any scalar may replace an '@'
// alias); a string sets a string array as a comma-separated list
// ("detectors.exclude" = "CDD, L2"). Output is deterministic TOML.
Result<std::string> render_effective_plan(const PlanTemplate& tmpl, const ParamOverrides& overrides,
                                          const LoadOptions& options = {});

Result<MeasurementPlan> resolve_plan(std::string_view effective_toml, const PlanResolvers& resolvers,
                                     std::string_view source = "plan");

Result<LoadedPlan> load_plan(const PlanTemplate& tmpl, const ParamOverrides& overrides,
                             const PlanResolvers& resolvers, const LoadOptions& options = {});

}  // namespace pychron::experiment::plan
