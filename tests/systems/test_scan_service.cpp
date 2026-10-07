#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <mutex>
#include <thread>

#include "pychron/core/virtual_clock.hpp"
#include "pychron/systems/spectrometer/data_dir.hpp"
#include "pychron/systems/spectrometer/scan_service.hpp"
#include "spectrometer_fakes.hpp"
#include "virtual_time.hpp"

using namespace pychron;
using namespace pychron::spectrometer;
using namespace pychron::spectrometer::testing;
using namespace std::chrono_literals;

namespace {

const std::filesystem::path kIntegrated =
    std::filesystem::path(PYCHRON_EXAMPLE_CONFIGS_DIR) / "spectrometer.sim-integrated.toml";

const std::vector<ChannelId> kChannels{"H2", "H1", "AX", "L1", "L2", "CDD"};

// sim-integrated config with fake roles; the engine runs on a manual clock and
// is driven by poll().
struct Rig {
  CallLog log;
  ManualClock clock{TimePoint{} + 100s};
  SignalBus bus;
  Scheduler scheduler;
  FakePositioner positioner{log};
  FakeSource source;
  FakeAcquirer acquirer{kChannels};
  std::mutex statuses_mutex;  // publishes are no longer serialised by the service
  std::vector<ScanStatus> statuses;
  SignalBus::Subscription sub = bus.subscribe<ScanStatus>([this](const ScanStatus& s) {
    std::lock_guard lock(statuses_mutex);
    statuses.push_back(s);
  });
  std::unique_ptr<Spectrometer> spec;
  std::unique_ptr<ScanService> service;
  std::uint64_t seq = 0;

  explicit Rig(std::size_t threads = 0) : scheduler(clock, &bus, Scheduler::Options{threads}) {
    auto d = cfg::load_spectrometer(kIntegrated);
    EXPECT_TRUE(d.has_value()) << (d ? "" : d.error().what);
    std::map<std::string, FieldTable> tables;
    for (const auto& [name, tf] : d->tables) tables.emplace(name, to_field_table(tf));
    SpectrometerRoles roles;
    roles.positioner = &positioner;
    roles.source = &source;
    roles.acquirers = {{"sim", &acquirer}};
    auto made = Spectrometer::create(d->config, MolecularWeights(d->weights), std::move(tables), std::move(roles),
                                     SpectrometerContext{clock, scheduler, bus});
    EXPECT_TRUE(made.has_value()) << (made ? "" : made.error().what);
    spec = std::move(*made);
    service = std::make_unique<ScanService>(*spec, bus, clock);
  }

  // One integrated frame, vendor-snapped to `span`.
  void feed(Duration span) {
    Frame f;
    f.ts = clock.now() + 1s;
    f.seq = ++seq;
    f.integrated = true;
    f.span = span;
    for (const auto& c : kChannels) f.values.emplace_back(c, 1.0);
    acquirer.push(std::move(f));
    spec->acquisition().poll(0);
  }
};

}  // namespace

TEST(ScanService, StartPublishesStatusAndReadingsFlow) {
  Rig r;
  ASSERT_TRUE(r.service->start(1s).has_value());
  ASSERT_EQ(r.statuses.size(), 1U);
  EXPECT_TRUE(r.statuses[0].running);
  EXPECT_FALSE(r.statuses[0].paused);
  EXPECT_EQ(r.statuses[0].integration, 1s);
  EXPECT_EQ(r.statuses[0].spectrometer, r.spec->name());
  EXPECT_EQ(r.statuses[0].ts, r.clock.now());
  EXPECT_TRUE(r.statuses[0].error.empty());
  EXPECT_TRUE(r.spec->acquisition().running());

  r.feed(1s);
  ASSERT_EQ(r.statuses.size(), 2U);  // first reading reports the snapped value once
  r.feed(1s);
  EXPECT_EQ(r.statuses.size(), 2U);
}

TEST(ScanService, StartTwiceSameIntegrationIsNoop) {
  Rig r;
  ASSERT_TRUE(r.service->start(1s).has_value());
  EXPECT_TRUE(r.service->start(1s).has_value());
  EXPECT_EQ(r.statuses.size(), 1U);
  EXPECT_EQ(r.acquirer.starts, 1);
}

TEST(ScanService, SetIntegrationRestartsAndReportsSnapped) {
  Rig r;
  ASSERT_TRUE(r.service->start(1s).has_value());
  ASSERT_TRUE(r.service->set_integration(150ms).has_value());
  EXPECT_EQ(r.acquirer.configured, 150ms);
  EXPECT_EQ(r.acquirer.starts, 2);
  EXPECT_TRUE(r.service->running());
  EXPECT_EQ(r.service->integration(), 150ms);

  r.feed(200ms);  // vendor snaps 0.15 s -> 0.2 s
  EXPECT_EQ(r.statuses.back().integration, 200ms);
  EXPECT_EQ(r.service->integration(), 200ms);
  EXPECT_TRUE(r.statuses.back().running);
}

TEST(ScanService, StopPublishesNotRunning) {
  Rig r;
  ASSERT_TRUE(r.service->start(1s).has_value());
  r.service->stop();
  EXPECT_FALSE(r.spec->acquisition().running());
  EXPECT_FALSE(r.service->running());
  ASSERT_EQ(r.statuses.size(), 2U);
  EXPECT_FALSE(r.statuses.back().running);
  r.service->stop();  // already stopped: nothing more to say
  EXPECT_EQ(r.statuses.size(), 2U);
}

TEST(ScanService, PauseResumeKeepsIntegration) {
  Rig r;
  ASSERT_TRUE(r.service->start(500ms).has_value());
  r.service->pause();
  EXPECT_TRUE(r.service->paused());
  EXPECT_FALSE(r.service->running());
  EXPECT_FALSE(r.spec->acquisition().running());
  EXPECT_TRUE(r.statuses.back().paused);

  ASSERT_TRUE(r.service->resume().has_value());
  EXPECT_FALSE(r.service->paused());
  EXPECT_TRUE(r.service->running());
  EXPECT_EQ(r.service->integration(), 500ms);
  EXPECT_EQ(r.acquirer.configured, 500ms);
}

TEST(ScanService, SetIntegrationWhilePausedAppliesOnResume) {
  Rig r;
  ASSERT_TRUE(r.service->start(500ms).has_value());
  r.service->pause();
  ASSERT_TRUE(r.service->set_integration(2s).has_value());
  EXPECT_TRUE(r.service->paused());
  EXPECT_FALSE(r.spec->acquisition().running());
  ASSERT_TRUE(r.service->resume().has_value());
  EXPECT_EQ(r.acquirer.configured, 2s);
}

TEST(ScanService, ResumeWithoutPauseIsNoop) {
  Rig r;
  EXPECT_TRUE(r.service->resume().has_value());
  EXPECT_FALSE(r.service->running());
  EXPECT_TRUE(r.statuses.empty());
  ASSERT_TRUE(r.service->start(1s).has_value());
  EXPECT_TRUE(r.service->resume().has_value());
  EXPECT_EQ(r.statuses.size(), 1U);
  EXPECT_EQ(r.acquirer.starts, 1);
}

TEST(ScanService, StartWhileEngineBusyReturnsErrorAndPublishesIt) {
  Rig r;
  ASSERT_TRUE(r.spec->acquisition().start(1s).has_value());
  auto res = r.service->start(1s);
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().kind, ErrorKind::Config);
  EXPECT_FALSE(r.service->status().running);
  EXPECT_FALSE(r.service->status().error.empty());
  ASSERT_EQ(r.statuses.size(), 1U);
  EXPECT_FALSE(r.statuses[0].running);
  EXPECT_FALSE(r.statuses[0].error.empty());
}

TEST(ScanService, EngineStartFailureLeavesServiceStopped) {
  Rig r;
  r.acquirer.fail_start = true;
  auto res = r.service->start(1s);
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().kind, ErrorKind::Io);
  EXPECT_FALSE(r.service->running());
  EXPECT_FALSE(r.statuses.back().error.empty());
}

TEST(ScanService, StallAlarmSetsErrorButKeepsRunning) {
  Rig r;
  ASSERT_TRUE(r.service->start(1s).has_value());
  Alarm a;
  a.source = "acquisition";
  a.message = "no frame from acquirer 0";
  a.ts = r.clock.now();
  r.bus.publish(a);
  EXPECT_TRUE(r.service->running());
  EXPECT_EQ(r.service->status().error, "no frame from acquirer 0");
  EXPECT_TRUE(r.statuses.back().running);
  EXPECT_EQ(r.statuses.back().error, "no frame from acquirer 0");

  ASSERT_TRUE(r.service->start(2s).has_value());
  EXPECT_TRUE(r.service->status().error.empty());
}

TEST(ScanService, StartAtSameIntegrationClearsError) {
  Rig r;
  ASSERT_TRUE(r.service->start(1s).has_value());
  Alarm a;
  a.source = "acquisition";
  a.message = "stalled";
  a.ts = r.clock.now();
  r.bus.publish(a);
  ASSERT_EQ(r.statuses.size(), 2U);
  ASSERT_TRUE(r.service->start(1s).has_value());
  EXPECT_TRUE(r.service->status().error.empty());
  ASSERT_EQ(r.statuses.size(), 3U);  // clearing the error is announced
  EXPECT_TRUE(r.statuses.back().error.empty());
  EXPECT_TRUE(r.statuses.back().running);
}

TEST(ScanService, UnrelatedAlarmIgnored) {
  Rig r;
  ASSERT_TRUE(r.service->start(1s).has_value());
  Alarm a;
  a.source = "IG1";
  a.message = "pressure";
  r.bus.publish(a);
  EXPECT_TRUE(r.service->status().error.empty());
  EXPECT_EQ(r.statuses.size(), 1U);
}

TEST(ScanService, DestructorStopsOnlyWhatItStarted) {
  Rig r;
  ASSERT_TRUE(r.spec->acquisition().start(1s).has_value());
  EXPECT_FALSE(r.service->start(1s).has_value());
  r.service.reset();
  EXPECT_TRUE(r.spec->acquisition().running());
  r.spec->acquisition().stop();

  r.service = std::make_unique<ScanService>(*r.spec, r.bus, r.clock);
  ASSERT_TRUE(r.service->start(1s).has_value());
  r.service.reset();
  EXPECT_FALSE(r.spec->acquisition().running());
}

TEST(ScanService, RapidSetIntegrationLastWins) {
  Rig r;
  ASSERT_TRUE(r.service->start(1s).has_value());
  const Duration a = 100ms, b = 200ms, c = 500ms, d = 2s, e = 5s;
  std::thread t1([&] {
    (void)r.service->set_integration(a);
    (void)r.service->set_integration(b);
  });
  std::thread t2([&] {
    (void)r.service->set_integration(c);
    (void)r.service->set_integration(d);
    (void)r.service->set_integration(e);
  });
  t1.join();
  t2.join();
  EXPECT_TRUE(r.service->running());
  EXPECT_TRUE(r.spec->acquisition().running());
  const Duration got = r.service->integration();
  EXPECT_TRUE(got == a || got == b || got == c || got == d || got == e);
}

TEST(ScanService, StaleReadingAndAlarmFromEarlierRunAreIgnored) {
  Rig r;
  ASSERT_TRUE(r.service->start(1s).has_value());
  r.clock.advance(10s);
  const TimePoint before = r.clock.now() - 5s;
  ASSERT_TRUE(r.service->set_integration(150ms).has_value());

  IntensityReading stale;
  stale.reading.ts = before;
  stale.reading.integration = 1s;
  r.bus.publish(stale);
  Alarm old;
  old.source = "acquisition";
  old.message = "old stall";
  old.ts = before;
  r.bus.publish(old);
  EXPECT_EQ(r.statuses.size(), 2U);
  EXPECT_TRUE(r.service->status().error.empty());

  r.feed(200ms);  // the first fresh reading still reports the snapped value
  ASSERT_EQ(r.statuses.size(), 3U);
  EXPECT_EQ(r.statuses.back().integration, 200ms);
}

TEST(ScanService, StatusSubscriberMayCallBackIntoService) {
  Rig r;
  std::vector<ScanStatus> seen;
  auto sub = r.bus.subscribe<ScanStatus>([&](const ScanStatus& s) {
    seen.push_back(r.service->status());
    if (s.running) r.service->stop();
  });
  ASSERT_TRUE(r.service->start(1s).has_value());
  EXPECT_FALSE(r.service->running());
  EXPECT_FALSE(r.spec->acquisition().running());
  ASSERT_GE(seen.size(), 2U);
}

TEST(ScanService, DestroyWhileReadingsFlowOnThreadedScheduler) {
  Rig r(2);
  r.scheduler.start();
  for (int i = 0; i < 40; ++i) {
    auto service = std::make_unique<ScanService>(*r.spec, r.bus, r.clock);
    ASSERT_TRUE(service->start(100ms).has_value());
    for (int k = 0; k < 3; ++k) {
      Frame f;
      f.ts = r.clock.now() + 1s;
      f.seq = ++r.seq;
      f.integrated = true;
      f.span = 100ms;
      for (const auto& c : kChannels) f.values.emplace_back(c, 1.0);
      r.acquirer.push(std::move(f));
      r.clock.advance(25ms);
    }
    std::this_thread::sleep_for(2ms);
    service.reset();  // polls may still be delivering
    EXPECT_FALSE(r.spec->acquisition().running());
  }
  r.scheduler.stop();
}

// ---- On a VirtualClock: the test's thread takes part in the clock's time ----

namespace {

struct ScanServiceVirtual : pychron::testing::VirtualTimeTest {};

}  // namespace

// A start holds the service while the instrument takes its integration time,
// which is clock time. A stop that arrives meanwhile waits for it through the
// clock: blocked any other way it looks runnable and time stands.
TEST_F(ScanServiceVirtual, AStopWaitsForAStartWithoutStallingTime) {
  VirtualClock clock;
  Clock::Participant main(clock, "test");
  SignalBus bus;
  Scheduler scheduler{clock, &bus, Scheduler::Options{0}};
  CallLog log;
  FakePositioner positioner{log};
  FakeSource source;
  FakeAcquirer acquirer{kChannels};
  acquirer.on_configure = [&] { clock.sleep_for(3s); };  // the instrument is slow to answer
  auto d = cfg::load_spectrometer(kIntegrated);
  ASSERT_TRUE(d.has_value()) << d.error().what;
  std::map<std::string, FieldTable> tables;
  for (const auto& [name, tf] : d->tables) tables.emplace(name, to_field_table(tf));
  SpectrometerRoles roles;
  roles.positioner = &positioner;
  roles.source = &source;
  roles.acquirers = {{"sim", &acquirer}};
  auto spec = Spectrometer::create(d->config, MolecularWeights(d->weights), std::move(tables), std::move(roles),
                                   SpectrometerContext{clock, scheduler, bus});
  ASSERT_TRUE(spec.has_value()) << spec.error().what;
  ScanService service(**spec, bus, clock);
  const TimePoint start = clock.now();
  const auto real_start = std::chrono::steady_clock::now();

  TimePoint started{}, stopped{};
  bool start_ok = false;
  pychron::testing::Crew crew(clock);
  crew.start("start", [&] {
    start_ok = service.start(1s).has_value();
    started = clock.now();
  });
  ASSERT_TRUE(pychron::testing::await_waiters(clock, 1));  // the start, inside configure()
  ASSERT_EQ(acquirer.configures.load(), 1);
  crew.start("stop", [&] {
    service.stop();
    stopped = clock.now();
  });
  crew.join();

  EXPECT_TRUE(start_ok);
  EXPECT_EQ(started, start + 3s);
  // The stop came after the whole start, and stopped what it started.
  EXPECT_EQ(stopped, start + 3s);
  EXPECT_EQ(acquirer.starts, 1);
  EXPECT_EQ(acquirer.stops, 1);
  EXPECT_FALSE(service.running());
  EXPECT_FALSE((*spec)->acquisition().running());
  EXPECT_EQ(clock.now(), start + 3s);
  EXPECT_LT(std::chrono::steady_clock::now() - real_start, 5s);
}
