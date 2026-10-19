#include "pychron/experiment/measurement/results.hpp"

#include <algorithm>
#include <array>
#include <functional>
#include <cmath>
#include <map>
#include <sstream>
#include <set>
#include <tuple>
#include <utility>

#include "pychron/core/number.hpp"
#include "pychron/experiment/conditionals/evaluator.hpp"
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

const plan::Gas kNoGas{};

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

// The fit of a series at its place in the plan (plan::fit_for).
reduction::FitSpec fit_of(const RecordKey& key, const plan::MeasurementPlan& plan) {
  const auto* gas = gas_named(plan, key.gas);
  return plan::fit_for(plan, gas != nullptr ? *gas : kNoGas, segment_of(plan, key.gas, key.segment), key.kind,
                       key.label, key.detector);
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

// A modifier's expressions: intercept names ("Ar40", "Ar40@H1") for its
// variable, `x` for its model.
class ValuesContext final : public MetricContext {
 public:
  explicit ValuesContext(std::function<std::optional<double>(const std::string&)> value) : value_(std::move(value)) {}
  std::optional<std::vector<double>> series(const MetricRef& m) const override {
    if (auto v = scalar(m)) return std::vector<double>{*v};
    return std::nullopt;
  }
  std::optional<double> scalar(const MetricRef& m) const override {
    if (m.kind != MetricRef::Kind::Isotope) return std::nullopt;
    return value_(qualified_a(m));
  }
  std::optional<double> elapsed() const override { return std::nullopt; }

 private:
  std::function<std::optional<double>(const std::string&)> value_;
};

Result<double> evaluate_text(const std::string& text, const ValuesContext& ctx) {
  auto expr = parse_expression(text);
  if (!expr) return fail(expr.error());
  return evaluate(**expr, ctx, Variables{});
}

// A model CSV: lines "x, baseline" or "x, baseline, error"; '#' starts a
// comment line, and a first line that is not numbers is a header.
Result<std::vector<std::array<double, 3>>> read_model(const std::string& text) {
  std::vector<std::array<double, 3>> out;
  std::istringstream in(text);
  std::string line;
  int n = 0;
  while (std::getline(in, line)) {
    ++n;
    const auto first = line.find_first_not_of(" \t\r");
    if (first == std::string::npos || line[first] == '#') continue;
    std::vector<double> cells;
    bool numbers = true;
    std::size_t at = 0;
    while (at <= line.size()) {
      const auto comma = line.find(',', at);
      std::string cell = line.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
      const auto b = cell.find_first_not_of(" \t\r"), e = cell.find_last_not_of(" \t\r");
      cell = b == std::string::npos ? std::string() : cell.substr(b, e - b + 1);
      const auto v = parse_double(cell);
      if (!v) numbers = false;
      cells.push_back(v.value_or(0.0));
      if (comma == std::string::npos) break;
      at = comma + 1;
    }
    if (!numbers) {
      if (out.empty()) continue;  // the header
      return fail(ErrorKind::Config, "line " + std::to_string(n) + ": not numbers");
    }
    if (cells.size() != 2 && cells.size() != 3)
      return fail(ErrorKind::Config, "line " + std::to_string(n) + ": expected x, baseline[, error]");
    out.push_back({cells[0], cells[1], cells.size() == 3 ? cells[2] : 0.0});
  }
  if (out.size() < 2) return fail(ErrorKind::Config, "fewer than two points");
  return out;
}

// The weighted least-squares line through the model's points (weights
// 1/error^2; without errors, equal weights and the scatter's variance) at
// `x0`: value and standard error.
Result<std::pair<double, double>> model_at(const std::vector<std::array<double, 3>>& points, double x0) {
  const bool weighted = std::all_of(points.begin(), points.end(), [](const auto& p) { return p[2] > 0; });
  double sw = 0, sx = 0, sy = 0, sxx = 0, sxy = 0;
  for (const auto& [x, y, e] : points) {
    const double w = weighted ? 1.0 / (e * e) : 1.0;
    sw += w;
    sx += w * x;
    sy += w * y;
    sxx += w * x * x;
    sxy += w * x * y;
  }
  const double d = sw * sxx - sx * sx;
  if (!(std::abs(d) > 0)) return fail(ErrorKind::Config, "the model's points do not fix a line");
  const double a = (sxx * sy - sx * sxy) / d, b = (sw * sxy - sx * sy) / d;
  double scale = 1;
  if (!weighted) {
    double rss = 0;
    for (const auto& [x, y, e] : points) rss += (y - a - b * x) * (y - a - b * x);
    scale = points.size() > 2 ? rss / static_cast<double>(points.size() - 2) : 0.0;
  }
  const double var = scale * (sxx - 2 * x0 * sx + x0 * x0 * sw) / d;
  return std::make_pair(a + b * x0, std::sqrt(std::max(var, 0.0)));
}

}  // namespace

record::Data to_record_data(const collect::RunData& data, const plan::MeasurementPlan& plan,
                            const PositionStates& states) {
  record::Data out;
  const auto& timing = data.timing;
  const auto zero_of = [&](const std::string& gas) {
    const auto it = timing.time_zeros.find(gas);
    return it != timing.time_zeros.end() ? it->second : timing.time_zero.value_or(0.0);
  };
  out.time_zero = timing.time_zero.value_or(0.0);
  out.counts = data.counts;

  // Every gas measured, in plan order (others after, by name).
  std::vector<std::string> order;
  order.reserve(plan.gases.size());
  for (const auto& g : plan.gases) order.push_back(g.name);
  std::set<std::string> measured;
  for (const auto& [key, s] : data.series) measured.insert(key.gas);
  for (const auto& name : measured)
    if (std::find(order.begin(), order.end(), name) == order.end()) order.push_back(name);
  std::map<std::string, std::size_t> gas_at;
  for (const auto& name : order) {
    if (!measured.contains(name)) continue;
    gas_at[name] = out.gases.size();
    record::GasRec rec;
    rec.name = name;
    rec.time_zero = zero_of(name);
    out.gases.push_back(std::move(rec));
  }
  // A place's state as the record has it; a place with none, its gas's main's integration.
  const auto state_of = [&](const collect::SeriesKey& key) {
    record::SpectrometerState st;
    if (const auto it = states.find(Place{key.gas, key.segment, key.position}); it != states.end()) {
      st.deflections = it->second.deflections;
      st.integration_time = it->second.integration_s;
      st.optics = it->second.optics;
      st.table = it->second.table;
    } else if (const auto* g = gas_named(plan, key.gas)) {
      if (const auto* main = plan::main_segment(*g)) st.integration_time = main->integration_s;
    }
    return st;
  };
  const auto index_of = [&](const collect::SeriesKey& key) {
    auto& gas = out.gases[gas_at.at(key.gas)];
    const auto st = state_of(key);
    const auto it = std::find(gas.states.begin(), gas.states.end(), st);
    if (it != gas.states.end()) return static_cast<int>(it - gas.states.begin());
    gas.states.push_back(st);
    return static_cast<int>(gas.states.size() - 1);
  };

  for (const auto& [key, s] : record_series(data)) {
    const collect::SeriesKey sk{key.label, key.detector, key.kind, key.gas, key.segment, key.position};
    const double t0 = zero_of(key.gas);
    record::DataSeries ds(key.label, key.detector, key.kind, {}, key.gas, key.segment, key.position, index_of(sk));
    for (double t : s->t) ds.trace.t.push_back(static_cast<float>(t - t0));
    for (double v : s->v) ds.trace.v.push_back(static_cast<float>(v));
    for (double e : s->sigma) ds.trace.sigma.push_back(static_cast<float>(e));
    out.series.push_back(std::move(ds));
  }
  return out;
}

FitOutput fit_results(const collect::RunData& data, const plan::MeasurementPlan& plan,
                      std::string_view spectrometer_reference, const ModelFiles& model_files) {
  FitOutput out;
  // Each gas's series on its own time zero (spec 3.4).
  const auto zero_of = [&](const std::string& gas) {
    const auto it = data.timing.time_zeros.find(gas);
    return it != data.timing.time_zeros.end() ? it->second : data.timing.time_zero.value_or(0.0);
  };
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
  std::map<std::pair<std::string, std::string>, const RecordKey*> signal_first;  // its fit's segment
  std::set<std::pair<std::string, std::string>> across;  // read in more than one gas
  for (const auto& [key, s] : ordered) {
    if (key.kind != collect::kSignal) continue;
    const auto id = std::make_pair(key.label, key.detector);
    if (const auto first = signal_first.find(id); first != signal_first.end() && first->second->gas != key.gas)
      across.insert(id);
    signal_parts[id].push_back(s);
    signal_first.try_emplace(id, &key);
    if (!signals.contains(id)) {
      Signal sig;
      sig.key = record::qualified_key(key.label, key.detector);
      sig.detector = key.detector;
      sig.gas = key.gas;
      sig.at = target_of(key.gas, key.segment, key.position);
      signals.emplace(id, std::move(sig));
    }
  }
  for (const auto& id : across) {
    // Two gases' readings are not one series (the plan's rule, spec 3.7, keeps
    // a valid plan from this): neither is fitted as one.
    out.errors.push_back(record::qualified_key(id.first, id.second) +
                         ": signal read in more than one gas; not fitted as one series");
    signal_parts.erase(id);
    signals.erase(id);
  }
  for (const auto& [id, parts] : signal_parts) {
    const auto s = collect::merge_series(parts);
    auto& sig = signals.at(id);
    reduction::Series series;
    series.y = s.v;
    const double t0 = zero_of(sig.gas);  // a label on a detector is one gas's (spec 3.7)
    for (double t : s.t) series.x.push_back(t - t0);
    if (!s.t.empty()) {
      sig.first = s.t.front();
      sig.last = s.t.back();
      sig.mean_t = mean(s.t);
    }
    auto spec = fit_of(*signal_first.at(id), plan);
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
  std::map<Where, const RecordKey*> baseline_first;
  for (const auto& [key, s] : ordered) {
    if (key.kind != collect::kBaseline) continue;
    const Where where{key.detector, key.gas, key.segment, key.position};
    baseline_parts[where].push_back(s);
    baseline_first.try_emplace(where, &key);
  }
  std::vector<Baseline> baselines;
  for (const auto& [where, parts] : baseline_parts) {
    const auto& [det, gas, segment, position] = where;
    const auto s = collect::merge_series(parts);
    reduction::Series series;
    series.y = s.v;
    const double t0 = zero_of(gas);
    for (double t : s.t) series.x.push_back(t - t0);
    const auto spec = fit_of(*baseline_first.at(where), plan);
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
    if (!c) {
      // A run with baselines and none on this signal's detector: said.
      if (!baselines.empty()) out.errors.push_back(sig.key + ": no baseline on " + sig.detector);
      continue;
    }
    if (!out.results.baselines.contains(sig.detector)) {
      const auto& fit = baselines[c->baselines.front()].rec.result.fit;
      out.results.baselines[sig.detector] = record::BaselineResult{c->value, c->error, fit};
    }
    out.results.baseline_choices[sig.key] = std::move(*c);
  }
  for (const auto& b : baselines)
    if (!out.results.baselines.contains(b.rec.detector)) out.results.baselines[b.rec.detector] = b.rec.result;

  // Baseline modifiers (spec 3.5): evaluated and recorded; the baseline
  // itself is left as fitted (the reduction applies them).
  const ValuesContext intercepts([&](const std::string& name) -> std::optional<double> {
    const auto& ints = out.results.intercepts;
    if (name.find('@') != std::string::npos) {
      const auto it = ints.find(name);
      return it == ints.end() ? std::nullopt : std::optional(it->second.intercept.value);
    }
    const auto it = std::find_if(ints.begin(), ints.end(), [&](const auto& e) { return e.first.starts_with(name + "@"); });
    return it == ints.end() ? std::nullopt : std::optional(it->second.intercept.value);
  });
  for (const auto& [det, mod] : plan.baseline_modifiers) {
    const auto failed = [&, &d = det](const std::string& why) {
      std::string what = "baseline modifier " + d;
      what += ": ";
      what += why;
      out.errors.push_back(std::move(what));
    };
    auto x = evaluate_text(mod.variable, intercepts);
    if (!x) {
      failed("variable '" + mod.variable + "': " + x.error().what);
      continue;
    }
    record::BaselineModifierRec rec;
    rec.variable = mod.variable;
    rec.model = mod.model;
    rec.error = mod.error;
    if (plan::is_model_file(mod.model)) {
      const auto file = model_files.find(mod.model);
      if (file == model_files.end()) {
        failed("model file '" + mod.model + "' not read");
        continue;
      }
      auto points = read_model(file->second);
      auto at = points ? model_at(*points, *x) : Result<std::pair<double, double>>(fail(points.error()));
      if (!at) {
        failed(mod.model + ": " + at.error().what);
        continue;
      }
      rec.value = at->first;
      rec.error_value = at->second;
    } else {
      const double x0 = *x;
      const ValuesContext in_x([x0](const std::string& name) -> std::optional<double> {
        if (name == "x") return x0;
        return std::nullopt;
      });
      auto v = evaluate_text(mod.model, in_x);
      if (!v) {
        failed("model '" + mod.model + "': " + v.error().what);
        continue;
      }
      rec.value = *v;
    }
    out.results.baseline_modifiers[det] = std::move(rec);
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
