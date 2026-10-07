#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
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

// The same for a thread that has to be asleep in the clock, not merely
// started, before the test goes on: `n` waits registered and not woken.
[[nodiscard]] bool await_waiters(const VirtualClock& clock, std::size_t n) {
  const auto give_up = Real::now() + 5s;
  while (clock.waiters() != n) {
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

TEST_F(VirtualClockTest, PacingTakesDeltaOverSpeed) {
  VirtualClock::Options options;
  options.speed = 100;
  VirtualClock clock(options);
  Clock::Participant main(clock, "test");
  const TimePoint kStart = clock.now();
  const auto real_start = Real::now();
  clock.sleep_for(1s);
  const auto real = Real::now() - real_start;
  EXPECT_GE(real, 8ms);
  EXPECT_LT(real, 600ms);
  EXPECT_EQ(clock.now(), kStart + 1s);
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
  EXPECT_LT(woken.real - notified, 500ms);
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
  EXPECT_LT(woken.real - real_set, 1s);
  EXPECT_EQ(woken.now, kStart + 100s);
  EXPECT_EQ(clock.now(), kStart + 100s);
}

TEST_F(VirtualClockTest, ALeaveHandsThePacingToASleeper) {
  VirtualClock::Options options;
  options.speed = 100;
  VirtualClock clock(options);
  const TimePoint kStart = clock.now();
  std::promise<void> release_b;
  Worker b([&] {
    Clock::Participant p(clock, "b");
    release_b.get_future().wait();
  });
  ASSERT_TRUE(await_participants(clock, 1));
  Real::time_point real_woke;
  Worker a([&] {
    Clock::Participant p(clock, "a");
    clock.sleep_for(1s);
    real_woke = Real::now();
  });
  ASSERT_TRUE(await_waiters(clock, 1));
  const auto real_release = Real::now();
  release_b.set_value();
  // B's leave returns at once; the ten milliseconds are slept by A.
  ASSERT_TRUE(b.join_within());
  ASSERT_TRUE(a.join_within());
  EXPECT_GE(real_woke - real_release, 8ms);
  EXPECT_EQ(clock.now(), kStart + 1s);
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
