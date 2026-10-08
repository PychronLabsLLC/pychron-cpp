#pragma once

// Counters, gauges and histograms, and their rendering in the Prometheus text
// format (version 0.0.4).
//
// A series is found by its family name and label set; once found, its
// reference stays valid for the registry's life and is updated without a
// lock, so a bus handler may keep it or look it up again on every event.
// Nothing here blocks for longer than a map lookup and nothing throws: a
// lookup that cannot be honoured (an invalid name, a name already used for
// another type, a family that is full) returns a series that is updated like
// any other and never rendered.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pychron::metrics {

// Label name -> value. The order given does not matter.
using Labels = std::vector<std::pair<std::string, std::string>>;

// Seconds since the epoch, in real time. What a timestamp gauge holds: a
// dashboard compares it with the time of the scrape, so it never comes from
// the line's Clock, which may be simulated.
using UnixClock = std::function<double()>;
UnixClock system_unix_clock();

enum class MetricType { Counter, Gauge, Histogram };

class Counter {
 public:
  Counter() = default;
  Counter(const Counter&) = delete;
  Counter& operator=(const Counter&) = delete;

  // A negative or non-finite increment is ignored: a counter never falls.
  void inc(double by = 1.0) noexcept;
  // For a source that already counts. The counter rises by the increase
  // since the last total; a total below the last one means the source began
  // again from zero, and the counter rises by the new total.
  void set_total(double total) noexcept;
  double value() const noexcept { return value_.load(std::memory_order_relaxed); }

 private:
  std::atomic<double> value_{0.0};
  std::mutex total_mutex_;
  double last_total_ = 0.0;
};

class Gauge {
 public:
  Gauge() = default;
  Gauge(const Gauge&) = delete;
  Gauge& operator=(const Gauge&) = delete;

  void set(double v) noexcept { value_.store(v, std::memory_order_relaxed); }
  void inc(double by = 1.0) noexcept;
  void dec(double by = 1.0) noexcept { inc(-by); }
  double value() const noexcept { return value_.load(std::memory_order_relaxed); }

 private:
  std::atomic<double> value_{0.0};
};

class Histogram {
 public:
  // `bounds` are the buckets' upper limits; sorted here, duplicates and
  // non-finite values dropped. The +Inf bucket is implied.
  explicit Histogram(std::vector<double> bounds);
  Histogram(const Histogram&) = delete;
  Histogram& operator=(const Histogram&) = delete;

  // NaN is ignored (it would make the sum NaN for good).
  void observe(double v) noexcept;
  std::uint64_t count() const noexcept;
  double sum() const noexcept { return sum_.load(std::memory_order_relaxed); }

  const std::vector<double>& bounds() const noexcept { return bounds_; }
  // Observations in bucket `i` alone (not cumulative); `i == bounds().size()` is +Inf.
  std::uint64_t in_bucket(std::size_t i) const noexcept { return counts_[i].load(std::memory_order_relaxed); }

 private:
  std::vector<double> bounds_;
  std::unique_ptr<std::atomic<std::uint64_t>[]> counts_;
  std::atomic<double> sum_{0.0};
};

// RAII handle of Registry::add_collector; removes the collector on
// destruction or reset(). May outlive the registry.
class CollectorHandle {
 public:
  CollectorHandle() = default;
  ~CollectorHandle() { reset(); }
  CollectorHandle(CollectorHandle&& other) noexcept;
  CollectorHandle& operator=(CollectorHandle&& other) noexcept;
  CollectorHandle(const CollectorHandle&) = delete;
  CollectorHandle& operator=(const CollectorHandle&) = delete;

  // Returns once the collector is not running and will not run again.
  void reset();

 private:
  friend class Registry;
  struct Collectors;
  CollectorHandle(std::weak_ptr<Collectors> owner, std::uint64_t id) : owner_(std::move(owner)), id_(id) {}

  std::weak_ptr<Collectors> owner_;
  std::uint64_t id_ = 0;
};

class Registry {
 public:
  // A labelling bug must not grow without bound: a family holds this many
  // series, and a label set beyond that is counted and not rendered.
  static constexpr std::size_t kMaxSeriesPerFamily = 1000;

  Registry();
  ~Registry();
  Registry(const Registry&) = delete;
  Registry& operator=(const Registry&) = delete;

  // The series of `name` with `labels`, created on first use. `help` and
  // (for a histogram) `buckets` are the family's and are taken from whoever
  // names it first. Looking a series up shows it again after remove().
  Counter& counter(std::string_view name, std::string_view help, const Labels& labels = {});
  Gauge& gauge(std::string_view name, std::string_view help, const Labels& labels = {});
  Histogram& histogram(std::string_view name, std::string_view help, std::vector<double> buckets,
                       const Labels& labels = {});

  // Names a family whose label values are not known yet, so that names()
  // lists it before its first series exists. It renders nothing by itself.
  void declare(MetricType type, std::string_view name, std::string_view help, std::vector<double> buckets = {});

  // Hides a series from render() (a reading that no longer applies). Its
  // references stay valid.
  void remove(std::string_view name, const Labels& labels);

  // Runs on the rendering thread at the start of every render(), for values
  // that are read rather than pushed. An exception it throws is swallowed.
  using Collector = std::function<void(Registry&)>;
  [[nodiscard]] CollectorHandle add_collector(Collector collector);

  // Called once per family, outside the registry's lock, the first time that
  // family refuses a series for being full.
  void on_family_full(std::function<void(std::string_view family)> handler);

  // Families sorted by name, series by labels: the same state renders the
  // same text.
  std::string render();
  std::vector<std::string> names() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace pychron::metrics
