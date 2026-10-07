#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "pychron/core/virtual_clock.hpp"
#include "virtual_time.hpp"

using namespace pychron;
using namespace std::chrono_literals;

namespace {

using Real = std::chrono::steady_clock;
using pychron::testing::await_participants;
using pychron::testing::await_waiters;

// A clock that loses a wake-up leaves a thread asleep for good. A test cannot
// unwind past such a thread, so each one runs under a real-time bound: when it
// is exceeded the process says so and aborts, well inside the ctest timeout.
class VirtualClockTest : public pychron::testing::VirtualTimeTest {
 protected:
  VirtualClockTest() : VirtualTimeTest(45s) {}
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
  EXPECT_LT(Real::now() - real_start, 5s);
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
    ASSERT_TRUE(await_waiters(clock, 1)) << "round " << round;
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
  ASSERT_TRUE(await_waiters(clock, 1));
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
  // B is asleep in the clock before this thread sleeps; A, detached, holds
  // neither of them back, and B wakes at its own deadline.
  TimePoint woke{};
  Worker b([&] {
    Clock::Participant p(clock, "b");
    clock.sleep_for(500ms);
    woke = clock.now();
  });
  ASSERT_TRUE(await_waiters(clock, 1));
  EXPECT_EQ(clock.now(), kStart);
  clock.sleep_for(1s);
  EXPECT_EQ(clock.now(), kStart + 1s);
  EXPECT_FALSE(a.finished());
  ASSERT_TRUE(b.join_within());
  EXPECT_EQ(woke, kStart + 500ms);
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
  // A is asleep, so the jump is B's leave and not A's own wait.
  ASSERT_TRUE(await_waiters(clock, 1));
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
  // The outsider is asleep with the earlier deadline when A, the only
  // participant, blocks.
  ASSERT_TRUE(await_waiters(clock, 1));
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

namespace {

void expect_threads_starting_and_leaving_never_lose_a_jump(double speed, int rounds) {
  VirtualClock::Options options;
  options.speed = speed;
  VirtualClock clock(options);
  Clock::Participant main(clock, "test");
  for (int round = 0; round < rounds; ++round) {
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

}  // namespace

TEST_F(VirtualClockTest, ThreadsStartingAndLeavingNeverLoseAJump) {
  expect_threads_starting_and_leaving_never_lose_a_jump(std::numeric_limits<double>::infinity(),
                                                        2'000);
}

// The same race with every jump paced (ten simulated milliseconds cost ten
// real microseconds), so that sleeps are interrupted and handed over.
TEST_F(VirtualClockTest, PacedThreadsStartingAndLeavingNeverLoseAJump) {
  expect_threads_starting_and_leaving_never_lose_a_jump(1'000, 1'000);
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

// A hundred simulated seconds at ten thousand to one are ten real
// milliseconds. A pace cannot end early, so the lower bound is exact; the
// upper one only has to be far below the hundred seconds an unpaced-for speed
// would take, and leaves a slow machine its room.
TEST_F(VirtualClockTest, PacingTakesDeltaOverSpeed) {
  VirtualClock::Options options;
  options.speed = 10'000;
  VirtualClock clock(options);
  Clock::Participant main(clock, "test");
  const TimePoint kStart = clock.now();
  const auto real_start = Real::now();
  clock.sleep_for(100s);
  const auto real = Real::now() - real_start;
  EXPECT_GE(real, 8ms);
  EXPECT_LT(real, 5s);
  EXPECT_EQ(clock.now(), kStart + 100s);
}

namespace {

// What a thread woken out of a paced jump saw.
struct Woken {
  TimePoint now{};
  Real::time_point real{};
};

// One participant waits untimed on a flag, another sleeps sixty seconds at
// speed one; `waiter_blocks_last` says which of them is asleep first and so
// which one does the pacing sleep. The test thread, no participant, notifies.
void expect_a_notify_ends_the_pacing_sleep(bool waiter_blocks_last) {
  VirtualClock::Options options;
  options.speed = 1;
  VirtualClock clock(options);
  const TimePoint kStart = clock.now();
  std::mutex m;
  std::condition_variable cv;
  bool flag = false;
  Woken woken;
  std::unique_ptr<Worker> w;
  std::unique_ptr<Worker> sleeper;
  const auto start_waiter = [&] {
    w = std::make_unique<Worker>([&] {
      Clock::Participant p(clock, "w");
      std::unique_lock lock(m);
      while (!flag) clock.wait(cv, lock);
      woken = {clock.now(), Real::now()};
    });
  };
  const auto start_sleeper = [&] {
    sleeper = std::make_unique<Worker>([&] {
      Clock::Participant p(clock, "sleeper");
      clock.sleep_for(60s);
    });
  };
  // The first is alone and asleep (an untimed wait moves nothing; a lone
  // sleeper starts pacing) before the second exists.
  if (waiter_blocks_last) {
    start_sleeper();
    ASSERT_TRUE(await_waiters(clock, 1));
    start_waiter();
  } else {
    start_waiter();
    ASSERT_TRUE(await_waiters(clock, 1));
    start_sleeper();
  }
  ASSERT_TRUE(await_waiters(clock, 2));
  std::this_thread::sleep_for(50ms);
  Real::time_point notified;
  {
    std::lock_guard lock(m);
    flag = true;
    notified = Real::now();
  }
  clock.notify_all(cv);
  ASSERT_TRUE(w->join_within());
  EXPECT_LT(woken.real - notified, 5s);  // not the minute the sleep is for
  EXPECT_GE(woken.now - kStart, 20ms);
  EXPECT_LE(woken.now - kStart, 5s);
  // W has left and the sleeper is pacing the rest of its minute: let it go.
  EXPECT_FALSE(sleeper->finished());
  clock.set_speed(std::numeric_limits<double>::infinity());
  ASSERT_TRUE(sleeper->join_within());
  EXPECT_EQ(clock.now(), kStart + 60s);
}

}  // namespace

TEST_F(VirtualClockTest, ANotifyEndsThePacingSleep) {
  expect_a_notify_ends_the_pacing_sleep(/*waiter_blocks_last=*/false);
}

TEST_F(VirtualClockTest, ANotifyEndsThePacingSleepOfTheNotifiedThread) {
  expect_a_notify_ends_the_pacing_sleep(/*waiter_blocks_last=*/true);
}

TEST_F(VirtualClockTest, SetSpeedTakesEffectDuringASleep) {
  VirtualClock::Options options;
  options.speed = 1;
  VirtualClock clock(options);
  const TimePoint kStart = clock.now();
  Woken woken;
  Worker sleeper([&] {
    Clock::Participant p(clock, "sleeper");
    clock.sleep_for(100s);
    woken = {clock.now(), Real::now()};
  });
  ASSERT_TRUE(await_waiters(clock, 1));
  const auto real_set = Real::now();
  clock.set_speed(std::numeric_limits<double>::infinity());
  ASSERT_TRUE(sleeper.join_within());
  EXPECT_LT(woken.real - real_set, 5s);  // not the hundred seconds left
  EXPECT_EQ(woken.now, kStart + 100s);
  EXPECT_EQ(clock.now(), kStart + 100s);
}

TEST_F(VirtualClockTest, ALeaveHandsThePacingToASleeper) {
  VirtualClock::Options options;
  options.speed = 1;
  VirtualClock clock(options);
  const TimePoint kStart = clock.now();
  std::promise<void> release_b;
  Real::duration leave_took{};
  Worker b([&] {
    auto p = std::make_unique<Clock::Participant>(clock, "b");
    release_b.get_future().wait();
    const auto real_before = Real::now();
    p.reset();
    leave_took = Real::now() - real_before;
  });
  ASSERT_TRUE(await_participants(clock, 1));
  Worker a([&] {
    Clock::Participant p(clock, "a");
    clock.sleep_for(100s);
  });
  ASSERT_TRUE(await_waiters(clock, 1));
  release_b.set_value();
  // B's leave returns at once; the hundred real seconds are for A to sleep.
  ASSERT_TRUE(b.join_within());
  EXPECT_LT(leave_took, 5s);
  EXPECT_FALSE(a.finished());
  // A is pacing, or nobody would hear this.
  clock.set_speed(std::numeric_limits<double>::infinity());
  ASSERT_TRUE(a.join_within());
  EXPECT_EQ(clock.now(), kStart + 100s);
}

TEST_F(VirtualClockTest, NowAdvancesDuringAPace) {
  VirtualClock::Options options;
  options.speed = 1;
  VirtualClock clock(options);  // the test thread is not a participant
  const TimePoint kStart = clock.now();
  Worker sleeper([&] {
    Clock::Participant p(clock, "sleeper");
    clock.sleep_for(60s);
  });
  ASSERT_TRUE(await_waiters(clock, 1));
  const TimePoint first = clock.now();
  const WallTime wall_first = clock.wall_now();
  std::this_thread::sleep_for(50ms);
  const TimePoint second = clock.now();
  const WallTime wall_second = clock.wall_now();
  EXPECT_GE(second - first, 30ms);
  EXPECT_LE(second - first, 5s);
  EXPECT_GE(wall_second - wall_first, 30ms);
  EXPECT_LE(wall_second - wall_first, 5s);
  clock.set_speed(std::numeric_limits<double>::infinity());
  ASSERT_TRUE(sleeper.join_within());
  EXPECT_EQ(clock.now(), kStart + 60s);
}

TEST_F(VirtualClockTest, AnOutsidersSleepDuringAPaceTakesItsOwnTime) {
  VirtualClock::Options options;
  options.speed = 1;
  VirtualClock clock(options);  // the test thread is not a participant
  const TimePoint kStart = clock.now();
  Worker sleeper([&] {
    Clock::Participant p(clock, "sleeper");
    clock.sleep_for(60s);
  });
  ASSERT_TRUE(await_waiters(clock, 1));
  std::this_thread::sleep_for(50ms);
  const auto real_before = Real::now();
  const TimePoint before = clock.now();
  clock.sleep_for(100ms);
  const TimePoint after = clock.now();
  const auto real = Real::now() - real_before;
  EXPECT_GE(real, 90ms);
  EXPECT_LT(real, 5s);
  EXPECT_GE(after - before, 100ms);
  clock.set_speed(std::numeric_limits<double>::infinity());
  ASSERT_TRUE(sleeper.join_within());
  EXPECT_EQ(clock.now(), kStart + 60s);
}

namespace {

// A thread outside the clock that waits on a condition variable and is woken
// through the clock again and again: every such notify interrupts a pacing
// sleep and leaves nobody runnable.
class Heckler {
 public:
  Heckler(const VirtualClock& clock, std::chrono::microseconds every)
      : clock_(clock),
        listener_([this] {
          std::unique_lock lock(m_);
          while (!done_) clock_.wait(cv_, lock);
        }),
        notifier_([this, every] {
          while (!stop_.load()) {
            clock_.notify_all(cv_);
            std::this_thread::sleep_for(every);
          }
        }) {}
  ~Heckler() {
    stop_.store(true);
    notifier_.join();
    {
      std::lock_guard lock(m_);
      done_ = true;
    }
    clock_.notify_all(cv_);
    listener_.join();
  }

 private:
  const VirtualClock& clock_;
  std::mutex m_;
  std::condition_variable cv_;
  bool done_ = false;
  std::atomic<bool> stop_{false};
  std::thread listener_;
  std::thread notifier_;
};

}  // namespace

TEST_F(VirtualClockTest, NowNeverGoesBackwards) {
  VirtualClock::Options options;
  options.speed = 1'000;
  VirtualClock clock(options);  // the test thread is not a participant
  const TimePoint kStart = clock.now();
  std::atomic<bool> done{false};
  std::atomic<int> backwards{0};
  std::atomic<int> reading{0};
  const auto reader = [&] {
    TimePoint last = clock.now();
    reading.fetch_add(1);
    while (!done.load()) {
      const TimePoint t = clock.now();
      if (t < last) backwards.fetch_add(1);
      last = t;
    }
  };
  Worker first(reader);
  Worker second(reader);
  while (reading.load() != 2) std::this_thread::yield();
  {
    Heckler heckler(clock, 100us);
    Worker sleeper([&] {
      Clock::Participant p(clock, "sleeper");
      for (int i = 0; i < 200; ++i) clock.sleep_for(10ms);
    });
    EXPECT_TRUE(sleeper.join_within());
  }
  done.store(true);
  ASSERT_TRUE(first.join_within());
  ASSERT_TRUE(second.join_within());
  EXPECT_EQ(backwards.load(), 0);
  EXPECT_EQ(clock.now(), kStart + 2s);
}

TEST_F(VirtualClockTest, FrequentNotifiesDoNotSlowPacedTime) {
  VirtualClock::Options options;
  options.speed = 100;
  VirtualClock clock(options);  // the test thread is not a participant
  const TimePoint kStart = clock.now();
  Real::duration real{};
  {
    Heckler heckler(clock, 200us);
    Worker sleeper([&] {
      Clock::Participant p(clock, "sleeper");
      const auto real_start = Real::now();
      clock.sleep_for(10s);
      real = Real::now() - real_start;
    });
    EXPECT_TRUE(sleeper.join_within());
  }
  EXPECT_GE(real, 90ms);
  // A notify that started the sleep over would never let it end; one that
  // only costs a little each time has room here on a slow machine.
  EXPECT_LT(real, 10s);
  EXPECT_EQ(clock.now(), kStart + 10s);
}

TEST_F(VirtualClockTest, AHoldEndsThePacingSleepAndKeepsTimeStill) {
  VirtualClock::Options options;
  options.speed = 1;
  VirtualClock clock(options);  // the test thread is not a participant
  const TimePoint kStart = clock.now();
  Worker sleeper([&] {
    Clock::Participant p(clock, "sleeper");
    clock.sleep_for(60s);
  });
  ASSERT_TRUE(await_waiters(clock, 1));
  std::this_thread::sleep_for(20ms);
  {
    Clock::Hold hold(clock);
    // What had been paid for is kept, and no more is taken.
    const TimePoint held = clock.now();
    EXPECT_GE(held - kStart, 20ms);
    EXPECT_LT(held - kStart, 5s);
    std::this_thread::sleep_for(50ms);
    EXPECT_EQ(clock.now(), held);
    // Not even for nothing.
    clock.set_speed(std::numeric_limits<double>::infinity());
    std::this_thread::sleep_for(20ms);
    EXPECT_EQ(clock.now(), held);
    EXPECT_FALSE(sleeper.finished());
  }
  ASSERT_TRUE(sleeper.join_within());
  EXPECT_EQ(clock.now(), kStart + 60s);
}

TEST_F(VirtualClockTest, AStallUnderAHoldSaysSo) {
  std::mutex reports_mutex;
  std::vector<std::string> reports;
  VirtualClock::Options options;
  options.stall_report_after = 50ms;
  options.on_stall = [&](std::string report) {
    std::lock_guard lock(reports_mutex);
    reports.push_back(std::move(report));
  };
  VirtualClock clock(options);  // the test thread is not a participant
  const TimePoint kStart = clock.now();
  auto hold = std::make_unique<Clock::Hold>(clock);
  Worker sleeper([&] {
    Clock::Participant p(clock, "sleeper");
    clock.sleep_for(1s);
  });
  const auto give_up = Real::now() + 5s;
  for (;;) {
    {
      std::lock_guard lock(reports_mutex);
      if (!reports.empty()) break;
    }
    if (Real::now() > give_up) break;
    std::this_thread::sleep_for(1ms);
  }
  EXPECT_EQ(clock.now(), kStart);
  hold.reset();
  ASSERT_TRUE(sleeper.join_within());
  EXPECT_EQ(clock.now(), kStart + 1s);
  std::lock_guard lock(reports_mutex);
  ASSERT_EQ(reports.size(), 1u);
  EXPECT_EQ(reports[0], "virtual clock held; a thread being started has not entered");
}

// An outsider with the earliest deadline is the one a leave hands the pacing
// to. Notified in the middle of that sleep it goes back to its caller at once
// and the sleep passes to the participant behind it.
TEST_F(VirtualClockTest, ANotifiedOutsiderPassesThePacingOn) {
  VirtualClock::Options options;
  options.speed = 1;
  VirtualClock clock(options);  // the test thread is not a participant
  const TimePoint kStart = clock.now();
  std::promise<void> release_b;
  Worker b([&] {
    Clock::Participant p(clock, "b");
    release_b.get_future().wait();
  });
  ASSERT_TRUE(await_participants(clock, 1));
  Worker a([&] {
    Clock::Participant p(clock, "a");
    clock.sleep_for(100s);
  });
  std::mutex m;
  std::condition_variable cv;
  Worker outsider([&] {
    std::unique_lock lock(m);
    clock.wait_until(cv, lock, kStart + 50s);
  });
  ASSERT_TRUE(await_waiters(clock, 2));
  release_b.set_value();
  ASSERT_TRUE(b.join_within());
  // Not needed for the result: it only makes it likely that the outsider has
  // begun the fifty real seconds.
  std::this_thread::sleep_for(20ms);
  clock.notify_all(cv);
  const bool returned = outsider.join_within();
  EXPECT_TRUE(returned);
  EXPECT_LT(clock.now(), kStart + 50s);
  // A is pacing now, or nobody would hear this.
  EXPECT_FALSE(a.finished());
  clock.set_speed(std::numeric_limits<double>::infinity());
  ASSERT_TRUE(a.join_within());
  EXPECT_EQ(clock.now(), kStart + 100s);
}

// The same hand-over with the notify racing the request to pace: it lands
// before the outsider was asked, before it woke to the request (on even
// rounds B notifies straight after its leave, which is that case as often as
// not) or in the middle of its sleep. However it falls, time gets to A.
TEST_F(VirtualClockTest, APacingRequestIsNeverDropped) {
  VirtualClock::Options options;
  options.speed = 10'000;
  VirtualClock clock(options);  // the test thread is not a participant
  for (int round = 0; round < 1'000; ++round) {
    const TimePoint start = clock.now();
    const bool b_notifies = round % 2 == 0;
    TimePoint woke{};
    std::mutex m;
    std::condition_variable cv;
    std::promise<void> release_b;
    Worker b([&] {
      {
        Clock::Participant p(clock, "b");
        release_b.get_future().wait();
      }
      if (b_notifies) clock.notify_all(cv);
    });
    ASSERT_TRUE(await_participants(clock, 1)) << "round " << round;
    Worker a([&] {
      Clock::Participant p(clock, "a");
      clock.sleep_for(3s);
      woke = clock.now();
    });
    Worker outsider([&] {
      std::unique_lock lock(m);
      clock.wait_until(cv, lock, start + 1s);
    });
    ASSERT_TRUE(await_waiters(clock, 2)) << "round " << round;
    release_b.set_value();
    if (!b_notifies) {
      for (int spin = 0; spin < (round % 50) * 20; ++spin) std::this_thread::yield();
      clock.notify_all(cv);
    }
    ASSERT_TRUE(b.join_within()) << "round " << round;
    ASSERT_TRUE(outsider.join_within()) << "round " << round;
    ASSERT_TRUE(a.join_within()) << "round " << round;
    ASSERT_EQ(woke, start + 3s) << "round " << round;
    ASSERT_EQ(clock.participants(), 0u) << "round " << round;
  }
}

TEST_F(VirtualClockTest, AStallNamesTheRunnableParticipant) {
  std::mutex reports_mutex;
  std::vector<std::string> reports;
  VirtualClock::Options options;
  options.stall_report_after = 50ms;
  options.on_stall = [&](std::string report) {
    std::lock_guard lock(reports_mutex);
    reports.push_back(std::move(report));
  };
  VirtualClock clock(options);
  const auto count = [&] {
    std::lock_guard lock(reports_mutex);
    return reports.size();
  };
  std::mutex m;
  std::condition_variable raw;
  bool released = false;
  // The culprit first: a sleeper alone would jump by itself.
  Worker culprit([&] {
    Clock::Participant p(clock, "culprit");
    std::unique_lock lock(m);
    raw.wait(lock, [&] { return released; });
  });
  ASSERT_TRUE(await_participants(clock, 1));
  Worker sleeper([&] {
    Clock::Participant p(clock, "sleeper");
    clock.sleep_for(1s);
  });
  const auto give_up = Real::now() + 5s;
  while (count() == 0 && Real::now() < give_up) std::this_thread::sleep_for(1ms);
  // One report for one stall, however long it lasts.
  std::this_thread::sleep_for(150ms);
  {
    std::lock_guard lock(reports_mutex);
    EXPECT_EQ(reports.size(), 1u);
    if (!reports.empty()) {
      EXPECT_EQ(reports[0], "virtual clock stalled; runnable: culprit");
      EXPECT_EQ(reports[0].find("sleeper"), std::string::npos);
    }
  }
  {
    std::lock_guard lock(m);
    released = true;
  }
  raw.notify_all();
  ASSERT_TRUE(culprit.join_within());
  ASSERT_TRUE(sleeper.join_within());
}

TEST_F(VirtualClockTest, NoStallReportWhilePacing) {
  std::atomic<int> reports{0};
  VirtualClock::Options options;
  options.speed = 1;
  options.stall_report_after = 20ms;
  options.on_stall = [&](std::string) { reports.fetch_add(1); };
  VirtualClock clock(options);
  Clock::Participant main(clock, "test");
  const auto real_start = Real::now();
  clock.sleep_for(200ms);
  EXPECT_GE(Real::now() - real_start, 190ms);
  EXPECT_EQ(reports.load(), 0);
}

TEST_F(VirtualClockTest, AHoldKeepsTimeStillUntilTheChildHasEntered) {
  VirtualClock clock;
  Clock::Participant main(clock, "test");
  const TimePoint kStart = clock.now();
  TimePoint woke{};
  auto hold = std::make_shared<Clock::Hold>(clock);
  Worker child([&clock, &woke, hold]() mutable {
    std::this_thread::sleep_for(20ms);
    Clock::Participant p(clock, "child");
    hold.reset();
    clock.sleep_for(1s);
    woke = clock.now();
  });
  hold.reset();
  clock.sleep_for(10s);
  ASSERT_TRUE(child.join_within());
  EXPECT_EQ(woke, kStart + 1s);
  EXPECT_EQ(clock.now(), kStart + 10s);
}
