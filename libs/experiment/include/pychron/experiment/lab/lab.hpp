#pragma once

// A lab directory (experiment-window design 4.1): everything a queue needs
// that comes from files, loaded without hardware, and the full check of a
// queue against it. Shared by `elctl exp` and the UI so both agree on what a
// lab is and on what makes a queue runnable.
//
//   <lab>/plans/*.toml            measurement plan templates
//   <lab>/scripts/                extraction, post-equilibration, post-measurement, hooks
//   <lab>/conditionals/*.toml     named conditional sets
//   <lab>/identifiers.toml        identifier rules (default rules if absent)
//   <lab>/peak_center.toml        named peak-center configs (optional)

#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pychron/core/config/system_config.hpp"
#include "pychron/experiment/conditionals/library.hpp"
#include "pychron/experiment/conditionals/validate.hpp"
#include "pychron/experiment/measurement/adapters.hpp"
#include "pychron/experiment/model/identifiers.hpp"
#include "pychron/experiment/model/queue_validation.hpp"
#include "pychron/experiment/plan/plan_library.hpp"
#include "pychron/scripting/services.hpp"
#include "pychron/systems/jobs/peak_center.hpp"
#include "pychron/systems/spectrometer/data_dir.hpp"

namespace pychron::experiment::lab {

struct LabPaths {
  std::filesystem::path dir;
  std::filesystem::path line_config;   // extraction_line.toml; empty: none
  std::filesystem::path spectrometer;  // spectrometer config; empty: none
};

// A script name exists in any of the script directories (queue validation
// does not know the kind).
class LabScripts final : public IScriptResolver {
 public:
  explicit LabScripts(std::filesystem::path root);
  bool has_script(std::string_view name) const override;
  const scripting::DirectoryScriptResolver& resolver() const { return resolver_; }

 private:
  scripting::DirectoryScriptResolver resolver_;
};

class LabConditionals final : public IConditionalResolver {
 public:
  explicit LabConditionals(const IConditionalSource& source) : source_(source) {}
  bool has_conditional(std::string_view name, std::string_view kind) const override;
  bool has_conditional_set(std::string_view name) const override;

 private:
  const IConditionalSource& source_;
};

struct Lab {
  LabPaths paths;
  IdentifierRules ids = IdentifierRules::defaults();
  std::optional<config::SystemConfig> line;
  std::optional<spectrometer::cfg::SpectrometerData> spectrometer;
  std::unique_ptr<measurement::SystemConfigAliases> aliases;
  std::unique_ptr<measurement::SpectrometerCatalog> catalog;
  std::unique_ptr<plan::PlanLibrary> plans;
  std::unique_ptr<DirectoryConditionalSource> condition_source;
  std::unique_ptr<ConditionalLibrary> conditionals;
  std::unique_ptr<LabScripts> scripts;
  std::unique_ptr<LabConditionals> condition_names;
  std::map<std::string, jobs::PeakCenterConfig> peak_centers;
  std::vector<std::string> problems;  // files that did not load

  QueueResolvers resolvers() const { return {plans.get(), scripts.get(), condition_names.get()}; }
  // Gauges, detectors and isotopes the conditionals may name.
  MetricCatalog metric_catalog() const;
};

// Never fails: whatever does not load is reported in Lab::problems.
Lab load_lab(const LabPaths& paths);

struct LabCheck {
  QueueReport report;             // check_queue against the lab's resolvers
  std::vector<Diagnostic> extra;  // lab problems (field "lab"), peak-center configs, conditionals
  bool ok() const;
  std::vector<Diagnostic> all() const;  // report.diagnostics, then extra
};

// check_queue plus what only the lab knows: files that did not load, plans
// naming a peak-center config the lab lacks, and each run's conditionals
// checked against the metric catalog (each distinct message once).
LabCheck check_lab_queue(const Lab& lab, const QueueSpec& queue);

// "runs[3].plan: unknown plan 'x'", "queue.delays: ...", "lab: ...".
std::string describe(const Diagnostic& d);

}  // namespace pychron::experiment::lab
