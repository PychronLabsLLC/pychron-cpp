#pragma once

// Collector (experiment spec 8.1): turns spectrometer readings into series.
//
// One class serves every collection. The MeasurementEngine calls begin() with
// a CollectionSpec (kind, counts, label -> detector channels, and the gas,
// segment and position it collects for), feeds it readings with add(), and the
// Collector appends one point per active detector to Series[label, detector,
// kind, gas, segment, position]. Peak hop is the engine calling it once per
// hop; a label measured on two detectors keeps two series, and one measured at
// two positions on one detector two series that read as one (series_of).
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
#include "pychron/experiment/plan/plan.hpp"
#include "pychron/reduction/arar.hpp"
#include "pychron/reduction/fits.hpp"
#include "pychron/systems/spectrometer/acquisition.hpp"

namespace pychron::experiment::collect {

// What a series holds. An open set: these four, "peak_center", and whatever
// a later kind of collection adds.
inline constexpr std::string_view kSignal = "signal", kBaseline = "baseline", kSniff = "sniff", kWhiff = "whiff";

// One series: a label on a detector, of one kind, in one gas, segment and
// position of the plan. A label measured at several positions is several
// series; Collector::intercept fits them as one.
struct SeriesKey {
  std::string label;  // empty for a baseline segment's series (per detector)
  std::string detector;
  std::string kind{kSignal};
  std::string gas{};  // braced: a key written {label, detector, kind} leaves the rest at their defaults
  int segment = 0;
  int position = 0;
  friend auto operator<=>(const SeriesKey&, const SeriesKey&) = default;
  friend bool operator==(const SeriesKey&, const SeriesKey&) = default;
};

// "Ar40@H1:signal:<gas>:<segment>.<position>", e.g. "Ar40@H1:signal::0.2" or
// "@H1:baseline:argon:3.0".
std::string to_string(const SeriesKey& key);

struct Series {
  std::vector<double> t;      // seconds since the measurement epoch
  std::vector<double> v;      // detector units
  std::vector<double> sigma;  // empty, or parallel to v (host-integrated readings)
  std::vector<bool> saturated;
};

// The points of several series as one, in time order (a point's t, v, sigma
// and saturated stay together). sigma is filled with 0 for the points of a
// part without one when any part has it.
Series merge_series(const std::vector<const Series*>& parts);

// One label on one detector during a collection.
struct Channel {
  std::string label;
  std::string detector;
};

struct CollectionSpec {
  std::string kind{kSignal};
  int counts = 0;
  std::vector<Channel> channels;
  std::string label{};  // block name for events, e.g. "main", "baseline.after"
  std::string gas{};    // where in the plan: every series this collection makes is keyed by these
  int segment = 0;
  int position = 0;
};

// Published on the SignalBus once per reading added.
struct SeriesUpdated {
  std::string label;
  std::string kind{kSignal};
  int count = 0;   // readings in this collection so far
  int target = 0;  // current target
  std::vector<std::pair<SeriesKey, double>> values;  // the points just appended (t, v): v
  double t = 0;
};

// Published after each signal or baseline reading (after its SeriesUpdated):
// the live fit of every series the reading touched (a signal series together
// with the other series of its label on its detector), with the plan's fit, the
// same computation as the conditionals' intercept and the record's. x is
// seconds since time zero (the epoch until it is set). A series with fewer
// points than its fit's parameters is averaged; one that cannot be fitted is
// left out.
struct SeriesFit {
  SeriesKey key;
  reduction::Intercept fit;
};
struct FitsUpdated {
  std::string label;
  std::string kind{kSignal};
  double time_zero = 0;  // seconds since the measurement epoch
  std::vector<SeriesFit> fits;
};

struct Timing {
  TimePoint epoch{};                // measurement start
  std::optional<double> time_zero;  // seconds since epoch: the first gas's (the first set)
  std::map<std::string, double> time_zeros;  // by gas name (set_time_zero(gas, s))
  std::optional<double> inlet_open, inlet_close;
};

// Plain-data output of a measurement.
struct RunData {
  std::map<SeriesKey, Series> series;
  std::vector<Trip> trips;
  Timing timing;
  std::map<std::string, int> counts;  // readings per collection label, summed over its collections
};

// Every series of a label on a detector of one kind, wherever in the plan it
// was measured, merged in time order; nullopt when there is none.
std::optional<Series> merged_series(const RunData& data, std::string_view label, std::string_view detector,
                                    std::string_view kind);

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

  // A gas's time zero (several gases, spec 3.4): its series are fitted on
  // x = t - that. The first set is also time_zero(). The one-argument form
  // sets time_zero() alone, for every series without a gas's own.
  void set_time_zero(const std::string& gas, double seconds_since_epoch);
  void set_time_zero(double seconds_since_epoch);
  std::optional<double> time_zero() const;
  std::optional<double> time_zero(const std::string& gas) const;
  void set_inlet_open(double s);
  void set_inlet_close(double s);
  // Readings resumed after a gap at `s` (seconds since the epoch): windowed
  // conditionals look no further back (MetricContext::contiguous_from).
  void mark_gap(double s);

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
  // Every series of `kind` for a label on one detector, merged in time order.
  // `name` is "Ar40" (its most recently measured detector) or "Ar40@H1".
  std::optional<Series> series_of(const std::string& name, std::string_view kind) const;

  // x = t - time_zero (or t if time zero is unset).
  std::optional<reduction::Series> fit_series(const SeriesKey& key) const;

  // Conditionals view of the collected data (conditionals spec section 5, L9).
  //   Ar40              live corrected value: (intercept - mean baseline) * icfactor
  //                     (series: raw signal points; sniff points before any signal)
  //   Ar40.intercept    raw intercept at time zero, fitted with the plan's fit
  //   Ar40.std_dev      the intercept's error
  //   Ar40.bs_corrected intercept - mean baseline of its detector
  //   Ar40.ic_corrected same as Ar40
  //   Ar40.cur          latest raw point
  //   Ar40.bs           baseline series of Ar40's detector
  //   Ar40/Ar39         ratio of the corrected values (series: raw point-wise)
  //   H1.intensity      latest value on H1, any kind
  //   age, kca, ...     reduction::compute_arar over the corrected values
  //                     (instant_age: over the latest baseline-corrected points)
  //   elapsed()         seconds since time zero (since the epoch before it is set)
  // A fit with fewer points than its parameters falls back to the average.
  // Every other metric is asked of `fallback` (spectrometer, line, devices).
  const MetricContext& metrics() const noexcept { return metrics_; }
  void set_fallback(const MetricContext* fallback) { metrics_.fallback = fallback; }
  // The plan whose fits the live fits use (plan::fit_for, by each series'
  // gas and segment). set_fits: a plan of these fits alone.
  void set_plan(plan::MeasurementPlan plan);
  void set_fits(plan::Fits fits);
  void set_icfactors(std::map<std::string, double> icfactors);  // by detector; 1.0 when absent
  void set_arar(std::optional<reduction::ArArConstants> constants);

  // Live intercept of a label: "Ar40" fits its most recently measured
  // detector, "Ar40@H1" that detector. The series of one label on one detector
  // (several gases, segments or positions) are fitted as one, in time order.
  std::optional<reduction::Intercept> intercept(const std::string& name) const;

 private:
  class Metrics final : public MetricContext {
   public:
    explicit Metrics(const Collector& c) : c_(c) {}
    std::optional<std::vector<double>> series(const MetricRef& m) const override;
    std::optional<double> scalar(const MetricRef& m) const override;
    std::optional<double> elapsed() const override;
    std::size_t contiguous_from(const MetricRef& m) const override;
    const MetricContext* fallback = nullptr;

   private:
    const Collector& c_;
  };

  // Values of a label ("Ar40" or "Ar40@H1"). Unqualified, on several
  // detectors: the detector with the most recent point.
  std::optional<std::vector<double>> isotope_values_locked(const std::string& iso, std::string_view kind) const;
  std::optional<std::string> detector_of_locked(const std::string& iso) const;
  std::optional<std::vector<double>> baseline_values_locked(const std::string& det) const;
  // Every series of `kind` of the label on its detector, merged in time order.
  // `first`: the key of its first part (for its gas and segment).
  std::optional<Series> isotope_series_locked(const std::string& iso, std::string_view kind,
                                              std::string* det = nullptr, SeriesKey* first = nullptr) const;
  reduction::FitSpec fit_spec_locked(const SeriesKey& key) const;
  double time_zero_locked(const std::string& gas) const;  // the gas's, else time_zero(), else 0
  std::optional<reduction::Intercept> intercept_locked(const std::string& iso, std::string* det = nullptr) const;
  double baseline_mean_locked(const std::string& det) const;
  double icfactor_locked(const std::string& det) const;
  std::optional<double> corrected_locked(const std::string& iso) const;
  std::optional<double> computed_locked(const std::string& name) const;

  void publish_fits(const SeriesUpdated& update);  // FitsUpdated for the series `update` touched

  const Clock& clock_;
  SignalBus* bus_;
  Metrics metrics_{*this};

  mutable std::mutex mutex_;
  std::optional<double> resumed_at_;  // the last gap's end (mark_gap)
  RunData data_;
  CollectionSpec spec_;
  ReadingHook hook_;
  plan::MeasurementPlan plan_;
  std::optional<std::string> first_gas_;  // the gas whose time zero is time_zero()
  std::map<std::string, double> icfactors_;
  std::optional<reduction::ArArConstants> arar_;
  int count_ = 0;
  std::atomic<int> target_{0};
  std::atomic<bool> truncate_{false};
  bool truncated_ = false;
};

}  // namespace pychron::experiment::collect
