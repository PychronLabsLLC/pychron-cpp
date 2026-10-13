#include "pychron/reduction/fits.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace pr = pychron::reduction;

namespace {

const std::string kSeriesDir = std::string(PYCHRON_TEST_DATA_DIR) + "/series/";

std::vector<std::string> split(const std::string& line, char sep) {
  std::vector<std::string> out;
  std::string cell;
  std::istringstream in(line);
  while (std::getline(in, cell, sep)) out.push_back(cell);
  if (!line.empty() && line.back() == sep) out.emplace_back();
  return out;
}

// Reads "x,y" rows, skipping '#' comments and the header.
pr::Series load_series(const std::string& name) {
  pr::Series s;
  std::ifstream in(kSeriesDir + name + ".csv");
  EXPECT_TRUE(in.is_open()) << name;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#' || line[0] == 'x') continue;
    auto cells = split(line, ',');
    s.x.push_back(std::stod(cells.at(0)));
    s.y.push_back(std::stod(cells.at(1)));
  }
  return s;
}

struct Expectation {
  std::string series;
  pr::FitSpec spec;
  double value = 0, error = 0, residual_sd = 0;
  std::size_t n_used = 0;
  std::vector<std::size_t> filtered;
  double rtol = 0, atol = 0;
};

std::vector<Expectation> load_expectations() {
  std::vector<Expectation> out;
  std::ifstream in(kSeriesDir + "expected.csv");
  EXPECT_TRUE(in.is_open());
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#' || line.starts_with("series,")) continue;
    auto c = split(line, ',');
    Expectation e;
    e.series = c.at(0);
    e.spec.kind = pr::parse_fit_kind(c.at(1)).value();
    e.spec.degree = std::stoi(c.at(2));
    e.spec.error = c.at(3) == "sd" ? pr::ErrorType::Sd : pr::ErrorType::Sem;
    e.spec.outliers = {c.at(4) == "1", std::stoi(c.at(5)), std::stod(c.at(6))};
    e.value = std::stod(c.at(7));
    e.error = std::stod(c.at(8));
    e.residual_sd = std::stod(c.at(9));
    e.n_used = std::stoul(c.at(10));
    for (const auto& idx : split(c.at(11), ';'))
      if (!idx.empty()) e.filtered.push_back(std::stoul(idx));
    e.rtol = std::stod(c.at(12));
    e.atol = std::stod(c.at(13));
    out.push_back(e);
  }
  return out;
}

void expect_close(double got, double want, double rtol, double atol, const std::string& what) {
  EXPECT_LE(std::abs(got - want), atol + rtol * std::abs(want))
      << what << ": got " << got << " want " << want;
}

pr::Series line(std::vector<double> x, double c0, double c1) {
  pr::Series s{std::move(x), {}};
  for (double xi : s.x) s.y.push_back(c0 + c1 * xi);
  return s;
}

}  // namespace

// Every row of tests/data/series/expected.csv: values generated from the
// pychron regressor formulas (see generate_fixtures.py for the tolerance rule).
TEST(Fits, MatchesPychronRegressionFixtures) {
  auto cases = load_expectations();
  ASSERT_GE(cases.size(), 20u);
  for (const auto& e : cases) {
    std::string label = e.series + "/" + std::string(pr::to_string(e.spec.kind)) +
                        (e.spec.error == pr::ErrorType::Sd ? "/sd" : "/sem") +
                        (e.spec.outliers.enabled ? "/filtered" : "");
    SCOPED_TRACE(label);
    auto r = pr::fit(load_series(e.series), e.spec);
    ASSERT_TRUE(r.has_value()) << r.error().what;
    expect_close(r->value, e.value, e.rtol, e.atol, "value");
    expect_close(r->error, e.error, e.rtol, e.atol, "error");
    expect_close(r->residual_sd, e.residual_sd, e.rtol, e.atol, "residual_sd");
    EXPECT_EQ(r->n_used, e.n_used);
    EXPECT_EQ(r->filtered_idx, e.filtered);
  }
}

// Draper & Smith p.8 as asserted by pychron's OLSRegressionTest.
TEST(Fits, DraperSmithLinearMatchesPychronUnitTest) {
  auto r = pr::fit(load_series("draper_smith"), {.kind = pr::FitKind::Linear});
  ASSERT_TRUE(r.has_value());
  EXPECT_NEAR(r->value, 13.623, 1e-3);
  EXPECT_EQ(r->n_used, 25u);
}

// pychron FilterOLSRegressionTest: one planted point is filtered in one iteration.
TEST(Fits, OutlierFilteringDropsPlantedPoint) {
  pr::FitSpec spec{.kind = pr::FitKind::Linear, .outliers = {true, 1, 2.0}};
  auto r = pr::fit(load_series("draper_smith_outlier"), spec);
  ASSERT_TRUE(r.has_value());
  EXPECT_NEAR(r->value, 13.623, 1e-3);
  EXPECT_EQ(r->filtered_idx, std::vector<std::size_t>{25});
  EXPECT_EQ(r->n_used, 25u);
}

TEST(Fits, OutliersIgnoredWhenDisabled) {
  auto r = pr::fit(load_series("draper_smith_outlier"), {.kind = pr::FitKind::Linear});
  ASSERT_TRUE(r.has_value());
  EXPECT_TRUE(r->filtered_idx.empty());
  EXPECT_EQ(r->n_used, 26u);
}

TEST(Fits, PerfectFitFiltersNothing) {
  // Zero residual sd must not flag every point (|r| >= 0 is always true).
  pr::FitSpec spec{.kind = pr::FitKind::Linear, .outliers = {true, 3, 2.0}};
  auto r = pr::fit(line({0, 1, 2, 3, 4}, 2.0, 1.0), spec);
  ASSERT_TRUE(r.has_value());
  EXPECT_NEAR(r->value, 2.0, 1e-12);
  EXPECT_TRUE(r->filtered_idx.empty());
  EXPECT_NEAR(r->error, 0.0, 1e-12);
}

TEST(Fits, ExactlyDeterminedFitHasZeroError) {
  auto r = pr::fit(line({1, 2}, 5.0, -1.0), {.kind = pr::FitKind::Linear});
  ASSERT_TRUE(r.has_value());
  EXPECT_NEAR(r->value, 5.0, 1e-12);
  EXPECT_EQ(r->error, 0.0);
  EXPECT_EQ(r->residual_sd, 0.0);
}

TEST(Fits, AverageSdIsSampleStandardDeviation) {
  pr::Series s{{0, 1, 2, 3}, {1, 2, 3, 4}};
  auto sd = pr::fit(s, {.kind = pr::FitKind::Average, .error = pr::ErrorType::Sd});
  auto sem = pr::fit(s, {.kind = pr::FitKind::Average, .error = pr::ErrorType::Sem});
  ASSERT_TRUE(sd && sem);
  EXPECT_DOUBLE_EQ(sd->value, 2.5);
  EXPECT_NEAR(sd->error, std::sqrt(5.0 / 3.0), 1e-12);
  EXPECT_NEAR(sem->error, std::sqrt(5.0 / 3.0) / 2.0, 1e-12);
}

TEST(Fits, CustomPolyZeroIsAverage) {
  auto s = load_series("evo_cubic_noisy");
  auto avg = pr::fit(s, {.kind = pr::FitKind::Average});
  auto p0 = pr::fit(s, {.kind = pr::FitKind::CustomPoly, .degree = 0});
  ASSERT_TRUE(avg && p0);
  EXPECT_NEAR(p0->value, avg->value, 1e-12);
  EXPECT_NEAR(p0->error, avg->error, 1e-12);
}

TEST(Fits, ExponentialRecoversDecayIntercept) {
  pr::Series s;
  for (int i = 0; i < 40; ++i) {
    double x = 5.0 + 10.0 * i;
    s.x.push_back(x);
    s.y.push_back(8.0 * std::exp(-0.01 * x) + 20.0);
  }
  auto r = pr::fit(s, {.kind = pr::FitKind::Exponential});
  ASSERT_TRUE(r.has_value()) << r.error().what;
  EXPECT_NEAR(r->value, 28.0, 1e-7);
}

TEST(Fits, RejectsBadInput) {
  EXPECT_FALSE(pr::fit({{0, 1, 2}, {0, 1}}, {}).has_value());
  EXPECT_FALSE(pr::fit({}, {.kind = pr::FitKind::Average}).has_value());
  EXPECT_FALSE(pr::fit({{0, 1}, {0, 1}}, {.kind = pr::FitKind::Parabolic}).has_value());
  EXPECT_FALSE(pr::fit({{0, 1, 2}, {0, 1, 2}}, {.kind = pr::FitKind::CustomPoly, .degree = -1})
                   .has_value());
  EXPECT_FALSE(pr::fit({{0, 1, 2}, {0, NAN, 2}}, {}).has_value());
  // All x identical: linear design is singular.
  EXPECT_FALSE(pr::fit({{3, 3, 3}, {1, 2, 3}}, {.kind = pr::FitKind::Linear}).has_value());
}

TEST(Fits, ParameterCount) {
  EXPECT_EQ(pr::parameter_count({.kind = pr::FitKind::Average}), 1u);
  EXPECT_EQ(pr::parameter_count({.kind = pr::FitKind::Cubic}), 4u);
  EXPECT_EQ(pr::parameter_count({.kind = pr::FitKind::Exponential}), 3u);
  EXPECT_EQ(pr::parameter_count({.kind = pr::FitKind::CustomPoly, .degree = 5}), 6u);
}

TEST(Fits, KindNamesRoundTrip) {
  for (auto k : {pr::FitKind::Average, pr::FitKind::Linear, pr::FitKind::Parabolic,
                 pr::FitKind::Cubic, pr::FitKind::Exponential, pr::FitKind::CustomPoly}) {
    EXPECT_EQ(pr::parse_fit_kind(pr::to_string(k)), k);
  }
  EXPECT_FALSE(pr::parse_fit_kind("weighted_mean").has_value());
}

namespace pychron::reduction {
namespace {

TEST(Fits, TheCurveIsKeptForDrawing) {
  Series s;
  for (int i = 0; i < 10; ++i) {
    s.x.push_back(i);
    s.y.push_back(3.0 + 2.0 * i + 0.5 * i * i);
  }
  auto quad = fit(s, FitSpec{FitKind::Parabolic});
  ASSERT_TRUE(quad);
  EXPECT_EQ(quad->kind, FitKind::Parabolic);
  ASSERT_EQ(quad->params.size(), 3u);
  EXPECT_NEAR(predict(*quad, 0.0), quad->value, 1e-9);
  EXPECT_NEAR(predict(*quad, 4.0), 3.0 + 8.0 + 8.0, 1e-9);

  auto avg = fit(s, FitSpec{FitKind::Average});
  ASSERT_TRUE(avg);
  EXPECT_NEAR(predict(*avg, 123.0), avg->value, 1e-12);

  Series e;
  for (int i = 0; i < 20; ++i) {
    e.x.push_back(i);
    e.y.push_back(100.0 * std::exp(-0.1 * i) + 10.0);
  }
  auto ex = fit(e, FitSpec{FitKind::Exponential});
  ASSERT_TRUE(ex) << ex.error().what;
  EXPECT_EQ(ex->params.size(), 3u);
  EXPECT_NEAR(predict(*ex, 5.0), 100.0 * std::exp(-0.5) + 10.0, 1e-6);

  EXPECT_TRUE(std::isnan(predict(Intercept{}, 1.0)));
}

}  // namespace
}  // namespace pychron::reduction
