#pragma once

// A call that must get through while the line is in a state not safe to
// leave it in (executor hardening, spec 4.4 and 4.7): a valve or detector
// call during a measurement, a step of the safe state. It is repeated through
// transient failures until it succeeds or its window passes.

#include <chrono>
#include <functional>
#include <string>

#include "pychron/core/clock.hpp"
#include "pychron/core/error.hpp"
#include "pychron/core/events.hpp"
#include "pychron/core/signal_bus.hpp"

namespace pychron::experiment::measurement {

// Spec 4.4: the critical window, and when an alarm first says so.
inline constexpr pychron::Duration kCriticalWindow = std::chrono::minutes(10);
inline constexpr pychron::Duration kCriticalAlarmAfter = std::chrono::seconds(20);

struct KeepTrying {
  const Clock& clock;
  SignalBus* bus = nullptr;
  std::string source;  // the Alarm's source
  // Waits on the clock; false: stop trying (the run was cancelled).
  std::function<bool(pychron::Duration)> sleep;
  // Said with each alarm (a run note, a log line); may be empty.
  std::function<void(const std::string&)> say;
  pychron::Duration window = kCriticalWindow;
  pychron::Duration alarm_after = kCriticalAlarmAfter;
};

// `op` until it succeeds. A transient failure (Timeout, Io, NotConnected) is
// tried again after a backoff of 1 s doubling to 30 s, for `window`; a
// warning Alarm goes out at `alarm_after`, a critical one when it gives up.
// A definite error gives up at once, with a critical Alarm. `what` names the
// call in the alarms ("close inlet B").
[[nodiscard]] Result<void> keep_trying(const KeepTrying& how, const std::string& what,
                                       const std::function<Result<void>()>& op);

}  // namespace pychron::experiment::measurement
