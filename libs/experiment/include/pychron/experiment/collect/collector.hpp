#pragma once

// Collector (experiment spec 8.1): turns spectrometer readings into series.
//
// One class serves every collection. The MeasurementEngine calls begin() with
// a CollectionSpec (kind, counts, detector -> isotope map), feeds it readings
// with add(), and the Collector appends one point per active detector to
// Series[isotope, detector, kind]. Peak hop is the engine calling it once per
// hop; series stay keyed by (isotope, detector, kind) so an isotope measured
// on two detectors keeps two series.
//
// Time: every point's `t` is seconds since the measurement epoch (set by
// start()); time_zero is a separate offset on the same axis, set once by the
// engine's time-zero policy. Fits use x = t - time_zero (fit_series()).
//
// Thread-safety: the engine thread calls begin/add/finish. set_target(),
// truncate() and the read accessors may be called from any thread.

#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pychron/core/clock.hpp"
#include "pychron/core/signal_bus.hpp"
#include "pychron/experiment/conditionals/conditional.hpp"
#include "pychron/experiment/conditionals/evaluator.hpp"
#include "pychron/reduction/fits.hpp"
#include "pychron/systems/spectrometer/acquisition.hpp"

namespace pychron::experiment::collect {

enum class SeriesKind { Signal, Baseline, Sniff, Whiff };

std::string_view to_string(SeriesKind kind) noexcept;

struct SeriesKey {
  std::string isotope;   // empty for a before/after baseline block (per detector)
  std::string detector;
  SeriesKind kind = SeriesKind::Signal;
  friend auto operator<=>(const SeriesKey&, const SeriesKey&) = default;
  friend bool operator==(const SeriesKey&, const SeriesKey&) = default;
};

// "Ar40:H1:signal" or ":H1:baseline".
std::string to_string(const SeriesKey& key);

struct Series {
  std::vector<double> t;      // seconds since the measurement epoch
  std::vector<double> v;      // detector units
  std::vector<double> sigma;  // empty, or parallel to v (host-integrated readings)
  std::vector<bool> saturated;
};

// One isotope on one detector during a collection.
struct Channel {
  std::string isotope;
  std::string detector;
};

struct CollectionSpec {
  SeriesKind kind = SeriesKind::Signal;
  int counts = 0;
  std::vector<Channel> channels;
  std::string label;  // block name for events, e.g. "main", "baseline.after"
};

// Published on the SignalBus once per reading added.
struct SeriesUpdated {
  std::string label;
  SeriesKind kind = SeriesKind::Signal;
  int count = 0;   // readings in this collection so far
  int target = 0;  // current target
  std::vector<std::pair<SeriesKey, double>> values;  // the points just appended (t, v): v
  double t = 0;
};

struct Timing {
  TimePoint epoch{};                // measurement start
  std::optional<double> time_zero;  // seconds since epoch
  std::optional<double> inlet_open, inlet_close;
};

// Plain-data output of a measurement.
struct RunData {
  std::map<SeriesKey, Series> series;
  std::vector<Trip> trips;
  Timing timing;
  std::map<std::string, int> counts;  // readings per collection label
};

// Why a collection stopped.
enum class CollectStatus { Running, Complete, Truncated };

// Called after each reading is appended, outside the Collector's lock, with
// the 1-based count in this collection. Return true to end the collection
// (the engine uses this for conditional trips).
using ReadingHook = std::function<bool(int count, double t)>;

class Collector {
 public:
  Collector(const Clock& clock, SignalBus* bus = nullptr);

  // Starts the measurement clock: t of every point is relative to this.
  void start(TimePoint epoch);
  TimePoint epoch() const;

  // Seconds since the epoch for a clock time.
  double seconds(TimePoint t) const;

  void set_time_zero(double seconds_since_epoch);
  std::optional<double> time_zero() const;
  void set_inlet_open(double s);
  void set_inlet_close(double s);

  // Begins a collection. Any earlier target change or truncate is discarded.
  void begin(CollectionSpec spec, ReadingHook hook = {});
  // Appends one reading. Detectors absent from the reading (or nullopt) get no
  // point; the reading still counts. Returns the collection status after it.
  CollectStatus add(const spectrometer::Reading& reading);
  // Ends the collection; returns how many readings it took.
  int finish();

  // User count change on the running collection (>= 1). The collection ends
  // as soon as count >= target.
  void set_target(int counts);
  int target() const;
  int count() const;
  // Ends the running collection at the next reading.
  void truncate();
  bool truncated() const;

  void add_trips(const std::vector<Trip>& trips);

  // Copies, safe from any thread.
  RunData data() const;
  std::optional<Series> series(const SeriesKey& key) const;

  // x = t - time_zero (or t if time zero is unset).
  std::optional<reduction::Series> fit_series(const SeriesKey& key) const;

  // Conditionals view of the collected data.
  //   Ar40           latest signal value of Ar40 (series: all signal values)
  //   Ar40/Ar39      ratio of the latest values (series: point-wise over the shorter tail)
  //   Ar40.cur       latest signal value
  //   Ar40.bs        baseline series of Ar40's detector
  //   Ar40.bs_corrected / .ic_corrected
  //                  latest signal minus the mean baseline of its detector (icfactor 1)
  //   H1.intensity   latest value on H1, any kind
  //   elapsed()      seconds since time zero (since the epoch before it is set)
  // Every other metric is asked of `fallback` (spectrometer, line, reduction).
  const MetricContext& metrics() const noexcept { return metrics_; }
  void set_fallback(const MetricContext* fallback) { metrics_.fallback = fallback; }

 private:
  class Metrics final : public MetricContext {
   public:
    explicit Metrics(const Collector& c) : c_(c) {}
    std::optional<std::vector<double>> series(const MetricRef& m) const override;
    std::optional<double> scalar(const MetricRef& m) const override;
    std::optional<double> elapsed() const override;
    const MetricContext* fallback = nullptr;

   private:
    const Collector& c_;
  };

  // Signal values of an isotope (latest-written detector first wins when the
  // isotope is on several detectors: the one with the most recent point).
  std::optional<std::vector<double>> isotope_values_locked(const std::string& iso, SeriesKind kind) const;
  std::optional<std::string> detector_of_locked(const std::string& iso) const;
  std::optional<std::vector<double>> baseline_values_locked(const std::string& det) const;

  const Clock& clock_;
  SignalBus* bus_;
  Metrics metrics_{*this};

  mutable std::mutex mutex_;
  RunData data_;
  CollectionSpec spec_;
  ReadingHook hook_;
  int count_ = 0;
  std::atomic<int> target_{0};
  std::atomic<bool> truncate_{false};
  bool truncated_ = false;
};

}  // namespace pychron::experiment::collect
