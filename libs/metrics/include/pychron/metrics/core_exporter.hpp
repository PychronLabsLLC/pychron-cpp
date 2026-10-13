#pragma once

// Instrument and application health, taken from the events the core already
// publishes: pressures, temperatures, heaters, valves, alarms, transports and
// log records. Nothing in the control path is instrumented for this; the
// exporter is one more subscriber.
//
// Its handlers run on whichever thread published, so they do no more than
// find a series and set it. Label values are the configured names the events
// carry (a gauge, a valve, a transport) and the names of enumerations. The
// free text of an event (an alarm's message, an error, a log line) is never
// a label: every distinct value would be a new series for good.

#include <map>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "pychron/core/events.hpp"
#include "pychron/core/signal_bus.hpp"
#include "pychron/metrics/registry.hpp"

namespace pychron::metrics {

// What pychron_build_info says about this build.
struct BuildInfo {
  std::string version, os, compiler;
};

// `version` with this build's platform ("macos", "linux", "windows") and
// compiler ("clang 17.0", "gcc 14.2", "msvc 1940").
BuildInfo build_info(std::string version);

class CoreExporter {
 public:
  // `registry` and `bus` must outlive the exporter. `now` is real time: it
  // measures how long ago each source was read and how long the application
  // has been up, both worked out at the scrape.
  CoreExporter(Registry& registry, SignalBus& bus, BuildInfo build, RealClock now = steady_real_clock());
  ~CoreExporter();  // unsubscribes
  CoreExporter(const CoreExporter&) = delete;
  CoreExporter& operator=(const CoreExporter&) = delete;

 private:
  void on_valve(const std::string& valve, ValveState state, bool count_transition);
  // `source` of `kind` was read now, and is from now on a source that may alarm.
  void read(const char* kind, const std::string& source, const std::string& alarm_source);
  void collect(Registry& registry);

  Registry& registry_;
  RealClock now_;
  double started_ = 0.0;
  std::mutex mutex_;
  std::map<std::string, ValveState> valves_;   // the last state seen, to tell a change from a repeat
  std::map<std::string, bool> transports_;     // connected, as last said
  std::map<std::pair<std::string, std::string>, double> read_at_;  // (kind, source) -> when last read
  std::set<std::string> alarm_sources_;        // whose alarm counters exist
  std::set<std::string> components_;           // whose log counters exist
  CollectorHandle collector_;
  std::vector<SignalBus::Subscription> subscriptions_;
};

}  // namespace pychron::metrics
