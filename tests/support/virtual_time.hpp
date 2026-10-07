#pragma once

// Helpers for tests that run on a VirtualClock.
//
// Code that waits past the clock (a raw cv.wait, a join) leaves time standing
// and the test asleep in it, and a test cannot unwind past a thread that is
// stuck. So a test on a VirtualClock runs under a real-time bound (DeadMan,
// VirtualTimeTest), and what it has to wait for in real time (another thread
// getting as far as its sleep in the clock) it waits for with a limit
// (eventually_real, await_waiters, await_participants): a test fails with a
// message, it does not hang.

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include "pychron/core/virtual_clock.hpp"

namespace pychron::testing {

// Alive for longer than `limit` of real time, it says `what` did not finish
// and aborts the process, well inside the ctest timeout.
class DeadMan {
 public:
  explicit DeadMan(std::string what, std::chrono::seconds limit = std::chrono::seconds(30))
      : thread_([this, what = std::move(what), limit] {
          std::unique_lock lock(mutex_);
          if (finished_cv_.wait_for(lock, limit, [this] { return finished_; })) return;
          std::fprintf(stderr, "%s did not finish within %lld s of real time: a thread is stuck\n",
                       what.c_str(), static_cast<long long>(limit.count()));
          std::abort();
        }) {}
  ~DeadMan() {
    {
      std::lock_guard lock(mutex_);
      finished_ = true;
    }
    finished_cv_.notify_all();
    thread_.join();
  }
  DeadMan(const DeadMan&) = delete;
  DeadMan& operator=(const DeadMan&) = delete;

 private:
  std::mutex mutex_;
  std::condition_variable finished_cv_;
  bool finished_ = false;
  std::thread thread_;  // last: it uses the members above
};

// The fixture for tests on a VirtualClock: each test runs under a DeadMan
// that names it.
class VirtualTimeTest : public ::testing::Test {
 protected:
  explicit VirtualTimeTest(std::chrono::seconds limit = std::chrono::seconds(30))
      : deadman_(current_test_name(), limit) {}

 private:
  static std::string current_test_name() {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    if (info == nullptr) return "the test";
    return std::string(info->test_suite_name()) + "." + info->name();
  }

  DeadMan deadman_;
};

// Waits, in real time, until `pred` holds; false when `limit` passes first.
// For what another thread does in its own time. While the calling thread is a
// participant that does not wait in the clock, time stays where it is.
template <class Pred>
[[nodiscard]] bool eventually_real(Pred pred, std::chrono::milliseconds limit = std::chrono::seconds(5)) {
  const auto give_up = std::chrono::steady_clock::now() + limit;
  while (!pred()) {
    if (std::chrono::steady_clock::now() > give_up) return false;
    std::this_thread::yield();
  }
  return true;
}

// A thread becomes a participant on its own, some real time after it is
// started. Until it has, the clock does not know to wait for it, so a test
// that is about to let time go first waits here.
[[nodiscard]] inline bool await_participants(const VirtualClock& clock, std::size_t n) {
  return eventually_real([&] { return clock.participants() == n; });
}

// The same for threads that have to be asleep in the clock, not merely
// started, before the test goes on: `n` waits registered and not woken.
[[nodiscard]] inline bool await_waiters(const VirtualClock& clock, std::size_t n) {
  return eventually_real([&] { return clock.waiters() == n; });
}

}  // namespace pychron::testing
