#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <thread>

#include "pychron/systems/spectrometer/acquisition.hpp"
#include "spectrometer_fakes.hpp"

using namespace pychron;
using namespace pychron::spectrometer;
using namespace std::chrono_literals;

namespace {

const TimePoint kT0 = TimePoint{} + 100s;

struct FakeAcquirer : IIntensityAcquirer {
  FakeAcquirer(std::vector<ChannelId> chans, bool integrating, const Clock* auto_clock = nullptr)
      : chans_(std::move(chans)), integrating_(integrating), auto_clock_(auto_clock) {}

  std::vector<ChannelId> channels() const override { return chans_; }
  bool integrates() const override { return integrating_; }
  Result<void> configure(Duration d) override {
    configured = d;
    return {};
  }
  Result<void> start() override {
    ++starts;
    return {};
  }
  Result<void> stop() override {
    ++stops;
    return {};
  }
  Result<void> trigger() override {
    std::lock_guard lock(m_);
    ++triggers;
    if (auto_clock_) {
      Frame f;
      f.ts = auto_clock_->now();
      f.seq = ++auto_seq_;
      f.integrated = true;
      f.span = 1s;
      for (const auto& c : chans_) f.values.emplace_back(c, 2.0);
      frames_.push_back(f);
    }
    return {};
  }
  Result<std::optional<Frame>> next(Duration) override {
    std::lock_guard lock(m_);
    if (frames_.empty()) return std::optional<Frame>{};
    Frame f = frames_.front();
    frames_.pop_front();
    return std::optional<Frame>(f);
  }

  void push(Frame f) {
    std::lock_guard lock(m_);
    frames_.push_back(std::move(f));
  }

  Duration configured{};
  int starts = 0, stops = 0, triggers = 0;

 private:
  std::vector<ChannelId> chans_;
  bool integrating_;
  const Clock* auto_clock_;
  std::mutex m_;
  std::deque<Frame> frames_;
  std::uint64_t auto_seq_ = 0;
};

Frame raw(TimePoint ts, std::uint64_t seq, std::vector<std::pair<ChannelId, double>> values,
          Duration span = 100ms) {
  Frame f;
  f.ts = ts;
  f.seq = seq;
  f.values = std::move(values);
  f.span = span;
  return f;
}

Frame integrated(TimePoint ts, std::uint64_t seq, std::vector<std::pair<ChannelId, double>> values,
                 Duration span = 1s) {
  Frame f = raw(ts, seq, std::move(values), span);
  f.integrated = true;
  return f;
}

DetectorConfig faraday(std::string name, std::string channel, double gain = 1.0) {
  DetectorConfig d;
  d.name = std::move(name);
  d.kind = DetectorKind::Faraday;
  d.channel = std::move(channel);
  d.software_gain = gain;
  return d;
}

DetectorConfig counter(std::string name, std::string channel, double dead_time_ns) {
  DetectorConfig d;
  d.name = std::move(name);
  d.kind = DetectorKind::Counter;
  d.channel = std::move(channel);
  d.dead_time_ns = dead_time_ns;
  return d;
}

// Declare acquirers before the Fixture: the engine does not own them and its
// destructor calls stop() on each, so they must outlive it.
struct Fixture {
  ManualClock clock{kT0};
  SignalBus bus;
  Scheduler sched{clock, &bus, Scheduler::Options{0}};
  std::vector<IntensityReading> events;
  std::vector<Alarm> alarms;
  SignalBus::Subscription s1 =
      bus.subscribe<IntensityReading>([this](const IntensityReading& e) { events.push_back(e); });
  SignalBus::Subscription s2 = bus.subscribe<Alarm>([this](const Alarm& e) { alarms.push_back(e); });
  std::unique_ptr<AcquisitionEngine> engine;

  void make(std::vector<IIntensityAcquirer*> acq, std::vector<DetectorConfig> dets) {
    auto r = AcquisitionEngine::create(std::move(acq), std::move(dets), sched, bus, clock);
    ASSERT_TRUE(r.has_value()) << to_string(r.error());
    engine = std::move(*r);
  }
  Reading next_reading() {
    auto r = engine->stream()->next(0ms);
    EXPECT_TRUE(r.has_value());
    EXPECT_TRUE(r->has_value());
    return r && *r ? **r : Reading{};
  }
};

}  // namespace

TEST(AcquisitionEngine, RejectsDetectorWithoutExactlyOneChannelCarrier) {
  FakeAcquirer a({"H1"}, true), b({"H1"}, true);
  Fixture f;
  auto missing = AcquisitionEngine::create({&a}, {faraday("L1", "L1")}, f.sched, f.bus, f.clock);
  ASSERT_FALSE(missing.has_value());
  EXPECT_EQ(missing.error().kind, ErrorKind::Config);
  auto dup = AcquisitionEngine::create({&a, &b}, {faraday("H1", "H1")}, f.sched, f.bus, f.clock);
  ASSERT_FALSE(dup.has_value());
  EXPECT_EQ(dup.error().kind, ErrorKind::Config);
}

TEST(AcquisitionEngine, IntegratedFramePassesThroughWithGainAndSnappedIntegration) {
  FakeAcquirer a({"H1", "AX"}, true);
  Fixture f;
  f.make({&a}, {faraday("H1", "H1", 2.0), faraday("AX", "AX")});
  ASSERT_TRUE(f.engine->start(1s).has_value());
  EXPECT_EQ(a.configured, 1s);
  EXPECT_EQ(a.starts, 1);

  a.push(integrated(kT0 + 1s, 1, {{"H1", 3.0}, {"AX", 5.0}}, 2s));  // vendor snapped 1 s -> 2 s
  f.engine->poll(0);

  EXPECT_EQ(f.engine->integration(), 2s);
  const Reading r = f.next_reading();
  EXPECT_EQ(r.integration, 2s);
  EXPECT_EQ(r.ts, kT0 + 1s);
  ASSERT_TRUE(r.values.at("H1").has_value());
  EXPECT_DOUBLE_EQ(r.values.at("H1")->mean, 6.0);
  EXPECT_FALSE(r.values.at("H1")->sigma.has_value());
  EXPECT_DOUBLE_EQ(r.values.at("AX")->mean, 5.0);
  ASSERT_EQ(f.events.size(), 1U);
  EXPECT_EQ(f.events[0].reading.ts, r.ts);
}

TEST(AcquisitionEngine, HostIntegrationBinsFaradaySamplesToMeanSigmaN) {
  FakeAcquirer a({"H1"}, false);
  Fixture f;
  f.make({&a}, {faraday("H1", "H1", 2.0)});
  ASSERT_TRUE(f.engine->start(1s).has_value());

  for (int k = 0; k < 10; ++k) a.push(raw(kT0 + k * 100ms, static_cast<std::uint64_t>(k + 1), {{"H1", 1.0 + k}}));
  f.engine->poll(0);
  EXPECT_EQ(f.engine->stream()->size(), 0U);  // bin still open

  f.clock.advance(1s);  // bin [T0, T0+1s) complete
  f.engine->poll(0);
  const Reading r = f.next_reading();
  EXPECT_EQ(r.ts, kT0 + 1s);
  const Value v = *r.values.at("H1");
  EXPECT_NEAR(v.mean, 5.5 * 2.0, 1e-12);
  EXPECT_NEAR(*v.sigma, std::sqrt(82.5 / 9.0) * 2.0, 1e-9);
  EXPECT_EQ(*v.n, 10U);
}

TEST(AcquisitionEngine, NextBinClosesPreviousWhenFrameArrives) {
  FakeAcquirer a({"H1"}, false);
  Fixture f;
  f.make({&a}, {faraday("H1", "H1")});
  ASSERT_TRUE(f.engine->start(1s).has_value());
  a.push(raw(kT0 + 100ms, 1, {{"H1", 1.0}}));
  a.push(raw(kT0 + 1100ms, 2, {{"H1", 9.0}}));
  f.engine->poll(0);
  const Reading r = f.next_reading();
  EXPECT_DOUBLE_EQ(r.values.at("H1")->mean, 1.0);
  EXPECT_EQ(*r.values.at("H1")->n, 1U);
  EXPECT_FALSE(r.values.at("H1")->sigma.has_value());
}

TEST(AcquisitionEngine, CounterSumsCountsAndAppliesDeadTime) {
  FakeAcquirer a({"counter:0"}, false);
  Fixture f;
  f.make({&a}, {counter("CDD", "counter:0", 1e5)});  // tau = 100 us
  ASSERT_TRUE(f.engine->start(1s).has_value());
  for (int k = 0; k < 10; ++k) a.push(raw(kT0 + k * 100ms, static_cast<std::uint64_t>(k + 1), {{"counter:0", 100.0}}));
  f.clock.advance(1s);
  f.engine->poll(0);
  const Value v = *f.next_reading().values.at("CDD");
  // 1000 counts in 1 s = 1000 cps raw; 1000 / (1 - 1000 * 1e-4)
  EXPECT_NEAR(v.mean, 1000.0 / 0.9, 1e-9);
  EXPECT_NEAR(*v.sigma, 0.0, 1e-9);
  EXPECT_EQ(*v.n, 10U);
  EXPECT_FALSE(v.saturated);
}

TEST(AcquisitionEngine, FaradayAndCounterBinsShareEpochAndMergeIntoOneRow) {
  FakeAcquirer adc({"H1"}, false), pc({"counter:0"}, false);
  Fixture f;
  f.make({&adc, &pc}, {faraday("H1", "H1"), counter("CDD", "counter:0", 0.0)});
  ASSERT_TRUE(f.engine->start(1s).has_value());
  for (int k = 0; k < 10; ++k) {
    adc.push(raw(kT0 + k * 100ms, static_cast<std::uint64_t>(k + 1), {{"H1", 4.0}}));
    pc.push(raw(kT0 + k * 100ms + 30ms, static_cast<std::uint64_t>(k + 1), {{"counter:0", 50.0}}));
  }
  f.clock.advance(1s);
  f.engine->poll(0);
  EXPECT_EQ(f.engine->stream()->size(), 0U);  // waits for the counter bin
  f.engine->poll(1);

  ASSERT_EQ(f.events.size(), 1U);
  const Reading r = f.next_reading();
  EXPECT_DOUBLE_EQ(r.values.at("H1")->mean, 4.0);
  EXPECT_DOUBLE_EQ(r.values.at("CDD")->mean, 500.0);
  EXPECT_EQ(f.engine->stream()->size(), 0U);
}

TEST(AcquisitionEngine, LaggingAcquirerYieldsNulloptButRowIsStillEmitted) {
  FakeAcquirer adc({"H1"}, false), pc({"counter:0"}, false);
  Fixture f;
  f.make({&adc, &pc}, {faraday("H1", "H1"), counter("CDD", "counter:0", 0.0)});
  ASSERT_TRUE(f.engine->start(1s).has_value());
  adc.push(raw(kT0 + 100ms, 1, {{"H1", 4.0}}));
  f.clock.advance(1s);
  f.engine->poll(0);
  EXPECT_EQ(f.engine->stream()->size(), 0U);
  f.clock.advance(1s);  // grace over
  f.engine->poll(0);
  const Reading r = f.next_reading();
  EXPECT_DOUBLE_EQ(r.values.at("H1")->mean, 4.0);
  ASSERT_EQ(r.values.count("CDD"), 1U);
  EXPECT_FALSE(r.values.at("CDD").has_value());
}

TEST(AcquisitionEngine, MissingChannelInFrameIsNullopt) {
  FakeAcquirer a({"H1", "AX"}, true);
  Fixture f;
  f.make({&a}, {faraday("H1", "H1"), faraday("AX", "AX")});
  ASSERT_TRUE(f.engine->start(1s).has_value());
  a.push(integrated(kT0 + 1s, 1, {{"H1", 1.0}}));
  f.engine->poll(0);
  const Reading r = f.next_reading();
  EXPECT_TRUE(r.values.at("H1").has_value());
  EXPECT_FALSE(r.values.at("AX").has_value());
}

TEST(AcquisitionEngine, StaleFramesBeforeRequestStartAreDiscarded) {
  FakeAcquirer a({"H1"}, true);
  Fixture f;
  f.make({&a}, {faraday("H1", "H1")});
  ASSERT_TRUE(f.engine->start(1s).has_value());
  a.push(integrated(kT0 - 1s, 1, {{"H1", 1.0}}));
  a.push(integrated(kT0 + 1s, 2, {{"H1", 2.0}}));
  f.engine->poll(0);
  EXPECT_EQ(f.engine->stale_frames(), 1U);
  ASSERT_EQ(f.events.size(), 1U);
  EXPECT_DOUBLE_EQ(f.events[0].reading.values.at("H1")->mean, 2.0);
}

TEST(AcquisitionEngine, SeqGapsCountAsDroppedFrames) {
  FakeAcquirer a({"H1"}, true);
  Fixture f;
  f.make({&a}, {faraday("H1", "H1")});
  ASSERT_TRUE(f.engine->start(1s).has_value());
  a.push(integrated(kT0 + 1s, 1, {{"H1", 1.0}}));
  a.push(integrated(kT0 + 2s, 4, {{"H1", 1.0}}));
  f.engine->poll(0);
  EXPECT_EQ(f.engine->dropped_frames(), 2U);
}

TEST(AcquisitionEngine, SaturationFlagSetWhenValueExceedsConfig) {
  FakeAcquirer a({"H1", "AX"}, true);
  Fixture f;
  auto h1 = faraday("H1", "H1");
  h1.saturation = 10.0;
  auto ax = faraday("AX", "AX");
  ax.saturation = 10.0;
  f.make({&a}, {h1, ax});
  ASSERT_TRUE(f.engine->start(1s).has_value());
  a.push(integrated(kT0 + 1s, 1, {{"H1", 11.0}, {"AX", 9.0}}));
  f.engine->poll(0);
  const Reading r = f.next_reading();
  EXPECT_TRUE(r.values.at("H1")->saturated);
  EXPECT_FALSE(r.values.at("AX")->saturated);
}

TEST(AcquisitionEngine, StallRaisesAlarmAndStreamTimeoutOnce) {
  FakeAcquirer a({"H1"}, true);
  Fixture f;
  f.make({&a}, {faraday("H1", "H1")});
  ASSERT_TRUE(f.engine->start(1s).has_value());
  f.clock.advance(5s);  // limit = 3 * 1 s + 3 s
  f.engine->poll(0);
  EXPECT_TRUE(f.alarms.empty());
  f.clock.advance(1100ms);
  f.engine->poll(0);
  ASSERT_EQ(f.alarms.size(), 1U);
  EXPECT_EQ(f.alarms[0].source, "acquisition");
  auto r = f.engine->stream()->next(0ms);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().kind, ErrorKind::Timeout);
  f.engine->poll(0);
  EXPECT_EQ(f.alarms.size(), 1U);

  // A frame recovers the stream.
  a.push(integrated(f.clock.now(), 1, {{"H1", 1.0}}));
  f.engine->poll(0);
  auto ok = f.engine->stream()->next(0ms);
  ASSERT_TRUE(ok.has_value());
  EXPECT_TRUE(ok->has_value());
}

TEST(IntensityStream, IsBoundedAndDropsOldest) {
  FakeAcquirer a({"H1"}, true);
  Fixture f;
  AcquisitionEngine::Options o;
  o.queue_capacity = 2;
  auto e = AcquisitionEngine::create({&a}, {faraday("H1", "H1")}, f.sched, f.bus, f.clock, o);
  ASSERT_TRUE(e.has_value());
  ASSERT_TRUE((*e)->start(1s).has_value());
  for (std::uint64_t k = 1; k <= 3; ++k) a.push(integrated(kT0 + std::chrono::seconds(k), k, {{"H1", double(k)}}));
  (*e)->poll(0);
  auto s = (*e)->stream();
  EXPECT_EQ(s->size(), 2U);
  EXPECT_EQ(s->dropped(), 1U);
  EXPECT_DOUBLE_EQ((**s->next(0ms)).values.at("H1")->mean, 2.0);
}

TEST(IntensityStream, NextReturnsNulloptAfterTimeoutOnManualClock) {
  ManualClock clock{kT0};
  IntensityStream s(clock, 4);
  auto r = s.next(0ms);
  ASSERT_TRUE(r.has_value());
  EXPECT_FALSE(r->has_value());
}

TEST(AcquisitionEngine, SchedulerJobPollsAcquirer) {
  FakeAcquirer a({"H1"}, true);
  Fixture f;
  f.make({&a}, {faraday("H1", "H1")});
  ASSERT_TRUE(f.engine->start(1s).has_value());
  EXPECT_EQ(f.sched.job_count(), 1U);
  a.push(integrated(kT0 + 1s, 1, {{"H1", 1.0}}));
  f.clock.advance(50ms);
  f.sched.run_pending();
  f.sched.wait_idle();
  EXPECT_EQ(f.events.size(), 1U);
  f.engine->stop();
  EXPECT_EQ(f.sched.job_count(), 0U);
  EXPECT_EQ(a.stops, 1);
}

TEST(AcquisitionEngine, AcquireNTriggersPerPeriodAndReturnsNReadings) {
  Fixture f;
  FakeAcquirer a({"H1"}, true, &f.clock);
  f.make({&a}, {faraday("H1", "H1")});
  auto fut = std::async(std::launch::async, [&] { return f.engine->acquire(3); });
  while (fut.wait_for(1ms) != std::future_status::ready) {
    f.clock.advance(10ms);
    f.engine->poll(0);
  }
  auto r = fut.get();
  ASSERT_TRUE(r.has_value()) << to_string(r.error());
  EXPECT_EQ(r->size(), 3U);
  EXPECT_EQ(a.triggers, 3);
  EXPECT_EQ(a.starts, 1);
  EXPECT_EQ(a.stops, 1);
  EXPECT_FALSE(f.engine->running());
}

TEST(AcquisitionEngine, AcquireDurationReturnsReadingsWithinWindow) {
  Fixture f;
  FakeAcquirer a({"H1"}, true, &f.clock);
  f.make({&a}, {faraday("H1", "H1")});
  auto fut = std::async(std::launch::async, [&] { return f.engine->acquire(50ms); });
  while (fut.wait_for(1ms) != std::future_status::ready) {
    f.clock.advance(10ms);
    f.engine->poll(0);
  }
  auto r = fut.get();
  ASSERT_TRUE(r.has_value()) << to_string(r.error());
  EXPECT_GE(r->size(), 1U);
}

TEST(AcquisitionEngine, CancelStopsAcquire) {
  Fixture f;
  FakeAcquirer a({"H1"}, true);  // never produces frames
  f.make({&a}, {faraday("H1", "H1")});
  auto fut = std::async(std::launch::async, [&] { return f.engine->acquire(5); });
  while (fut.wait_for(5ms) != std::future_status::ready) f.engine->cancel();
  auto r = fut.get();
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().kind, ErrorKind::Cancelled);
}

TEST(AcquisitionEngine, AcquireFailsWithTimeoutWhenAcquirerStalls) {
  FakeAcquirer a({"H1"}, true);
  Fixture f;
  f.make({&a}, {faraday("H1", "H1")});
  auto fut = std::async(std::launch::async, [&] { return f.engine->acquire(1); });
  while (fut.wait_for(1ms) != std::future_status::ready) {
    f.clock.advance(1s);
    f.engine->poll(0);
  }
  auto r = fut.get();
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().kind, ErrorKind::Timeout);
}

// ---- Quiescence: stop() waits for an in-flight poll -------------------------

namespace {

namespace fakes = pychron::spectrometer::testing;

struct Gate {
  void open() {
    {
      std::lock_guard lock(m);
      is_open = true;
    }
    cv.notify_all();
  }
  void close() {
    std::lock_guard lock(m);
    is_open = false;
  }
  // Bounded so a regression fails the test instead of hanging it.
  bool wait(std::chrono::milliseconds limit = 2000ms) {
    std::unique_lock lock(m);
    return cv.wait_for(lock, limit, [&] { return is_open; });
  }
  std::mutex m;
  std::condition_variable cv;
  bool is_open = false;
};

// Polls run on Scheduler workers. Declare the acquirer before this fixture.
struct ThreadedFixture {
  ManualClock clock{kT0};
  SignalBus bus;
  Scheduler sched{clock, &bus, Scheduler::Options{2}};
  std::unique_ptr<AcquisitionEngine> engine;

  void make(fakes::FakeAcquirer& a, fakes::FakeAcquirer* b = nullptr) {
    std::vector<IIntensityAcquirer*> acq{&a};
    std::vector<DetectorConfig> dets{faraday("H1", "H1")};
    if (b != nullptr) {
      acq.push_back(b);
      dets.push_back(faraday("L1", "L1"));
    }
    auto r = AcquisitionEngine::create(std::move(acq), std::move(dets), sched, bus, clock);
    ASSERT_TRUE(r.has_value()) << to_string(r.error());
    engine = std::move(*r);
    sched.start();
  }
  void tick() { clock.advance(50ms); }  // past the 20 ms poll interval
};

}  // namespace

TEST(AcquisitionEngine, StopWaitsForInFlightNext) {
  fakes::CallLog log;
  fakes::FakeAcquirer a({"H1"});
  a.log = &log;
  Gate entered, release;
  a.on_next = [&] {
    entered.open();
    release.wait();
  };
  ThreadedFixture f;
  f.make(a);
  ASSERT_TRUE(f.engine->start(1s).has_value());
  f.tick();
  ASSERT_TRUE(entered.wait());

  auto stopped = std::async(std::launch::async, [&] { f.engine->stop(); });
  EXPECT_EQ(stopped.wait_for(50ms), std::future_status::timeout);  // next() still in flight
  release.open();
  ASSERT_EQ(stopped.wait_for(2s), std::future_status::ready);
  stopped.get();

  EXPECT_EQ(log, (fakes::CallLog{"start", "next-enter", "next-exit", "stop"}));
  EXPECT_FALSE(f.engine->running());
}

TEST(AcquisitionEngine, StartAfterStopNeverOverlapsPreviousNext) {
  fakes::FakeAcquirer a({"H1"});
  Gate entered;
  std::atomic<bool> stopping{false};
  a.on_next = [&] {
    entered.open();
    // Outlive the stop() request by a while, so a stop() that does not wait
    // lets the following configure()/start() land inside this next().
    while (!stopping) std::this_thread::yield();
    for (int k = 0; k < 1000; ++k) std::this_thread::yield();
  };
  ThreadedFixture f;
  f.make(a);
  ASSERT_TRUE(f.engine->start(1s).has_value());
  int restarts = 0;
  for (; restarts < 50; ++restarts) {
    stopping = false;
    entered.close();
    f.tick();
    if (!entered.wait()) break;
    stopping = true;
    f.engine->stop();
    if (!f.engine->start(1s).has_value()) break;
  }
  stopping = true;  // before any assertion: a parked next() would hang teardown
  f.engine->stop();
  EXPECT_EQ(restarts, 50);
  EXPECT_EQ(a.overlaps.load(), 0);
  EXPECT_EQ(a.stops, a.starts);
}

TEST(AcquisitionEngine, StopFromInsidePollDoesNotDeadlock) {
  fakes::FakeAcquirer a({"H1"});
  ThreadedFixture f;
  Gate done;
  a.on_next = [&] {
    f.engine->stop();
    done.open();
  };
  f.make(a);
  ASSERT_TRUE(f.engine->start(1s).has_value());
  f.tick();
  ASSERT_TRUE(done.wait());
  f.sched.wait_idle();
  EXPECT_FALSE(f.engine->running());
  EXPECT_EQ(a.stops, 1);
  EXPECT_EQ(f.sched.job_count(), 0U);

  // The engine is still usable afterwards.
  a.on_next = nullptr;
  ASSERT_TRUE(f.engine->start(1s).has_value());
  EXPECT_EQ(a.starts, 2);
}

TEST(AcquisitionEngine, StopWithNothingRunningIsImmediate) {
  fakes::FakeAcquirer a({"H1"});
  ThreadedFixture f;
  f.make(a);
  f.engine->stop();  // never started
  EXPECT_EQ(a.stops, 0);

  ASSERT_TRUE(f.engine->start(1s).has_value());
  f.engine->stop();  // started, no poll dispatched
  EXPECT_EQ(a.stops, 1);
  f.engine->stop();
  EXPECT_EQ(a.stops, 1);
  EXPECT_EQ(a.overlaps.load(), 0);
}

TEST(AcquisitionEngine, RestartByASecondCallerWaitsForTheFirstStopToFinish) {
  fakes::CallLog log;
  fakes::FakeAcquirer a({"H1"});
  a.log = &log;
  Gate entered, release, restarted_gate;
  bool restarted_during_stop = false;
  a.on_next = [&] {
    entered.open();
    release.wait();
  };
  // X's acquirer stop() lingers, giving Y every chance to restart under it.
  a.on_stop = [&] { restarted_during_stop = restarted_gate.wait(50ms); };
  ThreadedFixture f;
  f.make(a);
  ASSERT_TRUE(f.engine->start(1s).has_value());
  f.tick();
  ASSERT_TRUE(entered.wait());

  // X stops and blocks behind the parked next(); Y then stops and restarts.
  auto x = std::async(std::launch::async, [&] { f.engine->stop(); });
  while (f.engine->running()) std::this_thread::yield();
  auto y = std::async(std::launch::async, [&] {
    f.engine->stop();
    auto r = f.engine->start(1s);
    restarted_gate.open();
    return r;
  });
  release.open();
  ASSERT_EQ(x.wait_for(2s), std::future_status::ready);
  ASSERT_EQ(y.wait_for(2s), std::future_status::ready);
  x.get();
  auto restarted = y.get();
  ASSERT_TRUE(restarted.has_value()) << to_string(restarted.error());

  // X's acquirer stop() landed on the old run, not on the one Y started.
  EXPECT_FALSE(restarted_during_stop);
  EXPECT_EQ(log, (fakes::CallLog{"start", "next-enter", "next-exit", "stop", "start"}));
  EXPECT_TRUE(f.engine->running());
  EXPECT_EQ(a.starts, 2);
  EXPECT_EQ(a.stops, 1);
  EXPECT_EQ(a.overlaps.load(), 0);
}

TEST(AcquisitionEngine, TwoPollsStoppingAtOnceDoNotDeadlock) {
  fakes::FakeAcquirer a({"H1"}), b({"L1"});
  ThreadedFixture f;
  std::atomic<int> arrived{0}, returned{0};
  Gate both_in, done;
  auto stop_from_next = [&] {
    if (++arrived == 2) both_in.open();
    both_in.wait();  // both workers are inside a poll before either stops
    f.engine->stop();
    if (++returned == 2) done.open();
  };
  a.on_next = stop_from_next;
  b.on_next = stop_from_next;
  f.make(a, &b);
  ASSERT_TRUE(f.engine->start(1s).has_value());
  f.tick();
  ASSERT_TRUE(done.wait());
  f.sched.wait_idle();
  EXPECT_FALSE(f.engine->running());
  EXPECT_EQ(f.sched.job_count(), 0U);
  EXPECT_EQ(a.stops, 1);
  EXPECT_EQ(b.stops, 1);
}

TEST(AcquisitionEngine, StartFromInsideAPollDuringAnotherThreadsStopFails) {
  fakes::FakeAcquirer a({"H1"});
  ThreadedFixture f;
  Gate entered, release, done;
  Result<void> restart;
  a.on_next = [&] {
    entered.open();
    release.wait();
    restart = f.engine->start(1s);  // the stopper is waiting for this poll
    done.open();
  };
  f.make(a);
  ASSERT_TRUE(f.engine->start(1s).has_value());
  f.tick();
  ASSERT_TRUE(entered.wait());
  auto x = std::async(std::launch::async, [&] { f.engine->stop(); });
  while (f.engine->running()) std::this_thread::yield();
  release.open();
  ASSERT_TRUE(done.wait());
  ASSERT_EQ(x.wait_for(2s), std::future_status::ready);
  ASSERT_FALSE(restart.has_value());
  EXPECT_EQ(restart.error().kind, ErrorKind::Config);
  EXPECT_FALSE(f.engine->running());
  EXPECT_EQ(a.starts, 1);
  EXPECT_EQ(a.stops, 1);
}

// ---- start() is serialised ---------------------------------------------------

TEST(AcquisitionEngine, ConcurrentStartsRegisterOneRun) {
  fakes::FakeAcquirer a({"H1"});
  Gate entered, release;
  a.on_configure = [&] {
    entered.open();
    release.wait();  // a slow configure(): wire I/O in a real driver
  };
  ThreadedFixture f;
  f.make(a);

  auto first = std::async(std::launch::async, [&] { return f.engine->start(1s); });
  ASSERT_TRUE(entered.wait());
  auto second = std::async(std::launch::async, [&] { return f.engine->start(1s); });
  EXPECT_EQ(second.wait_for(50ms), std::future_status::timeout);  // waits for the first
  release.open();
  ASSERT_EQ(first.wait_for(2s), std::future_status::ready);
  ASSERT_EQ(second.wait_for(2s), std::future_status::ready);
  auto r1 = first.get();
  auto r2 = second.get();

  ASSERT_TRUE(r1.has_value()) << to_string(r1.error());
  ASSERT_FALSE(r2.has_value());  // the normal "already running" path
  EXPECT_EQ(r2.error().kind, ErrorKind::Config);
  EXPECT_NE(r2.error().what.find("already running"), std::string::npos) << r2.error().what;
  EXPECT_TRUE(f.engine->running());
  EXPECT_EQ(a.configures.load(), 1);
  EXPECT_EQ(a.starts, 1);
  EXPECT_EQ(f.sched.job_count(), 1U);  // one job per acquirer
  EXPECT_EQ(a.overlaps.load(), 0);

  f.engine->stop();
  EXPECT_EQ(a.stops, 1);
  EXPECT_EQ(f.sched.job_count(), 0U);
}

TEST(AcquisitionEngine, ConcurrentStartsWithTwoAcquirersRegisterOneJobEach) {
  fakes::FakeAcquirer a({"H1"}), b({"L1"});
  Gate entered, release;
  b.on_configure = [&] {
    entered.open();
    release.wait();
  };
  ThreadedFixture f;
  f.make(a, &b);
  auto first = std::async(std::launch::async, [&] { return f.engine->start(1s); });
  ASSERT_TRUE(entered.wait());
  auto second = std::async(std::launch::async, [&] { return f.engine->start(1s); });
  EXPECT_EQ(second.wait_for(50ms), std::future_status::timeout);
  release.open();
  ASSERT_EQ(first.wait_for(2s), std::future_status::ready);
  ASSERT_EQ(second.wait_for(2s), std::future_status::ready);
  const bool first_ok = first.get().has_value();
  const bool second_ok = second.get().has_value();
  EXPECT_NE(first_ok, second_ok);  // exactly one of them started it
  EXPECT_EQ(f.sched.job_count(), 2U);
  EXPECT_EQ(a.starts, 1);
  EXPECT_EQ(b.starts, 1);
  EXPECT_EQ(a.overlaps.load() + b.overlaps.load(), 0);
  f.engine->stop();
  EXPECT_EQ(f.sched.job_count(), 0U);
}

TEST(AcquisitionEngine, StopDuringAStartInProgressLeavesTheEngineStopped) {
  fakes::CallLog log;
  fakes::FakeAcquirer a({"H1"});
  a.log = &log;
  Gate entered, release;
  a.on_configure = [&] {
    entered.open();
    release.wait();
  };
  ThreadedFixture f;
  f.make(a);

  auto started = std::async(std::launch::async, [&] { return f.engine->start(1s); });
  ASSERT_TRUE(entered.wait());
  auto stopped = std::async(std::launch::async, [&] { f.engine->stop(); });
  EXPECT_EQ(stopped.wait_for(50ms), std::future_status::timeout);  // waits for the start
  release.open();
  ASSERT_EQ(started.wait_for(2s), std::future_status::ready);
  ASSERT_EQ(stopped.wait_for(2s), std::future_status::ready);
  stopped.get();
  auto r = started.get();
  ASSERT_TRUE(r.has_value()) << to_string(r.error());

  EXPECT_FALSE(f.engine->running());
  EXPECT_EQ(log, (fakes::CallLog{"start", "stop"}));
  EXPECT_EQ(f.sched.job_count(), 0U);

  // No tick reaches next() afterwards.
  f.tick();
  f.sched.wait_idle();
  EXPECT_EQ(log, (fakes::CallLog{"start", "stop"}));
}

// A start() that fails must not leave the engine marked as starting.
TEST(AcquisitionEngine, FailedStartDoesNotBlockLaterStartOrStop) {
  fakes::FakeAcquirer a({"H1"});
  ThreadedFixture f;
  f.make(a);
  a.fail_start = true;
  ASSERT_FALSE(f.engine->start(1s).has_value());
  EXPECT_FALSE(f.engine->running());
  EXPECT_EQ(f.sched.job_count(), 0U);

  auto stopped = std::async(std::launch::async, [&] { f.engine->stop(); });
  ASSERT_EQ(stopped.wait_for(2s), std::future_status::ready);
  a.fail_start = false;
  auto restarted = std::async(std::launch::async, [&] { return f.engine->start(1s); });
  ASSERT_EQ(restarted.wait_for(2s), std::future_status::ready);
  EXPECT_TRUE(restarted.get().has_value());
  f.engine->stop();
}
