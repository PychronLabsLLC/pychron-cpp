#pragma once

// Where conditionals come from (conditionals spec section 4) and the checks
// between runs (section 6.2).
//
//   level   source
//   system  <lab>/conditionals/system.toml (optional)
//   queue   <lab>/conditionals/<QueueSpec.queue_conditionals>.toml
//   plan    plan [conditionals].include ("@conditionals.NAME" or "NAME")
//           plus the plan's inline truncations
//   run     <lab>/conditionals/<RunSpec.conditionals[i].name>.toml
//
// Levels merge system -> queue -> plan -> run (merge_levels); every
// conditional is stamped with its level and source.

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>

#include "pychron/experiment/conditionals/conditional.hpp"
#include "pychron/experiment/conditionals/queue_actions.hpp"
#include "pychron/experiment/model/run_spec.hpp"
#include "pychron/experiment/plan/plan.hpp"

namespace pychron::experiment {

// Text of a named conditionals file; nullopt when it does not exist.
class IConditionalSource {
 public:
  virtual ~IConditionalSource() = default;
  virtual Result<std::optional<std::string>> text(std::string_view name) const = 0;
  virtual std::string location(std::string_view name) const = 0;
};

// <dir>/<name>.toml. Names must be plain (no path separators or "..").
class DirectoryConditionalSource final : public IConditionalSource {
 public:
  explicit DirectoryConditionalSource(std::filesystem::path dir) : dir_(std::move(dir)) {}
  Result<std::optional<std::string>> text(std::string_view name) const override;
  std::string location(std::string_view name) const override;

 private:
  std::filesystem::path dir_;
};

class MapConditionalSource final : public IConditionalSource {
 public:
  void add(std::string name, std::string text) { files_[std::move(name)] = std::move(text); }
  Result<std::optional<std::string>> text(std::string_view name) const override;
  std::string location(std::string_view name) const override { return std::string(name) + ".toml"; }

 private:
  std::map<std::string, std::string, std::less<>> files_;
};

// The plan's inline [conditionals].truncations as a set (names
// "plan.truncation[i]").
Result<ConditionalSet> plan_truncations(const plan::MeasurementPlan& plan);

class ConditionalLibrary {
 public:
  explicit ConditionalLibrary(const IConditionalSource& source) : source_(source) {}

  // A named file, stamped; Config error when missing. "@conditionals.NAME" accepted.
  Result<ConditionalSet> load(std::string_view name, ConditionalLevel level) const;

  Result<ConditionalSet> system() const;  // empty when there is no system.toml
  Result<ConditionalSet> queue(const QueueSpec& queue) const;
  Result<ConditionalSet> plan(const plan::MeasurementPlan& plan, std::string_view plan_name = "plan") const;
  Result<ConditionalSet> run(const RunSpec& run) const;

  // Everything that applies during the run's measurement, merged.
  Result<ConditionalSet> for_run(const QueueSpec& queue, const RunSpec& run, const plan::MeasurementPlan& plan) const;
  // The queue-wide sets (system, queue) used for pre-run and post-run checks.
  Result<ConditionalSet> for_queue(const QueueSpec& queue) const;

 private:
  const IConditionalSource& source_;
};

struct PostRunOutcome {
  Trip trip;
  bool cancel_queue = false;
  std::optional<QueueChange> change;
};

// Pre-run and post-run checks across a queue (legacy L21, L22). State is kept
// between runs, so ntrips counts consecutive runs a conditional applied to.
class RunChecks {
 public:
  explicit RunChecks(ConditionalSet queue_level) : engine_(std::move(queue_level)) {}

  // Before extraction and before measurement. A trip means: fail the step and
  // cancel the queue.
  std::optional<Trip> pre_run(const RunSpec& run, const MetricContext& ctx, const Variables& vars = {});

  // After the run is saved, against the record's values. A cancel trip asks
  // to cancel the queue; a queue action is applied to `queue` after `current`.
  Result<std::optional<PostRunOutcome>> post_run(const RunSpec& run, const MetricContext& ctx, ExperimentQueue& queue,
                                                 std::size_t current, const Variables& vars = {},
                                                 const BlankFactory& blank = default_blank_factory(
                                                     IdentifierRules::defaults()));

  const std::vector<Trip>& trips() const noexcept { return engine_.trips(); }

 private:
  ConditionalEngine engine_;
};

}  // namespace pychron::experiment
