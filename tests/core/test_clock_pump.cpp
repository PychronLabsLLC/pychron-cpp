#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "pychron/core/clock_pump.hpp"

namespace pychron {
namespace {

using namespace std::chrono_literals;

// Waits (real time) until `done` or 5 s.
template <class F>
bool eventually(F done) {
  const auto until = std::chrono::steady_clock::now() + 5s;
  while (!done()) {
    if (std::chrono::steady_clock::now() > until) return false;
    std::this_thread::sleep_for(1ms);
  }
  return true;
}

TEST(ClockPump, AdvancesTheClockSpeedTimesFaster) {
  ManualClock clock;
  const TimePoint start = clock.now();
  ClockPump pump(clock, 1000);  // 1 s of clock per real ms
  EXPECT_DOUBLE_EQ(pump.speed(), 1000);
  ASSERT_TRUE(eventually([&] { return clock.now() - start >= 50s; }));
  pump.stop();
  const TimePoint stopped = clock.now();
  std::this_thread::sleep_for(20ms);
  EXPECT_EQ(clock.now(), stopped);
  pump.stop();  // idempotent
}

TEST(ClockPump, RunsADrivenSchedulersJobsInlineUntilReleased) {
  ManualClock clock;
  Scheduler scheduler(clock, nullptr, Scheduler::Options{0});
  std::atomic<int> runs{0};
  std::atomic<std::thread::id> where{};
  ASSERT_TRUE(scheduler.every("tick", 100ms, [&] {
    where = std::this_thread::get_id();
    ++runs;
  }));
  ClockPump pump(clock, 1000);
  std::this_thread::sleep_for(20ms);
  EXPECT_EQ(runs.load(), 0);  // not driven yet: nothing dispatches
  pump.drive(&scheduler);
  ASSERT_TRUE(eventually([&] { return runs >= 5; }));
  EXPECT_NE(where.load(), std::this_thread::get_id());  // ran on the pump thread
  pump.drive(nullptr);  // waits for a step in progress
  const int after = runs;
  std::this_thread::sleep_for(20ms);
  EXPECT_EQ(runs.load(), after);
}

}  // namespace
}  // namespace pychron
