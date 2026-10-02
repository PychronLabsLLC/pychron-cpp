#include "pychron/experiment/collect/collector.hpp"

#include <algorithm>
#include <chrono>
#include <numeric>

namespace pychron::experiment::collect {

std::string_view to_string(SeriesKind kind) noexcept {
  switch (kind) {
    case SeriesKind::Signal: return "signal";
    case SeriesKind::Baseline: return "baseline";
    case SeriesKind::Sniff: return "sniff";
    case SeriesKind::Whiff: return "whiff";
  }
  return "signal";
}

std::string to_string(const SeriesKey& key) {
  return key.isotope + ":" + key.detector + ":" + std::string(to_string(key.kind));
}

Collector::Collector(const Clock& clock, SignalBus* bus) : clock_(clock), bus_(bus) {
  data_.timing.epoch = clock_.now();
}

void Collector::start(TimePoint epoch) {
  std::lock_guard lock(mutex_);
  data_ = RunData{};
  data_.timing.epoch = epoch;
}

TimePoint Collector::epoch() const {
  std::lock_guard lock(mutex_);
  return data_.timing.epoch;
}

double Collector::seconds(TimePoint t) const {
  std::lock_guard lock(mutex_);
  return std::chrono::duration<double>(t - data_.timing.epoch).count();
}

void Collector::set_time_zero(double s) {
  std::lock_guard lock(mutex_);
  data_.timing.time_zero = s;
}

std::optional<double> Collector::time_zero() const {
  std::lock_guard lock(mutex_);
  return data_.timing.time_zero;
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
      SeriesKey key{ch.isotope, ch.detector, spec_.kind};
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
  if (bus_ != nullptr) bus_->publish(update);
  if (hook && hook(count, t)) truncate();

  std::lock_guard lock(mutex_);
  if (truncate_) {
    truncated_ = true;
    return CollectStatus::Truncated;
  }
  return count_ >= target_ ? CollectStatus::Complete : CollectStatus::Running;
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

std::optional<reduction::Series> Collector::fit_series(const SeriesKey& key) const {
  std::lock_guard lock(mutex_);
  auto it = data_.series.find(key);
  if (it == data_.series.end()) return std::nullopt;
  const double t0 = data_.timing.time_zero.value_or(0.0);
  reduction::Series out;
  out.y = it->second.v;
  out.x.reserve(it->second.t.size());
  for (double t : it->second.t) out.x.push_back(t - t0);
  return out;
}

// ---- metrics ----------------------------------------------------------------

std::optional<std::vector<double>> Collector::isotope_values_locked(const std::string& iso, SeriesKind kind) const {
  const Series* best = nullptr;
  for (const auto& [key, s] : data_.series) {
    if (key.kind != kind || key.isotope != iso || s.v.empty()) continue;
    if (best == nullptr || s.t.back() > best->t.back()) best = &s;
  }
  if (best == nullptr) return std::nullopt;
  return best->v;
}

std::optional<std::string> Collector::detector_of_locked(const std::string& iso) const {
  std::optional<std::string> det;
  double latest = 0;
  for (const auto& [key, s] : data_.series) {
    if (key.isotope != iso || s.t.empty()) continue;
    if (key.kind != SeriesKind::Signal && key.kind != SeriesKind::Sniff) continue;
    if (!det || s.t.back() > latest) {
      det = key.detector;
      latest = s.t.back();
    }
  }
  return det;
}

std::optional<std::vector<double>> Collector::baseline_values_locked(const std::string& det) const {
  std::vector<double> out;
  for (const auto& [key, s] : data_.series) {
    if (key.kind == SeriesKind::Baseline && key.detector == det) out.insert(out.end(), s.v.begin(), s.v.end());
  }
  if (out.empty()) return std::nullopt;
  return out;
}

namespace {

std::optional<double> mean(const std::optional<std::vector<double>>& v) {
  if (!v || v->empty()) return std::nullopt;
  return std::accumulate(v->begin(), v->end(), 0.0) / static_cast<double>(v->size());
}

}  // namespace

std::optional<std::vector<double>> Collector::Metrics::series(const MetricRef& m) const {
  using K = MetricRef::Kind;
  {
    std::lock_guard lock(c_.mutex_);
    switch (m.kind) {
      case K::Isotope:
        if (auto v = c_.isotope_values_locked(m.a, SeriesKind::Signal)) return v;
        if (auto v = c_.isotope_values_locked(m.a, SeriesKind::Sniff)) return v;
        break;
      case K::Ratio: {
        auto a = c_.isotope_values_locked(m.a, SeriesKind::Signal);
        auto b = c_.isotope_values_locked(m.b, SeriesKind::Signal);
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
        if (m.field == "bs") {
          auto det = c_.detector_of_locked(m.a);
          if (det) return c_.baseline_values_locked(*det);
          break;
        }
        auto v = c_.isotope_values_locked(m.a, SeriesKind::Signal);
        if (!v) break;
        if (m.field == "cur") return v;
        if (m.field == "bs_corrected" || m.field == "ic_corrected") {
          auto det = c_.detector_of_locked(m.a);
          const double bs = det ? mean(c_.baseline_values_locked(*det)).value_or(0.0) : 0.0;
          for (auto& x : *v) x -= bs;
          return v;
        }
        break;
      }
      default: break;
    }
  }
  if (fallback != nullptr) return fallback->series(m);
  return std::nullopt;
}

std::optional<double> Collector::Metrics::scalar(const MetricRef& m) const {
  using K = MetricRef::Kind;
  if (m.kind == K::DetectorField && m.field == "intensity") {
    std::lock_guard lock(c_.mutex_);
    const Series* best = nullptr;
    for (const auto& [key, s] : c_.data_.series) {
      if (key.detector != m.a || s.v.empty()) continue;
      if (best == nullptr || s.t.back() > best->t.back()) best = &s;
    }
    if (best != nullptr) return best->v.back();
  }
  if (m.kind == K::Isotope || m.kind == K::Ratio ||
      (m.kind == K::IsotopeField && m.field != "bs")) {
    if (auto s = series(m); s && !s->empty()) return s->back();
    return std::nullopt;
  }
  if (fallback != nullptr) return fallback->scalar(m);
  return std::nullopt;
}

std::optional<double> Collector::Metrics::elapsed() const {
  const double now = c_.seconds(c_.clock_.now());
  std::lock_guard lock(c_.mutex_);
  return now - c_.data_.timing.time_zero.value_or(0.0);
}

}  // namespace pychron::experiment::collect
