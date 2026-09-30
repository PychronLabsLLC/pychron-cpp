#include <gtest/gtest.h>

#include <atomic>
#include <future>
#include <mutex>
#include <vector>

#include "pychron/core/scheduler.hpp"
#include "pychron/core/signal_bus.hpp"

using namespace pychron;
using namespace std::chrono_literals;

namespace {
// threads = 0: jobs run inline inside run_pending(), fully deterministic.
Scheduler::Options inline_pool() { return Scheduler::Options{0}; }
}  // namespace

TEST(Scheduler, PeriodicRunsOncePerInterval) {
  ManualClock clock;
  Scheduler s(clock, nullptr, inline_pool());
  int runs = 0;
  auto id = s.every("tick", 100ms, [&] { ++runs; });
  ASSERT_TRUE(id);

  EXPECT_EQ(s.run_pending(), 0u);  // not due yet
  clock.advance(99ms);
  s.run_pending();
  EXPECT_EQ(runs, 0);
  clock.advance(1ms);
  s.run_pending();
  EXPECT_EQ(runs, 1);
  s.run_pending();
  EXPECT_EQ(runs, 1);  // same instant: no double fire
  clock.advance(100ms);
  s.run_pending();
  EXPECT_EQ(runs, 2);
  EXPECT_EQ(s.stats(*id)->runs, 2u);
}

TEST(Scheduler, PeriodicDropsMissedTicksInsteadOfBursting) {
  ManualClock clock;
  Scheduler s(clock, nullptr, inline_pool());
  int runs = 0;
  ASSERT_TRUE(s.every("tick", 100ms, [&] { ++runs; }));
  clock.advance(1s);  // ten intervals late
  s.run_pending();
  s.run_pending();
  EXPECT_EQ(runs, 1);
  clock.advance(100ms);
  s.run_pending();
  EXPECT_EQ(runs, 2);
}

TEST(Scheduler, RejectsNonPositiveInterval) {
  ManualClock clock;
  Scheduler s(clock, nullptr, inline_pool());
  auto r = s.every("bad", 0ms, [] {});
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Config);
  EXPECT_FALSE(s.watchdog("bad", -1ms, [] {}));
  EXPECT_FALSE(s.scan("bad", 0ms, [] { return Result<Sample>(Sample{}); }));
}

TEST(Scheduler, OneShotFiresOnceAndIsRemoved) {
  ManualClock clock;
  Scheduler s(clock, nullptr, inline_pool());
  int runs = 0;
  auto id = s.after("once", 50ms, [&] { ++runs; });
  ASSERT_TRUE(id);
  EXPECT_EQ(s.job_count(), 1u);
  clock.advance(50ms);
  s.run_pending();
  clock.advance(1s);
  s.run_pending();
  EXPECT_EQ(runs, 1);
  EXPECT_EQ(s.job_count(), 0u);
  EXPECT_FALSE(s.stats(*id).has_value());
}

TEST(Scheduler, CancelPreventsExecution) {
  ManualClock clock;
  Scheduler s(clock, nullptr, inline_pool());
  int runs = 0;
  auto id = s.after("once", 10ms, [&] { ++runs; });
  EXPECT_TRUE(s.cancel(*id));
  EXPECT_FALSE(s.cancel(*id));
  clock.advance(1s);
  s.run_pending();
  EXPECT_EQ(runs, 0);
}

TEST(Scheduler, WatchdogFiresOnlyWhenHeartbeatMissed) {
  ManualClock clock;
  Scheduler s(clock, nullptr, inline_pool());
  int missed = 0;
  auto id = s.watchdog("valve_bus", 1s, [&] { ++missed; });
  ASSERT_TRUE(id);

  for (int i = 0; i < 5; ++i) {
    clock.advance(900ms);
    ASSERT_TRUE(s.heartbeat(*id));
    s.run_pending();
  }
  EXPECT_EQ(missed, 0);

  clock.advance(1s);
  s.run_pending();
  EXPECT_EQ(missed, 1);
  clock.advance(500ms);
  s.run_pending();
  EXPECT_EQ(missed, 1);  // re-armed for a full timeout
  clock.advance(500ms);
  s.run_pending();
  EXPECT_EQ(missed, 2);  // still silent: fires again

  ASSERT_TRUE(s.heartbeat(*id));
  clock.advance(999ms);
  s.run_pending();
  EXPECT_EQ(missed, 2);
}

TEST(Scheduler, HeartbeatOnCancelledJobIsCancelledError) {
  ManualClock clock;
  Scheduler s(clock, nullptr, inline_pool());
  auto id = s.watchdog("w", 1s, [] {});
  s.cancel(*id);
  auto r = s.heartbeat(*id);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Cancelled);

  auto periodic = s.every("p", 1s, [] {});
  auto wrong = s.heartbeat(*periodic);
  ASSERT_FALSE(wrong);
  EXPECT_EQ(wrong.error().kind, ErrorKind::Config);
}

TEST(Scheduler, ScanPublishesSamples) {
  ManualClock clock;
  SignalBus bus;
  std::vector<Sample> samples;
  auto sub = bus.subscribe<Sample>([&](const Sample& e) { samples.push_back(e); });
  Scheduler s(clock, &bus, inline_pool());

  double v = 1e-9;
  ASSERT_TRUE(s.scan("IG1", 1s, [&]() -> Result<Sample> { return Sample{"", {}, v *= 10}; }));
  clock.advance(1s);
  s.run_pending();
  clock.advance(1s);
  s.run_pending();

  ASSERT_EQ(samples.size(), 2u);
  EXPECT_EQ(samples[0].device, "IG1");
  EXPECT_DOUBLE_EQ(samples[0].value, 1e-8);
  EXPECT_EQ(samples[1].ts, clock.now());
}

TEST(Scheduler, ScanErrorIsLoggedAndCounted) {
  ManualClock clock;
  SignalBus bus;
  int sample_events = 0;
  std::vector<Log> logs;
  auto s1 = bus.subscribe<Sample>([&](const Sample&) { ++sample_events; });
  auto s2 = bus.subscribe<Log>([&](const Log& e) { logs.push_back(e); });
  Scheduler s(clock, &bus, inline_pool());

  auto id = s.scan("IG1", 1s, []() -> Result<Sample> { return fail(ErrorKind::Timeout, "no reply", "IG1"); });
  clock.advance(1s);
  s.run_pending();

  EXPECT_EQ(sample_events, 0);
  ASSERT_EQ(logs.size(), 1u);
  EXPECT_EQ(logs[0].level, LogLevel::Warn);
  EXPECT_NE(logs[0].message.find("timeout"), std::string::npos);
  EXPECT_EQ(s.stats(*id)->failures, 1u);
}

TEST(Scheduler, ThrowingTaskDoesNotKillScheduler) {
  ManualClock clock;
  Scheduler s(clock, nullptr, inline_pool());
  int after = 0;
  auto bad = s.every("bad", 1s, [] { throw 42; });
  ASSERT_TRUE(s.every("good", 1s, [&] { ++after; }));
  clock.advance(1s);
  s.run_pending();
  EXPECT_EQ(after, 1);
  EXPECT_EQ(s.stats(*bad)->failures, 1u);
}

// A slow scan never stacks on itself: while one run is in flight, later ticks
// of the same job are skipped, not queued.
TEST(Scheduler, SlowJobNeverOverlapsItself) {
  ManualClock clock;
  Scheduler s(clock, nullptr, Scheduler::Options{4});
  std::atomic<int> concurrent{0};
  std::atomic<int> max_concurrent{0};
  std::atomic<int> runs{0};
  std::promise<void> release;
  auto gate = release.get_future().share();

  auto id = s.every("slow", 100ms, [&] {
    const int now = concurrent.fetch_add(1) + 1;
    int prev = max_concurrent.load();
    while (now > prev && !max_concurrent.compare_exchange_weak(prev, now)) {
    }
    if (runs.fetch_add(1) == 0) gate.wait();  // first run blocks until released
    concurrent.fetch_sub(1);
  });
  ASSERT_TRUE(id);

  clock.advance(100ms);
  EXPECT_EQ(s.run_pending(), 1u);
  for (int i = 0; i < 5; ++i) {
    clock.advance(100ms);
    EXPECT_EQ(s.run_pending(), 0u);  // previous run still in flight
  }
  release.set_value();
  s.wait_idle();

  EXPECT_EQ(runs.load(), 1);
  EXPECT_EQ(max_concurrent.load(), 1);
  EXPECT_EQ(s.stats(*id)->skipped_overlaps, 5u);

  clock.advance(100ms);
  EXPECT_EQ(s.run_pending(), 1u);
  s.wait_idle();
  EXPECT_EQ(runs.load(), 2);
}

TEST(Scheduler, DifferentJobsRunInParallelOnPool) {
  ManualClock clock;
  Scheduler s(clock, nullptr, Scheduler::Options{2});
  std::promise<void> a_started;
  std::promise<void> release;
  auto gate = release.get_future().share();
  std::atomic<bool> b_ran{false};

  ASSERT_TRUE(s.after("a", 10ms, [&] {
    a_started.set_value();
    gate.wait();
  }));
  ASSERT_TRUE(s.after("b", 10ms, [&] { b_ran = true; }));
  clock.advance(10ms);
  s.run_pending();
  a_started.get_future().wait();
  // b must be able to complete while a is still blocked.
  for (int i = 0; i < 2000 && !b_ran; ++i) std::this_thread::sleep_for(1ms);
  EXPECT_TRUE(b_ran.load());
  release.set_value();
  s.wait_idle();
}

TEST(Scheduler, BackgroundDispatcherFollowsManualClock) {
  ManualClock clock;
  Scheduler s(clock, nullptr, Scheduler::Options{2});
  std::atomic<int> runs{0};
  ASSERT_TRUE(s.every("tick", 1s, [&] { runs.fetch_add(1); }));
  s.start();
  EXPECT_TRUE(s.started());

  for (int target = 1; target <= 3; ++target) {
    clock.advance(1s);
    for (int i = 0; i < 2000 && runs.load() < target; ++i) std::this_thread::sleep_for(1ms);
    EXPECT_EQ(runs.load(), target);
  }
  s.stop();
  s.wait_idle();
  EXPECT_FALSE(s.started());
}

TEST(Scheduler, BackgroundDispatcherWithSteadyClock) {
  SteadyClock clock;
  Scheduler s(clock);
  std::atomic<int> runs{0};
  std::promise<void> done;
  ASSERT_TRUE(s.every("fast", 2ms, [&] {
    if (runs.fetch_add(1) + 1 == 5) done.set_value();
  }));
  s.start();
  EXPECT_EQ(done.get_future().wait_for(5s), std::future_status::ready);
  s.stop();
  s.wait_idle();
}
