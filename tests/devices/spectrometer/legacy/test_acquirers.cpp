#include <gtest/gtest.h>

#include <atomic>
#include <future>

#include "pychron/codecs/modbus_adc.hpp"
#include "pychron/core/virtual_clock.hpp"
#include "pychron/devices/driver_registry.hpp"
#include "pychron/devices/spectrometer/legacy/adc_bank.hpp"
#include "pychron/devices/spectrometer/legacy/pulse_counter.hpp"
#include "spectrometer/legacy/sim_util.hpp"
#include "virtual_time.hpp"

using namespace pychron;
using namespace pychron::spectrometer;
using namespace legacy_test;
using namespace std::chrono_literals;

namespace mb = pychron::codec::modbus_adc;

// --- AdcBank ----------------------------------------------------------------

TEST(AdcBank, ReadsOneFloatPerChannelAndScales) {
  std::vector<std::uint16_t> regs;
  for (float v : {1.5F, -0.25F}) {
    auto pair = mb::to_registers(v);
    regs.insert(regs.end(), pair.begin(), pair.end());
  }
  auto sim = open_scripted({raw(mb::read_channels(1, 7, 10, 2)->tx, mb::encode_registers(1, 7, regs))});
  ManualClock clock;
  AdcBank adc("faradays", *sim, {"H1", "AX"}, {100.0, 7, 10, 2.0}, &clock);
  ASSERT_TRUE(adc.start());
  auto f = adc.next(1s);
  ASSERT_TRUE(f) << to_string(f.error());
  ASSERT_TRUE(*f);
  EXPECT_FALSE((*f)->integrated);
  EXPECT_EQ((*f)->seq, 1U);
  EXPECT_EQ((*f)->span, 10ms);
  ASSERT_EQ((*f)->values.size(), 2U);
  EXPECT_EQ((*f)->values[0], (std::pair<ChannelId, double>{"H1", 3.0}));
  EXPECT_EQ((*f)->values[1], (std::pair<ChannelId, double>{"AX", -0.5}));
  expect_verified(*sim);
}

TEST(AdcBank, WaitsForNextSamplePeriodOnClock) {
  ManualClock clock;
  auto sim = open_hooked(adc_sim_hook({1, 0, [](std::size_t) { return 0.1; }}));
  AdcBank adc("faradays", *sim, {"H1"}, {100.0, 1, 0, 1.0}, &clock);
  ASSERT_TRUE(adc.start());
  ASSERT_TRUE(*adc.next(1s));
  auto early = adc.next(0ms);  // clock not advanced: next sample not due
  ASSERT_TRUE(early);
  EXPECT_FALSE(*early);
  clock.advance(10ms);
  auto f = adc.next(1s);
  ASSERT_TRUE(f && *f);
  EXPECT_EQ((*f)->seq, 2U);
  EXPECT_EQ((*f)->ts, TimePoint{} + 10ms);
}

// A timeout is the clock's alone: real time going by does not end the wait.
// (The 200 ms only give a wait that ends on real time the chance to show.)
TEST(AdcBank, ATimeoutWaitsForTheClockNotForRealTime) {
  ManualClock clock;
  auto sim = open_hooked(adc_sim_hook({1, 0, [](std::size_t) { return 0.1; }}));
  AdcBank adc("faradays", *sim, {"H1"}, {100.0, 1, 0, 1.0}, &clock);
  ASSERT_TRUE(adc.start());
  ASSERT_TRUE(*adc.next(1s));
  auto pending = std::async(std::launch::async, [&] { return adc.next(5ms); });
  EXPECT_EQ(pending.wait_for(200ms), std::future_status::timeout) << "the wait ended with the clock at its start";
  clock.advance(5ms);
  if (pending.wait_for(10s) != std::future_status::ready) {
    ADD_FAILURE() << "next(5ms) did not return";
    (void)adc.stop();  // wakes it
  }
  auto early = pending.get();
  ASSERT_TRUE(early);
  EXPECT_FALSE(*early);
}

namespace {

struct AdcBankVirtual : pychron::testing::VirtualTimeTest {
  VirtualClock clock;
  Clock::Participant main{clock, "test"};
  std::unique_ptr<SimTransport> sim = open_hooked(adc_sim_hook({1, 0, [](std::size_t) { return 0.1; }}));
  AdcBank adc{"faradays", *sim, {"H1"}, {100.0, 1, 0, 1.0}, &clock};  // a sample every 10 ms
};

}  // namespace

TEST_F(AdcBankVirtual, SamplesArriveOnThePeriodAndATimeoutEndsOnTheClock) {
  const TimePoint began = clock.now();
  ASSERT_TRUE(adc.start());
  ASSERT_TRUE(*adc.next(1s));  // due at once
  auto early = adc.next(5ms);
  ASSERT_TRUE(early);
  EXPECT_FALSE(*early);
  EXPECT_EQ(clock.now(), began + 5ms);
  auto f = adc.next(1s);
  ASSERT_TRUE(f && *f);
  EXPECT_EQ((*f)->seq, 2U);
  EXPECT_EQ((*f)->ts, began + 10ms);
}

// stop() reaches a next() asleep in the clock at the time of the stop, not
// at the time its sample would have been due.
TEST_F(AdcBankVirtual, StopWakesANextWaitingForItsSample) {
  const TimePoint began = clock.now();
  ASSERT_TRUE(adc.start());
  ASSERT_TRUE(*adc.next(1s));
  Result<std::optional<Frame>> got = std::optional<Frame>{Frame{}};
  TimePoint returned{};
  pychron::testing::Crew crew(clock);
  crew.start("next", [&] {
    got = adc.next(1s);  // the sample is due in 10 ms
    returned = clock.now();
  });
  // The crew's thread takes part in the clock from its start, so time stands
  // until it is asleep in its wait: the sleep below ends with it waiting.
  clock.sleep_for(4ms);
  ASSERT_TRUE(adc.stop());
  crew.join();
  ASSERT_TRUE(got) << to_string(got.error());
  EXPECT_FALSE(got->has_value());
  EXPECT_EQ(returned, began + 4ms);
}

// On hardware the clock is a SteadyClock: the period and the timeout are real.
TEST(AdcBankSteady, ATimeoutAndThePeriodAreRealTime) {
  SteadyClock clock;
  auto sim = open_hooked(adc_sim_hook({1, 0, [](std::size_t) { return 0.1; }}));
  AdcBank adc("faradays", *sim, {"H1"}, {1.0, 1, 0, 1.0}, &clock);  // a sample a second
  const auto began = std::chrono::steady_clock::now();
  ASSERT_TRUE(adc.start());
  ASSERT_TRUE(*adc.next(1s));  // due at once
  const auto asked = std::chrono::steady_clock::now();
  auto early = adc.next(20ms);
  ASSERT_TRUE(early);
  EXPECT_FALSE(*early);  // the timeout came first, not the sample
  EXPECT_GE(std::chrono::steady_clock::now() - asked, 20ms);
  auto f = adc.next(5s);
  ASSERT_TRUE(f && *f);
  EXPECT_EQ((*f)->seq, 2U);
  EXPECT_GE(std::chrono::steady_clock::now() - began, 1s);
}

TEST(AdcBank, ExceptionReplyIsProtocolErrorAndConsumesSeq) {
  ManualClock clock;
  auto sim = open_hooked(adc_sim_hook({1, 0, [](std::size_t i) -> std::optional<double> {
                                         if (i == 0) return 1.0;
                                         return std::nullopt;
                                       }}));
  AdcBank adc("faradays", *sim, {"H1", "AX"}, {100.0, 1, 0, 1.0}, &clock);
  ASSERT_TRUE(adc.start());
  auto f = adc.next(1s);
  ASSERT_FALSE(f);
  EXPECT_EQ(f.error().kind, ErrorKind::Protocol);
  EXPECT_EQ(f.error().device, "faradays");
}

TEST(AdcBank, NextBeforeStartIsConfig) {
  auto sim = open_scripted({});
  AdcBank adc("faradays", *sim, {"H1"}, {});
  auto f = adc.next(1ms);
  ASSERT_FALSE(f);
  EXPECT_EQ(f.error().kind, ErrorKind::Config);
}

TEST(AdcBank, ConfigureRecordsHostIntegration) {
  auto sim = open_scripted({});
  AdcBank adc("faradays", *sim, {"H1"}, {});
  EXPECT_FALSE(adc.integrates());
  ASSERT_TRUE(adc.configure(2s));
  EXPECT_EQ(adc.integration(), 2s);
  EXPECT_EQ(adc.sample_period(), 10ms);
}

TEST(AdcBank, CreateFromConfig) {
  auto sim = open_scripted({});
  auto dev = DriverRegistry::global().create(
      "adc_bank", *sim, table_of("roles = [\"acquirer\"]\nsample_hz = 50\nchannels = [\"AX\", \"H1\", \"L1\"]"),
      DriverContext{"faradays"});
  ASSERT_TRUE(dev) << to_string(dev.error());
  auto* acq = dynamic_cast<IIntensityAcquirer*>(dev->get());
  ASSERT_NE(acq, nullptr);
  EXPECT_EQ(acq->channels(), (std::vector<ChannelId>{"AX", "H1", "L1"}));

  for (const char* bad : {"channels = []", "channels = [\"A\", \"A\"]", "channels = [\"A\"]\nsample_hz = 0",
                          "channels = [\"A\"]\nunit = 300", "channels = [\"A\"]\nstart_register = 65535",
                          "channels = [\"A\"]\nscale = 0.0", ""}) {
    const auto table = table_of(bad);
    auto r = AdcBank::create(DriverArgs{"faradays", *sim, table});
    ASSERT_FALSE(r) << bad;
    EXPECT_EQ(r.error().kind, ErrorKind::Config) << bad;
  }
}

// --- PulseCounter -----------------------------------------------------------

TEST(PulseCounter, StartDiscardsAccumulatedCountsThenFramesCarryCounts) {
  auto sim = open_scripted({step("R\r", "999,999\r"), step("R\r", "12,3\r")});
  ManualClock clock;
  PulseCounter pc("multiplier", *sim, {"EM", "CDD"}, 10.0, &clock);
  ASSERT_TRUE(pc.start());
  auto f = pc.next(1s);
  ASSERT_TRUE(f) << to_string(f.error());
  ASSERT_TRUE(*f);
  EXPECT_FALSE((*f)->integrated);
  EXPECT_EQ((*f)->span, 100ms);
  EXPECT_EQ((*f)->values, (std::vector<std::pair<ChannelId, double>>{{"EM", 12.0}, {"CDD", 3.0}}));
  expect_verified(*sim);
}

TEST(PulseCounter, CounterErrorFailsStart) {
  auto sim = open_scripted({step("R\r", "E OVERFLOW\r")});
  PulseCounter pc("multiplier", *sim, {"EM"});
  auto r = pc.start();
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Protocol);
  EXPECT_FALSE(pc.next(1ms));
}

TEST(PulseCounter, WrongChannelCountIsProtocolError) {
  ManualClock clock;
  auto sim = open_hooked(counter_sim_hook({1, [](std::size_t) { return 5; }}));
  PulseCounter pc("multiplier", *sim, {"EM", "CDD"}, 10.0, &clock);
  auto r = pc.start();
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Protocol);
}

TEST(PulseCounter, CreateFromConfig) {
  auto sim = open_scripted({});
  auto dev = DriverRegistry::global().create("pulse_counter", *sim, table_of("channels = [\"EM\"]"),
                                             DriverContext{"multiplier"});
  ASSERT_TRUE(dev) << to_string(dev.error());
  auto* pc = dynamic_cast<PulseCounter*>(dev->get());
  ASSERT_NE(pc, nullptr);
  EXPECT_EQ(pc->sample_period(), 100ms);
  const auto bad = table_of("channels = [\"EM\"]\nsample_hz = -1");
  EXPECT_FALSE(PulseCounter::create(DriverArgs{"multiplier", *sim, bad}));
}
