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
    const auto spec = plan::signal_fit(fits, key.isotope);
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

}  // namespace pychron::experiment::measurement
