#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "pychron/core/clock.hpp"

using namespace pychron;
using namespace std::chrono_literals;

TEST(ManualClock, OnlyMovesWhenAdvanced) {
  ManualClock clock;
  const auto t0 = clock.now();
  std::this_thread::sleep_for(2ms);
  EXPECT_EQ(clock.now(), t0);
  clock.advance(250ms);
  EXPECT_EQ(clock.now() - t0, Duration(250ms));
  clock.set(t0 + 1s);
  EXPECT_EQ(clock.now() - t0, Duration(1s));
}

TEST(ManualClock, WaitUntilReturnsImmediatelyWhenDeadlinePassed) {
  ManualClock clock;
  std::mutex m;
  std::condition_variable cv;
  std::unique_lock lock(m);
  clock.wait_until(cv, lock, clock.now());  // must not hang
  SUCCEED();
}

TEST(SteadyClock, IsMonotonic) {
  SteadyClock clock;
  const auto a = clock.now();
  const auto b = clock.now();
  EXPECT_LE(a, b);
}

TEST(SteadyClock, WaitUntilHonoursDeadline) {
  SteadyClock clock;
  std::mutex m;
  std::condition_variable cv;
  std::unique_lock lock(m);
  const auto deadline = clock.now() + 5ms;
  while (clock.now() < deadline) clock.wait_until(cv, lock, deadline);
  EXPECT_GE(clock.now(), deadline);
}

TEST(SteadyClock, WallNowIsSystemTime) {
  SteadyClock clock;
  const auto diff = clock.wall_now() - std::chrono::system_clock::now();
  EXPECT_LT(diff < 0s ? -diff : diff, 1s);
}

TEST(SteadyClock, WaitAndNotifyPassThrough) {
  SteadyClock clock;
  std::mutex m;
  std::condition_variable cv;
  bool flag = false;
  std::thread waiter([&] {
    std::unique_lock lock(m);
    while (!flag) clock.wait(cv, lock);
  });
  {
    std::lock_guard lock(m);
    flag = true;
  }
  clock.notify_all(cv);
  waiter.join();
  SUCCEED();
}

TEST(ManualClock, WallNowFollowsAdvance) {
  const WallTime epoch = WallTime{} + 1000h;
  ManualClock clock(TimePoint{}, epoch);
  EXPECT_EQ(clock.wall_now(), epoch);
  clock.advance(90s);
  EXPECT_EQ(clock.wall_now(), epoch + 90s);
}

TEST(ManualClock, SleepForReturnsWhenAdvanced) {
  ManualClock clock;
  std::atomic<bool> woke{false};
  std::thread sleeper([&] {
    clock.sleep_for(5s);
    woke = true;
  });
  // The sleeper may not have read the clock yet; advancing until it wakes
  // does not depend on when it does.
  while (!woke) {
    clock.advance(5s);
    std::this_thread::yield();
  }
  sleeper.join();
  SUCCEED();
}

TEST(Clock, GuardsAreNoOpsOnSteadyClock) {
  SteadyClock clock;
  const auto before = clock.now();
  {
    Clock::Participant p(clock, "test");
    Clock::Detached d(clock);
  }
  EXPECT_GE(clock.now(), before);
}
