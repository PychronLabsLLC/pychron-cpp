#include <gtest/gtest.h>

#include "pychron/experiment/conditionals/expr.hpp"
#include "pychron/experiment/conditionals/metrics.hpp"

using namespace pychron::experiment;

namespace {

MetricRef iso(std::string a, std::string field = "") {
  MetricRef m;
  m.kind = field.empty() ? MetricRef::Kind::Isotope : MetricRef::Kind::IsotopeField;
  m.a = std::move(a);
  m.field = std::move(field);
  return m;
}

record::AnalysisRecord finished_run() {
  record::AnalysisRecord r;
  auto series = [&](std::string i, std::string det, std::string kind, std::vector<float> v) {
    record::DataSeries s;
    s.iso = std::move(i);
    s.det = std::move(det);
    s.kind = std::move(kind);
    for (size_t k = 0; k < v.size(); ++k) s.trace.t.push_back(static_cast<float>(k + 1));
    s.trace.v = std::move(v);
    r.data.series.push_back(std::move(s));
  };
  series("Ar40", "H1", "signal", {1010, 1020, 1030});
  series("Ar39", "AX", "signal", {101, 102, 103});
  series("Ar36", "CDD", "signal", {1, 1, 1});
  series("Ar36", "L2", "signal", {2, 2});
  series("", "H1", "baseline", {9, 11});
  pychron::reduction::FitSpec lin;
  r.results.intercepts["Ar40"] = {pychron::reduction::Intercept{1000, 2, 3, {}, 0, pychron::reduction::FitKind::Linear, {}}, lin};
  r.results.intercepts["Ar39"] = {pychron::reduction::Intercept{100, 1, 3, {}, 0, pychron::reduction::FitKind::Linear, {}}, lin};
  r.results.intercepts["Ar36:CDD"] = {pychron::reduction::Intercept{1, 0.1, 3, {}, 0, pychron::reduction::FitKind::Linear, {}}, lin};
  r.results.intercepts["Ar36:L2"] = {pychron::reduction::Intercept{2, 0.1, 2, {}, 0, pychron::reduction::FitKind::Linear, {}}, lin};
  r.results.baselines["H1"] = {10, 1, {}};
  r.results.icfactors["H1"] = 1.5;
  return r;
}

TEST(RecordMetrics, CorrectedValuesFieldsAndSeries) {
  const auto rec = finished_run();
  RecordMetrics m(rec);
  EXPECT_DOUBLE_EQ(*m.scalar(iso("Ar40")), (1000 - 10) * 1.5);
  EXPECT_DOUBLE_EQ(*m.scalar(iso("Ar40", "intercept")), 1000);
  EXPECT_DOUBLE_EQ(*m.scalar(iso("Ar40", "std_dev")), 2);
  EXPECT_DOUBLE_EQ(*m.scalar(iso("Ar40", "bs_corrected")), 990);
  EXPECT_DOUBLE_EQ(*m.scalar(iso("Ar40", "ic_corrected")), 1485);
  EXPECT_DOUBLE_EQ(*m.scalar(iso("Ar40", "cur")), 1030);
  EXPECT_EQ(*m.series(iso("Ar40", "bs")), (std::vector<double>{9, 11}));
  EXPECT_EQ(*m.series(iso("Ar39")), (std::vector<double>{101, 102, 103}));
  EXPECT_DOUBLE_EQ(*m.scalar(iso("Ar36")), 1);  // CDD has the most points
  MetricRef ratio{MetricRef::Kind::Ratio, "Ar40", "Ar39", "", "", ""};
  EXPECT_DOUBLE_EQ(*m.scalar(ratio), 1485.0 / 100);
  EXPECT_FALSE(m.scalar(iso("Ar37")));
  EXPECT_FALSE(m.elapsed());
  EXPECT_FALSE(m.scalar(MetricRef{MetricRef::Kind::Computed, "radiogenic_yield", "", "", "", ""}));  // no constants
}

TEST(RecordMetrics, ComputedAndConditionalChecks) {
  const auto rec = finished_run();
  pychron::reduction::ArArConstants c;
  RecordMetrics m(rec, c);
  const double ar40 = 1485, ar36 = 1;
  EXPECT_NEAR(*m.scalar(MetricRef{MetricRef::Kind::Computed, "radiogenic_yield", "", "", "", ""}),
              100 * (ar40 - ar36 * c.atm4036) / ar40, 1e-9);
  auto e = parse_expression("Ar40 > 1000 and radiogenic_yield > 50 and average(Ar40.bs) == 10");
  ASSERT_TRUE(e);
  auto r = evaluate_check(**e, m, {});
  ASSERT_TRUE(r) << r.error().what;
  EXPECT_TRUE(r->tripped);
}

TEST(ChainContext, FirstAnswerWins) {
  MapContext a, b;
  a.series_data["Ar40"] = {1};
  b.series_data["Ar40"] = {2};
  b.series_data["gauge.ig.pressure"] = {1e-9};
  b.elapsed_s = 5;
  ChainContext chain({&a, nullptr, &b});
  EXPECT_EQ(*chain.scalar(iso("Ar40")), 1);
  EXPECT_EQ(*chain.scalar(MetricRef{MetricRef::Kind::Gauge, "ig", "", "pressure", "", ""}), 1e-9);
  EXPECT_EQ(*chain.elapsed(), 5);
  EXPECT_FALSE(chain.scalar(iso("Ar39")));
}

}  // namespace

// A detector qualifier (plan v2 spec 6.3): "Ar40@H1.cur" reads H1's series
// when Ar40 was measured on H1 and H2; the bare name keeps its v1 meaning.
TEST(ConditionalMetrics, DetectorQualifier) {
  auto parsed = parse_expression("Ar40@H1.cur");
  ASSERT_TRUE(parsed) << parsed.error().what;
  const MetricRef cur = (*parsed)->metric;
  EXPECT_EQ(cur.kind, MetricRef::Kind::IsotopeField);
  EXPECT_EQ(cur.a, "Ar40");
  EXPECT_EQ(cur.detector, "H1");
  EXPECT_EQ(cur.field, "cur");
  EXPECT_EQ(to_string(cur), "Ar40@H1.cur");
  auto ratio = parse_expression("Ar40@H1/Ar39");
  ASSERT_TRUE(ratio) << ratio.error().what;
  EXPECT_EQ((*ratio)->metric.kind, MetricRef::Kind::Ratio);
  EXPECT_EQ((*ratio)->metric.detector, "H1");
  EXPECT_EQ((*ratio)->metric.b, "Ar39");
  EXPECT_EQ(to_string((*ratio)->metric), "Ar40@H1/Ar39");
  auto mode = parse_expression("Ar40@H2(CDD).intercept");
  ASSERT_TRUE(mode) << mode.error().what;
  EXPECT_EQ((*mode)->metric.detector, "H2(CDD)");
  EXPECT_FALSE(parse_expression("Ar40@H1.deflection"));  // a detector field takes a detector, not a label

  record::AnalysisRecord r;
  r.data.series.push_back({"Ar40", "H1", "signal", {{1, 2}, {100, 110}, {}}});
  r.data.series.push_back({"Ar40", "H2", "signal", {{1, 2, 3}, {50, 55, 60}, {}}});
  pychron::reduction::FitSpec lin;
  r.results.intercepts["Ar40@H1"] = {pychron::reduction::Intercept{95, 1, 2, {}, 0, pychron::reduction::FitKind::Linear, {}}, lin};
  r.results.intercepts["Ar40@H2"] = {pychron::reduction::Intercept{45, 1, 3, {}, 0, pychron::reduction::FitKind::Linear, {}}, lin};
  RecordMetrics m(r);
  EXPECT_DOUBLE_EQ(*m.scalar(cur), 110);
  EXPECT_DOUBLE_EQ(*m.scalar(iso("Ar40", "cur")), 60);  // bare: the most-populated series, H2
  MetricRef h1 = iso("Ar40", "intercept");
  h1.detector = "H1";
  EXPECT_DOUBLE_EQ(*m.scalar(h1), 95);
  h1.detector = "AX";
  EXPECT_FALSE(m.scalar(h1));
}
