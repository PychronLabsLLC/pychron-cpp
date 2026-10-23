#pragma once

// Bus tap for the events libs/systems publishes (jobs, peak center,
// acquisition, spectrometer, scan service): each one record on logger
// "trace.<EventName>", as pychron::EventRecorder does for the core events
// (pychron/core/event_recorder.hpp, which says how).

#include <span>
#include <vector>

#include "pychron/core/event_recorder.hpp"
#include "pychron/core/signal_bus.hpp"

namespace pychron::systems {

class SystemsEventRecorder {
 public:
  explicit SystemsEventRecorder(SignalBus& bus);

 private:
  std::vector<SignalBus::Subscription> subscriptions_;
};

[[nodiscard]] std::span<const EventClass> systems_event_classes() noexcept;

}  // namespace pychron::systems
