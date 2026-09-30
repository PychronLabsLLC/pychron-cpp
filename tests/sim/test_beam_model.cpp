#include "pychron/sim/spectrometer/beam_model.hpp"

#include <chrono>
#include <cmath>

#include <gtest/gtest.h>

namespace {

using namespace pychron;
using namespace pychron::sim;
using namespace std::chrono_literals;
using spectrometer::ParamId;
using spectrometer::SourceParam;

struct Fixture {
  ManualClock clock;
  BeamModel beam{clock};

  Fixture() {
    beam.ensure_detector("H1");
    beam.ensure_detector("AX");
    beam.ensure_detector("EM");
  }
  double center(const char* det, const char* iso) { return *beam.peak_center(det, iso); }
  double read(const char* det) { return beam.intensity(det)->value; }
};

TEST(BeamModel, PeakTopReadsAbundanceAndOffPeakReadsBaseline) {
  Fixture f;
  f.beam.set_magnet(f.center("H1", "Ar40"));
  EXPECT_NEAR(f.read("H1"), 1e6, 1e4);

  f.beam.set_magnet(f.center("H1", "Ar40") + 0.2);
  EXPECT_LT(std::abs(f.read("H1")), 20.0);
}

TEST(BeamModel, BaselineAtMass34Point2IsZeroPlusNoise) {
  Fixture f;
  // table value of mass 34.2 with the default table
  f.beam.set_magnet(34.2 / 5.0);
  for (int i = 0; i < 20; ++i) EXPECT_LT(std::abs(f.read("H1")), 10.0);
}

TEST(BeamModel, PeakIsFlatTopped) {
  Fixture f;
  double c = f.center("H1", "Ar40");
  f.beam.set_magnet(c);
  double top = f.read("H1");
  f.beam.set_magnet(c + 0.015);
  EXPECT_NEAR(f.read("H1"), top, 1e4);
  f.beam.set_magnet(c + 0.025);  // on the edge ramp
  double edge = f.read("H1");
  EXPECT_LT(edge, 0.9 * top);
  EXPECT_GT(edge, 0.1 * top);
}

TEST(BeamModel, HvScalesPeakCenterBySqrt) {
  Fixture f;
  double c0 = f.center("H1", "Ar40");
  f.beam.set_hv(4.0 * f.beam.nominal_hv());
  EXPECT_NEAR(f.center("H1", "Ar40"), 2.0 * c0, 1e-9);

  f.beam.set_magnet(c0);  // old position no longer on the peak
  EXPECT_LT(std::abs(f.read("H1")), 20.0);
}

TEST(BeamModel, DeflectionShiftsPeakBySignedPolynomial) {
  Fixture f;
  double c0 = f.center("H1", "Ar40");
  ASSERT_TRUE(f.beam.set_deflection("H1", 100.0));
  EXPECT_NEAR(f.center("H1", "Ar40"), c0 + 0.0012 * 100.0, 1e-9);

  f.beam.set_magnet(c0);
  EXPECT_LT(std::abs(f.read("H1")), 20.0);
  f.beam.set_magnet(c0 + 0.12);
  EXPECT_NEAR(f.read("H1"), 1e6, 1e4);
}

TEST(BeamModel, TrapCurrentScalesSensitivity) {
  Fixture f;
  f.beam.set_magnet(f.center("H1", "Ar40"));
  f.beam.set_source_param(ParamId{SourceParam::TrapCurrent}, 50.0);
  EXPECT_NEAR(f.read("H1"), 0.5e6, 1e4);
}

TEST(BeamModel, SymmetryShiftsPeak) {
  Fixture f;
  double c0 = f.center("H1", "Ar40");
  f.beam.set_source_param(ParamId{SourceParam::YSymmetry}, 50.0);
  EXPECT_NEAR(f.center("H1", "Ar40"), c0 + 0.05, 1e-9);
}

TEST(BeamModel, FocusHasAnOptimum) {
  Fixture f;
  f.beam.set_magnet(f.center("H1", "Ar40"));
  double best = f.read("H1");
  f.beam.set_source_param(ParamId{SourceParam::ExtractionFocus}, 40.0);
  EXPECT_LT(f.read("H1"), 0.2 * best);
}

TEST(BeamModel, SaturationClampsAndFlags) {
  Fixture f;
  f.beam.set_gas({{"Ar40", 39.96238, 1e7, 0.0}});
  f.beam.set_magnet(f.center("H1", "Ar40"));
  auto r = f.beam.intensity("H1");
  ASSERT_TRUE(r);
  EXPECT_TRUE(r->saturated);
  EXPECT_DOUBLE_EQ(r->value, 4.9e6);

  f.beam.set_gas({{"Ar40", 39.96238, 1e6, 0.0}});
  EXPECT_FALSE(f.beam.intensity("H1")->saturated);
}

TEST(BeamModel, GainScalesSignal) {
  Fixture f;
  f.beam.set_magnet(f.center("H1", "Ar40"));
  ASSERT_TRUE(f.beam.set_gain("H1", 2.0));
  EXPECT_NEAR(f.read("H1"), 2e6, 2e4);
}

TEST(BeamModel, GasDecaysExponentially) {
  Fixture f;
  f.beam.set_gas({{"Ar40", 39.96238, 1e6, -0.1}});
  f.beam.set_magnet(f.center("H1", "Ar40"));
  f.clock.advance(10s);
  EXPECT_NEAR(f.read("H1"), 1e6 * std::exp(-1.0), 1e4);
}

TEST(BeamModel, BlankRemovesBeam) {
  Fixture f;
  f.beam.set_magnet(f.center("H1", "Ar40"));
  f.beam.blank(true);
  EXPECT_LT(std::abs(f.read("H1")), 20.0);
  f.beam.blank(false);
  EXPECT_GT(f.read("H1"), 9e5);
}

TEST(BeamModel, UnprotectedCounterSeeingLargeBeamOverloads) {
  Fixture f;
  f.beam.set_magnet(f.center("EM", "Ar40"));
  auto r = f.beam.intensity("EM");
  ASSERT_TRUE(r);
  EXPECT_TRUE(r->overloaded);
  EXPECT_TRUE(f.beam.overloaded("EM"));
  // dead time keeps the measured rate below the true rate
  EXPECT_LT(r->value, 1e6);

  ASSERT_TRUE(f.beam.clear_overload("EM"));
  ASSERT_TRUE(f.beam.protect("EM", true));
  auto safe = f.beam.intensity("EM");
  EXPECT_FALSE(safe->overloaded);
  EXPECT_FALSE(f.beam.overloaded("EM"));
  EXPECT_EQ(safe->value, 0.0);
}

TEST(BeamModel, SmallBeamOnCounterIsPoissonCps) {
  Fixture f;
  f.beam.set_magnet(f.center("EM", "Ar36"));
  auto r = f.beam.intensity("EM");
  EXPECT_FALSE(r->overloaded);
  EXPECT_NEAR(r->value, 3e3 * 0.985, 400.0);
}

TEST(BeamModel, CddVoltageBelowPlateauKillsGain) {
  Fixture f;
  f.beam.set_magnet(f.center("EM", "Ar36"));
  ASSERT_TRUE(f.beam.set_cdd_voltage("EM", 900.0));
  EXPECT_LT(f.read("EM"), 200.0);
}

TEST(BeamModel, SameSeedSameNoise) {
  ManualClock c1, c2;
  BeamSettings s;
  s.seed = 42;
  BeamModel a(c1, s), b(c2, s);
  a.ensure_detector("H1");
  b.ensure_detector("H1");
  for (int i = 0; i < 5; ++i) EXPECT_EQ(a.intensity("H1")->value, b.intensity("H1")->value);
}

TEST(BeamModel, UnknownDetectorIsConfigError) {
  Fixture f;
  auto r = f.beam.intensity("nope");
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Config);
  EXPECT_FALSE(f.beam.set_deflection("nope", 1.0));
}

}  // namespace
