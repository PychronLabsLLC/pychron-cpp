#include "pychron/reduction/stats.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <numbers>
#include <vector>

namespace pr = pychron::reduction;

namespace {

// Reference values below were produced by running legacy pychron
// (core/stats/core.py, core/regression/new_york_regressor.py) on the same
// inputs with numpy 2 / scipy 1.16.

const std::vector<double> kV{10.1, 10.3, 9.9, 10.6, 10.2};
const std::vector<double> kE{0.1, 0.2, 0.15, 0.3, 0.1};

TEST(Stats, WeightedMeanMatchesLegacy) {
  auto m = pr::weighted_mean(kV, kE);
  ASSERT_TRUE(m);
  EXPECT_NEAR(m->value, 10.14158415841584, 1e-12);
  EXPECT_NEAR(m->sem, 0.059702231412599352, 1e-14);
  EXPECT_NEAR(m->mswd, 1.517601760176015, 1e-12);
  EXPECT_EQ(m->n, 5u);
  EXPECT_DOUBLE_EQ(m->error, m->sem);
}

TEST(Stats, ErrorKinds) {
  auto msem = pr::weighted_mean(kV, kE, pr::MeanErrorKind::Msem);
  ASSERT_TRUE(msem);
  EXPECT_NEAR(msem->error, msem->sem * std::sqrt(msem->mswd), 1e-15);
  auto sd = pr::weighted_mean(kV, kE, pr::MeanErrorKind::Sd);
  ASSERT_TRUE(sd);
  EXPECT_DOUBLE_EQ(sd->error, sd->sd);
  // Msem never shrinks the error when mswd < 1.
  std::vector<double> v{1.0, 1.0001}, e{1.0, 1.0};
  auto tight = pr::weighted_mean(v, e, pr::MeanErrorKind::Msem);
  ASSERT_TRUE(tight);
  EXPECT_DOUBLE_EQ(tight->error, tight->sem);
}

TEST(Stats, WeightedMeanSkipsZeroErrors) {
  std::vector<double> v{1.0, 100.0, 3.0}, e{1.0, 0.0, 1.0};
  auto m = pr::weighted_mean(v, e);
  ASSERT_TRUE(m);
  EXPECT_EQ(m->n, 2u);
  EXPECT_DOUBLE_EQ(m->value, 2.0);
  std::vector<double> z{0.0};
  EXPECT_FALSE(pr::weighted_mean(std::vector<double>{1.0}, z));
  EXPECT_FALSE(pr::weighted_mean(v, std::vector<double>{1.0}));
}

TEST(Stats, ArithmeticMean) {
  auto m = pr::arithmetic_mean(kV, kE);
  ASSERT_TRUE(m);
  EXPECT_NEAR(m->value, 10.22, 1e-12);
  EXPECT_NEAR(m->sd, 0.2588435821108957, 1e-12);
  EXPECT_NEAR(m->sem, m->sd / std::sqrt(5.0), 1e-15);
  EXPECT_GT(m->mswd, 0.0);
}

TEST(Stats, MswdLimitsMatchScipy) {
  auto l1 = pr::mswd_limits(2);
  EXPECT_NEAR(l1.first, 0.00098206911717525834, 1e-10);
  EXPECT_NEAR(l1.second, 5.0238861873148881, 1e-9);
  auto l4 = pr::mswd_limits(5);
  EXPECT_NEAR(l4.first, 0.12110463927198253, 1e-10);
  EXPECT_NEAR(l4.second, 2.785821695469449, 1e-9);
  auto l10 = pr::mswd_limits(11);
  EXPECT_NEAR(l10.first, 0.32469727802368425, 1e-10);
  EXPECT_NEAR(l10.second, 2.0483177350807389, 1e-9);
  auto l50 = pr::mswd_limits(51);
  EXPECT_NEAR(l50.first, 0.64714727391317317, 1e-10);
  EXPECT_NEAR(l50.second, 1.4284039037501284, 1e-9);
  EXPECT_EQ(pr::mswd_limits(1), (std::pair<double, double>{0.0, 0.0}));
  EXPECT_TRUE(pr::mswd_acceptable(1.0, 5));
  EXPECT_FALSE(pr::mswd_acceptable(3.0, 5));
  EXPECT_FALSE(pr::mswd_acceptable(1.0, 1));
}

TEST(Stats, MswdProbability) {
  EXPECT_NEAR(pr::mswd_probability(1.7, 4), 0.14684238782543477, 1e-12);
  EXPECT_DOUBLE_EQ(pr::mswd_probability(1.0, 0), 0.0);
  EXPECT_NEAR(pr::chi2_quantile(pr::chi2_cdf(3.3, 7.0), 7.0), 3.3, 1e-10);
}

TEST(Stats, CumulativeProbabilityIntegratesToN) {
  std::vector<double> v{10.0, 12.0}, e{0.5, 0.25};
  auto c = pr::cumulative_probability(v, e, 0.0, 22.0, 2201);
  ASSERT_EQ(c.x.size(), 2201u);
  double area = 0.0;
  for (std::size_t i = 1; i < c.x.size(); ++i) area += 0.5 * (c.y[i] + c.y[i - 1]) * (c.x[i] - c.x[i - 1]);
  EXPECT_NEAR(area, 2.0, 1e-6);
  // The peak of the narrower Gaussian is at its value.
  const auto at12 = c.y[1200];
  EXPECT_NEAR(at12, 1.0 / (0.25 * std::sqrt(2 * std::numbers::pi)), 1e-3);
}

// Ten steps: 0-1 young, 2-7 flat at ~100 Ma, 8-9 old.
struct Spectrum {
  std::vector<double> ages{60, 80, 99.5, 100.2, 100.0, 99.8, 100.4, 100.1, 120, 140};
  std::vector<double> errors{1, 1, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 1, 1};
  std::vector<double> signals{5, 5, 10, 15, 15, 15, 10, 10, 10, 5};
};

TEST(Stats, FleckPlateau) {
  Spectrum s;
  auto p = pr::find_plateau(s.ages, s.errors, s.signals, {}, pr::PlateauCriteria{});
  ASSERT_TRUE(p);
  EXPECT_EQ(*p, (pr::StepRange{2, 7}));
  auto m = pr::plateau_mean(s.ages, s.errors, s.signals, {}, *p);
  ASSERT_TRUE(m);
  EXPECT_EQ(m->nsteps, 6u);
  EXPECT_NEAR(m->gas_fraction, 75.0, 1e-12);
  EXPECT_NEAR(m->mean.value, 100.0, 0.1);
}

TEST(Stats, PlateauCriteriaAndExclusions) {
  Spectrum s;
  pr::PlateauCriteria strict;
  strict.gas_fraction = 80;
  EXPECT_FALSE(pr::find_plateau(s.ages, s.errors, s.signals, {}, strict));

  // Excluding a step that breaks the overlap lets the plateau run through it.
  s.ages[5] = 90.0;
  auto broken = pr::find_plateau(s.ages, s.errors, s.signals, {}, pr::PlateauCriteria{});
  EXPECT_FALSE(broken && broken->last - broken->first >= 5);
  bool ex[10] = {};
  ex[5] = true;
  auto p = pr::find_plateau(s.ages, s.errors, s.signals, ex, pr::PlateauCriteria{});
  ASSERT_TRUE(p);
  EXPECT_EQ(*p, (pr::StepRange{2, 7}));
  auto m = pr::plateau_mean(s.ages, s.errors, s.signals, ex, *p);
  ASSERT_TRUE(m);
  EXPECT_EQ(m->nsteps, 5u);  // the excluded step is not averaged
}

TEST(Stats, MahonPlateau) {
  Spectrum s;
  pr::PlateauCriteria c;
  c.method = pr::PlateauMethod::Mahon;
  auto p = pr::find_plateau(s.ages, s.errors, s.signals, {}, c);
  ASSERT_TRUE(p);
  EXPECT_EQ(*p, (pr::StepRange{2, 7}));
}

TEST(Stats, VolumeFractionPlateau) {
  Spectrum s;
  auto m = pr::plateau_mean(s.ages, s.errors, s.signals, {}, {2, 7}, pr::PlateauWeighting::VolumeFraction,
                            pr::MeanErrorKind::Sem);
  ASSERT_TRUE(m);
  double sw = 0, swa = 0;
  for (int i = 2; i <= 7; ++i) {
    sw += s.signals[i];
    swa += s.signals[i] * s.ages[i];
  }
  EXPECT_NEAR(m->mean.value, swa / sw, 1e-12);
  EXPECT_FALSE(pr::plateau_mean(s.ages, s.errors, s.signals, {}, {3, 12}));
}

std::vector<pr::XyPoint> isochron_points() {
  const double xs[] = {0.01, 0.05, 0.1, 0.15, 0.2, 0.25};
  const double ys[] = {0.0031, 0.0027, 0.0021, 0.0017, 0.00105, 0.0006};
  const double ye[] = {5e-5, 6e-5, 4e-5, 5e-5, 5e-5, 6e-5};
  std::vector<pr::XyPoint> pts;
  for (int i = 0; i < 6; ++i) pts.push_back({xs[i], xs[i] * 0.01 + 0.0005, ys[i], ye[i], 0.0});
  return pts;
}

TEST(Stats, YorkMatchesLegacy) {
  auto f = pr::york_fit(isochron_points(), pr::YorkMethod::York);
  ASSERT_TRUE(f);
  EXPECT_TRUE(f->converged);
  EXPECT_NEAR(f->intercept, 0.0031985739329463449, 1e-15);
  EXPECT_NEAR(f->intercept_err, 3.9623339286083918e-05, 1e-15);
  EXPECT_NEAR(f->slope, -0.010500506362941414, 1e-13);
  EXPECT_NEAR(f->slope_err, 0.0002856945850088793, 1e-14);
  EXPECT_NEAR(f->mswd, 1.089782254816656, 1e-9);
  EXPECT_NEAR(f->x_intercept, 0.30461139895451256, 1e-10);
  EXPECT_NEAR(f->x_intercept_err, 0.0055544812944023046, 1e-10);
}

TEST(Stats, NewYorkMatchesLegacy) {
  auto f = pr::york_fit(isochron_points(), pr::YorkMethod::NewYork);
  ASSERT_TRUE(f);
  EXPECT_NEAR(f->intercept_err, 3.9659014929910858e-05, 1e-15);
  EXPECT_NEAR(f->slope_err, 0.00028622545872837178, 1e-14);
  EXPECT_NEAR(f->x_intercept_err, 0.0055623477964508788, 1e-10);
}

TEST(Stats, ReedMatchesLegacy) {
  auto f = pr::york_fit(isochron_points(), pr::YorkMethod::Reed);
  ASSERT_TRUE(f);
  EXPECT_NEAR(f->intercept, 0.0031985739329510824, 1e-12);
  EXPECT_NEAR(f->intercept_err, 4.1363848536324752e-05, 1e-12);
  EXPECT_NEAR(f->slope_err, 0.00029824410952937859, 1e-11);
  EXPECT_NEAR(f->x_intercept_err, 0.0057984694651578043, 1e-9);
}

TEST(Stats, YorkRejectsBadInput) {
  auto pts = isochron_points();
  EXPECT_FALSE(pr::york_fit(std::span(pts).first(2)));
  pts[0].sx = 0;
  EXPECT_FALSE(pr::york_fit(pts));
  pts = isochron_points();
  pts[1].rho = 1.0;
  EXPECT_FALSE(pr::york_fit(pts));
}

TEST(Stats, YorkExactLine) {
  std::vector<pr::XyPoint> pts;
  for (int i = 0; i < 5; ++i) pts.push_back({double(i), 0.1, 2.0 + 3.0 * i, 0.1, 0.3});
  auto f = pr::york_fit(pts);
  ASSERT_TRUE(f);
  EXPECT_NEAR(f->slope, 3.0, 1e-9);
  EXPECT_NEAR(f->intercept, 2.0, 1e-9);
  EXPECT_NEAR(f->mswd, 0.0, 1e-12);
}

TEST(Stats, Names) {
  EXPECT_EQ(pr::parse_mean_error_kind("MSE"), pr::MeanErrorKind::Msem);
  EXPECT_EQ(pr::parse_mean_error_kind("SE"), pr::MeanErrorKind::Sem);
  EXPECT_EQ(pr::to_string(pr::MeanErrorKind::Sd), "sd");
  EXPECT_EQ(pr::parse_york_method("NewYork"), pr::YorkMethod::NewYork);
  EXPECT_FALSE(pr::parse_york_method("ols"));
}

}  // namespace
