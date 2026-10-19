#include "pychron/experiment/collect/collector.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <numeric>

namespace pychron::experiment::collect {

namespace {

// "Ar40@H1" -> ("Ar40", "H1"); "Ar40" -> ("Ar40", "").
std::pair<std::string_view, std::string_view> split_name(std::string_view name) {
  const auto at = name.find('@');
  if (at == std::string_view::npos) return {name, {}};
  return {name.substr(0, at), name.substr(at + 1)};
}

// The kinds a label's live value is read from, in order of preference.
constexpr std::array<std::string_view, 3> kLabelKinds{kSignal, kSniff, kWhiff};

}  // namespace

std::string to_string(const SeriesKey& key) {
  return key.label + "@" + key.detector + ":" + key.kind + ":" + key.gas + ":" + std::to_string(key.segment) + "." +
         std::to_string(key.position);
}

Series merge_series(const std::vector<const Series*>& parts) {
  Series out;
  if (parts.size() == 1) return *parts.front();
  struct Point {
    double t;
    const Series* s;
    std::size_t i;
  };
  std::vector<Point> points;
  bool any_sigma = false;
  for (const Series* s : parts) {
    any_sigma |= !s->sigma.empty();
    for (std::size_t i = 0; i < s->t.size(); ++i) points.push_back({s->t[i], s, i});
  }
  std::stable_sort(points.begin(), points.end(), [](const Point& a, const Point& b) { return a.t < b.t; });
  out.t.reserve(points.size());
  out.v.reserve(points.size());
  out.saturated.reserve(points.size());
  for (const auto& p : points) {
    out.t.push_back(p.t);
    out.v.push_back(p.s->v[p.i]);
    if (any_sigma) out.sigma.push_back(p.i < p.s->sigma.size() ? p.s->sigma[p.i] : 0.0);
    out.saturated.push_back(p.i < p.s->saturated.size() && p.s->saturated[p.i]);
  }
  return out;
}

std::optional<Series> merged_series(const RunData& data, std::string_view label, std::string_view detector,
                                    std::string_view kind) {
  std::vector<const Series*> parts;
  for (const auto& [key, s] : data.series)
    if (key.label == label && key.detector == detector && key.kind == kind) parts.push_back(&s);
  if (parts.empty()) return std::nullopt;
  return merge_series(parts);
}

Collector::Collector(const Clock& clock, SignalBus* bus) : clock_(clock), bus_(bus) {
  data_.timing.epoch = clock_.now();
}

void Collector::start(TimePoint epoch) {
  std::lock_guard lock(mutex_);
  data_ = RunData{};
  data_.timing.epoch = epoch;
  resumed_at_.reset();
  first_gas_.reset();
}

TimePoint Collector::epoch() const {
  std::lock_guard lock(mutex_);
  return data_.timing.epoch;
}

double Collector::seconds(TimePoint t) const {
  std::lock_guard lock(mutex_);
  return std::chrono::duration<double>(t - data_.timing.epoch).count();
}

void Collector::set_time_zero(const std::string& gas, double s) {
  std::lock_guard lock(mutex_);
  data_.timing.time_zeros[gas] = s;
  // time_zero() is the first gas's: the gas first given one, set again with it.
  if (!first_gas_) first_gas_ = gas;
  if (*first_gas_ == gas) data_.timing.time_zero = s;
}

void Collector::set_time_zero(double s) {
  std::lock_guard lock(mutex_);
  data_.timing.time_zero = s;
}

std::optional<double> Collector::time_zero() const {
  std::lock_guard lock(mutex_);
  return data_.timing.time_zero;
}

std::optional<double> Collector::time_zero(const std::string& gas) const {
  std::lock_guard lock(mutex_);
  const auto it = data_.timing.time_zeros.find(gas);
  if (it == data_.timing.time_zeros.end()) return std::nullopt;
  return it->second;
}

double Collector::time_zero_locked(const std::string& gas) const {
  const auto it = data_.timing.time_zeros.find(gas);
  if (it != data_.timing.time_zeros.end()) return it->second;
  return data_.timing.time_zero.value_or(0.0);
}

void Collector::set_inlet_open(double s) {
  std::lock_guard lock(mutex_);
  data_.timing.inlet_open = s;
}

void Collector::set_inlet_close(double s) {
  std::lock_guard lock(mutex_);
  data_.timing.inlet_close = s;
}

void Collector::begin(CollectionSpec spec, ReadingHook hook) {
  std::lock_guard lock(mutex_);
  target_ = std::max(spec.counts, 0);
  truncate_ = false;
  truncated_ = false;
  count_ = 0;
  spec_ = std::move(spec);
  hook_ = std::move(hook);
}

CollectStatus Collector::add(const spectrometer::Reading& reading) {
  SeriesUpdated update;
  ReadingHook hook;
  int count = 0;
  double t = 0;
  {
    std::lock_guard lock(mutex_);
    t = std::chrono::duration<double>(reading.ts - data_.timing.epoch).count();
    for (const auto& ch : spec_.channels) {
      auto it = reading.values.find(ch.detector);
      if (it == reading.values.end() || !it->second) continue;
      const auto& value = *it->second;
      SeriesKey key{ch.label, ch.detector, spec_.kind, spec_.gas, spec_.segment, spec_.position};
      auto& s = data_.series[key];
      // sigma stays parallel to v once any point carries one.
      if (value.sigma && s.sigma.size() < s.v.size()) s.sigma.resize(s.v.size(), 0.0);
      s.t.push_back(t);
      s.v.push_back(value.mean);
      if (value.sigma || !s.sigma.empty()) s.sigma.push_back(value.sigma.value_or(0.0));
      s.saturated.push_back(value.saturated);
      update.values.emplace_back(std::move(key), value.mean);
    }
    count = ++count_;
    ++data_.counts[spec_.label];
    update.label = spec_.label;
    update.kind = spec_.kind;
    update.count = count;
    update.target = target_;
    update.t = t;
    hook = hook_;
  }
  if (bus_ != nullptr) {
    bus_->publish(update);
    if (update.kind == kSignal || update.kind == kBaseline) publish_fits(update);
  }
  if (hook && hook(count, t)) truncate();

  std::lock_guard lock(mutex_);
  if (truncate_) {
    truncated_ = true;
    return CollectStatus::Truncated;
  }
  return count_ >= target_ ? CollectStatus::Complete : CollectStatus::Running;
}

void Collector::publish_fits(const SeriesUpdated& update) {
  FitsUpdated out;
  out.label = update.label;
  out.kind = update.kind;
  struct Work {
    SeriesKey key;
    reduction::Series series;
    reduction::FitSpec spec;
  };
  std::vector<Work> work;
  {
    std::lock_guard lock(mutex_);
    out.time_zero = update.values.empty() ? time_zero_locked({}) : time_zero_locked(update.values.front().first.gas);
    for (const auto& [key, value] : update.values) {
      // A signal series is fitted with the rest of its label on its detector,
      // as the intercept is (with the fit of its first part's segment); a
      // baseline series as it is.
      std::optional<Series> merged;
      SeriesKey first = key;
      if (key.kind == kBaseline) {
        auto it = data_.series.find(key);
        if (it != data_.series.end()) merged = it->second;
      } else {
        merged = isotope_series_locked(key.label + "@" + key.detector, key.kind, nullptr, &first);
      }
      if (!merged) continue;
      reduction::Series s;
      s.y = std::move(merged->v);
      s.x.reserve(merged->t.size());
      const double t0 = time_zero_locked(key.gas);
      for (double t : merged->t) s.x.push_back(t - t0);
      work.push_back({key, std::move(s), fit_spec_locked(first)});
    }
  }
  // Fitted outside the lock: readers (conditionals, the engine) are not held up.
  for (auto& [key, series, spec] : work) {
    if (series.x.size() < reduction::parameter_count(spec)) spec = reduction::FitSpec{reduction::FitKind::Average};
    if (auto r = reduction::fit(series, spec)) out.fits.push_back({key, std::move(*r)});
  }
  if (!out.fits.empty()) bus_->publish(out);
}

int Collector::finish() {
  std::lock_guard lock(mutex_);
  hook_ = {};
  return count_;
}

void Collector::set_target(int counts) { target_ = std::max(counts, 1); }

int Collector::target() const { return target_; }

int Collector::count() const {
  std::lock_guard lock(mutex_);
  return count_;
}

void Collector::truncate() { truncate_ = true; }

bool Collector::truncated() const {
  std::lock_guard lock(mutex_);
  return truncated_;
}

void Collector::add_trips(const std::vector<Trip>& trips) {
  std::lock_guard lock(mutex_);
  data_.trips.insert(data_.trips.end(), trips.begin(), trips.end());
}

RunData Collector::data() const {
  std::lock_guard lock(mutex_);
  return data_;
}

std::optional<Series> Collector::series(const SeriesKey& key) const {
  std::lock_guard lock(mutex_);
  auto it = data_.series.find(key);
  if (it == data_.series.end()) return std::nullopt;
  return it->second;
}

std::optional<Series> Collector::series_of(const std::string& name, std::string_view kind) const {
  std::lock_guard lock(mutex_);
  return isotope_series_locked(name, kind);
}

std::optional<reduction::Series> Collector::fit_series(const SeriesKey& key) const {
  std::lock_guard lock(mutex_);
  auto it = data_.series.find(key);
  if (it == data_.series.end()) return std::nullopt;
  const double t0 = time_zero_locked(key.gas);
  reduction::Series out;
  out.y = it->second.v;
  out.x.reserve(it->second.t.size());
  for (double t : it->second.t) out.x.push_back(t - t0);
  return out;
}

// ---- metrics ----------------------------------------------------------------

void Collector::set_plan(plan::MeasurementPlan plan) {
  std::lock_guard lock(mutex_);
  plan_ = std::move(plan);
}

void Collector::set_fits(plan::Fits fits) {
  std::lock_guard lock(mutex_);
  plan_ = plan::MeasurementPlan{};
  plan_.fits = std::move(fits);
}

reduction::FitSpec Collector::fit_spec_locked(const SeriesKey& key) const {
  const plan::Gas* gas = nullptr;
  for (const auto& g : plan_.gases)
    if (g.name == key.gas) gas = &g;
  if (gas == nullptr) {
    static const plan::Gas kNone{};
    return plan::fit_for(plan_, kNone, nullptr, key.kind, key.label, key.detector);
  }
  const plan::Segment* segment = nullptr;
  if (key.segment >= 0 && static_cast<std::size_t>(key.segment) < gas->segments.size())
    segment = &gas->segments[static_cast<std::size_t>(key.segment)];
  return plan::fit_for(plan_, *gas, segment, key.kind, key.label, key.detector);
}

void Collector::set_icfactors(std::map<std::string, double> icfactors) {
  std::lock_guard lock(mutex_);
  icfactors_ = std::move(icfactors);
}

void Collector::set_arar(std::optional<reduction::ArArConstants> constants) {
  std::lock_guard lock(mutex_);
  arar_ = std::move(constants);
}

std::optional<reduction::Intercept> Collector::intercept(const std::string& isotope) const {
  std::lock_guard lock(mutex_);
  return intercept_locked(isotope);
}

std::optional<Series> Collector::isotope_series_locked(const std::string& iso, std::string_view kind,
                                                      std::string* det, SeriesKey* first) const {
  const auto [label, qualifier] = split_name(iso);
  // The detector: the one named, else the one with the most recent point.
  std::string_view detector = qualifier;
  if (detector.empty()) {
    double latest = 0;
    bool found = false;
    for (const auto& [key, s] : data_.series) {
      if (key.kind != kind || key.label != label || s.v.empty()) continue;
      if (!found || s.t.back() > latest) {
        latest = s.t.back();
        detector = key.detector;
        found = true;
      }
    }
    if (!found) return std::nullopt;
  }
  std::vector<const Series*> parts;
  for (const auto& [key, s] : data_.series) {
    if (key.kind != kind || key.label != label || key.detector != detector || s.v.empty()) continue;
    if (parts.empty() && first != nullptr) *first = key;
    parts.push_back(&s);
  }
  if (parts.empty()) return std::nullopt;
  if (det != nullptr) *det = std::string(detector);
  return merge_series(parts);
}

std::optional<std::vector<double>> Collector::isotope_values_locked(const std::string& iso,
                                                                    std::string_view kind) const {
  auto s = isotope_series_locked(iso, kind);
  if (!s) return std::nullopt;
  return std::move(s->v);
}

std::optional<std::string> Collector::detector_of_locked(const std::string& iso) const {
  std::string det;
  for (auto kind : kLabelKinds)
    if (isotope_series_locked(iso, kind, &det)) return det;
  return std::nullopt;
}

std::optional<std::vector<double>> Collector::baseline_values_locked(const std::string& det) const {
  std::vector<double> out;
  for (const auto& [key, s] : data_.series) {
    if (key.kind == kBaseline && key.detector == det) out.insert(out.end(), s.v.begin(), s.v.end());
  }
  if (out.empty()) return std::nullopt;
  return out;
}

double Collector::baseline_mean_locked(const std::string& det) const {
  auto v = baseline_values_locked(det);
  if (!v) return 0.0;
  return std::accumulate(v->begin(), v->end(), 0.0) / static_cast<double>(v->size());
}

double Collector::icfactor_locked(const std::string& det) const {
  auto it = icfactors_.find(det);
  return it == icfactors_.end() ? 1.0 : it->second;
}

std::optional<reduction::Intercept> Collector::intercept_locked(const std::string& iso, std::string* det) const {
  std::optional<Series> s;
  SeriesKey first;
  for (auto kind : kLabelKinds) {
    s = isotope_series_locked(iso, kind, det, &first);
    if (s) break;
  }
  if (!s) return std::nullopt;
  const double t0 = time_zero_locked(first.gas);
  reduction::Series series;
  series.y = std::move(s->v);
  series.x.reserve(s->t.size());
  for (double t : s->t) series.x.push_back(t - t0);
  auto spec = fit_spec_locked(first);
  if (series.x.size() < reduction::parameter_count(spec)) spec = reduction::FitSpec{reduction::FitKind::Average};
  auto r = reduction::fit(series, spec);
  if (!r) return std::nullopt;
  return *r;
}

std::optional<double> Collector::corrected_locked(const std::string& iso) const {
  std::string det;
  auto i = intercept_locked(iso, &det);
  if (!i) return std::nullopt;
  return (i->value - baseline_mean_locked(det)) * icfactor_locked(det);
}

std::optional<double> Collector::computed_locked(const std::string& name) const {
  if (!arar_) return std::nullopt;
  reduction::ArArIntensities in;
  auto value = [&](const std::string& iso) -> std::optional<double> {
    if (name != "instant_age") return corrected_locked(iso);
    std::string det;
    const auto s = isotope_series_locked(iso, kSignal, &det);
    if (!s) return std::nullopt;
    return (s->v.back() - baseline_mean_locked(det)) * icfactor_locked(det);
  };
  in.ar36 = value("Ar36");
  in.ar37 = value("Ar37");
  in.ar38 = value("Ar38");
  in.ar39 = value("Ar39");
  in.ar40 = value("Ar40");
  auto all = reduction::compute_arar(in, *arar_);
  auto it = all.find(name == "instant_age" ? "age" : name);
  if (it == all.end()) return std::nullopt;
  return it->second;
}

std::optional<std::vector<double>> Collector::Metrics::series(const MetricRef& m) const {
  using K = MetricRef::Kind;
  const std::string qa = qualified_a(m), qb = qualified_b(m);
  {
    std::lock_guard lock(c_.mutex_);
    switch (m.kind) {
      case K::Isotope:
        for (auto kind : kLabelKinds)
          if (auto v = c_.isotope_values_locked(qa, kind)) return v;
        break;
      case K::Ratio: {
        auto a = c_.isotope_values_locked(qa, kSignal);
        auto b = c_.isotope_values_locked(qb, kSignal);
        if (!a || !b) break;
        const std::size_t n = std::min(a->size(), b->size());
        std::vector<double> out;
        out.reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
          const double den = (*b)[b->size() - n + i];
          if (den == 0.0) return std::nullopt;
          out.push_back((*a)[a->size() - n + i] / den);
        }
        return out;
      }
      case K::IsotopeField: {
        auto det = c_.detector_of_locked(qa);
        if (!det) break;
        if (m.field == "bs") return c_.baseline_values_locked(*det);
        std::optional<std::vector<double>> v;
        for (auto kind : kLabelKinds) {
          v = c_.isotope_values_locked(qa, kind);
          if (v) break;
        }
        if (!v) break;
        if (m.field == "bs_corrected" || m.field == "ic_corrected") {
          const double bs = c_.baseline_mean_locked(*det);
          const double ic = m.field == "ic_corrected" ? c_.icfactor_locked(*det) : 1.0;
          for (auto& x : *v) x = (x - bs) * ic;
        }
        return v;  // cur, intercept, std_dev: the raw points
      }
      default: break;
    }
  }
  if (fallback != nullptr) return fallback->series(m);
  return std::nullopt;
}

std::optional<double> Collector::Metrics::scalar(const MetricRef& m) const {
  using K = MetricRef::Kind;
  const std::string qa = qualified_a(m), qb = qualified_b(m);
  {
    std::lock_guard lock(c_.mutex_);
    switch (m.kind) {
      case K::Isotope:
        if (auto v = c_.corrected_locked(qa)) return v;
        break;
      case K::Ratio: {
        auto a = c_.corrected_locked(qa), b = c_.corrected_locked(qb);
        if (a && b && *b != 0.0) return *a / *b;
        break;
      }
      case K::IsotopeField: {
        if (m.field == "bs") break;
        std::string det;
        if (m.field == "cur") {
          for (auto kind : kLabelKinds)
            if (const auto s = c_.isotope_series_locked(qa, kind)) return s->v.back();
          break;
        }
        auto i = c_.intercept_locked(qa, &det);
        if (!i) break;
        if (m.field == "intercept") return i->value;
        if (m.field == "std_dev") return i->error;
        const double bs = i->value - c_.baseline_mean_locked(det);
        return m.field == "ic_corrected" ? bs * c_.icfactor_locked(det) : bs;
      }
      case K::DetectorField:
        if (m.field == "intensity") {
          const Series* best = nullptr;
          for (const auto& [key, s] : c_.data_.series) {
            if (key.detector != m.a || s.v.empty()) continue;
            if (best == nullptr || s.t.back() > best->t.back()) best = &s;
          }
          if (best != nullptr) return best->v.back();
        }
        break;
      case K::Computed:
        if (auto v = c_.computed_locked(m.a)) return v;
        break;
      default: break;
    }
  }
  if (fallback != nullptr) return fallback->scalar(m);
  return std::nullopt;
}

void Collector::mark_gap(double s) {
  std::lock_guard lock(mutex_);
  resumed_at_ = s;
}

std::size_t Collector::Metrics::contiguous_from(const MetricRef& m) const {
  using K = MetricRef::Kind;
  std::lock_guard lock(c_.mutex_);
  if (!c_.resumed_at_) return 0;
  const double at = *c_.resumed_at_;
  // Points of `s` before the gap's end.
  const auto before = [at](const Series& s) {
    return static_cast<std::size_t>(std::lower_bound(s.t.begin(), s.t.end(), at) - s.t.begin());
  };
  const auto isotope = [&](const std::string& iso) -> std::optional<Series> {
    for (auto kind : kLabelKinds)
      if (auto s = c_.isotope_series_locked(iso, kind)) return s;
    return std::nullopt;
  };
  switch (m.kind) {
    case K::Isotope:
    case K::IsotopeField: {
      if (m.kind == K::IsotopeField && m.field == "bs") return 0;
      const auto s = isotope(qualified_a(m));
      return s ? before(*s) : 0;
    }
    case K::Ratio: {
      const auto a = c_.isotope_series_locked(qualified_a(m), kSignal);
      const auto b = c_.isotope_series_locked(qualified_b(m), kSignal);
      if (!a || !b) return 0;
      // The ratio is the tail both have: n points, of which these came after.
      const std::size_t n = std::min(a->v.size(), b->v.size());
      const std::size_t after = std::min(a->t.size() - before(*a), b->t.size() - before(*b));
      return n > after ? n - after : 0;
    }
    default: return 0;
  }
}

std::optional<double> Collector::Metrics::elapsed() const {
  const double now = c_.seconds(c_.clock_.now());
  std::lock_guard lock(c_.mutex_);
  return now - c_.time_zero_locked(c_.spec_.gas);
}

}  // namespace pychron::experiment::collect
