// ClockMutex: a plain mutex on a SteadyClock, and on a VirtualClock one whose
// contenders are asleep in the clock, so time goes on for the holder.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "pychron/core/clock_mutex.hpp"
#include "pychron/core/virtual_clock.hpp"
#include "virtual_time.hpp"

using namespace std::chrono_literals;
using pychron::Clock;
using pychron::SteadyClock;
using pychron::TimePoint;
using pychron::VirtualClock;
using pychron::testing::await_waiters;
using pychron::testing::Crew;

namespace {

// (The fixture has the suite's name, so the class is spelt pychron::ClockMutex.)
struct ClockMutex : pychron::testing::VirtualTimeTest {};

struct ClockMutexVirtual : pychron::testing::VirtualTimeTest {
  VirtualClock clock;
  Clock::Participant main{clock, "test"};
  const TimePoint kStart = clock.now();
};

}  // namespace

TEST_F(ClockMutex, ExcludesOnSteadyClock) {
  SteadyClock clock;
  pychron::ClockMutex mutex(clock);
  int total = 0;  // plain: only the mutex keeps the increments apart
  std::vector<std::thread> threads;
  for (int t = 0; t < 4; ++t) {
    threads.emplace_back([&] {
      for (int i = 0; i < 10000; ++i) {
        std::lock_guard lock(mutex);
        ++total;
      }
    });
  }
  for (auto& t : threads) t.join();
  EXPECT_EQ(total, 40000);
}

TEST_F(ClockMutex, TryLockFailsWhileHeld) {
  SteadyClock clock;
  pychron::ClockMutex mutex(clock);
  ASSERT_TRUE(mutex.try_lock());
  EXPECT_FALSE(mutex.try_lock());
  // Nor from another thread.
  bool other = true;
  std::thread([&] { other = mutex.try_lock(); }).join();
  EXPECT_FALSE(other);
  mutex.unlock();
  std::unique_lock again(mutex, std::try_to_lock);
  EXPECT_TRUE(again.owns_lock());
}

// The holder sleeps in the clock with the mutex; the contender has to be
// asleep in the clock too, or nobody moves time on and the holder never wakes.
TEST_F(ClockMutexVirtual, AContenderDoesNotHoldTime) {
  pychron::ClockMutex mutex(clock);
  const auto real_start = std::chrono::steady_clock::now();
  TimePoint got{};
  Crew crew(clock);
  crew.start("holder", [&] {
    std::lock_guard lock(mutex);
    clock.sleep_for(5s);
  });
  ASSERT_TRUE(await_waiters(clock, 1));  // the holder, asleep with the mutex
  crew.start("contender", [&] {
    std::lock_guard lock(mutex);
    got = clock.now();
  });
  crew.join();
  EXPECT_EQ(got, kStart + 5s);
  EXPECT_EQ(clock.now(), kStart + 5s);
  EXPECT_LT(std::chrono::steady_clock::now() - real_start, 5s);
}

TEST_F(ClockMutexVirtual, HandOffIsInOrderOfNothingButCorrectness) {
  pychron::ClockMutex mutex(clock);
  std::vector<TimePoint> taken;  // under `mutex`
  Crew crew(clock);
  for (int i = 0; i < 3; ++i) {
    crew.start("contender " + std::to_string(i), [&] {
      std::lock_guard lock(mutex);
      taken.push_back(clock.now());
      clock.sleep_for(1s);
    });
  }
  crew.join();
  // One at a time, a second each, whoever came first.
  std::sort(taken.begin(), taken.end());
  EXPECT_EQ(taken, (std::vector<TimePoint>{kStart, kStart + 1s, kStart + 2s}));
  EXPECT_EQ(clock.now(), kStart + 3s);
}
