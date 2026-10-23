#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
#include <optional>
#include <random>
#include <regex>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "pychron/core/config/logging_config.hpp"
#include "log_capture.hpp"
#include "pychron/core/log_hub.hpp"
#include "pychron/core/process_log.hpp"
#include "pychron/core/scheduler.hpp"
#include "pychron/core/signal_bus.hpp"
#include "pychron/core/virtual_clock.hpp"
#include "virtual_time.hpp"

using namespace pychron;
using namespace std::chrono_literals;

namespace {
// threads = 0: jobs run inline inside run_pending(), fully deterministic.
Scheduler::Options inline_pool() { return Scheduler::Options{0}; }

using pychron::testing::await_waiters;

// A scheduler that waits past the clock leaves time standing and the test
// asleep in it. Each test on a VirtualClock runs under a real-time bound: when
// it is exceeded the process says so and aborts, well inside the ctest timeout.
class SchedulerVirtual : public pychron::testing::VirtualTimeTest {
 protected:
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
  std::vector<TimePoint> times_;
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

// A worker that is busy leaves the next job dispatched and waiting. What that
// job refers to may be destroyed once it is cancelled, so it must not begin.
TEST(Scheduler, CancelDropsADispatchedExecutionThatHasNotBegun) {
  ManualClock clock;
  Scheduler s(clock, nullptr, Scheduler::Options{1});
  std::promise<void> busy;
  std::promise<void> release;
  auto gate = release.get_future().share();
  std::atomic<int> runs{0};

  ASSERT_TRUE(s.after("blocker", 10ms, [&] {
    busy.set_value();
    gate.wait();
  }));
  clock.advance(10ms);
  EXPECT_EQ(s.run_pending(), 1u);
  busy.get_future().wait();  // the only worker is inside the blocker

  const auto periodic = s.every("periodic", 10ms, [&] { ++runs; });
  const auto once = s.after("once", 10ms, [&] { ++runs; });
  ASSERT_TRUE(periodic);
  ASSERT_TRUE(once);
  clock.advance(10ms);
  EXPECT_EQ(s.run_pending(), 2u);  // both queued behind the blocker

  EXPECT_TRUE(s.cancel(*periodic));
  EXPECT_FALSE(s.cancel_and_wait(*once));  // dispatched: no longer registered, and nothing to wait for
  release.set_value();
  s.wait_idle();
  EXPECT_EQ(runs.load(), 0);
}

TEST(Scheduler, CancelAndWaitWaitsForTheExecutionInProgress) {
  ManualClock clock;
  Scheduler s(clock, nullptr, Scheduler::Options{2});
  std::promise<void> entered;
  std::promise<void> release;
  auto gate = release.get_future().share();
  std::atomic<bool> finished{false};

  const auto id = s.every("slow", 10ms, [&] {
    entered.set_value();
    gate.wait();
    finished = true;
  });
  ASSERT_TRUE(id);
  clock.advance(10ms);
  EXPECT_EQ(s.run_pending(), 1u);
  entered.get_future().wait();

  auto cancelled = std::async(std::launch::async, [&] { return s.cancel_and_wait(*id); });
  EXPECT_EQ(cancelled.wait_for(100ms), std::future_status::timeout);  // the task is still running
  EXPECT_FALSE(finished.load());
  release.set_value();
  ASSERT_EQ(cancelled.wait_for(5s), std::future_status::ready);
  EXPECT_TRUE(finished.load());
  EXPECT_TRUE(cancelled.get());

  clock.advance(1s);
  EXPECT_EQ(s.run_pending(), 0u);
}

// The execution in progress is the caller: waiting for it would never end.
TEST(Scheduler, CancelAndWaitFromTheJobItselfReturns) {
  ManualClock clock;
  Scheduler s(clock, nullptr, inline_pool());
  int runs = 0;
  bool removed = false;
  JobId own = 0;
  const auto id = s.every("self", 10ms, [&] {
    ++runs;
    removed = s.cancel_and_wait(own);
  });
  ASSERT_TRUE(id);
  own = *id;
  clock.advance(10ms);
  s.run_pending();
  clock.advance(10ms);
  s.run_pending();
  EXPECT_EQ(runs, 1);
  EXPECT_TRUE(removed);
}

// Inline, the jobs due together run one after another on one thread: the
// second is dispatched while the first runs, and cannot be waited for there.
TEST(Scheduler, CancelAndWaitInlineDropsAJobDueInTheSamePass) {
  ManualClock clock;
  Scheduler s(clock, nullptr, inline_pool());
  int second_runs = 0;
  JobId second = 0;
  ASSERT_TRUE(s.every("first", 10ms, [&] { (void)s.cancel_and_wait(second); }));
  const auto id = s.every("second", 10ms, [&] { ++second_runs; });
  ASSERT_TRUE(id);
  second = *id;
  clock.advance(10ms);
  EXPECT_EQ(s.run_pending(), 2u);
  EXPECT_EQ(second_runs, 0);
  EXPECT_EQ(s.job_count(), 1u);
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

// Lines of `p` holding `needle`, read without their thread field and run id
// (`[warn] [t3] run=X core: m` is matched as `[warn] core: m`).
std::size_t lines_containing(const std::filesystem::path& p, const std::string& needle) {
  static const std::regex context(R"((\[(?:trace|debug|info|warn|error)\]) \[[^\]]*\] (?:run=\S+ )?)");
  std::ifstream in(p);
  std::size_t n = 0;
  for (std::string line; std::getline(in, line);)
    if (std::regex_replace(line, context, "$1 ").find(needle) != std::string::npos) ++n;
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
  // NOLINTNEXTLINE(bugprone-std-exception-baseclass): a job that throws what is not an exception is the case
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

// A job writes to the log with its thread named: the scheduler's.
TEST_F(SchedulerVirtual, JobsRunTaggedScheduler) {
  pychron::testing::LogCapture log;  // before the scheduler: its threads end first
  ASSERT_TRUE(log.ok());
  VirtualClock clock;
  Clock::Participant main(clock, "test");
  Scheduler s(clock);
  s.start();
  ASSERT_TRUE(s.after("once", 1s, [] { process_logger("t").info("from a job"); }));

  clock.sleep_for(2s);
  s.wait_idle();

  const auto lines = log.raw_lines();
  const auto it = std::find_if(lines.begin(), lines.end(),
                               [](const std::string& l) { return l.find(" t: from a job") != std::string::npos; });
  ASSERT_NE(it, lines.end());
  EXPECT_NE(it->find(" [info] [scheduler] "), std::string::npos) << *it;
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

// stop() waits for the dispatcher to say it has finished, and a job the
// dispatcher runs inline is the dispatcher: it would wait for itself. It is
// refused instead, and the job is a failed job like any that throws.
TEST_F(SchedulerVirtual, StopFromItsOwnJobIsAnErrorNotAHang) {
  VirtualClock clock;
  Clock::Participant main(clock, "test");
  const TimePoint kStart = clock.now();
  Scheduler s(clock, nullptr, inline_pool());
  s.start();
  std::mutex m;
  std::error_code refused;
  auto id = s.every("stops itself", 1s, [&] {
    try {
      s.stop();
    } catch (const std::system_error& e) {
      std::lock_guard lock(m);
      refused = e.code();
      throw;
    }
  });
  ASSERT_TRUE(id);

  clock.sleep_for(1s + 1ms);
  s.wait_idle();

  auto stats = s.stats(*id);
  ASSERT_TRUE(stats);
  EXPECT_EQ(stats->runs, 1u);
  EXPECT_EQ(stats->failures, 1u);
  {
    std::lock_guard lock(m);
    EXPECT_EQ(refused, std::make_error_code(std::errc::resource_deadlock_would_occur));
  }
  // Nothing was half stopped: it is still dispatching, and stops when asked
  // from outside.
  EXPECT_TRUE(s.started());
  s.stop();
  EXPECT_FALSE(s.started());
  EXPECT_EQ(clock.now(), kStart + 1s + 1ms);
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

// ---- job_stats(): what the metrics collector reads --------------------------

TEST(Scheduler, JobStatsListsPeriodicJobsByName) {
  ManualClock clock;
  Scheduler s(clock, nullptr, inline_pool());
  ASSERT_TRUE(s.every("b", 1s, [] {}).has_value());
  ASSERT_TRUE(s.scan("a", 1s, [] { return Result<Sample>(Sample{"a", {}, 1.0}); }).has_value());
  ASSERT_TRUE(s.watchdog("w", 10s, [] {}).has_value());
  ASSERT_TRUE(s.after("once", 1s, [] {}).has_value());
  for (int i = 0; i < 3; ++i) {
    clock.advance(1s);
    s.run_pending();
  }
  const std::vector<NamedJobStats> stats = s.job_stats();
  ASSERT_EQ(stats.size(), 3u);
  EXPECT_EQ(stats[0].name, "a");
  EXPECT_EQ(stats[1].name, "b");
  EXPECT_EQ(stats[2].name, "w");
  EXPECT_EQ(stats[0].stats.runs, 3u);
  EXPECT_EQ(stats[1].stats.runs, 3u);
}

TEST(Scheduler, JobStatsLeavesOutAOneShotStillWaiting) {
  ManualClock clock;
  Scheduler s(clock, nullptr, inline_pool());
  ASSERT_TRUE(s.after("once", 10s, [] {}).has_value());
  EXPECT_TRUE(s.job_stats().empty());
}

TEST(Scheduler, JobStatsCountsFailures) {
  ManualClock clock;
  Scheduler s(clock, nullptr, inline_pool());
  ASSERT_TRUE(s.scan("ig1", 1s, []() -> Result<Sample> { return fail(ErrorKind::Timeout, "no reply"); }).has_value());
  for (int i = 0; i < 2; ++i) {
    clock.advance(1s);
    s.run_pending();
  }
  const std::vector<NamedJobStats> stats = s.job_stats();
  ASSERT_EQ(stats.size(), 1u);
  EXPECT_EQ(stats[0].stats.runs, 2u);
  EXPECT_EQ(stats[0].stats.failures, 2u);
}

TEST(Scheduler, JobStatsOmitsACancelledJob) {
  ManualClock clock;
  Scheduler s(clock, nullptr, inline_pool());
  const auto id = s.every("gone", 1s, [] {});
  ASSERT_TRUE(id.has_value());
  ASSERT_TRUE(s.every("kept", 1s, [] {}).has_value());
  EXPECT_TRUE(s.cancel(*id));
  const std::vector<NamedJobStats> stats = s.job_stats();
  ASSERT_EQ(stats.size(), 1u);
  EXPECT_EQ(stats[0].name, "kept");
}
