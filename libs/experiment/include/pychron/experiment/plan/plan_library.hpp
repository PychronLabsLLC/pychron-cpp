#pragma once

#include <string>
#include <vector>

// In-memory set of plan templates, exposed to queue validation as an IPlanResolver.

#include <map>
#include <mutex>
#include <tuple>

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

  // A plan loads the same for the same name, overrides and options while the
  // library holds the same templates, so each such load is kept and handed
  // out again (a queue check loads every run's plan, several times). add()
  // forgets them; a copy of the library starts with none. Safe to call from
  // several threads.
  Result<LoadedPlan> load(std::string_view name, const ParamOverrides& overrides, const LoadOptions& options = {}) const;

  bool has_plan(std::string_view name) const override;
  // nullopt when the plan is unknown or the overrides do not load.
  std::optional<Duration> plan_duration(std::string_view name, const ParamOverrides& overrides,
                                        bool advanced = false) const override;

 private:
  using LoadKey = std::tuple<std::string, ParamOverrides, bool>;
  // Loads already made. Copied or moved, it is empty: what it holds belongs
  // to the templates of the library it was made by.
  class LoadCache {
   public:
    LoadCache() = default;
    ~LoadCache() = default;
    LoadCache(const LoadCache&) {}
    LoadCache(LoadCache&&) noexcept {}
    LoadCache& operator=(const LoadCache& other);
    LoadCache& operator=(LoadCache&& other) noexcept;

    std::optional<Result<LoadedPlan>> find(const LoadKey& key) const;
    void insert(LoadKey key, const Result<LoadedPlan>& loaded);
    void clear();

   private:
    mutable std::mutex mutex_;
    std::map<LoadKey, Result<LoadedPlan>> loads_;
  };

  PlanResolvers resolvers_;
  DurationOptions durations_;
  std::map<std::string, PlanTemplate, std::less<>> templates_;
  mutable LoadCache cache_;  // load() is const: what it keeps is not the library's state
};

}  // namespace pychron::experiment::plan
