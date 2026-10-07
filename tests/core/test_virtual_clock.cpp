#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "pychron/core/virtual_clock.hpp"

using namespace pychron;
using namespace std::chrono_literals;

namespace {

using Real = std::chrono::steady_clock;

// A clock that loses a wake-up leaves a thread asleep for good. A test cannot
// unwind past such a thread, so each one runs under a real-time bound: when it
// is exceeded the process says so and aborts, well inside the ctest timeout.
class VirtualClockTest : public ::testing::Test {
 protected:
  ~VirtualClockTest() override {
    {
      std::lock_guard lock(mutex_);
      finished_ = true;
    }
    finished_cv_.notify_all();
    deadman_.join();
  }

 private:
  std::mutex mutex_;
  std::condition_variable finished_cv_;
  bool finished_ = false;
  std::thread deadman_{[this] {
    std::unique_lock lock(mutex_);
    if (finished_cv_.wait_for(lock, 45s, [this] { return finished_; })) return;
    std::fputs("VirtualClock test did not finish within 45 s of real time: a thread is stuck\n",
               stderr);
    std::abort();
  }};
};

// A helper thread whose end the test waits for with a real-time bound.
class Worker {
 public:
  template <class F>
  explicit Worker(F body) {
    std::packaged_task<void()> task(std::move(body));
    done_ = task.get_future();
    thread_ = std::thread(std::move(task));
  }
  ~Worker() {
    if (thread_.joinable()) thread_.join();
  }
  Worker(const Worker&) = delete;
  Worker& operator=(const Worker&) = delete;

  bool finished() const { return done_.wait_for(0s) == std::future_status::ready; }

  [[nodiscard]] bool join_within(std::chrono::seconds bound = 5s) {
    if (done_.wait_for(bound) != std::future_status::ready) return false;
    thread_.join();
    return true;
  }

 private:
  std::future<void> done_;
  std::thread thread_;
};

// A thread becomes a participant on its own, some real time after it is
// started. Until it has, the clock does not know to wait for it, so a test
// that is about to let time go first waits here (the test thread is runnable
// meanwhile, which holds time where it is).
[[nodiscard]] bool await_participants(const VirtualClock& clock, std::size_t n) {
  const auto give_up = Real::now() + 5s;
  while (clock.participants() != n) {
    if (Real::now() > give_up) return false;
    std::this_thread::yield();
  }
  return true;
}

// With the calling thread a participant that does not wait, a sleeper stays
// asleep and time stays where it is; once the caller sleeps too, both go on.
void expect_a_runnable_participant_holds_time(const VirtualClock& clock) {
  const std::size_t before = clock.participants();
  const TimePoint start = clock.now();
  Worker a([&] {
    Clock::Participant p(clock, "a");
    clock.sleep_for(1s);
  });
  ASSERT_TRUE(await_participants(clock, before + 1));
  std::this_thread::sleep_for(20ms);
  EXPECT_EQ(clock.now(), start);
  EXPECT_FALSE(a.finished());
  clock.sleep_for(1s);
  ASSERT_TRUE(a.join_within());
  EXPECT_EQ(clock.now(), start + 1s);
}

}  // namespace

TEST_F(VirtualClockTest, SleepForJumpsToTheDeadline) {
  VirtualClock clock;
  Clock::Participant main(clock, "test");
  const TimePoint kStart = clock.now();
  const auto real_start = Real::now();
  clock.sleep_for(20min);
  EXPECT_EQ(clock.now(), kStart + 20min);
  EXPECT_LT(Real::now() - real_start, 100ms);
}

TEST_F(VirtualClockTest, WaitersWakeInDeadlineOrderAtTheirOwnDeadlines) {
  VirtualClock clock;
  Clock::Participant main(clock, "test");
  const TimePoint kStart = clock.now();
  std::mutex m;
  std::vector<TimePoint> woke;
  auto sleeper = [&](Duration d) {
    return [&, d] {
      Clock::Participant p(clock, "sleeper");
      clock.sleep_for(d);
      const TimePoint t = clock.now();
      std::lock_guard lock(m);
      woke.push_back(t);
    };
  };
  Worker five(sleeper(5s));
  Worker two(sleeper(2s));
  ASSERT_TRUE(await_participants(clock, 3));
  clock.sleep_for(10s);
  ASSERT_TRUE(five.join_within());
  ASSERT_TRUE(two.join_within());
  EXPECT_EQ(clock.now(), kStart + 10s);
  ASSERT_EQ(woke.size(), 2u);
  EXPECT_EQ(woke[0], kStart + 2s);
  EXPECT_EQ(woke[1], kStart + 5s);
}

TEST_F(VirtualClockTest, ARunnableParticipantHoldsTime) {
  VirtualClock clock;
  Clock::Participant main(clock, "test");
  expect_a_runnable_participant_holds_time(clock);
}

TEST_F(VirtualClockTest, NotifyMakesTheWaiterRunBeforeAnyJump) {
  VirtualClock clock;
  Clock::Participant main(clock, "test");
  for (int round = 0; round < 10'000; ++round) {
    std::mutex m;
    std::condition_variable cv;
    bool flag = false;
    TimePoint seen{};
    Worker b([&] {
      Clock::Participant p(clock, "b");
      std::unique_lock lock(m);
      while (!flag) clock.wait(cv, lock);
      seen = clock.now();
    });
    ASSERT_TRUE(await_participants(clock, 2)) << "round " << round;
    TimePoint notified{};
    {
      std::lock_guard lock(m);
      flag = true;
      notified = clock.now();
    }
    clock.notify_all(cv);
    clock.sleep_for(1h);
    ASSERT_TRUE(b.join_within()) << "round " << round;
    ASSERT_EQ(seen, notified) << "round " << round;
    ASSERT_EQ(clock.now(), notified + 1h) << "round " << round;
  }
}

TEST_F(VirtualClockTest, ANonParticipantNotifyWakesAnUntimedWaiter) {
  VirtualClock clock;
  const TimePoint kStart = clock.now();
  std::mutex m;
  std::condition_variable cv;
  bool flag = false;
  Worker waiter([&] {
    Clock::Participant p(clock, "waiter");
    std::unique_lock lock(m);
    while (!flag) clock.wait(cv, lock);
  });
  ASSERT_TRUE(await_participants(clock, 1));
  // Not needed for the result: it only makes it likely the waiter is asleep.
  std::this_thread::sleep_for(5ms);
  EXPECT_FALSE(waiter.finished());
  {
    std::lock_guard lock(m);
    flag = true;
  }
  clock.notify_one(cv);
  ASSERT_TRUE(waiter.join_within());
  EXPECT_EQ(clock.now(), kStart);
  EXPECT_EQ(clock.participants(), 0u);
}

TEST_F(VirtualClockTest, DetachedDoesNotHoldTime) {
  VirtualClock clock;
  Clock::Participant main(clock, "test");
  const TimePoint kStart = clock.now();
  std::promise<void> detached;
  std::promise<void> outside_world;
  Worker a([&] {
    Clock::Participant p(clock, "a");
    Clock::Detached d(clock);
    detached.set_value();
    outside_world.get_future().wait();
  });
  ASSERT_EQ(detached.get_future().wait_for(5s), std::future_status::ready);
  clock.sleep_for(1s);
  EXPECT_EQ(clock.now(), kStart + 1s);
  EXPECT_FALSE(a.finished());
  outside_world.set_value();
  ASSERT_TRUE(a.join_within());
}

TEST_F(VirtualClockTest, LeaveByTheLastRunnableThreadJumps) {
  VirtualClock clock;  // the test thread is not a participant
  const TimePoint kStart = clock.now();
  TimePoint woke{};
  std::promise<void> release_b;
  // B first: were A alone it would be the last runnable thread itself.
  Worker b([&] {
    Clock::Participant p(clock, "b");
    release_b.get_future().wait();
  });
  ASSERT_TRUE(await_participants(clock, 1));
  Worker a([&] {
    Clock::Participant p(clock, "a");
    clock.sleep_for(3s);
    woke = clock.now();
  });
  ASSERT_TRUE(await_participants(clock, 2));
  // Not needed for the result: it only makes it likely that A is asleep, so
  // that the jump is B's leave and not A's own wait.
  std::this_thread::sleep_for(5ms);
  EXPECT_EQ(clock.now(), kStart);
  EXPECT_FALSE(a.finished());
  release_b.set_value();
  ASSERT_TRUE(b.join_within());
  ASSERT_TRUE(a.join_within());
  EXPECT_EQ(woke, kStart + 3s);
  EXPECT_EQ(clock.now(), kStart + 3s);
}

TEST_F(VirtualClockTest, WakingOnlyANonParticipantDoesNotEndTheJump) {
  VirtualClock clock;  // the test thread is not a participant
  const TimePoint kStart = clock.now();
  TimePoint woke{};
  std::promise<void> release_a;
  Worker a([&] {
    Clock::Participant p(clock, "a");
    release_a.get_future().wait();
    clock.sleep_for(3s);
    woke = clock.now();
  });
  ASSERT_TRUE(await_participants(clock, 1));
  Worker outsider([&] { clock.sleep_for(1s); });
  // Not needed for the result: it only makes it likely that the outsider is
  // asleep with the earlier deadline when A, the only participant, blocks.
  std::this_thread::sleep_for(5ms);
  release_a.set_value();
  ASSERT_TRUE(a.join_within());
  ASSERT_TRUE(outsider.join_within());
  EXPECT_EQ(woke, kStart + 3s);
}

TEST_F(VirtualClockTest, NestedParticipantCountsOnce) {
  VirtualClock clock;
  Clock::Participant outer(clock, "test");
  {
    Clock::Participant inner(clock, "test again");
    EXPECT_EQ(clock.participants(), 1u);
  }
  EXPECT_EQ(clock.participants(), 1u);
  expect_a_runnable_participant_holds_time(clock);
}

TEST_F(VirtualClockTest, PastDeadlineAndNonPositiveSleepReturnAtOnce) {
  VirtualClock clock;
  Clock::Participant main(clock, "test");
  const TimePoint kStart = clock.now();
  std::mutex m;
  std::condition_variable cv;
  std::unique_lock lock(m);
  clock.wait_until(cv, lock, kStart - 1s);
  EXPECT_TRUE(lock.owns_lock());
  clock.wait_until(cv, lock, kStart);
  EXPECT_TRUE(lock.owns_lock());
  clock.sleep_for(0s);
  clock.sleep_for(-1s);
  EXPECT_EQ(clock.now(), kStart);
}

TEST_F(VirtualClockTest, ThreadsStartingAndLeavingNeverLoseAJump) {
  VirtualClock clock;
  Clock::Participant main(clock, "test");
  for (int round = 0; round < 2'000; ++round) {
    const TimePoint start = clock.now();
    std::atomic<int> entered{0};
    std::vector<std::unique_ptr<Worker>> workers;
    for (int i = 0; i < 4; ++i) {
      workers.push_back(std::make_unique<Worker>([&clock, &entered, i] {
        Clock::Participant p(clock, "worker");
        entered.fetch_add(1);
        clock.sleep_for(i * 1ms);
      }));
    }
    // The clock cannot wait for a thread it has not heard of, so every worker
    // has entered before this thread lets time go. Where each one is between
    // entering, sleeping and leaving is left to the race.
    const auto give_up = Real::now() + 5s;
    while (entered.load() != 4) {
      ASSERT_LT(Real::now(), give_up) << "round " << round;
      std::this_thread::yield();
    }
    clock.sleep_for(10ms);
    for (auto& w : workers) ASSERT_TRUE(w->join_within()) << "round " << round;
    ASSERT_EQ(clock.now(), start + 10ms) << "round " << round;
    ASSERT_EQ(clock.participants(), 1u) << "round " << round;
  }
}

TEST_F(VirtualClockTest, WallNowIsEpochPlusElapsed) {
  VirtualClock::Options options;
  options.epoch = WallTime{} + std::chrono::hours(24 * 365 * 30);
  VirtualClock clock(options);
  Clock::Participant main(clock, "test");
  EXPECT_EQ(clock.wall_now(), options.epoch);
  clock.sleep_for(90s);
  EXPECT_EQ(clock.wall_now(), options.epoch + 90s);
}
