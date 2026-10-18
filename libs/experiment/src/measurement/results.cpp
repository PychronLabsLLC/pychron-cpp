#include "pychron/experiment/measurement/results.hpp"

#include <map>
#include <set>

#include "pychron/experiment/record/names.hpp"

namespace pychron::experiment::measurement {

namespace {

// Until the record carries gas, segment and position (AnalysisRecord v3), a
// record has one series per label, detector and kind: the collector's series
// that differ only in where in the plan they were measured are merged, in
// time order. Ordered as the v2 record always was: by label, detector, then
// signal, baseline, sniff, whiff (and any other kind after them, by name).
struct RecordKey {
  std::string label, detector;
  int rank = 0;
  std::string kind;
  friend auto operator<=>(const RecordKey&, const RecordKey&) = default;
};

int rank_of(std::string_view kind) {
  if (kind == collect::kSignal) return 0;
  if (kind == collect::kBaseline) return 1;
  if (kind == collect::kSniff) return 2;
  if (kind == collect::kWhiff) return 3;
  return 4;
}

std::map<RecordKey, collect::Series> record_series(const collect::RunData& data) {
  std::map<RecordKey, std::vector<const collect::Series*>> parts;
  for (const auto& [key, s] : data.series)
    parts[RecordKey{key.label, key.detector, rank_of(key.kind), key.kind}].push_back(&s);
  std::map<RecordKey, collect::Series> out;
  for (const auto& [key, p] : parts) out.emplace(key, collect::merge_series(p));
  return out;
}

}  // namespace

record::Data to_record_data(const collect::RunData& data) {
  record::Data out;
  const double t0 = data.timing.time_zero.value_or(0.0);
  out.time_zero = t0;
  out.counts = data.counts;
  for (const auto& [key, s] : record_series(data)) {
    record::DataSeries ds;
    ds.iso = key.label;
    ds.det = key.detector;
    ds.kind = key.kind;
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
  const auto merged = record_series(data);

  std::map<std::string, std::set<std::string>> detectors_of;
  for (const auto& [key, s] : merged) {
    if (key.kind == collect::kSignal) detectors_of[key.label].insert(key.detector);
  }

  for (const auto& [key, s] : merged) {
    if (key.kind != collect::kSignal) continue;
    // On several detectors: one intercept a detector, under a name of its own
    // (record/names.hpp says what an analysis then calls it).
    const std::string name = detectors_of[key.label].size() > 1
                                 ? record::acquisition_isotope_key(key.label, key.detector)
                                 : key.label;
    reduction::Series series;
    series.y = s.v;
    for (double t : s.t) series.x.push_back(t - t0);
    auto spec = plan::signal_fit(fits, key.label);
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
  for (const auto& [key, s] : merged) {
    if (key.kind != collect::kBaseline) continue;
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
