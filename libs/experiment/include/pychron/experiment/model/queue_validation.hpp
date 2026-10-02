#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pychron/experiment/model/rules.hpp"

namespace pychron::experiment {

// Resolvers are injected so the model stays independent of where plans,
// scripts and conditionals live (template library, script dir, TOML files).
class IPlanResolver {
 public:
  virtual ~IPlanResolver() = default;
  virtual bool has_plan(std::string_view name) const = 0;
  // Measurement duration with overrides applied; nullopt if unknown or the
  // overrides do not load (`advanced`: any key, not only exposed ones).
  virtual std::optional<Duration> plan_duration(std::string_view name, const ParamOverrides& overrides,
                                                bool advanced) const = 0;
};

class IScriptResolver {
 public:
  virtual ~IScriptResolver() = default;
  // Extraction, post-equilibration/post-measurement scripts and measurement hooks.
  virtual bool has_script(std::string_view name) const = 0;
};

class IConditionalResolver {
 public:
  virtual ~IConditionalResolver() = default;
  virtual bool has_conditional(std::string_view name, std::string_view kind) const = 0;
  // Queue-level conditionals file (QueueSpec::queue_conditionals).
  virtual bool has_conditional_set(std::string_view name) const = 0;
};

// Any resolver may be null: its reference checks are skipped.
struct QueueResolvers {
  const IPlanResolver* plans = nullptr;
  const IScriptResolver* scripts = nullptr;
  const IConditionalResolver* conditionals = nullptr;
};

enum class Severity { Error, Warning };

struct Diagnostic {
  Severity severity = Severity::Error;
  int run = -1;  // index into QueueSpec::runs, -1 for queue-level
  std::string field;
  std::string message;
  friend bool operator==(const Diagnostic&, const Diagnostic&) = default;
};

struct QueueReport {
  std::vector<Diagnostic> diagnostics;
  std::vector<Duration> run_estimates;  // per run, 0 for skipped runs
  Duration eta{0};                      // queue_eta over non-skipped runs
  bool ok() const;                      // no Error diagnostics
};

// Schema/type rules (validate_queue), reference checks through the resolvers,
// fixed-aliquot uniqueness, end_after reachability, per-run estimates and ETA.
// All problems are collected; nothing stops at the first.
QueueReport check_queue(const QueueSpec& q, const IdentifierRules& ids, const QueueResolvers& resolvers);

}  // namespace pychron::experiment
