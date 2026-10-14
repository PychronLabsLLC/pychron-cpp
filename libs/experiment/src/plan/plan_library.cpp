#include "pychron/experiment/plan/plan_library.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace pychron::experiment::plan {

void PlanLibrary::add(PlanTemplate tmpl) {
  auto name = tmpl.name;
  templates_.insert_or_assign(std::move(name), std::move(tmpl));
  cache_.clear();
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
  // A NaN does not order, so overrides holding one are not a key.
  const bool keyable = std::ranges::none_of(overrides, [](const auto& kv) {
    const auto* d = std::get_if<double>(&kv.second);
    return d != nullptr && std::isnan(*d);
  });
  if (!keyable) return load_plan(*t, overrides, resolvers_, options);
  LoadKey key{std::string(name), overrides, options.advanced};
  if (auto kept = cache_.find(key)) return std::move(*kept);
  auto loaded = load_plan(*t, overrides, resolvers_, options);
  cache_.insert(std::move(key), loaded);
  return loaded;
}

bool PlanLibrary::has_plan(std::string_view name) const { return find(name) != nullptr; }

std::optional<Duration> PlanLibrary::plan_duration(std::string_view name, const ParamOverrides& overrides,
                                                   bool advanced) const {
  auto loaded = load(name, overrides, LoadOptions{advanced});
  if (!loaded) return std::nullopt;
  return estimate_duration(loaded->plan, durations_);
}

PlanLibrary::LoadCache& PlanLibrary::LoadCache::operator=(const LoadCache& other) {
  if (this != &other) clear();
  return *this;
}

PlanLibrary::LoadCache& PlanLibrary::LoadCache::operator=(LoadCache&& other) noexcept {
  if (this != &other) clear();
  return *this;
}

std::optional<Result<LoadedPlan>> PlanLibrary::LoadCache::find(const LoadKey& key) const {
  const std::lock_guard lock(mutex_);
  auto it = loads_.find(key);
  if (it == loads_.end()) return std::nullopt;
  return it->second;
}

void PlanLibrary::LoadCache::insert(LoadKey key, const Result<LoadedPlan>& loaded) {
  const std::lock_guard lock(mutex_);
  loads_.insert_or_assign(std::move(key), loaded);
}

void PlanLibrary::LoadCache::clear() {
  const std::lock_guard lock(mutex_);
  loads_.clear();
}

}  // namespace pychron::experiment::plan
