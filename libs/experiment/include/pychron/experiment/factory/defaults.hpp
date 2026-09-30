#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "pychron/core/error.hpp"
#include "pychron/experiment/model/run_spec.hpp"

namespace pychron::experiment {

// One defaults.toml entry: what a new run of (analysis_type, extract_device) starts with.
struct RunDefaults {
  std::string template_name;  // measurement plan template, e.g. "thermo_argus/multicollect"
  ParamOverrides overrides;
  std::string script;  // extraction script
  std::optional<std::string> post_equilibration, post_measurement;
  // Optional extraction field defaults; unset fields are left alone.
  std::optional<Unit> units;
  std::optional<double> value;
  std::optional<Duration> duration, cleanup, pre_cleanup, post_cleanup;
};

// Lab defaults.toml:
//   [<analysis_type>.<extract_device>]     # device "*" matches any device
//   template = "thermo_argus/multicollect"
//   script = "felix_co2"
//   post_equilibration = "..."   post_measurement = "..."
//   [<analysis_type>.<extract_device>.extraction]   units value duration cleanup pre_cleanup post_cleanup
//   [<analysis_type>.<extract_device>.overrides]    "main.counts" = 400
class DefaultsTable {
 public:
  static Result<DefaultsTable> from_toml(std::string_view text, std::string_view name = "defaults.toml");
  static Result<DefaultsTable> load(const std::string& path);

  // Exact device match first, then the "*" entry; nullptr if neither exists.
  const RunDefaults* find(AnalysisType type, std::string_view device) const;
  void set(AnalysisType type, std::string device, RunDefaults defaults);

 private:
  std::map<std::pair<AnalysisType, std::string>, RunDefaults, std::less<>> entries_;
};

// Replaces measurement plan/overrides, extraction script and post scripts with the
// defaults, and sets any extraction fields the defaults provide.
void apply_defaults(RunSpec& run, const RunDefaults& defaults);

// Clears every field rules_for(run.id.type) forbids, so the run validates.
void strip_for_type(RunSpec& run);

// New run for `identifier`: type classified through `ids`, device set, defaults for
// (type, device) applied when present, then stripped for the type.
Result<RunSpec> make_run(std::string_view identifier, const IdentifierRules& ids, std::string_view extract_device,
                         const DefaultsTable& defaults);

// make_run with the special identifier for `type` (error for AnalysisType::Unknown).
Result<RunSpec> make_special_run(AnalysisType type, const IdentifierRules& ids, std::string_view extract_device,
                                 const DefaultsTable& defaults);

}  // namespace pychron::experiment
