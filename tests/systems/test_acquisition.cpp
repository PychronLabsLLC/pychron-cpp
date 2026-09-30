#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <thread>

#include "pychron/systems/spectrometer/acquisition.hpp"

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
  Fixture f;
  FakeAcquirer a({"H1"}, true), b({"H1"}, true);
  auto missing = AcquisitionEngine::create({&a}, {faraday("L1", "L1")}, f.sched, f.bus, f.clock);
  ASSERT_FALSE(missing.has_value());
  EXPECT_EQ(missing.error().kind, ErrorKind::Config);
  auto dup = AcquisitionEngine::create({&a, &b}, {faraday("H1", "H1")}, f.sched, f.bus, f.clock);
  ASSERT_FALSE(dup.has_value());
  EXPECT_EQ(dup.error().kind, ErrorKind::Config);
}

TEST(AcquisitionEngine, IntegratedFramePassesThroughWithGainAndSnappedIntegration) {
  Fixture f;
  FakeAcquirer a({"H1", "AX"}, true);
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
  Fixture f;
  FakeAcquirer a({"H1"}, false);
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
  Fixture f;
  FakeAcquirer a({"H1"}, false);
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
  Fixture f;
  FakeAcquirer a({"counter:0"}, false);
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
  Fixture f;
  FakeAcquirer adc({"H1"}, false), pc({"counter:0"}, false);
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
  Fixture f;
  FakeAcquirer adc({"H1"}, false), pc({"counter:0"}, false);
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
  Fixture f;
  FakeAcquirer a({"H1", "AX"}, true);
  f.make({&a}, {faraday("H1", "H1"), faraday("AX", "AX")});
  ASSERT_TRUE(f.engine->start(1s).has_value());
  a.push(integrated(kT0 + 1s, 1, {{"H1", 1.0}}));
  f.engine->poll(0);
  const Reading r = f.next_reading();
  EXPECT_TRUE(r.values.at("H1").has_value());
  EXPECT_FALSE(r.values.at("AX").has_value());
}

TEST(AcquisitionEngine, StaleFramesBeforeRequestStartAreDiscarded) {
  Fixture f;
  FakeAcquirer a({"H1"}, true);
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
  Fixture f;
  FakeAcquirer a({"H1"}, true);
  f.make({&a}, {faraday("H1", "H1")});
  ASSERT_TRUE(f.engine->start(1s).has_value());
  a.push(integrated(kT0 + 1s, 1, {{"H1", 1.0}}));
  a.push(integrated(kT0 + 2s, 4, {{"H1", 1.0}}));
  f.engine->poll(0);
  EXPECT_EQ(f.engine->dropped_frames(), 2U);
}

TEST(AcquisitionEngine, SaturationFlagSetWhenValueExceedsConfig) {
  Fixture f;
  FakeAcquirer a({"H1", "AX"}, true);
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
  Fixture f;
  FakeAcquirer a({"H1"}, true);
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
  Fixture f;
  FakeAcquirer a({"H1"}, true);
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
  Fixture f;
  FakeAcquirer a({"H1"}, true);
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
  Fixture f;
  FakeAcquirer a({"H1"}, true);
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
