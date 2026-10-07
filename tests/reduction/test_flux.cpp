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

}  // namespace
