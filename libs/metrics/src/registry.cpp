#include "pychron/metrics/registry.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

#include "registry_impl.hpp"

namespace pychron::metrics {

namespace {

bool name_start(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
bool name_char(char c) { return name_start(c) || (c >= '0' && c <= '9'); }

bool valid_metric_name(std::string_view n) {
  if (n.empty() || !(name_start(n[0]) || n[0] == ':')) return false;
  return std::all_of(n.begin(), n.end(), [](char c) { return name_char(c) || c == ':'; });
}

bool valid_label_name(std::string_view n) {
  if (n.empty() || !name_start(n[0])) return false;
  if (n.size() >= 2 && n[0] == '_' && n[1] == '_') return false;  // reserved by Prometheus
  return std::all_of(n.begin(), n.end(), name_char);
}

// The labels sorted by name; false when a name is invalid or given twice.
bool normalize(const Labels& in, MetricType type, Labels& out) {
  out = in;
  std::sort(out.begin(), out.end());
  for (std::size_t i = 0; i < out.size(); ++i) {
    if (!valid_label_name(out[i].first)) return false;
    if (type == MetricType::Histogram && out[i].first == "le") return false;
    if (i > 0 && out[i].first == out[i - 1].first) return false;
  }
  return true;
}

void add(std::atomic<double>& a, double by) noexcept {
  double old = a.load(std::memory_order_relaxed);
  while (!a.compare_exchange_weak(old, old + by, std::memory_order_relaxed)) {
  }
}

std::vector<double> clean_bounds(std::vector<double> b) {
  b.erase(std::remove_if(b.begin(), b.end(), [](double v) { return !std::isfinite(v); }), b.end());
  std::sort(b.begin(), b.end());
  b.erase(std::unique(b.begin(), b.end()), b.end());
  return b;
}

}  // namespace

RealClock steady_real_clock() {
  return [] {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
  };
}

// ---- series ----------------------------------------------------------------

void Counter::inc(double by) noexcept {
  if (!(by > 0.0) || !std::isfinite(by)) return;
  add(value_, by);
}

void Counter::set_total(double total) noexcept {
  if (!(total >= 0.0) || !std::isfinite(total)) return;
  double delta = 0.0;
  {
    const std::lock_guard lock(total_mutex_);
    delta = total >= last_total_ ? total - last_total_ : total;
    last_total_ = total;
  }
  inc(delta);
}

void Gauge::inc(double by) noexcept { add(value_, by); }

Histogram::Histogram(std::vector<double> bounds)
    : bounds_(clean_bounds(std::move(bounds))),
      counts_(std::make_unique<std::atomic<std::uint64_t>[]>(bounds_.size() + 1)) {
  for (std::size_t i = 0; i <= bounds_.size(); ++i) counts_[i].store(0, std::memory_order_relaxed);
}

void Histogram::observe(double v) noexcept {
  if (std::isnan(v)) return;
  const auto it = std::lower_bound(bounds_.begin(), bounds_.end(), v);  // the first bound >= v
  counts_[static_cast<std::size_t>(it - bounds_.begin())].fetch_add(1, std::memory_order_relaxed);
  add(sum_, v);
}

std::uint64_t Histogram::count() const noexcept {
  std::uint64_t n = 0;
  for (std::size_t i = 0; i <= bounds_.size(); ++i) n += counts_[i].load(std::memory_order_relaxed);
  return n;
}

// ---- collectors ------------------------------------------------------------

CollectorHandle::CollectorHandle(CollectorHandle&& other) noexcept
    : owner_(std::move(other.owner_)), id_(other.id_) {
  other.id_ = 0;
}

CollectorHandle& CollectorHandle::operator=(CollectorHandle&& other) noexcept {
  if (this != &other) {
    reset();
    owner_ = std::move(other.owner_);
    id_ = other.id_;
    other.id_ = 0;
  }
  return *this;
}

void CollectorHandle::reset() {
  if (id_ == 0) return;
  if (const auto owner = owner_.lock()) {
    const std::lock_guard lock(owner->mutex);
    owner->items.erase(id_);
  }
  owner_.reset();
  id_ = 0;
}

// ---- registry --------------------------------------------------------------

struct Registry::Impl {
  mutable std::mutex mutex;
  std::map<std::string, detail::Family, std::less<>> families;
  std::shared_ptr<CollectorHandle::Collectors> collectors = std::make_shared<CollectorHandle::Collectors>();
  std::function<void(std::string_view)> full_handler;

  // What a refused lookup returns: updated like any series, never rendered.
  Counter sink_counter;
  Gauge sink_gauge;
  Histogram sink_histogram{{}};
  Counter* dropped = nullptr;

  // The family `name` if it may hold `type`, created when new; else nullptr.
  detail::Family* family(MetricType type, std::string_view name, std::string_view help, std::vector<double>& buckets) {
    if (!valid_metric_name(name)) return nullptr;
    auto it = families.find(name);
    if (it == families.end()) {
      detail::Family f;
      f.type = type;
      f.help = std::string(help);
      if (type == MetricType::Histogram) f.buckets = std::move(buckets);
      it = families.emplace(std::string(name), std::move(f)).first;
    }
    return it->second.type == type ? &it->second : nullptr;
  }

  // The series for `labels`, or nullptr when it must be refused. `full` is
  // set to the family's name the first time the family turns one away.
  detail::Series* series(MetricType type, std::string_view name, std::string_view help, std::vector<double> buckets,
                         const Labels& labels, std::string& full) {
    detail::Family* f = family(type, name, help, buckets);
    Labels key;
    if (f == nullptr || !normalize(labels, type, key)) return nullptr;
    auto it = f->series.find(key);
    if (it == f->series.end()) {
      if (f->series.size() >= kMaxSeriesPerFamily) {
        if (!f->reported_full) {
          f->reported_full = true;
          full = std::string(name);
        }
        return nullptr;
      }
      detail::Series s;
      switch (type) {
        case MetricType::Counter: s.counter = std::make_unique<Counter>(); break;
        case MetricType::Gauge: s.gauge = std::make_unique<Gauge>(); break;
        case MetricType::Histogram: s.histogram = std::make_unique<Histogram>(f->buckets); break;
      }
      it = f->series.emplace(std::move(key), std::move(s)).first;
    }
    it->second.hidden = false;
    return &it->second;
  }

  // One lookup: the series, or the count of a refusal and (outside the lock)
  // the word that a family is full.
  detail::Series* find(MetricType type, std::string_view name, std::string_view help, std::vector<double> buckets,
                       const Labels& labels) {
    std::string full;
    std::function<void(std::string_view)> handler;
    detail::Series* s = nullptr;
    {
      const std::lock_guard lock(mutex);
      s = series(type, name, help, std::move(buckets), labels, full);
      if (!full.empty()) handler = full_handler;
    }
    if (s == nullptr && dropped != nullptr) dropped->inc();
    if (handler) {
      try {
        handler(full);
      } catch (...) {  // NOLINT(bugprone-empty-catch): a handler's failure is not the caller's
      }
    }
    return s;
  }
};

Registry::Registry() : impl_(std::make_unique<Impl>()) {
  impl_->dropped = &counter("pychron_metrics_dropped_series_total",
                            "Series refused by the registry: an invalid name, a name used for two types, or a full family.");
}

Registry::~Registry() {
  // A collector running on another thread finishes before the state it uses goes.
  const std::lock_guard lock(impl_->collectors->mutex);
  impl_->collectors->items.clear();
}

Counter& Registry::counter(std::string_view name, std::string_view help, const Labels& labels) {
  detail::Series* s = impl_->find(MetricType::Counter, name, help, {}, labels);
  return s != nullptr ? *s->counter : impl_->sink_counter;
}

Gauge& Registry::gauge(std::string_view name, std::string_view help, const Labels& labels) {
  detail::Series* s = impl_->find(MetricType::Gauge, name, help, {}, labels);
  return s != nullptr ? *s->gauge : impl_->sink_gauge;
}

Histogram& Registry::histogram(std::string_view name, std::string_view help, std::vector<double> buckets,
                               const Labels& labels) {
  detail::Series* s = impl_->find(MetricType::Histogram, name, help, std::move(buckets), labels);
  return s != nullptr ? *s->histogram : impl_->sink_histogram;
}

void Registry::declare(MetricType type, std::string_view name, std::string_view help, std::vector<double> buckets) {
  bool refused = false;
  {
    const std::lock_guard lock(impl_->mutex);
    refused = impl_->family(type, name, help, buckets) == nullptr;
  }
  if (refused) impl_->dropped->inc();
}

void Registry::remove(std::string_view name, const Labels& labels) {
  const std::lock_guard lock(impl_->mutex);
  const auto f = impl_->families.find(name);
  if (f == impl_->families.end()) return;
  Labels key;
  if (!normalize(labels, f->second.type, key)) return;
  const auto s = f->second.series.find(key);
  if (s != f->second.series.end()) s->second.hidden = true;
}

CollectorHandle Registry::add_collector(Collector collector) {
  const std::lock_guard lock(impl_->collectors->mutex);
  const std::uint64_t id = impl_->collectors->next_id++;
  impl_->collectors->items.emplace(id, std::move(collector));
  return CollectorHandle(impl_->collectors, id);
}

void Registry::on_family_full(std::function<void(std::string_view family)> handler) {
  const std::lock_guard lock(impl_->mutex);
  impl_->full_handler = std::move(handler);
}

std::string Registry::render() {
  {
    // Collectors call back into the registry, so they run before its lock is taken.
    const std::lock_guard lock(impl_->collectors->mutex);
    for (const auto& [id, collect] : impl_->collectors->items) {
      try {
        collect(*this);
      } catch (...) {  // NOLINT(bugprone-empty-catch): one source's failure leaves the rest of the scrape
      }
    }
  }
  const std::lock_guard lock(impl_->mutex);
  return detail::render_text(impl_->families);
}

std::vector<std::string> Registry::names() const {
  const std::lock_guard lock(impl_->mutex);
  std::vector<std::string> out;
  out.reserve(impl_->families.size());
  for (const auto& [name, family] : impl_->families) out.push_back(name);
  return out;
}

}  // namespace pychron::metrics
