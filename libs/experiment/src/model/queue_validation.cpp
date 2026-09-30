#include "pychron/experiment/model/queue_validation.hpp"

#include <algorithm>
#include <map>
#include <tuple>

namespace pychron::experiment {
namespace {

class Collector {
 public:
  explicit Collector(std::vector<Diagnostic>& out) : out_(out) {}
  void error(int run, std::string field, std::string msg) { add(Severity::Error, run, std::move(field), std::move(msg)); }
  void warn(int run, std::string field, std::string msg) { add(Severity::Warning, run, std::move(field), std::move(msg)); }

 private:
  void add(Severity s, int run, std::string field, std::string msg) {
    out_.push_back({s, run, std::move(field), std::move(msg)});
  }
  std::vector<Diagnostic>& out_;
};

void check_references(const RunSpec& r, int i, const QueueResolvers& res, Collector& c) {
  if (res.plans && !r.measurement.plan.empty() && !res.plans->has_plan(r.measurement.plan))
    c.error(i, "measurement.plan", "unknown measurement plan '" + r.measurement.plan + "'");
  if (res.scripts) {
    auto script = [&](const std::string& name, const char* field) {
      if (!name.empty() && !res.scripts->has_script(name))
        c.error(i, field, std::string("unknown script '") + name + "'");
    };
    script(r.extraction.script, "extraction.script");
    if (r.post_equilibration) script(*r.post_equilibration, "post_equilibration");
    if (r.post_measurement) script(*r.post_measurement, "post_measurement");
    if (r.measurement.hook) script(*r.measurement.hook, "measurement.hook");
  }
  if (res.conditionals)
    for (const auto& ref : r.conditionals)
      if (!res.conditionals->has_conditional(ref.name, ref.kind))
        c.error(i, "conditionals", "unknown " + ref.kind + " conditional '" + ref.name + "'");
}

Duration measurement_time(const RunSpec& r, const QueueResolvers& res) {
  if (!res.plans || r.measurement.plan.empty()) return Duration(0);
  return res.plans->plan_duration(r.measurement.plan, r.measurement.overrides).value_or(Duration(0));
}

}  // namespace

bool QueueReport::ok() const {
  return std::none_of(diagnostics.begin(), diagnostics.end(),
                      [](const Diagnostic& d) { return d.severity == Severity::Error; });
}

QueueReport check_queue(const QueueSpec& q, const IdentifierRules& ids, const QueueResolvers& res) {
  QueueReport report;
  Collector c(report.diagnostics);

  for (auto& issue : validate_queue(q, ids)) c.error(issue.run, std::move(issue.field), std::move(issue.message));

  if (q.runs.empty()) c.warn(-1, "runs", "queue has no runs");
  if (res.conditionals && !q.queue_conditionals.empty() && !res.conditionals->has_conditional_set(q.queue_conditionals))
    c.error(-1, "queue.queue_conditionals", "unknown queue conditionals '" + q.queue_conditionals + "'");

  std::map<std::tuple<std::string, int, std::string>, int> fixed;  // user-fixed aliquots -> first run
  int first_end_after = -1;
  for (std::size_t n = 0; n < q.runs.size(); ++n) {
    const auto& r = q.runs[n];
    const int i = static_cast<int>(n);
    check_references(r, i, res, c);

    if (r.id.aliquot) {
      auto [it, inserted] = fixed.try_emplace({r.id.identifier, *r.id.aliquot, r.id.step}, i);
      if (!inserted)
        c.error(i, "aliquot", "aliquot " + std::to_string(*r.id.aliquot) + r.id.step + " of '" + r.id.identifier +
                                  "' already used by run " + std::to_string(it->second));
    }
    if (r.end_after && !r.skip) {
      if (first_end_after >= 0)
        c.warn(i, "end_after", "unreachable: queue ends after run " + std::to_string(first_end_after));
      else
        first_end_after = i;
    }
    report.run_estimates.push_back(r.skip ? Duration(0) : estimate_run(r, measurement_time(r, res)));
  }

  report.eta = queue_eta(q, [&](const RunSpec& r) { return measurement_time(r, res); });
  return report;
}

}  // namespace pychron::experiment
