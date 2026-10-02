#include "pychron/core/clock_pump.hpp"

#include <chrono>

namespace pychron {

ClockPump::ClockPump(ManualClock& clock, double speed) : clock_(clock), speed_(speed) {
  thread_ = std::thread([this] {
    const auto step = std::chrono::duration_cast<Duration>(std::chrono::duration<double>(0.001 * speed_));
    while (running_) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
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
