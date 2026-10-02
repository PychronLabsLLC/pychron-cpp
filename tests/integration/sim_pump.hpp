#pragma once

#include <atomic>
#include <chrono>
#include <thread>

#include "pychron/core/clock.hpp"
#include "pychron/core/scheduler.hpp"

namespace pychron::testing {

// Lets the threads under test run for about `d` of real time.
inline void pace(std::chrono::microseconds d) {
#ifdef _WIN32
  // sleep_for rounds up to the timer tick (1 to 16 ms on Windows), which makes
  // a 200 us pump some fifty times slower and times the tests out.
  const auto until = std::chrono::steady_clock::now() + d;
  while (std::chrono::steady_clock::now() < until) std::this_thread::yield();
#else
  std::this_thread::sleep_for(d);
#endif
}

// Advances a ManualClock and runs the Scheduler from a helper thread: 20 ms of
// simulated time every 200 us of real time.
class Pump {
 public:
  Pump(ManualClock& clock, Scheduler& scheduler) : clock_(clock), scheduler_(scheduler) {
    thread_ = std::thread([this] {
      while (!done_) {
        clock_.advance(std::chrono::milliseconds(20));
        scheduler_.run_pending();
        pace(std::chrono::microseconds(200));
      }
    });
  }
  ~Pump() {
    done_ = true;
    thread_.join();
  }

 private:
  ManualClock& clock_;
  Scheduler& scheduler_;
  std::atomic<bool> done_{false};
  std::thread thread_;
};

}  // namespace pychron::testing
