#include "pychron/sim/spectrometer/sim_drivers.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <future>
#include <memory>
#include <optional>

#include <gtest/gtest.h>

#include "pychron/core/virtual_clock.hpp"
#include "pychron/transport/sim_transport.hpp"
#include "virtual_time.hpp"

namespace {

using namespace pychron;
using namespace pychron::sim;
using namespace pychron::spectrometer;
using namespace std::chrono_literals;

struct Bench {
  ManualClock clock;
  std::shared_ptr<BeamModel> beam = std::make_shared<BeamModel>(clock);
  double center(const char* det, const char* iso) { return *beam->peak_center(det, iso); }
};

TEST(SimDriverRegistry, AllFiveKindsRegistered) {
  auto& registry = DriverRegistry::global();
  for (auto kind : {"sim_integrated", "sim_dac_positioner", "sim_adc_bank", "sim_pulse_counter", "sim_hv_supply"}) {
    EXPECT_TRUE(registry.contains(kind)) << kind;
  }
}

TEST(SimDriverRegistry, DriversBuiltFromConfigShareOneBeam) {
  ManualClock clock;
  auto beam = std::make_shared<BeamModel>(clock);
  BeamModelRegistry::global().set("test-shared", beam);
  auto transport = SimTransport::scripted({});

  auto table = toml::parse(R"(beam = "test-shared"
channels = ["H1"]
sample_hz = 10.0)");
  auto& registry = DriverRegistry::global();
  DriverContext ctx{"x", &clock};
  auto dac = registry.create("sim_dac_positioner", *transport, toml::parse(R"(beam = "test-shared")"), ctx);
  auto hv = registry.create("sim_hv_supply", *transport, toml::parse(R"(beam = "test-shared")"), ctx);
  auto adc = registry.create("sim_adc_bank", *transport, table, ctx);
  ASSERT_TRUE(dac) << to_string(dac.error());
  ASSERT_TRUE(hv) << to_string(hv.error());
  ASSERT_TRUE(adc) << to_string(adc.error());

  auto* positioner = dynamic_cast<IMassPositioner*>(dac->get());
  auto* source = dynamic_cast<IBeamSource*>(hv->get());
  auto* acquirer = dynamic_cast<IIntensityAcquirer*>(adc->get());
  ASSERT_TRUE(positioner && source && acquirer);

  ASSERT_TRUE(positioner->set(*beam->peak_center("H1", "Ar40")));
  ASSERT_TRUE(acquirer->start());
  clock.advance(100ms);
  auto frame = acquirer->next(0ms);
  ASSERT_TRUE(frame && *frame);
  EXPECT_NEAR(*(*frame)->value("H1"), 1e6, 2e4);

  // HV moves the peak away from the DAC setting
  ASSERT_TRUE(source->set_hv(5000.0));
  clock.advance(100ms);
  frame = acquirer->next(0ms);
  ASSERT_TRUE(frame && *frame);
  EXPECT_LT(std::abs(*(*frame)->value("H1")), 1e5);
  BeamModelRegistry::global().clear();
}

TEST(SimIntegrated, PlaysAllFiveRoles) {
  Bench b;
  SimIntegrated d("sim", b.beam, {});
  EXPECT_EQ(d.native_axis(), IMassPositioner::Axis::Dac);
  EXPECT_TRUE(d.integrates());
  EXPECT_EQ(d.channels().size(), 6U);
  EXPECT_TRUE(d.caps().has(DetectorCap::Deflection));
  EXPECT_TRUE(d.caps().has(DetectorCap::Protect));
  EXPECT_TRUE(d.caps().has(DetectorCap::Gain));
  EXPECT_TRUE(d.caps().has(DetectorCap::CddVoltage));
  EXPECT_TRUE(d.blank(true));
  EXPECT_TRUE(b.beam->blanked());
}

TEST(SimIntegrated, MovingIsTrueForConfiguredTime) {
  Bench b;
  SimIntegrated::Options o;
  o.move_time = 300ms;
  SimIntegrated d("sim", b.beam, o);
  EXPECT_FALSE(*d.moving());
  ASSERT_TRUE(d.set(5.0));
  EXPECT_TRUE(*d.moving());
  b.clock.advance(299ms);
  EXPECT_TRUE(*d.moving());
  b.clock.advance(2ms);
  EXPECT_FALSE(*d.moving());
  EXPECT_DOUBLE_EQ(*d.read(), 5.0);
  EXPECT_DOUBLE_EQ(b.beam->magnet(), 5.0);
}

TEST(SimIntegrated, SetOutsideLimitsFailsAndKeepsPosition) {
  Bench b;
  SimIntegrated d("sim", b.beam, {});
  ASSERT_TRUE(d.set(4.0));
  auto r = d.set(11.0);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Config);
  EXPECT_DOUBLE_EQ(*d.read(), 4.0);
}

TEST(SimIntegrated, ConfigureSnapsAndFramesAreIntegrated) {
  Bench b;
  SimIntegrated d("sim", b.beam, {});
  EXPECT_FALSE(d.configure(0ms));
  ASSERT_TRUE(d.configure(1040ms));
  EXPECT_EQ(d.integration(), 1000ms);

  ASSERT_TRUE(d.set(b.center("H1", "Ar40")));
  ASSERT_TRUE(d.start());
  auto none = d.next(0ms);
  ASSERT_TRUE(none);
  EXPECT_FALSE(*none);

  b.clock.advance(1s);
  auto r = d.next(0ms);
  ASSERT_TRUE(r && *r);
  const Frame& f = **r;
  EXPECT_TRUE(f.integrated);
  EXPECT_EQ(f.span, 1000ms);
  EXPECT_EQ(f.seq, 1U);
  EXPECT_NEAR(*f.value("H1"), 1e6, 1e4);

  b.clock.advance(1s);
  auto r2 = d.next(0ms);
  ASSERT_TRUE(r2 && *r2);
  EXPECT_EQ((*r2)->seq, 2U);
  EXPECT_GE((*r2)->ts, f.ts);
  ASSERT_TRUE(d.stop());
  EXPECT_FALSE(d.next(0ms));
}

TEST(SimIntegrated, DeflectionShiftsThePeakSeenThroughTheAcquirer) {
  Bench b;
  SimIntegrated d("sim", b.beam, {});
  double c = b.center("H1", "Ar40");
  ASSERT_TRUE(d.set(c));
  ASSERT_TRUE(d.set_deflection("H1", 100.0));
  EXPECT_DOUBLE_EQ(*d.read_deflection("H1"), 100.0);
  ASSERT_TRUE(d.start());
  b.clock.advance(1s);
  auto off = d.next(0ms);
  EXPECT_LT(std::abs(*(**off).value("H1")), 20.0);

  ASSERT_TRUE(d.set(c + 0.12));
  b.clock.advance(1s);
  auto on = d.next(0ms);
  EXPECT_NEAR(*(**on).value("H1"), 1e6, 1e4);
}

TEST(SimIntegrated, ProtectionKeepsCounterSafeAndUnprotectedOverloads) {
  Bench b;
  SimIntegrated d("sim", b.beam, {});
  ASSERT_TRUE(d.set(b.center("CDD", "Ar40")));
  ASSERT_TRUE(d.protect("CDD", true));
  ASSERT_TRUE(d.start());
  b.clock.advance(1s);
  auto safe = d.next(0ms);
  EXPECT_EQ(*(**safe).value("CDD"), 0.0);
  EXPECT_FALSE(b.beam->overloaded("CDD"));

  ASSERT_TRUE(d.protect("CDD", false));
  b.clock.advance(1s);
  auto hot = d.next(0ms);
  EXPECT_GT(*(**hot).value("CDD"), 1e5);
  EXPECT_TRUE(b.beam->overloaded("CDD"));
}

TEST(SimIntegrated, SaturationShowsInFrames) {
  Bench b;
  b.beam->set_gas({{"Ar40", 39.96238, 1e7, 0.0}});
  SimIntegrated d("sim", b.beam, {});
  ASSERT_TRUE(d.set(b.center("H1", "Ar40")));
  ASSERT_TRUE(d.start());
  b.clock.advance(1s);
  auto r = d.next(0ms);
  EXPECT_DOUBLE_EQ(*(**r).value("H1"), 4.9e6);
}

TEST(SimIntegrated, UnknownChannelIsConfigError) {
  Bench b;
  SimIntegrated d("sim", b.beam, {});
  auto r = d.protect("nope", true);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Config);
  EXPECT_FALSE(d.set_gain("nope", 1.0));
  EXPECT_TRUE(d.set_gain("H1", 2.0));
  EXPECT_DOUBLE_EQ(*d.read_gain("H1"), 2.0);
  EXPECT_TRUE(d.set_cdd_voltage("CDD", 1300.0));
}

TEST(SimIntegrated, SourceParamsUseVendorNamesAndRanges) {
  Bench b;
  SimIntegrated d("sim", b.beam, {});
  const ParamSpec* y = find_spec(d.params(), ParamId{SourceParam::YSymmetry});
  ASSERT_NE(y, nullptr);
  EXPECT_EQ(y->vendor_name, "Y-Symmetry Set");

  ASSERT_TRUE(d.set_param(ParamId{SourceParam::YSymmetry}, 10.0));
  auto rb = d.read_param(ParamId{SourceParam::YSymmetry});
  ASSERT_TRUE(rb);
  EXPECT_DOUBLE_EQ(rb->setpoint, 10.0);
  EXPECT_FALSE(d.set_param(ParamId{SourceParam::YSymmetry}, 1e6));
  EXPECT_FALSE(d.set_param(ParamId{Custom{"nope"}}, 1.0));
  EXPECT_FALSE(d.read_param(ParamId{SourceParam::PoleN}));

  ASSERT_TRUE(d.set_hv(3000.0));
  EXPECT_DOUBLE_EQ(*d.read_hv(), 3000.0);
  EXPECT_DOUBLE_EQ(b.beam->hv(), 3000.0);
  EXPECT_FALSE(d.set_hv(-1.0));
}

TEST(SimDacPositioner, ReadReturnsCachedSetpointAndChecksLimits) {
  Bench b;
  SimDacPositioner d("dac", b.beam, {});
  EXPECT_EQ(d.native_axis(), IMassPositioner::Axis::Dac);
  ASSERT_TRUE(d.set(6.5));
  EXPECT_DOUBLE_EQ(*d.read(), 6.5);
  EXPECT_DOUBLE_EQ(b.beam->magnet(), 6.5);
  EXPECT_FALSE(*d.moving());
  EXPECT_FALSE(d.set(-0.5));
  EXPECT_DOUBLE_EQ(*d.read(), 6.5);
}

TEST(SimAdcBank, RawFramesAtSampleRate) {
  Bench b;
  SimAdcBank d("adc", b.beam, {"AX", "H1"}, 100.0);
  EXPECT_FALSE(d.integrates());
  EXPECT_EQ(d.sample_period(), 10ms);
  b.beam->set_magnet(b.center("H1", "Ar40"));
  ASSERT_TRUE(d.start());
  auto none = d.next(0ms);
  EXPECT_FALSE(*none);

  b.clock.advance(10ms);
  auto r = d.next(0ms);
  ASSERT_TRUE(r && *r);
  EXPECT_FALSE((*r)->integrated);
  EXPECT_EQ((*r)->span, 10ms);
  EXPECT_NEAR(*(*r)->value("H1"), 1e6, 2e4);
  EXPECT_EQ((*r)->values.size(), 2U);

  b.clock.advance(10ms);
  auto r2 = d.next(0ms);
  EXPECT_EQ((*r2)->seq, (*r)->seq + 1);
}

TEST(SimPulseCounter, RawCountFrames) {
  Bench b;
  SimPulseCounter d("pc", b.beam, {"EM"}, 10.0);
  EXPECT_FALSE(d.integrates());
  b.beam->set_magnet(b.center("EM", "Ar36"));
  ASSERT_TRUE(d.start());
  b.clock.advance(100ms);
  auto r = d.next(0ms);
  ASSERT_TRUE(r && *r);
  double counts = *(*r)->value("EM");
  EXPECT_EQ(counts, std::round(counts));
  EXPECT_NEAR(counts, 3e3 * 0.985 * 0.1, 60.0);
  EXPECT_EQ((*r)->span, 100ms);
}

TEST(SimHvSupply, ExposesOnlyHv) {
  Bench b;
  SimHvSupply d("hv", b.beam);
  ASSERT_EQ(d.params().size(), 1U);
  EXPECT_EQ(d.params()[0].id, ParamId{SourceParam::HV});
  ASSERT_TRUE(d.set_hv(4000.0));
  EXPECT_DOUBLE_EQ(*d.read_hv(), 4000.0);
  EXPECT_FALSE(d.set_param(ParamId{SourceParam::TrapCurrent}, 1.0));
  EXPECT_FALSE(d.set_hv(20000.0));
}

TEST(SimDrivers, HvScalingSeenThroughLegacyStack) {
  Bench b;
  SimDacPositioner dac("dac", b.beam, {});
  SimHvSupply hv("hv", b.beam);
  SimAdcBank adc("adc", b.beam, {"H1"}, 10.0);
  double c = b.center("H1", "Ar40");
  ASSERT_TRUE(hv.set_hv(2000.0));
  ASSERT_TRUE(dac.set(c));
  ASSERT_TRUE(adc.start());
  b.clock.advance(100ms);
  EXPECT_LT(std::abs(*(**adc.next(0ms)).value("H1")), 20.0);

  ASSERT_TRUE(dac.set(c * std::sqrt(2000.0 / 4500.0)));
  b.clock.advance(100ms);
  EXPECT_NEAR(*(**adc.next(0ms)).value("H1"), 1e6, 2e4);
}

// --- FramePacer: the clock's time is the only time -----------------------------

// A timeout is the clock's alone: real time going by does not end the wait.
// (The 200 ms only give a wait that ends on real time the chance to show.)
TEST(FramePacer, ATimeoutWaitsForTheClockNotForRealTime) {
  ManualClock clock;
  sim::detail::FramePacer pacer(clock);
  pacer.start(1h);
  auto waited = std::async(std::launch::async, [&] {
    std::uint64_t seq = 0;
    return pacer.wait(50ms, seq);
  });
  EXPECT_EQ(waited.wait_for(200ms), std::future_status::timeout) << "the wait ended with the clock at its start";
  clock.advance(50ms);
  ASSERT_EQ(waited.wait_for(10s), std::future_status::ready);
  EXPECT_FALSE(waited.get().has_value());
}

struct FramePacerVirtual : pychron::testing::VirtualTimeTest {};

// Ten minutes of one-second frames, each stamped on its second.
TEST_F(FramePacerVirtual, FramesArriveOnThePeriodWithNoRealDelay) {
  VirtualClock clock;
  Clock::Participant main(clock, "test");
  const TimePoint kStart = clock.now();
  const auto real_start = std::chrono::steady_clock::now();
  sim::detail::FramePacer pacer(clock);
  pacer.start(1s);
  for (std::uint64_t k = 1; k <= 600; ++k) {
    std::uint64_t seq = 0;
    auto ts = pacer.wait(2s, seq);
    ASSERT_TRUE(ts.has_value()) << k;
    ASSERT_EQ(seq, k);
    ASSERT_EQ(*ts, kStart + k * 1s);
  }
  EXPECT_EQ(clock.now(), kStart + 600s);
  EXPECT_LT(std::chrono::steady_clock::now() - real_start, 5s);
}

// At half speed 100 ms of the clock's time cost 200 ms: the wait runs to the
// clock's deadline, however long that takes.
TEST_F(FramePacerVirtual, TimeoutIsClockTimeOnly) {
  VirtualClock::Options options;
  options.speed = 0.5;
  VirtualClock clock(options);
  Clock::Participant main(clock, "test");
  const TimePoint kStart = clock.now();
  sim::detail::FramePacer pacer(clock);
  pacer.start(1h);  // running, with no frame due inside the timeout
  const auto real_start = std::chrono::steady_clock::now();
  std::uint64_t seq = 0;
  auto ts = pacer.wait(100ms, seq);
  const auto real = std::chrono::steady_clock::now() - real_start;
  EXPECT_FALSE(ts.has_value());
  EXPECT_EQ(clock.now(), kStart + 100ms);
  EXPECT_GE(real, 190ms);
  EXPECT_LT(real, 5s);
}

// stop() reaches a waiter asleep in the clock, at the time of the stop and
// not at the waiter's own deadline.
TEST_F(FramePacerVirtual, StopWakesAWaiter) {
  VirtualClock clock;
  Clock::Participant main(clock, "test");
  const TimePoint kStart = clock.now();
  sim::detail::FramePacer pacer(clock);
  pacer.start(1h);
  std::optional<TimePoint> frame;
  TimePoint returned{};
  pychron::testing::Crew crew(clock);
  crew.start("waiter", [&] {
    std::uint64_t seq = 0;
    frame = pacer.wait(10s, seq);
    returned = clock.now();
  });
  // The crew's thread takes part in the clock from its start, so time stands
  // until it is asleep in its wait: the sleep below ends with it waiting.
  clock.sleep_for(1s);
  pacer.stop();
  crew.join();
  EXPECT_FALSE(frame.has_value());
  EXPECT_EQ(returned, kStart + 1s);
}

// start() moves the time the next frame is due: a waiter asleep until the
// old time hears of it, and its frame comes on the new period.
TEST_F(FramePacerVirtual, AStartWakesAWaiter) {
  VirtualClock clock;
  Clock::Participant main(clock, "test");
  const TimePoint kStart = clock.now();
  sim::detail::FramePacer pacer(clock);
  pacer.start(1h);
  std::optional<TimePoint> frame;
  pychron::testing::Crew crew(clock);
  crew.start("waiter", [&] {
    std::uint64_t seq = 0;
    frame = pacer.wait(10s, seq);
  });
  clock.sleep_for(1s);  // ends with the crew's thread asleep in its wait
  pacer.start(1s);
  crew.join();
  ASSERT_TRUE(frame.has_value());
  EXPECT_EQ(*frame, kStart + 2s);
}

}  // namespace
