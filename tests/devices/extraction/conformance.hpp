#pragma once

// Extraction-device conformance suites (spec section 9). A new device proves
// its contract by instantiating the suites for the features it exposes with a
// harness type that builds it on scripted/simulated transport:
//
//   struct MyLaserHarness {
//     MyLaserHarness();                                  // builds the device
//     pychron::extraction::IExtractionDevice& device();
//     // optional: void advance();                        // let simulated time pass
//   };
//   INSTANTIATE_TYPED_TEST_SUITE_P(MyLaser, ExtractionDeviceConformance,
//                                  ::testing::Types<MyLaserHarness>);
//   INSTANTIATE_TYPED_TEST_SUITE_P(MyLaser, LaserConformance,
//                                  ::testing::Types<MyLaserHarness>);
//
// Suites: ExtractionDeviceConformance (every device), LaserConformance,
// StageConformance, PatternConformance, FurnaceConformance. A feature suite
// requires the device to expose that feature.

#include <gtest/gtest.h>

#include <cmath>
#include <optional>
#include <stdexcept>
#include <string>

#include "pychron/devices/extraction/interfaces.hpp"

namespace pychron::extraction::conformance {

template <class H>
void advance(H& h) {
  if constexpr (requires { h.advance(); }) h.advance();
}

constexpr int kPollLimit = 1000;

// The value of `r`, or nullopt on error, so results compare with EXPECT_EQ.
template <class T>
std::optional<T> val(const Result<T>& r) {
  if (!r) return std::nullopt;
  return *r;
}

// Polls `busy` (a Result<bool> producer) with advance() until false.
template <class H, class F>
::testing::AssertionResult settles(H& h, F busy) {
  for (int i = 0; i < kPollLimit; ++i) {
    auto b = busy();
    if (!b) return ::testing::AssertionFailure() << to_string(b.error());
    if (!*b) return ::testing::AssertionSuccess();
    advance(h);
  }
  return ::testing::AssertionFailure() << "still busy after " << kPollLimit << " polls";
}

// A unit the device supports; every device supports at least one.
inline ExtractUnits supported_units(const IExtractionDevice& d) {
  for (auto u : {ExtractUnits::Percent, ExtractUnits::Watts, ExtractUnits::Celsius})
    if (d.supports(u)) return u;
  ADD_FAILURE() << "device supports no extract units";
  return ExtractUnits::Percent;
}

}  // namespace pychron::extraction::conformance

// --- IExtractionDevice ------------------------------------------------------

template <class H>
class ExtractionDeviceConformance : public ::testing::Test {
 protected:
  H harness;
  pychron::extraction::IExtractionDevice& device() { return harness.device(); }
};
TYPED_TEST_SUITE_P(ExtractionDeviceConformance);

TYPED_TEST_P(ExtractionDeviceConformance, HasANameAndSupportsSomeUnits) {
  using namespace pychron::extraction;
  EXPECT_FALSE(this->device().device_name().empty());
  conformance::supported_units(this->device());
}

TYPED_TEST_P(ExtractionDeviceConformance, PrepareSucceeds) {
  EXPECT_TRUE(this->device().prepare());
}

TYPED_TEST_P(ExtractionDeviceConformance, EnableAndDisableAreReflected) {
  auto& d = this->device();
  ASSERT_TRUE(d.enable());
  EXPECT_EQ(pychron::extraction::conformance::val(d.is_enabled()), true);
  ASSERT_TRUE(d.disable());
  EXPECT_EQ(pychron::extraction::conformance::val(d.is_enabled()), false);
}

TYPED_TEST_P(ExtractionDeviceConformance, ExtractSetsOutput) {
  using namespace pychron::extraction;
  auto& d = this->device();
  auto units = conformance::supported_units(d);
  ASSERT_TRUE(d.enable());
  ASSERT_TRUE(d.extract(5.0, units));
  auto out = d.output();
  ASSERT_TRUE(out);
  EXPECT_DOUBLE_EQ(*out, 5.0);
  EXPECT_TRUE(d.end_extract());
}

TYPED_TEST_P(ExtractionDeviceConformance, ExtractWhileDisabledIsInterlockAndChangesNothing) {
  using namespace pychron::extraction;
  auto& d = this->device();
  auto units = conformance::supported_units(d);
  ASSERT_TRUE(d.disable());
  auto r = d.extract(5.0, units);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, pychron::ErrorKind::Interlock);
  EXPECT_EQ(pychron::extraction::conformance::val(d.output()), 0.0);
}

TYPED_TEST_P(ExtractionDeviceConformance, NegativeOutputIsConfig) {
  using namespace pychron::extraction;
  auto& d = this->device();
  ASSERT_TRUE(d.enable());
  auto r = d.extract(-1.0, conformance::supported_units(d));
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, pychron::ErrorKind::Config);
  EXPECT_EQ(pychron::extraction::conformance::val(d.output()), 0.0);
  EXPECT_TRUE(d.disable());
}

TYPED_TEST_P(ExtractionDeviceConformance, UnsupportedUnitsAreNotSupported) {
  using namespace pychron::extraction;
  auto& d = this->device();
  ASSERT_TRUE(d.enable());
  for (auto u : {ExtractUnits::Percent, ExtractUnits::Watts, ExtractUnits::Celsius}) {
    if (d.supports(u)) continue;
    auto r = d.extract(1.0, u);
    ASSERT_FALSE(r) << to_string(u);
    EXPECT_TRUE(is_not_supported(r.error())) << to_string(r.error());
  }
  EXPECT_EQ(pychron::extraction::conformance::val(d.output()), 0.0);
  EXPECT_TRUE(d.disable());
}

TYPED_TEST_P(ExtractionDeviceConformance, EndExtractZeroesOutputAndIsIdempotent) {
  using namespace pychron::extraction;
  auto& d = this->device();
  ASSERT_TRUE(d.enable());
  ASSERT_TRUE(d.extract(3.0, conformance::supported_units(d)));
  ASSERT_TRUE(d.end_extract());
  EXPECT_EQ(pychron::extraction::conformance::val(d.output()), 0.0);
  EXPECT_TRUE(d.end_extract());
  EXPECT_EQ(pychron::extraction::conformance::val(d.output()), 0.0);
  EXPECT_TRUE(d.disable());
}

TYPED_TEST_P(ExtractionDeviceConformance, DisableEndsExtraction) {
  using namespace pychron::extraction;
  auto& d = this->device();
  ASSERT_TRUE(d.enable());
  ASSERT_TRUE(d.extract(3.0, conformance::supported_units(d)));
  ASSERT_TRUE(d.disable());
  EXPECT_EQ(pychron::extraction::conformance::val(d.output()), 0.0);
}

TYPED_TEST_P(ExtractionDeviceConformance, FeaturesAreStable) {
  using namespace pychron::extraction;
  auto& d = this->device();
  auto first = capabilities(d);
  EXPECT_EQ(capabilities(d), first);
  EXPECT_EQ(d.laser(), d.laser());
  EXPECT_EQ(d.stage(), d.stage());
  EXPECT_EQ(d.furnace(), d.furnace());
  EXPECT_FALSE(first.has(Capability::Valves)) << "line services are not device features";
  EXPECT_FALSE(first.has(Capability::Pressure)) << "line services are not device features";
}

REGISTER_TYPED_TEST_SUITE_P(ExtractionDeviceConformance, HasANameAndSupportsSomeUnits,
                            PrepareSucceeds, EnableAndDisableAreReflected, ExtractSetsOutput,
                            ExtractWhileDisabledIsInterlockAndChangesNothing,
                            NegativeOutputIsConfig, UnsupportedUnitsAreNotSupported,
                            EndExtractZeroesOutputAndIsIdempotent, DisableEndsExtraction,
                            FeaturesAreStable);

// --- ILaserDevice -----------------------------------------------------------

template <class H>
class LaserConformance : public ::testing::Test {
 protected:
  H harness;
  pychron::extraction::IExtractionDevice& device() { return harness.device(); }
  pychron::extraction::ILaserDevice& laser() {
    auto* l = harness.device().laser();
    if (!l) throw std::logic_error("LaserConformance needs a device with laser()");
    return *l;
  }
};
TYPED_TEST_SUITE_P(LaserConformance);

TYPED_TEST_P(LaserConformance, FireWhileDisabledIsInterlock) {
  ASSERT_TRUE(this->device().disable());
  auto r = this->laser().fire_laser();
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, pychron::ErrorKind::Interlock);
  EXPECT_EQ(pychron::extraction::conformance::val(this->laser().is_firing()), false);
}

TYPED_TEST_P(LaserConformance, FireAndStopAreReflected) {
  ASSERT_TRUE(this->device().enable());
  ASSERT_TRUE(this->laser().fire_laser());
  EXPECT_EQ(pychron::extraction::conformance::val(this->laser().is_firing()), true);
  ASSERT_TRUE(this->laser().stop_laser());
  EXPECT_EQ(pychron::extraction::conformance::val(this->laser().is_firing()), false);
  EXPECT_TRUE(this->device().disable());
}

TYPED_TEST_P(LaserConformance, EndExtractAndDisableStopFiring) {
  ASSERT_TRUE(this->device().enable());
  ASSERT_TRUE(this->laser().fire_laser());
  ASSERT_TRUE(this->device().end_extract());
  EXPECT_EQ(pychron::extraction::conformance::val(this->laser().is_firing()), false);
  ASSERT_TRUE(this->laser().fire_laser());
  ASSERT_TRUE(this->device().disable());
  EXPECT_EQ(pychron::extraction::conformance::val(this->laser().is_firing()), false);
}

TYPED_TEST_P(LaserConformance, WarmupSucceeds) { EXPECT_TRUE(this->laser().warmup()); }

REGISTER_TYPED_TEST_SUITE_P(LaserConformance, FireWhileDisabledIsInterlock,
                            FireAndStopAreReflected, EndExtractAndDisableStopFiring,
                            WarmupSucceeds);

// --- IStage -----------------------------------------------------------------

template <class H>
class StageConformance : public ::testing::Test {
 protected:
  H harness;
  pychron::extraction::IStage& stage() {
    auto* s = harness.device().stage();
    if (!s) throw std::logic_error("StageConformance needs a device with stage()");
    return *s;
  }
};
TYPED_TEST_SUITE_P(StageConformance);

TYPED_TEST_P(StageConformance, HasPositions) { EXPECT_FALSE(this->stage().positions().empty()); }

TYPED_TEST_P(StageConformance, UnknownPositionIsConfig) {
  auto r = this->stage().move_to_position("no-such-hole-xyz", false);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, pychron::ErrorKind::Config);
}

TYPED_TEST_P(StageConformance, MoveToEveryPositionSettles) {
  using namespace pychron::extraction;
  auto& s = this->stage();
  for (const auto& p : s.positions()) {
    ASSERT_TRUE(s.move_to_position(p, false)) << p;
    EXPECT_TRUE(conformance::settles(this->harness, [&] { return s.moving(); })) << p;
  }
}

TYPED_TEST_P(StageConformance, AxisMovesAreReadBack) {
  using namespace pychron::extraction;
  auto& s = this->stage();
  ASSERT_TRUE(s.set_xy(1.5, -2.0));
  ASSERT_TRUE(conformance::settles(this->harness, [&] { return s.moving(); }));
  ASSERT_TRUE(s.set_axis(IStage::Axis::Z, 0.5));
  ASSERT_TRUE(conformance::settles(this->harness, [&] { return s.moving(); }));
  auto at = s.position();
  ASSERT_TRUE(at);
  EXPECT_NEAR(at->x, 1.5, 1e-6);
  EXPECT_NEAR(at->y, -2.0, 1e-6);
  EXPECT_NEAR(at->z, 0.5, 1e-6);
}

REGISTER_TYPED_TEST_SUITE_P(StageConformance, HasPositions, UnknownPositionIsConfig,
                            MoveToEveryPositionSettles, AxisMovesAreReadBack);

// --- IPatternRunner ---------------------------------------------------------

template <class H>
class PatternConformance : public ::testing::Test {
 protected:
  H harness;
  pychron::extraction::IPatternRunner& runner() {
    auto* r = harness.device().pattern_runner();
    if (!r) throw std::logic_error("PatternConformance needs a device with pattern_runner()");
    return *r;
  }
};
TYPED_TEST_SUITE_P(PatternConformance);

TYPED_TEST_P(PatternConformance, UnknownPatternIsConfig) {
  auto r = this->runner().execute_pattern("no-such-pattern-xyz");
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, pychron::ErrorKind::Config);
  EXPECT_EQ(pychron::extraction::conformance::val(this->runner().running()), false);
}

TYPED_TEST_P(PatternConformance, StopEndsARunningPattern) {
  auto& r = this->runner();
  ASSERT_FALSE(r.patterns().empty());
  ASSERT_TRUE(r.execute_pattern(r.patterns().front()));
  ASSERT_TRUE(r.stop_pattern());
  EXPECT_EQ(pychron::extraction::conformance::val(r.running()), false);
}

REGISTER_TYPED_TEST_SUITE_P(PatternConformance, UnknownPatternIsConfig,
                            StopEndsARunningPattern);

// --- IFurnaceDevice ---------------------------------------------------------

template <class H>
class FurnaceConformance : public ::testing::Test {
 protected:
  H harness;
  pychron::extraction::IFurnaceDevice& furnace() {
    auto* f = harness.device().furnace();
    if (!f) throw std::logic_error("FurnaceConformance needs a device with furnace()");
    return *f;
  }
};
TYPED_TEST_SUITE_P(FurnaceConformance);

TYPED_TEST_P(FurnaceConformance, TemperatureIsFinite) {
  auto t = this->furnace().read_temperature();
  ASSERT_TRUE(t);
  EXPECT_TRUE(std::isfinite(*t));
}

TYPED_TEST_P(FurnaceConformance, PidAndDumpSucceed) {
  EXPECT_TRUE(this->furnace().set_pid_parameters(500.0));
  EXPECT_TRUE(this->furnace().dump_sample());
}

REGISTER_TYPED_TEST_SUITE_P(FurnaceConformance, TemperatureIsFinite, PidAndDumpSucceed);
