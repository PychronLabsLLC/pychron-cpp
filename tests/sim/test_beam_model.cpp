#include "pychron/sim/spectrometer/beam_model.hpp"

#include <chrono>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

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
  // A reading is keyed by its instant: move time to get another draw.
  for (int i = 0; i < 20; ++i) {
    f.clock.advance(1s);
    EXPECT_LT(std::abs(f.read("H1")), 10.0);
  }
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

TEST(BeamModel, CounterYieldIsThePlateauAtTheDetectorsVoltage) {
  Fixture f;
  // The defaults: 1450 V on a plateau centred at 1200 V, 60 V wide.
  const auto counter = f.beam.counter_yield("EM");
  ASSERT_TRUE(counter) << counter.error().what;
  EXPECT_NEAR(*counter, 1.0 / (1.0 + std::exp(-250.0 / 60.0)), 1e-15);
  const auto faraday = f.beam.counter_yield("H1");
  ASSERT_TRUE(faraday) << faraday.error().what;
  EXPECT_EQ(*faraday, 1.0);
  ASSERT_TRUE(f.beam.set_cdd_voltage("EM", 1200.0));
  const auto at_centre = f.beam.counter_yield("EM");
  ASSERT_TRUE(at_centre) << at_centre.error().what;
  EXPECT_DOUBLE_EQ(*at_centre, 0.5);
  auto unknown = f.beam.counter_yield("nope");
  ASSERT_FALSE(unknown);
  EXPECT_EQ(unknown.error().kind, ErrorKind::Config);

  // It is what the readings have: half the Ar36 is counted at the centre.
  f.beam.set_magnet(f.center("EM", "Ar36"));
  double sum = 0.0;
  const int n = 1000;
  for (int i = 0; i < n; ++i) {
    f.clock.advance(1ms);
    sum += f.read("EM");
  }
  EXPECT_NEAR(sum / n, 3e3 * 0.5, 5.0 * std::sqrt(3e3 * 0.5 / n));

  // The plateau is the model's own, not the default one.
  ManualClock clock;
  BeamSettings settings;
  settings.cdd_plateau_center = 1400.0;
  settings.cdd_plateau_width = 25.0;
  BeamModel other(clock, settings);
  other.ensure_detector("CDD");
  const auto own = other.counter_yield("CDD");
  ASSERT_TRUE(own) << own.error().what;
  EXPECT_NEAR(*own, 1.0 / (1.0 + std::exp(-2.0)), 1e-15);
}

TEST(BeamModel, SameSeedSameNoise) {
  ManualClock c1, c2;
  BeamSettings s;
  s.seed = 42;
  BeamModel a(c1, s), b(c2, s);
  a.ensure_detector("H1");
  b.ensure_detector("H1");
  // Five readings, not one read five times: the noise is of the instant.
  for (int i = 0; i < 5; ++i) {
    c1.advance(1ms);
    c2.advance(1ms);
    EXPECT_EQ(a.intensity("H1")->value, b.intensity("H1")->value);
  }
}

// A seed set afterwards is the seed from then on: the readings are those of
// a model built with it, not those of the seed before, on a Faraday and on a
// counter alike. The signal under the noise is the same.
TEST(BeamModel, ADifferentSeedGivesDifferentNoise) {
  ManualClock c1, c2, c3;
  BeamSettings s;
  BeamModel kept(c1, s), changed(c2, s);
  s.seed = 43;
  BeamModel built(c3, s);
  for (auto* m : {&kept, &changed, &built}) {
    m->ensure_detector("H1");
    m->ensure_detector("CDD");
    m->set_magnet(*m->peak_center("CDD", "Ar36"));  // 3e3 fA on the counter, off the peak on H1
  }
  changed.set_seed(43);
  int faraday_differs = 0, counter_differs = 0;
  double kept_sum = 0, changed_sum = 0;
  const int n = 200;
  for (int i = 0; i < n; ++i) {
    for (auto* c : {&c1, &c2, &c3}) c->advance(1ms);
    const double h1 = changed.intensity("H1")->value;
    const double cdd = changed.intensity("CDD")->value;
    EXPECT_EQ(h1, built.intensity("H1")->value);
    EXPECT_EQ(cdd, built.intensity("CDD")->value);
    const double kept_cdd = kept.intensity("CDD")->value;
    faraday_differs += h1 != kept.intensity("H1")->value ? 1 : 0;
    counter_differs += cdd != kept_cdd ? 1 : 0;
    kept_sum += kept_cdd;
    changed_sum += cdd;
  }
  EXPECT_EQ(faraday_differs, n);
  EXPECT_GT(counter_differs, n * 9 / 10) << "two counts of about 3000 are seldom the same";
  // About 2954 counts a reading (3e3 less the plateau's 1.5 %): the means of
  // 200 are within five standard errors of each other.
  EXPECT_NEAR(kept_sum / n, changed_sum / n, 5 * std::sqrt(2 * 3000.0 / n));
}

// Mean of `n` readings of `det`, one millisecond apart.
double mean_of(Fixture& f, const char* det, int n) {
  double sum = 0.0;
  for (int i = 0; i < n; ++i) {
    f.clock.advance(1ms);
    sum += f.read(det);
  }
  return sum / n;
}

TEST(BeamModel, WithAProviderTheSignalFollowsIt) {
  Fixture f;
  const TimePoint start = f.clock.now();
  // A rate on the provider's gas is ignored: the provider already answers
  // for the instant it is asked about.
  f.beam.set_gas_provider([start](TimePoint t) {
    double s = std::chrono::duration<double>(t - start).count();
    return std::vector<BeamGas>{{"Ar40", 39.96238, 1000.0 + 10.0 * s, -1.0}};
  });
  f.beam.set_magnet(f.center("H1", "Ar40"));
  for (int s : {0, 10, 100, 1000}) {
    double expected = 1000.0 + 10.0 * s;
    double sigma = 1.0 + 0.001 * expected;  // noise_floor + noise_rel * signal
    auto r = f.beam.intensity("H1", start + std::chrono::seconds(s));
    ASSERT_TRUE(r);
    EXPECT_NEAR(r->value, expected, 5.0 * sigma) << "at " << s << " s";
  }
  // The fixed list is no longer read: nothing at Ar36.
  f.beam.set_magnet(f.center("H1", "Ar36"));
  EXPECT_LT(std::abs(f.read("H1")), 10.0);
}

TEST(BeamModel, WithoutAProviderNothingChanges) {
  Fixture f;
  f.beam.set_gas({{"Ar40", 39.96238, 1e4, 0.05}});
  f.beam.set_magnet(f.center("H1", "Ar40"));
  f.clock.advance(20s);
  EXPECT_NEAR(f.read("H1"), 1e4 * std::exp(1.0), 5.0 * (1.0 + 0.001 * 1e4 * std::exp(1.0)));

  // An empty provider is no provider: the fixed list is back.
  f.beam.set_gas_provider([](TimePoint) { return std::vector<BeamGas>{{"Ar40", 39.96238, 7.0, 0.0}}; });
  EXPECT_LT(f.read("H1"), 100.0);
  f.beam.set_gas_provider({});
  EXPECT_NEAR(f.read("H1"), 1e4 * std::exp(1.0), 5.0 * (1.0 + 0.001 * 1e4 * std::exp(1.0)));
}

TEST(BeamModel, OffPeakReadsTheBaseline) {
  Fixture f;
  ASSERT_TRUE(f.beam.set_baseline("H1", 50.0, 0.0));
  f.beam.set_magnet(34.2 / 5.0);
  // sigma is the noise floor, 1 fA: the standard error of 1000 readings is
  // 1 / sqrt(1000). The draws are fixed by the seed; five of them leave room
  // for a change of how the noise is keyed to draw them again.
  const double five_se = 5.0 / std::sqrt(1000.0);
  EXPECT_NEAR(mean_of(f, "H1", 1000), 50.0, five_se);

  // The drift counts from when the model was built, a second ago; the
  // readings span another second two hours on.
  ASSERT_TRUE(f.beam.set_baseline("H1", 50.0, 2.0));
  f.clock.advance(2h);
  EXPECT_NEAR(mean_of(f, "H1", 1000), 54.0 + 2.0 * 1.5 / 3600.0, five_se);

  // The other detectors have none.
  f.clock.advance(1s);
  EXPECT_LT(std::abs(f.read("AX")), 10.0);
}

TEST(BeamModel, SetGasDoesNotRepeatTheNoise) {
  Fixture f;
  f.beam.set_magnet(34.2 / 5.0);
  // The same gas given twice: the readings after the second are not those
  // after the first over again.
  auto five = [&f] {
    f.beam.set_gas(default_argon_gas());
    std::vector<double> out;
    for (int i = 0; i < 5; ++i) {
      f.clock.advance(1ms);
      out.push_back(f.read("H1"));
    }
    return out;
  };
  const std::vector<double> first = five();
  const std::vector<double> second = five();
  for (std::size_t i = 0; i < first.size(); ++i) EXPECT_NE(first[i], second[i]) << "reading " << i;
}

TEST(BeamModel, SetGasDoesNotRestartTheDrift) {
  Fixture f;
  ASSERT_TRUE(f.beam.set_baseline("H1", 50.0, 2.0));
  f.beam.set_magnet(34.2 / 5.0);
  f.clock.advance(2h);
  // The drift is since the model was built, whatever gas it was given since;
  // the gas's own rate does count from when it was given.
  f.beam.set_gas({{"Ar40", 39.96238, 1e4, 0.05}});
  EXPECT_NEAR(mean_of(f, "H1", 1000), 54.0 + 2.0 * 0.5 / 3600.0, 5.0 / std::sqrt(1000.0));
  f.beam.set_magnet(f.center("H1", "Ar40"));
  f.clock.advance(19s);  // twenty seconds since set_gas
  const double peak = 1e4 * std::exp(1.0);
  EXPECT_NEAR(f.read("H1"), peak + 54.0, 5.0 * (1.0 + 0.001 * peak));
}

TEST(BeamModel, TheBaselineAddsToThePeak) {
  Fixture f;
  f.beam.set_gas({{"Ar40", 39.96238, 1000.0, 0.0}});
  ASSERT_TRUE(f.beam.set_baseline("H1", 50.0, 0.0));
  f.beam.set_magnet(f.center("H1", "Ar40"));
  // sigma = 1 + 0.001 * 1000 = 2: the baseline does not enter it.
  EXPECT_NEAR(mean_of(f, "H1", 1000), 1050.0, 3.0 * 2.0 / std::sqrt(1000.0));
}

TEST(BeamModel, AReadingDependsOnDetectorAndTimeNotOnOrder) {
  ManualClock c1, c2;
  BeamSettings s;
  s.seed = 42;
  BeamModel a(c1, s), b(c2, s);
  for (auto* m : {&a, &b}) {
    m->ensure_detector("H1");
    m->ensure_detector("CDD");
    m->set_magnet(*m->peak_center("CDD", "Ar36"));  // a small beam on both
  }
  const TimePoint t = c1.now() + 3s;
  double a_h1 = a.intensity("H1", t)->value;
  double a_cdd = a.intensity("CDD", t)->value;
  double b_cdd = b.intensity("CDD", t)->value;
  double b_h1 = b.intensity("H1", t)->value;
  EXPECT_EQ(a_h1, b_h1);
  EXPECT_EQ(a_cdd, b_cdd);

  // One detector at one instant is one reading, however often it is asked
  // for and whatever was read in between.
  EXPECT_EQ(a.intensity("H1", t)->value, a_h1);
  EXPECT_EQ(a.intensity("CDD", t)->value, a_cdd);
  // A model that never read the counter gives the same Faraday reading.
  BeamModel c(c1, s);
  c.ensure_detector("H1");
  c.set_magnet(a.magnet());
  EXPECT_EQ(c.intensity("H1", t)->value, a_h1);

  // Another instant, another detector or another seed is another draw.
  EXPECT_NE(a.intensity("H1", t + 1ns)->value, a_h1);
  EXPECT_NE(a.intensity("CDD", t + 1s)->value, a_cdd);
  a.ensure_detector("H2");
  EXPECT_NE(a.intensity("H2", t)->value, a_h1);
  s.seed = 43;
  BeamModel d(c1, s);
  d.ensure_detector("H1");
  d.set_magnet(a.magnet());
  EXPECT_NE(d.intensity("H1", t)->value, a_h1);
}

TEST(BeamModel, PeakCenterKnowsAnIsotopeTheProviderOmits) {
  Fixture f;
  double ar36 = f.center("H1", "Ar36");
  f.beam.set_gas_provider([](TimePoint) {
    return std::vector<BeamGas>{{"Ar40", 39.96238, 1e3, 0.0}, {"Xe132", 131.904, 1.0, 0.0}};
  });
  // From the provider's list.
  EXPECT_NEAR(f.center("H1", "Ar40"), 39.96238 / 5.0, 1e-12);
  EXPECT_NEAR(f.center("H1", "Xe132"), 131.904 / 5.0, 1e-12);
  // Not in it: the fixed list knows the mass, and a peak is where it is
  // however little gas there is.
  auto r = f.beam.peak_center("H1", "Ar36");
  ASSERT_TRUE(r);
  EXPECT_DOUBLE_EQ(*r, ar36);
  // In neither.
  auto unknown = f.beam.peak_center("H1", "Kr84");
  ASSERT_FALSE(unknown);
  EXPECT_EQ(unknown.error().kind, ErrorKind::Config);
}

TEST(BeamModel, ABlankedBeamReadsBaselineOnly) {
  Fixture f;
  ASSERT_TRUE(f.beam.set_baseline("H1", 50.0, 0.0));
  f.beam.set_magnet(f.center("H1", "Ar40"));
  const double five_se = 5.0 / std::sqrt(1000.0);
  f.beam.blank(true);
  EXPECT_NEAR(mean_of(f, "H1", 1000), 50.0, five_se);
  f.beam.blank(false);
  EXPECT_GT(f.read("H1"), 9e5);

  // A protected detector likewise.
  ASSERT_TRUE(f.beam.protect("H1", true));
  EXPECT_NEAR(mean_of(f, "H1", 1000), 50.0, five_se);
}

TEST(BeamModel, ACounterBaselineIsDarkCounts) {
  Fixture f;
  ASSERT_TRUE(f.beam.set_baseline("EM", 50.0, 0.0));
  f.beam.set_magnet(34.2 / 5.0);
  // Poisson: the variance of a one-second count is its mean.
  const double five_se = 5.0 * std::sqrt(50.0 / 1000.0);
  EXPECT_NEAR(mean_of(f, "EM", 1000), 50.0, five_se);

  // Protected on a large beam: dark counts only, and nothing to overload on.
  f.beam.set_magnet(f.center("EM", "Ar40"));
  ASSERT_TRUE(f.beam.protect("EM", true));
  EXPECT_NEAR(mean_of(f, "EM", 1000), 50.0, five_se);
  EXPECT_FALSE(f.beam.intensity("EM")->overloaded);
  EXPECT_FALSE(f.beam.overloaded("EM"));
}

TEST(BeamModel, ABaselineAboveSaturationClamps) {
  Fixture f;
  ASSERT_TRUE(f.beam.set_baseline("H1", 5e6, 0.0));
  f.beam.set_magnet(34.2 / 5.0);
  auto r = f.beam.intensity("H1");
  ASSERT_TRUE(r);
  EXPECT_TRUE(r->saturated);
  EXPECT_DOUBLE_EQ(r->value, 4.9e6);
}

TEST(BeamModel, SetBaselineRefusesWhatItCannotUse) {
  Fixture f;
  auto unknown = f.beam.set_baseline("nope", 1.0, 0.0);
  ASSERT_FALSE(unknown);
  EXPECT_EQ(unknown.error().kind, ErrorKind::Config);
  EXPECT_FALSE(f.beam.set_baseline("H1", std::nan(""), 0.0));
  EXPECT_FALSE(f.beam.set_baseline("H1", 1.0, HUGE_VAL));
  auto d = f.beam.detector("H1");
  ASSERT_TRUE(d);
  EXPECT_EQ(d->baseline, 0.0);
  ASSERT_TRUE(f.beam.set_baseline("H1", 50.0, 2.0));
  d = f.beam.detector("H1");
  EXPECT_EQ(d->baseline, 50.0);
  EXPECT_EQ(d->baseline_drift_per_h, 2.0);
}

TEST(BeamModel, UnknownDetectorIsConfigError) {
  Fixture f;
  auto r = f.beam.intensity("nope");
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Config);
  EXPECT_FALSE(f.beam.set_deflection("nope", 1.0));
}

}  // namespace
