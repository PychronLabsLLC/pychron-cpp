#pragma once

#include <string>
#include <vector>

// In-memory set of plan templates, exposed to queue validation as an IPlanResolver.

#include <map>

#include "pychron/experiment/model/queue_validation.hpp"
#include "pychron/experiment/plan/duration.hpp"
#include "pychron/experiment/plan/plan_loader.hpp"

namespace pychron::experiment::plan {

class PlanLibrary : public IPlanResolver {
 public:
  explicit PlanLibrary(PlanResolvers resolvers = {}, DurationOptions durations = {})
      : resolvers_(resolvers), durations_(durations) {}

  // Keyed by PlanTemplate::name; a later template with the same name replaces the earlier.
  void add(PlanTemplate tmpl);
  const PlanTemplate* find(std::string_view name) const;
  std::vector<std::string> names() const;  // sorted

  Result<LoadedPlan> load(std::string_view name, const ParamOverrides& overrides, const LoadOptions& options = {}) const;

  bool has_plan(std::string_view name) const override;
  // nullopt when the plan is unknown or the overrides do not load.
  std::optional<Duration> plan_duration(std::string_view name, const ParamOverrides& overrides,
                                        bool advanced = false) const override;

 private:
  PlanResolvers resolvers_;
  DurationOptions durations_;
  std::map<std::string, PlanTemplate, std::less<>> templates_;
};

}  // namespace pychron::experiment::plan
