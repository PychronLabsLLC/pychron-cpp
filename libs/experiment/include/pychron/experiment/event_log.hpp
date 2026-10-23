#pragma once

// Bus tap for the events libs/experiment publishes (executor, run, session,
// measurement engine, collector, persister, notifier): each one record on
// logger "trace.<EventName>", as pychron::EventRecorder does for the core
// events (pychron/core/event_recorder.hpp, which says how).

#include <span>
#include <vector>

#include "pychron/core/event_recorder.hpp"
#include "pychron/core/signal_bus.hpp"

namespace pychron::experiment {

class ExperimentEventRecorder {
 public:
  explicit ExperimentEventRecorder(SignalBus& bus);

 private:
  std::vector<SignalBus::Subscription> subscriptions_;
};

[[nodiscard]] std::span<const EventClass> experiment_event_classes() noexcept;

}  // namespace pychron::experiment
