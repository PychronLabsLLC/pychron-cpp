#include <gtest/gtest.h>

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
