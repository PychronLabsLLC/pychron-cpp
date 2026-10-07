#pragma once

// Acquisition engine: one IntensityStream contract for every consumer,
// whether the vendor integrates (Qtegra, NGX) or the host does (legacy ADC,
// pulse counter). One Scheduler job per acquirer runs its next() loop;
// readings are merged across acquirers by timestamp, published as
// IntensityReading on the SignalBus and queued on the IntensityStream.
//
// Policy (saturation protection, recording) is not here: the engine flags,
// subscribers and Spectrometer decide.

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "pychron/core/clock.hpp"
#include "pychron/core/error.hpp"
#include "pychron/core/events.hpp"
#include "pychron/core/scheduler.hpp"
#include "pychron/core/signal_bus.hpp"
#include "pychron/devices/spectrometer/detectors.hpp"
#include "pychron/devices/spectrometer/roles.hpp"
#include "pychron/devices/spectrometer/types.hpp"

namespace pychron::spectrometer {

// One detector's reading in detector units after software_gain.
struct Value {
  double mean = 0.0;
  std::optional<double> sigma;       // host-integrated bins with n >= 2
  std::optional<std::uint32_t> n;    // samples in the bin; host-integrated only
  bool saturated = false;

  friend bool operator==(const Value&, const Value&) = default;
};

// One row. Every configured detector has an entry; nullopt = its channel
// produced nothing for this period.
struct Reading {
  TimePoint ts{};
  Duration integration{};
  std::map<DetectorId, std::optional<Value>> values;
};

struct IntensityReading {
  Reading reading;
};

// Bounded FIFO of readings for synchronous consumers (peak center). When full
// the oldest reading is dropped. A stall (no frames within the health limit)
// is reported once as Error{Timeout} from next(), after queued readings.
class IntensityStream {
 public:
  IntensityStream(const Clock& clock, std::size_t capacity);

  // nullopt if nothing arrived within `timeout` (on the injected clock).
  Result<std::optional<Reading>> next(Duration timeout);

  std::size_t size() const;
  std::uint64_t dropped() const;

 private:
  friend class AcquisitionEngine;
  void push(Reading reading);
  void fail(Error error);
  void clear();

  const Clock& clock_;
  std::size_t capacity_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;  // waited on and notified through clock_
  std::deque<Reading> queue_;
  std::optional<Error> error_;
  std::uint64_t dropped_ = 0;
};

class AcquisitionEngine {
 public:
  struct Options {
    Duration integration = std::chrono::seconds(1);  // used by acquire() when not running
    double timeout_factor = 3.0;
    Duration timeout_slack = std::chrono::seconds(3);
    Duration poll_interval = std::chrono::milliseconds(20);  // Scheduler period per acquirer
    Duration poll_timeout{};                                 // next() timeout per poll
    std::size_t queue_capacity = 256;
  };

  // Config error unless every detector channel is carried by exactly one
  // acquirer. Acquirers are not owned and must outlive the engine.
  static Result<std::unique_ptr<AcquisitionEngine>> create(
      std::vector<IIntensityAcquirer*> acquirers, std::vector<DetectorConfig> detectors,
      Scheduler& scheduler, SignalBus& bus, const Clock& clock);
  static Result<std::unique_ptr<AcquisitionEngine>> create(
      std::vector<IIntensityAcquirer*> acquirers, std::vector<DetectorConfig> detectors,
      Scheduler& scheduler, SignalBus& bus, const Clock& clock, Options options);

  ~AcquisitionEngine();
  AcquisitionEngine(const AcquisitionEngine&) = delete;
  AcquisitionEngine& operator=(const AcquisitionEngine&) = delete;

  // Configures and starts every acquirer and registers one Scheduler job each.
  // Bins align to the epoch taken here. Calls are serialised: it waits for a
  // start() or stop() in progress on another thread to finish first, so a
  // second start() racing the first fails with "already running" and never
  // configures or registers anything. Called from inside a poll while another
  // thread's stop() or start() is in progress it does not wait (that would
  // deadlock) and fails with Config.
  //
  // Like stop(), it can wait for polls in flight, and polls publish on the
  // SignalBus synchronously: do not hold, across start(), a lock that a bus
  // subscriber running on a poll thread may take.
  Result<void> start(Duration integration);
  // Returns only when no poll() is executing on another thread and the
  // acquirers are stopped, so a start() that follows never overlaps a next()
  // from this run. A start() in progress on another thread is waited for and
  // then stopped. Called from inside a poll (a bus subscriber, say) it does
  // not wait for that poll, nor for a stop() or start() already in progress
  // elsewhere.
  //
  // Polls publish on the SignalBus synchronously, so do not hold, across
  // stop(), a lock that a bus subscriber running on a poll thread may take.
  void stop();
  bool running() const;

  // Integration period in effect: the requested value until an integrated
  // frame reports the value the vendor snapped to.
  Duration integration() const;

  std::shared_ptr<IntensityStream> stream() const { return stream_; }

  // Blocks until n readings, or `duration` (engine clock) has elapsed, or
  // cancel(). Starts the acquirers (with Options::integration) if not running
  // and stops them again on return. trigger() is issued once per period.
  // Cancelled error on cancel(); Timeout error if the acquirers stall.
  Result<std::vector<Reading>> acquire(std::size_t n);
  Result<std::vector<Reading>> acquire(Duration duration);
  void cancel();

  // One iteration of acquirer `index`'s job: drain next(), bin, merge,
  // publish, health check. Scheduler jobs call this; tests may too. A no-op
  // when the engine is not running.
  void poll(std::size_t index);

  // Frames lost to seq gaps / discarded by the stale-frame guard.
  std::uint64_t dropped_frames() const;
  std::uint64_t stale_frames() const;

 private:
  struct Acc {
    std::vector<double> samples;  // per-sample value (faraday) or corrected rate (counter)
    double total = 0.0;           // counter: summed counts
    double span_s = 0.0;          // counter: summed sample spans
  };
  struct Bin {
    bool open = false;
    std::int64_t index = 0;
    std::vector<Acc> acc;  // parallel to acq_dets_[i]
  };
  struct Partial {
    std::size_t acquirer = 0;
    TimePoint ts{};
    Duration integration{};
    std::vector<std::pair<std::size_t, std::optional<Value>>> values;  // detector index
  };

  AcquisitionEngine(std::vector<IIntensityAcquirer*> acquirers,
                    std::vector<DetectorConfig> detectors, Scheduler& scheduler, SignalBus& bus,
                    const Clock& clock, Options options);

  void begin_request();
  std::size_t polls_on_this_thread() const;  // caller holds mutex_
  // Neither starting nor stopping: running, or stopped with no poll in
  // flight. What start() and an already-stopped stop() wait for. Caller holds
  // mutex_.
  bool settled() const;
  void ingest(std::size_t i, const Frame& frame, TimePoint now);
  void ingest_integrated(std::size_t i, const Frame& frame);
  void ingest_raw(std::size_t i, const Frame& frame);
  void close_bin(std::size_t i);
  std::optional<Value> finish(const DetectorConfig& d, const Acc& acc) const;
  Value make_value(const DetectorConfig& d, double mean) const;
  TimePoint bin_end(std::int64_t index) const;
  void merge(TimePoint now, std::vector<Reading>& out);
  void check_health(std::size_t i, TimePoint now, std::vector<Alarm>& alarms);
  void deliver(std::vector<Reading>& readings, std::vector<Alarm>& alarms);
  Result<std::vector<Reading>> acquire_impl(std::size_t n, std::optional<Duration> duration);

  std::vector<IIntensityAcquirer*> acquirers_;
  std::vector<DetectorConfig> detectors_;
  std::vector<std::vector<std::size_t>> acq_dets_;  // acquirer -> detector indices
  Scheduler& scheduler_;
  SignalBus& bus_;
  const Clock& clock_;
  Options options_;
  std::shared_ptr<IntensityStream> stream_;

  mutable std::mutex mutex_;
  std::condition_variable collect_cv_;  // waited on and notified through clock_
  bool running_ = false;
  Duration integration_{};
  TimePoint request_start_{};
  TimePoint epoch_{};
  std::vector<JobId> jobs_;
  std::vector<std::thread::id> polling_;  // one entry per poll() in flight
  // polling_ shrank, or stopping_ or starting_ cleared. Waited on and
  // notified through clock_: a poll in flight may be waiting in clock time.
  std::condition_variable polls_cv_;
  bool stopping_ = false;                 // a stop() has yet to stop the acquirers
  bool starting_ = false;                 // a start() is between its check and its result
  std::vector<Bin> bins_;
  std::vector<Partial> pending_;
  std::vector<bool> have_seq_;
  std::vector<std::uint64_t> last_seq_;
  std::vector<TimePoint> last_frame_;
  std::vector<bool> stalled_;
  std::uint64_t dropped_frames_ = 0;
  std::uint64_t stale_frames_ = 0;

  bool collecting_ = false;
  bool cancel_ = false;
  std::vector<Reading> collected_;
  std::optional<Error> collect_error_;
};

}  // namespace pychron::spectrometer
