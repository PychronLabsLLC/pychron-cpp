#pragma once

// Runs and queues as the lab's Prometheus sees them: what the executor is
// doing, how runs end, how long each phase of a run takes, which conditionals
// trip. All of it comes from events the experiment system already publishes;
// this is one more subscriber and the executor does not know it exists.
//
// A run is followed by its id only to time its states, and forgotten when it
// ends. Neither the id nor the sample's identifier, nor any message, becomes
// a label: each would be a new series for every run.
//
// The spectrometer's jobs (a sweep, a peak center) are here too, whether a
// run asked for them or the operator did: how many, how they ended, how long
// they took, and how far the one now running is.
//
// Durations are differences of the events' own times, on the line's clock:
// in a simulation they are simulated seconds. The one age
// (pychron_last_run_finished_age_seconds) is real time, worked out at the
// scrape.

#include <cstddef>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "pychron/core/clock.hpp"
#include "pychron/core/signal_bus.hpp"
#include "pychron/experiment/run/state.hpp"
#include "pychron/metrics/registry.hpp"

namespace pychron::experiment::metrics {

class ExperimentMetrics {
 public:
  // `registry` and `bus` must outlive this object.
  ExperimentMetrics(pychron::metrics::Registry& registry, SignalBus& bus,
                    pychron::metrics::RealClock now = pychron::metrics::steady_real_clock());
  ~ExperimentMetrics();  // unsubscribes
  ExperimentMetrics(const ExperimentMetrics&) = delete;
  ExperimentMetrics& operator=(const ExperimentMetrics&) = delete;

  // Runs whose current state is being timed.
  std::size_t tracked_runs() const;

 private:
  struct Tracked {
    run::RunState state = run::RunState::Pending;
    TimePoint since{};
  };

  void set_queue(std::size_t total);
  void set_active(bool active);

  pychron::metrics::Registry& registry_;
  pychron::metrics::RealClock now_;
  mutable std::mutex mutex_;
  std::map<std::string, Tracked> runs_;  // by run id; a run leaves when it ends
  std::size_t from_row_ = 0;             // of the queue now running
  std::size_t done_ = 0;                 // runs it has finished
  std::optional<double> last_finished_;  // real time; none until a run finishes
  std::set<std::string> channels_;       // notification channels whose counters exist
  pychron::metrics::CollectorHandle collector_;
  std::vector<SignalBus::Subscription> subscriptions_;
};

}  // namespace pychron::experiment::metrics
