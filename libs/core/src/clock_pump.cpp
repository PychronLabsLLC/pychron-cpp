#include "pychron/core/clock_pump.hpp"

#include <chrono>

namespace pychron {

ClockPump::ClockPump(ManualClock& clock, double speed) : clock_(clock), speed_(speed) {
  thread_ = std::thread([this] {
    auto last = std::chrono::steady_clock::now();
    while (running_) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      // By the time that really passed, not by the millisecond asked for: on
      // a loaded machine, or one with coarse timers (the macOS runners), a
      // millisecond's sleep is several, and simulated time would fall behind
      // its speed by as much.
      const auto now = std::chrono::steady_clock::now();
      const auto step = std::chrono::duration_cast<Duration>((now - last) * speed_);
      last = now;
      clock_.advance(step);
      std::lock_guard lock(mutex_);
      if (scheduler_ != nullptr) scheduler_->run_pending();
    }
  });
}

ClockPump::~ClockPump() { stop(); }

void ClockPump::drive(Scheduler* scheduler) {
  std::lock_guard lock(mutex_);
  scheduler_ = scheduler;
}

void ClockPump::stop() {
  running_ = false;
  if (thread_.joinable()) thread_.join();
}

}  // namespace pychron
