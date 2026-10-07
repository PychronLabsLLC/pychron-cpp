#include "pychron/reduction/flux.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <vector>

namespace pr = pychron::reduction;

namespace {

const pr::MonitorConstants kMonitor{28.201e6, 5.463e-10};

double numerator() { return std::exp(kMonitor.age_a * kMonitor.lambda_k) - 1.0; }

// An analysis whose J is exactly `j` with relative error `rel`.
pr::MonitorAnalysis analysis_with_j(const char* id, double j, double rel, bool omitted = false) {
  const double f = numerator() / j;
  return {id, pr::UFloat::variable(f, f * rel), omitted};
}

TEST(Flux, JInvertsTheAgeEquation) {  // legacy argon_calculations_test.py:91-97
  const double f = 10.0;
  const double j = (std::exp(kMonitor.age_a * kMonitor.lambda_k) - 1.0) / f;
  auto r = pr::j_of(pr::UFloat::variable(f, 0.01), kMonitor);
  ASSERT_TRUE(r);
  EXPECT_NEAR(r->nominal(), j, 1e-15);
  EXPECT_NEAR(r->std_dev(), j * 0.01 / f, 1e-15);  // sigma_J = J sigma_F / F (F4)
}

TEST(Flux, ZeroNegativeOrNonFiniteFIsAnError) {  // X9, Review Focus 2
  for (double f : {0.0, -1.0, std::nan(""), std::numeric_limits<double>::infinity()})
    EXPECT_FALSE(pr::j_of(pr::UFloat::variable(f, 0.1), kMonitor)) << f;
}

TEST(Flux, MeanKindRoundTrips) {
  EXPECT_EQ(pr::to_string(pr::MeanKind::Arithmetic), "arithmetic");
  EXPECT_EQ(pr::to_string(pr::MeanKind::Weighted), "weighted");
  EXPECT_EQ(pr::parse_mean_kind("Weighted"), pr::MeanKind::Weighted);
  EXPECT_EQ(pr::parse_mean_kind("ARITHMETIC"), pr::MeanKind::Arithmetic);
  EXPECT_FALSE(pr::parse_mean_kind("median"));
}

// Legacy error_propagation.py:333-355: values 80 and 90 with weights 20 and
// 30, i.e. errors 20^-0.5 and 30^-0.5: mean 86, SEM 50^-0.5. Scaled by 1e-5.
TEST(Flux, WeightedMeanJ) {
  const std::vector<pr::MonitorAnalysis> a{
      analysis_with_j("a", 80e-5, std::pow(20.0, -0.5) / 80.0),
      analysis_with_j("b", 90e-5, std::pow(30.0, -0.5) / 90.0),
  };
  auto r = pr::mean_j(a, kMonitor, pr::MeanKind::Weighted, pr::MeanErrorKind::Sem);
  ASSERT_TRUE(r);
  EXPECT_EQ(r->n, 2);
  EXPECT_TRUE(r->rejected.empty());
  EXPECT_NEAR(r->j, 86.0e-5, 86.0e-5 * 1e-12);
  EXPECT_NEAR(r->j_err, 0.1414213562373095e-5, 0.1414213562373095e-5 * 1e-12);
}

TEST(Flux, ArithmeticMeanOfOneAnalysisHasThatAnalysisError) {  // X6
  const std::vector<pr::MonitorAnalysis> a{analysis_with_j("a", 1e-3, 0.01)};
  auto r = pr::mean_j(a, kMonitor, pr::MeanKind::Arithmetic, pr::MeanErrorKind::Sem);
  ASSERT_TRUE(r);
  EXPECT_EQ(r->n, 1);
  EXPECT_NEAR(r->j, 1e-3, 1e-15);
  EXPECT_NEAR(r->j_err, 1e-3 * 0.01, 1e-15);
  EXPECT_EQ(r->mswd, 0.0);
}

TEST(Flux, OmittedAnalysesTakeNoPart) {
  const std::vector<pr::MonitorAnalysis> a{
      analysis_with_j("a", 1.0e-3, 0.01),
      analysis_with_j("b", 1.1e-3, 0.01),
      analysis_with_j("c", 9.0e-3, 0.01, true),
  };
  auto r = pr::mean_j(a, kMonitor, pr::MeanKind::Arithmetic, pr::MeanErrorKind::Sem);
  ASSERT_TRUE(r);
  EXPECT_EQ(r->n, 2);
  EXPECT_TRUE(r->rejected.empty());
  EXPECT_NEAR(r->j, 1.05e-3, 1e-15);
}

TEST(Flux, AnAnalysisWithNoJIsRejectedAndNamed) {
  std::vector<pr::MonitorAnalysis> a{
      analysis_with_j("66001-01", 1.0e-3, 0.01),
      analysis_with_j("66001-03", 1.1e-3, 0.01),
  };
  a.insert(a.begin() + 1, {"66001-02", pr::UFloat::variable(0.0, 0.1), false});
  auto r = pr::mean_j(a, kMonitor, pr::MeanKind::Arithmetic, pr::MeanErrorKind::Sem);
  ASSERT_TRUE(r);
  EXPECT_EQ(r->n, 2);
  ASSERT_EQ(r->rejected.size(), 1u);
  EXPECT_EQ(r->rejected[0], "66001-02");
}

TEST(Flux, AWeightedAnalysisWithNoErrorIsRejectedAndNamed) {  // X5
  const std::vector<pr::MonitorAnalysis> a{
      analysis_with_j("a", 1.0e-3, 0.01),
      {"exact", pr::UFloat(numerator() / 1.05e-3), false},
      analysis_with_j("c", 1.1e-3, 0.01),
  };
  auto w = pr::mean_j(a, kMonitor, pr::MeanKind::Weighted, pr::MeanErrorKind::Sem);
  ASSERT_TRUE(w);
  EXPECT_EQ(w->n, 2);
  ASSERT_EQ(w->rejected.size(), 1u);
  EXPECT_EQ(w->rejected[0], "exact");
  // An arithmetic mean has no use for the error: the same analysis counts.
  auto m = pr::mean_j(a, kMonitor, pr::MeanKind::Arithmetic, pr::MeanErrorKind::Sem);
  ASSERT_TRUE(m);
  EXPECT_EQ(m->n, 3);
  EXPECT_TRUE(m->rejected.empty());
}

TEST(Flux, NoAnalysisLeftIsAnError) {
  EXPECT_FALSE(pr::mean_j({}, kMonitor, pr::MeanKind::Weighted, pr::MeanErrorKind::Sem));
  const std::vector<pr::MonitorAnalysis> omitted{analysis_with_j("a", 1e-3, 0.01, true)};
  EXPECT_FALSE(pr::mean_j(omitted, kMonitor, pr::MeanKind::Arithmetic, pr::MeanErrorKind::Sem));
  const std::vector<pr::MonitorAnalysis> rejected{{"a", pr::UFloat::variable(0.0, 0.1), false}};
  EXPECT_FALSE(pr::mean_j(rejected, kMonitor, pr::MeanKind::Weighted, pr::MeanErrorKind::Sem));
}

TEST(Flux, MsemScalesOnlyWhenMswdAboveOne) {
  const std::vector<pr::MonitorAnalysis> scattered{
      analysis_with_j("a", 0.9e-3, 0.001),
      analysis_with_j("b", 1.0e-3, 0.001),
      analysis_with_j("c", 1.1e-3, 0.001),
  };
  for (auto kind : {pr::MeanKind::Weighted, pr::MeanKind::Arithmetic}) {
    auto sem = pr::mean_j(scattered, kMonitor, kind, pr::MeanErrorKind::Sem);
    auto msem = pr::mean_j(scattered, kMonitor, kind, pr::MeanErrorKind::Msem);
    ASSERT_TRUE(sem);
    ASSERT_TRUE(msem);
    ASSERT_GT(sem->mswd, 1.0);
    EXPECT_NEAR(msem->j_err, sem->j_err * std::sqrt(sem->mswd), sem->j_err * 1e-12);
  }
  const std::vector<pr::MonitorAnalysis> tight{
      analysis_with_j("a", 1.0000e-3, 0.01),
      analysis_with_j("b", 1.0001e-3, 0.01),
      analysis_with_j("c", 0.9999e-3, 0.01),
  };
  for (auto kind : {pr::MeanKind::Weighted, pr::MeanKind::Arithmetic}) {
    auto sem = pr::mean_j(tight, kMonitor, kind, pr::MeanErrorKind::Sem);
    auto msem = pr::mean_j(tight, kMonitor, kind, pr::MeanErrorKind::Msem);
    ASSERT_TRUE(sem);
    ASSERT_TRUE(msem);
    ASSERT_LE(sem->mswd, 1.0);
    EXPECT_EQ(msem->j_err, sem->j_err);
  }
}

// ---- Flux models ---------------------------------------------------------------

pr::FitOptions options(pr::ModelKind kind, pr::Interpolation how = pr::Interpolation::WeightedMean) {
  pr::FitOptions o;
  o.kind = kind;
  o.interpolation = how;
  return o;
}

std::vector<pr::Monitor> four_in_a_row() {
  return {{"a", {0, 0}, 1.0, 0.1}, {"b", {10, 0}, 2.0, 0.2}, {"c", {20, 0}, 4.0, 0.4}, {"d", {30, 0}, 8.0, 0.8}};
}

TEST(FluxModels, BracketingLinearTwoMonitors) {  // regression.py:300-331
  const std::vector<pr::Monitor> m{{"1", {0, 0}, 1.0, 0.1}, {"2", {10, 0}, 2.0, 0.2}};
  auto f = pr::fit_flux(m, std::vector<pr::Point>{{5, 0}, {5, 3}},
                        options(pr::ModelKind::Bracketing, pr::Interpolation::Linear));
  ASSERT_TRUE(f);
  EXPECT_DOUBLE_EQ(f->at[0].j, 1.5);
  EXPECT_DOUBLE_EQ(f->at[0].j_err, std::sqrt(0.05 * 0.05 + 0.1 * 0.1));
  EXPECT_DOUBLE_EQ(f->at[1].j, 1.5);  // off the segment: projected
}

TEST(FluxModels, BracketingLinearPicksTheTwoNearestOfFour) {  // regression.py:334-372
  auto f = pr::fit_flux(four_in_a_row(), std::vector<pr::Point>{{12, 0}},
                        options(pr::ModelKind::Bracketing, pr::Interpolation::Linear));
  ASSERT_TRUE(f);
  EXPECT_NEAR(f->at[0].j, 2.4, 1e-12);
  EXPECT_NEAR(f->at[0].j_err, std::sqrt(std::pow(0.8 * 0.2, 2) + std::pow(0.2 * 0.4, 2)), 1e-12);
}

TEST(FluxModels, Bracketing1D) {  // regression.py:579-656
  const std::vector<pr::Monitor> m{{"1", {0, 0}, 1.0, 0.1}, {"2", {10, 0}, 2.0, 0.2}, {"3", {20, 0}, 4.0, 0.4}};
  const std::vector<pr::Point> at{{5, 0}, {2.5, 0}, {-10, 0}, {30, 0}};
  auto o = options(pr::ModelKind::Bracketing1D, pr::Interpolation::Linear);
  auto f = pr::fit_flux(m, at, o);
  ASSERT_TRUE(f);
  EXPECT_NEAR(f->at[0].j, 1.5, 1e-12);
  EXPECT_NEAR(f->at[0].j_err, std::sqrt(0.05 * 0.05 + 0.1 * 0.1), 1e-12);
  EXPECT_NEAR(f->at[1].j, 1.25, 1e-12);
  EXPECT_NEAR(f->at[2].j, 0.0, 1e-12);
  EXPECT_NEAR(f->at[3].j, 6.0, 1e-12);

  const std::vector<pr::Monitor> shuffled{m[2], m[0], m[1]};
  auto g = pr::fit_flux(shuffled, at, o);
  ASSERT_TRUE(g);
  for (std::size_t i = 0; i < at.size(); ++i) {
    EXPECT_DOUBLE_EQ(g->at[i].j, f->at[i].j);
    EXPECT_DOUBLE_EQ(g->at[i].j_err, f->at[i].j_err);
  }

  // Axis::Y reads y: the same monitors laid along y.
  std::vector<pr::Monitor> along_y;
  for (const auto& mon : m) along_y.push_back({mon.label, {99, mon.at.x}, mon.j, mon.j_err});
  std::vector<pr::Point> at_y;
  for (const auto& p : at) at_y.push_back({-7, p.x});
  o.axis = pr::Axis::Y;
  auto h = pr::fit_flux(along_y, at_y, o);
  ASSERT_TRUE(h);
  for (std::size_t i = 0; i < at.size(); ++i) EXPECT_DOUBLE_EQ(h->at[i].j, f->at[i].j);
}

TEST(FluxModels, ExtrapolationIsNoted) {  // X10
  const std::vector<pr::Monitor> m{{"1", {0, 0}, 1.0, 0.1}, {"2", {10, 0}, 2.0, 0.2}, {"3", {20, 0}, 4.0, 0.4}};
  auto f = pr::fit_flux(m, std::vector<pr::Point>{{-10, 0}, {5, 0}, {30, 0}},
                        options(pr::ModelKind::Bracketing1D, pr::Interpolation::Linear));
  ASSERT_TRUE(f);
  EXPECT_EQ(f->notes, (std::vector<pr::PointNote>{{0, pr::FitNote::Extrapolated}, {2, pr::FitNote::Extrapolated}}));

  auto b = pr::fit_flux(m, std::vector<pr::Point>{{5, 0}, {-5, 0}, {0, 0}},
                        options(pr::ModelKind::Bracketing, pr::Interpolation::Linear));
  ASSERT_TRUE(b);
  EXPECT_EQ(b->notes, (std::vector<pr::PointNote>{{1, pr::FitNote::Extrapolated}}));
}

TEST(FluxModels, MatchingTakesTheNearestMonitor) {
  const std::vector<pr::Monitor> m{{"a", {0, 0}, 1.0, 0.1}, {"b", {10, 0}, 2.0, 0.2}};
  auto f = pr::fit_flux(m, std::vector<pr::Point>{{8, 0}, {5, 0}}, options(pr::ModelKind::Matching));
  ASSERT_TRUE(f);
  EXPECT_DOUBLE_EQ(f->at[0].j, 2.0);
  EXPECT_DOUBLE_EQ(f->at[0].j_err, 0.2);
  EXPECT_DOUBLE_EQ(f->at[1].j, 1.0);  // tie: the earlier monitor
  EXPECT_DOUBLE_EQ(f->at[1].j_err, 0.1);
}

TEST(FluxModels, NearestNeighborsIsTheInverseVarianceMeanOfN) {
  auto o = options(pr::ModelKind::NearestNeighbors);
  o.n_neighbors = 3;
  auto f = pr::fit_flux(four_in_a_row(), std::vector<pr::Point>{{4, 0}}, o);
  ASSERT_TRUE(f);
  // The three nearest to x = 4 are a, b, c.
  const double wa = 1 / (0.1 * 0.1), wb = 1 / (0.2 * 0.2), wc = 1 / (0.4 * 0.4);
  EXPECT_NEAR(f->at[0].j, (wa * 1.0 + wb * 2.0 + wc * 4.0) / (wa + wb + wc), 1e-12);
  EXPECT_NEAR(f->at[0].j_err, std::pow(wa + wb + wc, -0.5), 1e-12);
}

TEST(FluxModels, BracketingAverageUsesTheSampleSd) {  // X8
  const std::vector<pr::Monitor> m{{"a", {0, 0}, 1.0, 0.1}, {"b", {10, 0}, 3.0, 0.1}};
  auto f = pr::fit_flux(m, std::vector<pr::Point>{{5, 0}}, options(pr::ModelKind::Bracketing, pr::Interpolation::Average));
  ASSERT_TRUE(f);
  EXPECT_DOUBLE_EQ(f->at[0].j, 2.0);
  EXPECT_NEAR(f->at[0].j_err, std::sqrt(2.0), 1e-12);
}

TEST(FluxModels, WeightedMeanGivesEveryPointTheSameJ) {
  const std::vector<pr::Monitor> m{{"a", {0, 0}, 80.0, std::pow(20.0, -0.5)}, {"b", {10, 0}, 90.0, std::pow(30.0, -0.5)}};
  const std::vector<pr::Point> at{{1, 1}, {50, -3}};
  auto o = options(pr::ModelKind::WeightedMean);
  o.error = pr::MeanErrorKind::Sem;
  auto f = pr::fit_flux(m, at, o);
  ASSERT_TRUE(f);
  ASSERT_EQ(f->at.size(), 2u);
  for (const auto& p : f->at) {
    EXPECT_NEAR(p.j, 86.0, 1e-9);
    EXPECT_NEAR(p.j_err, std::pow(50.0, -0.5), 1e-12);
  }
  EXPECT_EQ(f->dof, 1);
  EXPECT_GT(f->mswd, 0.0);

  o.kind = pr::ModelKind::WeightedMean1D;
  auto g = pr::fit_flux(m, at, o);
  ASSERT_TRUE(g);
  EXPECT_DOUBLE_EQ(g->at[0].j, f->at[0].j);
  EXPECT_DOUBLE_EQ(g->at[0].j_err, f->at[0].j_err);

  o.error = pr::MeanErrorKind::Sd;  // allowed here (F13)
  EXPECT_TRUE(pr::fit_flux(m, at, o));
}

TEST(FluxModels, MinimumMonitors) {
  auto n = [](pr::ModelKind k, int degree = 1, int neighbors = 2) {
    pr::FitOptions o;
    o.kind = k;
    o.degree = degree;
    o.n_neighbors = neighbors;
    return pr::minimum_monitors(o);
  };
  EXPECT_EQ(n(pr::ModelKind::Plane), 4u);
  EXPECT_EQ(n(pr::ModelKind::Bowl), 6u);
  EXPECT_EQ(n(pr::ModelKind::LeastSquares1D, 3), 5u);
  EXPECT_EQ(n(pr::ModelKind::WeightedMean), 1u);
  EXPECT_EQ(n(pr::ModelKind::WeightedMean1D), 1u);
  EXPECT_EQ(n(pr::ModelKind::Matching), 1u);
  EXPECT_EQ(n(pr::ModelKind::NearestNeighbors, 1, 5), 5u);
  EXPECT_EQ(n(pr::ModelKind::Bracketing), 2u);
  EXPECT_EQ(n(pr::ModelKind::Bracketing1D), 2u);
  EXPECT_TRUE(pr::is_least_squares(pr::ModelKind::Plane));
  EXPECT_TRUE(pr::is_least_squares(pr::ModelKind::Bowl));
  EXPECT_TRUE(pr::is_least_squares(pr::ModelKind::LeastSquares1D));
  EXPECT_FALSE(pr::is_least_squares(pr::ModelKind::Matching));
}

TEST(FluxModels, TooFewMonitorsIsAnErrorNamingTheCount) {  // X4/X11
  const std::vector<pr::Monitor> m{{"a", {0, 0}, 1.0, 0.1}, {"b", {10, 0}, 2.0, 0.2}};
  auto o = options(pr::ModelKind::NearestNeighbors);
  o.n_neighbors = 3;
  auto f = pr::fit_flux(m, std::vector<pr::Point>{{1, 0}}, o);
  ASSERT_FALSE(f);
  EXPECT_NE(f.error().what.find("nearest neighbors needs 3 monitor positions, 2 used"), std::string::npos)
      << f.error().what;
}

TEST(FluxModels, ZeroErrorInAWeightedModelNamesTheMonitor) {  // X5
  const std::vector<pr::Monitor> m{{"6", {0, 0}, 1.0, 0.1}, {"7", {10, 0}, 2.0, 0.0}};
  auto f = pr::fit_flux(m, std::vector<pr::Point>{{1, 0}}, options(pr::ModelKind::WeightedMean));
  ASSERT_FALSE(f);
  EXPECT_NE(f.error().what.find("7"), std::string::npos) << f.error().what;
  // Matching has no use for the weights.
  EXPECT_TRUE(pr::fit_flux(m, std::vector<pr::Point>{{1, 0}}, options(pr::ModelKind::Matching)));
}

TEST(FluxModels, NonFiniteInputNamesTheMonitor) {
  const double nan = std::numeric_limits<double>::quiet_NaN(), inf = std::numeric_limits<double>::infinity();
  const std::vector<pr::Point> at{{1, 0}};
  const auto o = options(pr::ModelKind::Matching);
  const std::vector<pr::Monitor> bad_j{{"p", {0, 0}, nan, 0.1}, {"q", {10, 0}, 2.0, 0.2}};
  const std::vector<pr::Monitor> bad_e{{"p", {0, 0}, 1.0, 0.1}, {"q", {10, 0}, 2.0, inf}};
  const std::vector<pr::Monitor> bad_x{{"p", {0, 0}, 1.0, 0.1}, {"q", {nan, 0}, 2.0, 0.2}};
  for (auto* m : {&bad_j, &bad_e, &bad_x}) {
    auto f = pr::fit_flux(*m, at, o);
    ASSERT_FALSE(f);
    const char* label = m == &bad_j ? "p" : "q";
    EXPECT_NE(f.error().what.find(std::string("monitor ") + label), std::string::npos) << f.error().what;
  }
  const std::vector<pr::Monitor> ok{{"p", {0, 0}, 1.0, 0.1}, {"q", {10, 0}, 2.0, 0.2}};
  auto f = pr::fit_flux(ok, std::vector<pr::Point>{{0, 0}, {nan, 1}}, o);
  ASSERT_FALSE(f);
  EXPECT_NE(f.error().what.find("position 1"), std::string::npos) << f.error().what;
}

TEST(FluxModels, Bracketing1DWithCoincidentMonitors) {
  const std::vector<pr::Monitor> m{{"a", {5, 0}, 1.0, 0.1}, {"b", {5, 0}, 3.0, 0.1}};
  auto f = pr::fit_flux(m, std::vector<pr::Point>{{5, 0}}, options(pr::ModelKind::Bracketing1D, pr::Interpolation::Linear));
  ASSERT_TRUE(f);
  EXPECT_DOUBLE_EQ(f->at[0].j, 1.0);
  EXPECT_TRUE(std::isfinite(f->at[0].j_err));
}

TEST(FluxModels, BadOptionsAreErrors) {
  const std::vector<pr::Monitor> m = four_in_a_row();
  const std::vector<pr::Point> at{{1, 0}};
  auto o = options(pr::ModelKind::NearestNeighbors);
  o.n_neighbors = 0;
  EXPECT_FALSE(pr::fit_flux(m, at, o));
  o = options(pr::ModelKind::LeastSquares1D);
  o.degree = 0;
  EXPECT_FALSE(pr::fit_flux(m, at, o));
  o.degree = 5;
  EXPECT_FALSE(pr::fit_flux(m, at, o));
}

}  // namespace
