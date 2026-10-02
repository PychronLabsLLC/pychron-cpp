#pragma once

// ClockPump: simulated time for apps (elctl --sim-speed, pychron-ui
// --sim-speed). A thread advances a ManualClock by `speed` ms every real ms
// and, when a scheduler is driven, runs its due jobs inline after each step.
//
// Driving the scheduler from the pump keeps polling in step with the clock
// however the threads are scheduled: a dispatcher thread starved for a few
// real milliseconds would otherwise miss whole integrations at high speeds.
// The driven scheduler must have threads = 0 and no dispatcher (it runs on
// the pump thread), and must outlive the pump or be released first with
// drive(nullptr), which waits for a step in progress.

#include <atomic>
#include <mutex>
#include <thread>

#include "pychron/core/clock.hpp"
#include "pychron/core/scheduler.hpp"

namespace pychron {

class ClockPump {
 public:
  // `clock` must outlive the pump. speed > 0.
  ClockPump(ManualClock& clock, double speed);
  ~ClockPump();
  ClockPump(const ClockPump&) = delete;
  ClockPump& operator=(const ClockPump&) = delete;

  void drive(Scheduler* scheduler);
  void stop();  // joins the pump thread; idempotent
  double speed() const noexcept { return speed_; }

 private:
  ManualClock& clock_;
  double speed_;
  std::mutex mutex_;  // held across a step's run_pending()
  Scheduler* scheduler_ = nullptr;
  std::atomic<bool> running_{true};
  std::thread thread_;
};

}  // namespace pychron
