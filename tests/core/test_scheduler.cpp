#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "pychron/core/config/logging_config.hpp"
#include "pychron/core/log_hub.hpp"
#include "pychron/core/scheduler.hpp"
#include "pychron/core/signal_bus.hpp"
#include "pychron/core/virtual_clock.hpp"

using namespace pychron;
using namespace std::chrono_literals;

namespace {
// threads = 0: jobs run inline inside run_pending(), fully deterministic.
Scheduler::Options inline_pool() { return Scheduler::Options{0}; }

// Waits, in real time, until `n` threads are asleep in the clock.
[[nodiscard]] bool await_waiters(const VirtualClock& clock, std::size_t n) {
  const auto give_up = std::chrono::steady_clock::now() + 5s;
  while (clock.waiters() != n) {
    if (std::chrono::steady_clock::now() > give_up) return false;
    std::this_thread::yield();
  }
  return true;
}

// A scheduler that waits past the clock leaves time standing and the test
// asleep in it. Each test on a VirtualClock runs under a real-time bound: when
// it is exceeded the process says so and aborts, well inside the ctest timeout.
class SchedulerVirtual : public ::testing::Test {
 protected:
  ~SchedulerVirtual() override {
    {
      std::lock_guard lock(mutex_);
      finished_ = true;
    }
    finished_cv_.notify_all();
    deadman_.join();
  }

  // Appended to by jobs on worker threads.
  void record(TimePoint t) {
    std::lock_guard lock(mutex_);
    times_.push_back(t);
  }
  std::vector<TimePoint> recorded() {
    std::lock_guard lock(mutex_);
    return times_;
  }

 private:
  std::mutex mutex_;
  std::condition_variable finished_cv_;
  bool finished_ = false;
  std::vector<TimePoint> times_;
  std::thread deadman_{[this] {
    std::unique_lock lock(mutex_);
    if (finished_cv_.wait_for(lock, 30s, [this] { return finished_; })) return;
    std::fputs("Scheduler test did not finish within 30 s of real time: a thread is stuck\n",
               stderr);
    std::abort();
  }};
};
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

namespace {

std::filesystem::path unique_log_dir(const char* tag) {
  std::random_device rd;
  return std::filesystem::temp_directory_path() / (std::string(tag) + std::to_string(std::mt19937_64(rd())()));
}

std::size_t lines_containing(const std::filesystem::path& p, const std::string& needle) {
  std::ifstream in(p);
  std::size_t n = 0;
  for (std::string line; std::getline(in, line);)
    if (line.find(needle) != std::string::npos) ++n;
  return n;
}

}  // namespace

TEST(Scheduler, ScanErrorGoesThroughHubToFileAndBusOnce) {
  ManualClock clock;
  SignalBus bus;
  std::vector<Log> logs;
  auto sub = bus.subscribe<Log>([&](const Log& e) { logs.push_back(e); });
  const auto dir = unique_log_dir("pychron_sched_hub_");
  config::LoggingConfig lc;
  lc.dir = dir;
  auto hub = LogHub::create(lc, clock, &bus);
  ASSERT_TRUE(hub);
  {
    Scheduler s(clock, &bus, inline_pool(), *hub);
    s.scan("IG1", 1s, []() -> Result<Sample> { return fail(ErrorKind::Timeout, "no reply", "IG1"); });
    clock.advance(1s);
    s.run_pending();
  }
  (*hub)->flush();
  ASSERT_EQ(logs.size(), 1u);  // via the hub only, not also published directly
  EXPECT_EQ(logs[0].logger, "scheduler");
  EXPECT_EQ(logs[0].level, LogLevel::Warn);
  EXPECT_EQ(lines_containing(dir / "pychron.log", "[warn] scheduler: scan IG1 failed"), 1u);
  hub->reset();
  std::filesystem::remove_all(dir);
}

TEST(Scheduler, SchedulerLevelRuleGatesItsRecords) {
  ManualClock clock;
  SignalBus bus;
  std::vector<Log> logs;
  auto sub = bus.subscribe<Log>([&](const Log& e) { logs.push_back(e); });
  const auto dir = unique_log_dir("pychron_sched_rule_");
  config::LoggingConfig lc;
  lc.dir = dir;
  lc.default_level = LogLevel::Trace;
  lc.levels.emplace_back("scheduler", LogLevel::Error);
  auto hub = LogHub::create(lc, clock, &bus);
  ASSERT_TRUE(hub);
  {
    Scheduler s(clock, &bus, inline_pool(), *hub);
    s.scan("IG1", 1s, []() -> Result<Sample> { return fail(ErrorKind::Timeout, "no reply", "IG1"); });
    s.after("boom", 1s, [] { throw std::runtime_error("x"); });
    clock.advance(1s);
    s.run_pending();
  }
  (*hub)->flush();
  // The warn (failed scan) is below the rule; the error (throwing job) is not.
  ASSERT_EQ(logs.size(), 1u);
  EXPECT_EQ(logs[0].level, LogLevel::Error);
  EXPECT_EQ(lines_containing(dir / "pychron.log", "scheduler: scan IG1 failed"), 0u);
  EXPECT_EQ(lines_containing(dir / "pychron.log", "[error] scheduler: job boom threw"), 1u);
  hub->reset();
  std::filesystem::remove_all(dir);
}

TEST(Scheduler, HubOnAnotherBusAlsoPublishesOnSchedulerBus) {
  ManualClock clock;
  SignalBus bus;
  SignalBus hub_bus;
  int on_bus = 0;
  int on_hub_bus = 0;
  auto s1 = bus.subscribe<Log>([&](const Log&) { ++on_bus; });
  auto s2 = hub_bus.subscribe<Log>([&](const Log&) { ++on_hub_bus; });
  auto hub = LogHub::create(config::LoggingConfig{}, clock, &hub_bus);
  ASSERT_TRUE(hub);
  Scheduler s(clock, &bus, inline_pool(), *hub);
  s.scan("IG1", 1s, []() -> Result<Sample> { return fail(ErrorKind::Timeout, "no reply", "IG1"); });
  clock.advance(1s);
  s.run_pending();
  EXPECT_EQ(on_bus, 1);
  EXPECT_EQ(on_hub_bus, 1);
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

TEST_F(SchedulerVirtual, PeriodicJobRunsAtExactTimes) {
  VirtualClock clock;
  Clock::Participant main(clock, "test");
  const TimePoint kStart = clock.now();
  Scheduler s(clock);
  s.start();
  ASSERT_TRUE(s.every("tick", 1s, [&] { record(clock.now()); }));

  clock.sleep_for(10s + 1ms);
  s.wait_idle();

  std::vector<TimePoint> expected;
  for (int i = 1; i <= 10; ++i) expected.push_back(kStart + i * 1s);
  EXPECT_EQ(recorded(), expected);
  EXPECT_EQ(clock.now(), kStart + 10s + 1ms);
}

TEST_F(SchedulerVirtual, AfterRunsOnceAtItsDelay) {
  VirtualClock clock;
  Clock::Participant main(clock, "test");
  const TimePoint kStart = clock.now();
  Scheduler s(clock);
  s.start();
  ASSERT_TRUE(s.after("once", 90s, [&] { record(clock.now()); }));

  clock.sleep_for(200s);
  s.wait_idle();

  EXPECT_EQ(recorded(), std::vector<TimePoint>{kStart + 90s});
  EXPECT_EQ(s.job_count(), 0u);
}

TEST_F(SchedulerVirtual, WatchdogFiresWithoutHeartbeat) {
  VirtualClock clock;
  Clock::Participant main(clock, "test");
  const TimePoint kStart = clock.now();
  Scheduler s(clock);
  s.start();
  ASSERT_TRUE(s.watchdog("w", 5s, [&] { record(clock.now()); }));

  clock.sleep_for(6s);
  s.wait_idle();

  EXPECT_EQ(recorded(), std::vector<TimePoint>{kStart + 5s});
}

// Nobody holds time back here: the scheduler's threads are the only
// participants and the job costs nothing, so time runs as fast as they can
// take it. stop() and the destructor must get in all the same.
TEST_F(SchedulerVirtual, StopReturnsWhileTimeRunsAway) {
  VirtualClock clock;
  std::atomic<int> runs{0};
  std::optional<Scheduler> s(std::in_place, clock);
  ASSERT_TRUE(s->every("tick", 1s, [&] { runs.fetch_add(1); }));
  s->start();
  const auto give_up = std::chrono::steady_clock::now() + 20s;
  while (runs.load() < 1 && std::chrono::steady_clock::now() < give_up) std::this_thread::yield();

  const auto real_start = std::chrono::steady_clock::now();
  s->stop();
  EXPECT_FALSE(s->started());
  s.reset();
  EXPECT_LT(std::chrono::steady_clock::now() - real_start, 10s);
  EXPECT_GE(runs.load(), 1);
}

// stop() waits for the dispatcher, a participant, to finish. If the caller
// stepped out of the clock to do that, nobody would be runnable once the
// dispatcher had gone and time would run to the sleeper's deadline.
TEST_F(SchedulerVirtual, StopFromAParticipantDoesNotMoveTime) {
  VirtualClock clock;
  Clock::Participant main(clock, "test");
  const TimePoint kStart = clock.now();
  Scheduler s(clock, nullptr, Scheduler::Options{2});
  s.start();
  ASSERT_TRUE(s.every("tick", 1s, [&] { record(clock.now()); }));
  std::thread sleeper([&] {
    Clock::Participant p(clock, "sleeper");
    clock.sleep_for(1h);
  });
  // Two workers, the dispatcher and the sleeper.
  EXPECT_TRUE(await_waiters(clock, 4));

  clock.sleep_for(3s + 1ms);
  const TimePoint t = clock.now();
  EXPECT_EQ(t, kStart + 3s + 1ms);
  s.stop();
  EXPECT_EQ(clock.now(), t);
  EXPECT_EQ(recorded().size(), 3u);

  clock.sleep_for(1h);  // past the sleeper's deadline
  sleeper.join();
}

TEST_F(SchedulerVirtual, DestructorWaitsForAJobThatWaitsInClockTime) {
  VirtualClock clock;
  Clock::Participant main(clock, "test");
  const TimePoint kStart = clock.now();
  std::atomic<bool> finished{false};
  std::optional<Scheduler> s(std::in_place, clock);
  s->start();
  ASSERT_TRUE(s->after("slow", 1s, [&] {
    record(clock.now());
    clock.sleep_for(5s);
    finished = true;
  }));

  clock.sleep_for(1s + 1ms);  // the job is asleep on a worker
  EXPECT_FALSE(finished.load());
  s.reset();

  EXPECT_TRUE(finished.load());
  ASSERT_EQ(recorded(), std::vector<TimePoint>{kStart + 1s});
  EXPECT_EQ(clock.now(), recorded().front() + 5s);
}

// With no worker pool the dispatcher runs the job itself.
TEST_F(SchedulerVirtual, StopWaitsForAJobTheDispatcherRunsInline) {
  VirtualClock clock;
  Clock::Participant main(clock, "test");
  const TimePoint kStart = clock.now();
  std::atomic<bool> finished{false};
  Scheduler s(clock, nullptr, inline_pool());
  s.start();
  ASSERT_TRUE(s.after("slow", 1s, [&] {
    clock.sleep_for(5s);
    finished = true;
  }));

  clock.sleep_for(1s + 1ms);
  EXPECT_FALSE(finished.load());
  s.stop();

  EXPECT_TRUE(finished.load());
  EXPECT_EQ(clock.now(), kStart + 6s);
}

TEST_F(SchedulerVirtual, RunPendingInlineStillWorks) {
  VirtualClock clock;
  Clock::Participant main(clock, "test");
  Scheduler s(clock, nullptr, inline_pool());
  int runs = 0;
  ASSERT_TRUE(s.every("tick", 1s, [&] { ++runs; }));
  for (int i = 0; i < 5; ++i) {
    clock.sleep_for(1s);
    EXPECT_EQ(s.run_pending(), 1u);
  }
  EXPECT_EQ(runs, 5);
}
