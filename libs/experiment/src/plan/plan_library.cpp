#include "pychron/experiment/plan/plan_library.hpp"

namespace pychron::experiment::plan {

void PlanLibrary::add(PlanTemplate tmpl) {
  auto name = tmpl.name;
  templates_.insert_or_assign(std::move(name), std::move(tmpl));
}

std::vector<std::string> PlanLibrary::names() const {
  std::vector<std::string> out;
  for (const auto& [name, t] : templates_) out.push_back(name);
  return out;
}

const PlanTemplate* PlanLibrary::find(std::string_view name) const {
  auto it = templates_.find(name);
  return it == templates_.end() ? nullptr : &it->second;
}

Result<LoadedPlan> PlanLibrary::load(std::string_view name, const ParamOverrides& overrides,
                                     const LoadOptions& options) const {
  const auto* t = find(name);
  if (!t) return fail(ErrorKind::Config, "unknown measurement plan '" + std::string(name) + "'");
  return load_plan(*t, overrides, resolvers_, options);
}

bool PlanLibrary::has_plan(std::string_view name) const { return find(name) != nullptr; }

std::optional<Duration> PlanLibrary::plan_duration(std::string_view name, const ParamOverrides& overrides) const {
  auto loaded = load(name, overrides);
  if (!loaded) return std::nullopt;
  return estimate_duration(loaded->plan, durations_);
}

}  // namespace pychron::experiment::plan
