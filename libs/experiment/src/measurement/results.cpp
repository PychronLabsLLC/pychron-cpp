#include "pychron/experiment/measurement/results.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <tuple>
#include <utility>

#include "pychron/experiment/record/names.hpp"

namespace pychron::experiment::measurement {

namespace {

// A record's series in the order of the v2 record (by label, detector, then
// signal, baseline, sniff, whiff, any other kind by name), and within that by
// where in the plan they were read.
struct RecordKey {
  std::string label, detector;
  int rank = 0;
  std::string kind, gas;
  int segment = 0, position = 0;
  friend auto operator<=>(const RecordKey&, const RecordKey&) = default;
};

int rank_of(std::string_view kind) {
  if (kind == collect::kSignal) return 0;
  if (kind == collect::kBaseline) return 1;
  if (kind == collect::kSniff) return 2;
  if (kind == collect::kWhiff) return 3;
  return 4;
}

std::map<RecordKey, const collect::Series*> record_series(const collect::RunData& data) {
  std::map<RecordKey, const collect::Series*> out;
  for (const auto& [key, s] : data.series)
    out.emplace(RecordKey{key.label, key.detector, rank_of(key.kind), key.kind, key.gas, key.segment, key.position},
                &s);
  return out;
}

const plan::Gas* gas_named(const plan::MeasurementPlan& plan, const std::string& name) {
  for (const auto& g : plan.gases)
    if (g.name == name) return &g;
  return plan.gases.size() == 1 && name.empty() ? &plan.gases.front() : nullptr;
}

const plan::Segment* segment_of(const plan::MeasurementPlan& plan, const std::string& gas, int segment) {
  const auto* g = gas_named(plan, gas);
  if (g == nullptr || segment < 0 || static_cast<std::size_t>(segment) >= g->segments.size()) return nullptr;
  return &g->segments[static_cast<std::size_t>(segment)];
}

const plan::Position* position_of(const plan::MeasurementPlan& plan, const std::string& gas, int segment,
                                  int position) {
  const auto* seg = segment_of(plan, gas, segment);
  if (seg == nullptr || position < 0 || static_cast<std::size_t>(position) >= seg->positions.size()) return nullptr;
  return &seg->positions[static_cast<std::size_t>(position)];
}

double mean(const std::vector<double>& v) {
  if (v.empty()) return 0;
  double sum = 0;
  for (double x : v) sum += x;
  return sum / static_cast<double>(v.size());
}

// One baseline position of one detector, with what the rule reads of it.
struct Baseline {
  record::BaselineRec rec;
  std::optional<plan::At> at;
  double first = 0, last = 0, mean_t = 0;
};

// One signal series (a label on a detector, every position it was read at),
// with what the rule reads of it.
struct Signal {
  std::string key;  // "<label>@<detector>"
  std::string detector, gas;
  std::optional<plan::At> at;  // its first position's target
  double first = 0, last = 0, mean_t = 0;
};

// Spec 6.2: among the detector's baseline positions in the signal's gas (all
// of the detector's when it has none there), those whose `at` is the signal
// position's target, else all of them; two of those that bracket the signal
// (the latest to end before its first point, the earliest to start after its
// last) give their average, error sqrt(e1^2 + e2^2) / 2; otherwise the
// nearest by mean time. The rule recorded is "bracketing_average", else "at"
// when the candidates matched, else "nearest".
std::optional<record::BaselineChoice> choose(const Signal& sig, const std::vector<Baseline>& baselines) {
  std::vector<std::size_t> of_det, in_gas;
  for (std::size_t i = 0; i < baselines.size(); ++i) {
    if (baselines[i].rec.detector != sig.detector) continue;
    of_det.push_back(i);
    if (baselines[i].rec.gas == sig.gas) in_gas.push_back(i);
  }
  const auto& pool = in_gas.empty() ? of_det : in_gas;
  if (pool.empty()) return std::nullopt;
  std::vector<std::size_t> matching;
  if (sig.at)
    for (auto i : pool)
      if (baselines[i].at && *baselines[i].at == *sig.at) matching.push_back(i);
  const bool matched = !matching.empty();
  const auto& candidates = matched ? matching : pool;

  std::optional<std::size_t> before, after;
  for (auto i : candidates) {
    const auto& b = baselines[i];
    if (b.last < sig.first && (!before || b.last > baselines[*before].last)) before = i;
    if (b.first > sig.last && (!after || b.first < baselines[*after].first)) after = i;
  }
  record::BaselineChoice c;
  if (before && after) {
    const auto& b1 = baselines[*before].rec.result;
    const auto& b2 = baselines[*after].rec.result;
    c.rule = "bracketing_average";
    c.baselines = {*before, *after};
    c.value = (b1.value + b2.value) / 2;
    c.error = std::sqrt(b1.error * b1.error + b2.error * b2.error) / 2;
    return c;
  }
  std::size_t best = candidates.front();
  for (auto i : candidates)
    if (std::abs(baselines[i].mean_t - sig.mean_t) < std::abs(baselines[best].mean_t - sig.mean_t)) best = i;
  c.rule = matched ? "at" : "nearest";
  c.baselines = {best};
  c.value = baselines[best].rec.result.value;
  c.error = baselines[best].rec.result.error;
  return c;
}

}  // namespace

record::Data to_record_data(const collect::RunData& data, const plan::MeasurementPlan& plan) {
  record::Data out;
  const double t0 = data.timing.time_zero.value_or(0.0);
  out.time_zero = t0;
  out.counts = data.counts;
  std::set<std::string> gases;
  for (const auto& [key, s] : record_series(data)) {
    record::DataSeries ds(key.label, key.detector, key.kind, {}, key.gas, key.segment, key.position, 0);
    for (double t : s->t) ds.trace.t.push_back(static_cast<float>(t - t0));
    for (double v : s->v) ds.trace.v.push_back(static_cast<float>(v));
    for (double e : s->sigma) ds.trace.sigma.push_back(static_cast<float>(e));
    gases.insert(key.gas);
    out.series.push_back(std::move(ds));
  }
  // Every gas measured, in plan order, each with the run's time zero and one
  // state (Phase 6 of the v2 plan gives each gas its own).
  auto add_gas = [&](const std::string& name, const plan::Gas* g) {
    record::GasRec rec;
    rec.name = name;
    rec.time_zero = t0;
    record::SpectrometerState state;
    if (g != nullptr)
      if (const auto* main = plan::main_segment(*g)) state.integration_time = main->integration_s;
    rec.states.push_back(std::move(state));
    out.gases.push_back(std::move(rec));
  };
  for (const auto& g : plan.gases)
    if (gases.erase(g.name) == 1) add_gas(g.name, &g);
  for (const auto& name : gases) add_gas(name, nullptr);
  return out;
}

FitOutput fit_results(const collect::RunData& data, const plan::MeasurementPlan& plan,
                      std::string_view spectrometer_reference) {
  FitOutput out;
  const double t0 = data.timing.time_zero.value_or(0.0);
  const auto ordered = record_series(data);
  auto target_of = [&](const std::string& gas, int segment, int position) -> std::optional<plan::At> {
    const auto* pos = position_of(plan, gas, segment, position);
    if (pos == nullptr) return std::nullopt;
    return plan::position_target(plan, *pos, spectrometer_reference);
  };

  // Intercepts: a label on a detector, every position it was read at fitted
  // as one series in time order.
  std::map<std::pair<std::string, std::string>, std::vector<const collect::Series*>> signal_parts;
  std::map<std::pair<std::string, std::string>, Signal> signals;
  for (const auto& [key, s] : ordered) {
    if (key.kind != collect::kSignal) continue;
    const auto id = std::make_pair(key.label, key.detector);
    signal_parts[id].push_back(s);
    if (!signals.contains(id)) {
      Signal sig;
      sig.key = record::qualified_key(key.label, key.detector);
      sig.detector = key.detector;
      sig.gas = key.gas;
      sig.at = target_of(key.gas, key.segment, key.position);
      signals.emplace(id, std::move(sig));
    }
  }
  for (const auto& [id, parts] : signal_parts) {
    const auto s = collect::merge_series(parts);
    auto& sig = signals.at(id);
    reduction::Series series;
    series.y = s.v;
    for (double t : s.t) series.x.push_back(t - t0);
    if (!s.t.empty()) {
      sig.first = s.t.front();
      sig.last = s.t.back();
      sig.mean_t = mean(s.t);
    }
    auto spec = plan::signal_fit(plan.fits, id.first);
    // Too few points for the plan's fit (e.g. truncated early): average them;
    // the recorded fit says so.
    if (!series.x.empty() && series.x.size() < reduction::parameter_count(spec)) {
      spec.kind = reduction::FitKind::Average;
      spec.degree = 0;
    }
    auto r = reduction::fit(series, spec);
    if (!r) {
      out.errors.push_back(sig.key + ": " + r.error().what);
      continue;
    }
    out.results.intercepts[sig.key] = record::InterceptResult{*r, spec};
  }

  // Baselines: one a baseline position a detector (labels of one position on
  // one detector are one position).
  using Where = std::tuple<std::string, std::string, int, int>;  // detector, gas, segment, position
  std::map<Where, std::vector<const collect::Series*>> baseline_parts;
  for (const auto& [key, s] : ordered)
    if (key.kind == collect::kBaseline) baseline_parts[{key.detector, key.gas, key.segment, key.position}].push_back(s);
  std::vector<Baseline> baselines;
  for (const auto& [where, parts] : baseline_parts) {
    const auto& [det, gas, segment, position] = where;
    const auto s = collect::merge_series(parts);
    reduction::Series series;
    series.y = s.v;
    for (double t : s.t) series.x.push_back(t - t0);
    const auto spec = plan::baseline_fit(plan.fits, det);
    auto r = reduction::fit(series, spec);
    if (!r) {
      std::string what = "baseline " + det + " at ";
      if (!gas.empty()) what += gas + ".";
      what += "segment[" + std::to_string(segment) + "].positions[" + std::to_string(position) + "]: ";
      what += r.error().what;
      out.errors.push_back(std::move(what));
      continue;
    }
    Baseline b;
    b.rec.detector = det;
    b.rec.gas = gas;
    b.rec.segment = segment;
    b.rec.position = position;
    if (const auto* seg = segment_of(plan, gas, segment)) b.rec.when = std::string(plan::to_string(seg->when));
    b.rec.result = record::BaselineResult{r->value, r->error, spec};
    b.at = target_of(gas, segment, position);
    if (!s.t.empty()) {
      b.first = s.t.front();
      b.last = s.t.back();
      b.mean_t = mean(s.t);
    }
    baselines.push_back(std::move(b));
  }
  for (const auto& b : baselines) out.results.baseline_positions.push_back(b.rec);

  // Each signal's baseline; a detector's stored one is its first signal's,
  // or, on a detector with no signal, its first baseline position.
  for (const auto& [id, sig] : signals) {  // record order: by label, then detector
    (void)id;
    auto c = choose(sig, baselines);
    if (!c) continue;
    if (!out.results.baselines.contains(sig.detector)) {
      const auto& fit = baselines[c->baselines.front()].rec.result.fit;
      out.results.baselines[sig.detector] = record::BaselineResult{c->value, c->error, fit};
    }
    out.results.baseline_choices[sig.key] = std::move(*c);
  }
  for (const auto& b : baselines)
    if (!out.results.baselines.contains(b.rec.detector)) out.results.baselines[b.rec.detector] = b.rec.result;
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
