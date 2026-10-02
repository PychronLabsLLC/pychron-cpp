#include "pychron/experiment/measurement/results.hpp"

#include <map>
#include <set>

namespace pychron::experiment::measurement {

record::Data to_record_data(const collect::RunData& data) {
  record::Data out;
  const double t0 = data.timing.time_zero.value_or(0.0);
  out.time_zero = t0;
  out.counts = data.counts;
  for (const auto& [key, s] : data.series) {
    record::DataSeries ds;
    ds.iso = key.isotope;
    ds.det = key.detector;
    ds.kind = std::string(collect::to_string(key.kind));
    for (double t : s.t) ds.trace.t.push_back(static_cast<float>(t - t0));
    for (double v : s.v) ds.trace.v.push_back(static_cast<float>(v));
    for (double e : s.sigma) ds.trace.sigma.push_back(static_cast<float>(e));
    out.series.push_back(std::move(ds));
  }
  return out;
}

FitOutput fit_results(const collect::RunData& data, const plan::Fits& fits) {
  FitOutput out;
  const double t0 = data.timing.time_zero.value_or(0.0);

  std::map<std::string, std::set<std::string>> detectors_of;
  for (const auto& [key, s] : data.series) {
    if (key.kind == collect::SeriesKind::Signal) detectors_of[key.isotope].insert(key.detector);
  }

  for (const auto& [key, s] : data.series) {
    if (key.kind != collect::SeriesKind::Signal) continue;
    const std::string name = detectors_of[key.isotope].size() > 1 ? key.isotope + ":" + key.detector : key.isotope;
    reduction::Series series;
    series.y = s.v;
    for (double t : s.t) series.x.push_back(t - t0);
    auto spec = plan::signal_fit(fits, key.isotope);
    // Too few points for the plan's fit (e.g. truncated early): average them;
    // the recorded fit says so.
    if (!series.x.empty() && series.x.size() < reduction::parameter_count(spec)) {
      spec.kind = reduction::FitKind::Average;
      spec.degree = 0;
    }
    auto r = reduction::fit(series, spec);
    if (!r) {
      out.errors.push_back(name + ": " + r.error().what);
      continue;
    }
    out.results.intercepts[name] = record::InterceptResult{*r, spec};
  }

  std::map<std::string, reduction::Series> baselines;
  for (const auto& [key, s] : data.series) {
    if (key.kind != collect::SeriesKind::Baseline) continue;
    auto& b = baselines[key.detector];
    b.y.insert(b.y.end(), s.v.begin(), s.v.end());
    for (double t : s.t) b.x.push_back(t - t0);
  }
  for (const auto& [det, series] : baselines) {
    const auto spec = plan::baseline_fit(fits, det);
    auto r = reduction::fit(series, spec);
    if (!r) {
      out.errors.push_back("baseline " + det + ": " + r.error().what);
      continue;
    }
    out.results.baselines[det] = record::BaselineResult{r->value, r->error, spec};
  }
  return out;
}

record::InstalledConditional to_record(const Conditional& c) {
  record::InstalledConditional r;
  r.id = c.id();
  r.name = c.name;
  r.kind = std::string(to_string(c.kind));
  r.level = std::string(to_string(c.level));
  r.location = c.location;
  r.check = c.effective_check();
  r.start = c.start;
  r.frequency = c.frequency;
  r.ntrips = c.ntrips;
  r.window = c.window.value_or(0);
  r.mapper = c.mapper;
  r.analysis_types = c.analysis_types;
  r.abbreviated_count_ratio = c.abbreviated_count_ratio;
  r.action = to_string(c.action);
  r.resume = c.resume;
  r.truncate = c.truncate;
  r.terminate = c.terminate;
  return r;
}

record::TrippedConditional to_record(const Trip& t) {
  record::TrippedConditional r;
  r.id = t.id;
  r.name = t.name;
  r.kind = std::string(to_string(t.kind));
  r.check = t.check;
  r.action = to_string(t.action);
  r.reading = t.reading;
  r.count = t.count;
  r.t = t.ts;
  r.value = t.value;
  for (const auto& m : t.context) r.context[m.metric] = m.value;
  return r;
}

record::Conditionals to_record_conditionals(const std::vector<Conditional>& installed, const std::vector<Trip>& trips,
                                            const std::vector<ConditionalError>& errors) {
  record::Conditionals out;
  for (const auto& c : installed) out.installed.push_back(to_record(c));
  for (const auto& t : trips) out.tripped.push_back(to_record(t));
  for (const auto& e : errors) out.errors.push_back({e.name, e.message, e.count});
  return out;
}

}  // namespace pychron::experiment::measurement
