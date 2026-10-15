#include "pychron/experiment/measurement/keep_trying.hpp"

#include <algorithm>
#include <utility>

#include "pychron/devices/link.hpp"

namespace pychron::experiment::measurement {

namespace {

void alarm(const KeepTrying& how, AlarmSeverity severity, std::string message) {
  if (how.say) how.say(message);
  if (how.bus != nullptr) how.bus->publish(Alarm{how.source, severity, std::move(message), how.clock.now()});
}

}  // namespace

Result<void> keep_trying(const KeepTrying& how, const std::string& what, const std::function<Result<void>()>& op) {
  const auto start = how.clock.now();
  pychron::Duration backoff = std::chrono::seconds(1);
  bool warned = false;
  for (;;) {
    auto r = op();
    if (r) return r;
    const auto elapsed = how.clock.now() - start;
    if (!is_transient(r.error().kind) || elapsed >= how.window) {
      alarm(how, AlarmSeverity::Critical, how.source + ": " + what + " failed: " + r.error().what);
      return r;
    }
    if (!warned && elapsed >= how.alarm_after) {
      warned = true;
      alarm(how, AlarmSeverity::Warning, how.source + ": " + what + " still failing: " + r.error().what);
    }
    // Wake for the warning and for the window's end, whichever backoff misses.
    pychron::Duration wait = std::min<pychron::Duration>(backoff, how.window - elapsed);
    if (!warned) wait = std::min<pychron::Duration>(wait, how.alarm_after - elapsed);
    if (!how.sleep(wait)) return r;  // cancelled: the run is ending anyway
    backoff = std::min<pychron::Duration>(backoff * 2, std::chrono::seconds(30));
  }
}

}  // namespace pychron::experiment::measurement
