// Extended conditionals grammar (conditionals spec section 3): arithmetic,
// abs(), legacy field aliases, qualified detector names, computed metrics,
// and the window / mapper transforms.

#include <gtest/gtest.h>

#include "pychron/experiment/conditionals/evaluator.hpp"
#include "pychron/experiment/conditionals/expr.hpp"

using namespace pychron::experiment;

namespace {

std::string canon(const std::string& s) {
  auto e = parse_expression(s);
  EXPECT_TRUE(e) << s << ": " << (e ? "" : e.error().what);
  return e ? to_string(**e) : "";
}

double eval(const std::string& s, const MetricContext& ctx, const Variables& vars = {}) {
  auto e = parse_expression(s);
  EXPECT_TRUE(e) << s << ": " << (e ? "" : e.error().what);
  if (!e) return -999;
  auto r = evaluate(**e, ctx, vars);
  EXPECT_TRUE(r) << s << ": " << (r ? "" : r.error().what);
  return r ? *r : -999;
}

TEST(ConditionalsGrammar, ArithmeticRoundTripsWithMinimalParentheses) {
  EXPECT_EQ(canon("Ar40+1000>900"), "Ar40 + 1000 > 900");
  EXPECT_EQ(canon("(Ar40 + Ar39) * 2 > 1"), "(Ar40 + Ar39) * 2 > 1");
  EXPECT_EQ(canon("Ar40 - (Ar39 - Ar38) > 0"), "Ar40 - (Ar39 - Ar38) > 0");
  EXPECT_EQ(canon("Ar40 - Ar39 - Ar38 > 0"), "Ar40 - Ar39 - Ar38 > 0");
  EXPECT_EQ(canon("-Ar36 < 0"), "-Ar36 < 0");
  EXPECT_EQ(canon("-(Ar36 + 1) < 0"), "-(Ar36 + 1) < 0");
  EXPECT_EQ(canon("Ar40 > -5"), "Ar40 > -5");
  EXPECT_EQ(canon("Ar40 - -5 > 0"), "Ar40 - -5 > 0");
  EXPECT_EQ(canon("abs(slope(Ar40)) > 10"), "abs(slope(Ar40)) > 10");
  EXPECT_EQ(canon("(Ar40 > 1) == (Ar39 > 1)"), "(Ar40 > 1) == (Ar39 > 1)");
  for (const char* s : {"Ar40 * 2 / (Ar39 + 1) >= 3", "not abs(Ar40 - Ar39) < 1 and age > $limit",
                        "Ar40/Ar39 * 2 > 1", "(Ar40 / 2) / Ar39 > 1", "between(Ar40 - 1, -5, 5)"}) {
    const auto once = canon(s);
    EXPECT_EQ(canon(once), once) << s;
  }
}

TEST(ConditionalsGrammar, RatioVersusDivision) {
  auto r = parse_expression("Ar40/Ar36 > 295");
  ASSERT_TRUE(r);
  EXPECT_EQ((*r)->children[0]->kind, Expr::Kind::Metric);
  EXPECT_EQ((*r)->children[0]->metric.kind, MetricRef::Kind::Ratio);
  // Not two bare names: arithmetic.
  for (const char* s : {"Ar40.cur/Ar36 > 1", "Ar40/2 > 1", "age/Ar40 > 1", "Ar40/abs(Ar36) > 1"}) {
    auto d = parse_expression(s);
    ASSERT_TRUE(d) << s;
    EXPECT_EQ((*d)->children[0]->kind, Expr::Kind::Div) << s;
  }
  EXPECT_TRUE(parse_expression("slope(Ar40/Ar39) > 0"));
  EXPECT_FALSE(parse_expression("slope(Ar40/2) > 0"));  // a series function needs a metric
}

TEST(ConditionalsGrammar, LegacyFieldAliasesCanonicalize) {
  EXPECT_EQ(canon("Ar40.current > 1"), "Ar40.cur > 1");
  EXPECT_EQ(canon("Ar40.sd > 1"), "Ar40.std_dev > 1");
  EXPECT_EQ(canon("Ar40.stddev > 1"), "Ar40.std_dev > 1");
  EXPECT_EQ(canon("Ar40.std_dev > 1"), "Ar40.std_dev > 1");
  EXPECT_EQ(canon("Ar40.intercept > 1"), "Ar40.intercept > 1");
}

TEST(ConditionalsGrammar, QualifiedDetectorNames) {
  auto e = parse_expression("L2(CDD).deflection == 3250 or not AX(CDD).inactive");
  ASSERT_TRUE(e) << e.error().what;
  const auto m = metrics_of(**e);
  ASSERT_EQ(m.size(), 2u);
  EXPECT_EQ(m[0].kind, MetricRef::Kind::DetectorField);
  EXPECT_EQ(m[0].a, "L2(CDD)");
  EXPECT_EQ(m[0].field, "deflection");
  EXPECT_EQ(to_string(m[1]), "AX(CDD).inactive");
  EXPECT_EQ(canon("L2(CDD).deflection==3250"), "L2(CDD).deflection == 3250");
  EXPECT_FALSE(parse_expression("L2(CDD) > 1"));        // needs a detector field
  EXPECT_FALSE(parse_expression("L2(CDD).bs_corrected > 1"));
  EXPECT_TRUE(parse_expression("max(Ar40) > 1"));       // functions are not qualifiers
}

TEST(ConditionalsGrammar, ComputedMetrics) {
  for (auto name : computed_metric_names()) {
    auto e = parse_expression(std::string(name) + " > 1");
    ASSERT_TRUE(e) << name;
    EXPECT_EQ((*e)->children[0]->metric.kind, MetricRef::Kind::Computed) << name;
    EXPECT_EQ((*e)->children[0]->metric.a, name);
  }
  EXPECT_EQ(canon("rad40_percent>50 and instant_age<1e4"), "rad40_percent > 50 and instant_age < 10000");
}

TEST(ConditionalsGrammar, ArithmeticAndAbsEvaluate) {
  MapContext ctx;
  ctx.series_data["Ar40"] = {100};
  ctx.series_data["Ar39"] = {10};
  ctx.series_data["Ar36"] = {-3, -1, 2};
  EXPECT_DOUBLE_EQ(eval("Ar40 + Ar39 * 2", ctx), 120);
  EXPECT_DOUBLE_EQ(eval("(Ar40 - Ar39) / 9", ctx), 10);
  EXPECT_DOUBLE_EQ(eval("-Ar39", ctx), -10);
  EXPECT_DOUBLE_EQ(eval("abs(min(Ar36))", ctx), 3);
  EXPECT_DOUBLE_EQ(eval("abs(Ar40 - 150) == 50", ctx), 1);
  EXPECT_DOUBLE_EQ(eval("Ar40 > Ar39 and Ar39 > 0", ctx), 1);  // metric vs metric
  auto e = parse_expression("Ar40 / (Ar39 - 10) > 1");
  ASSERT_TRUE(e);
  auto r = evaluate(**e, ctx, {});
  ASSERT_FALSE(r);
  EXPECT_NE(r.error().what.find("division by zero"), std::string::npos);
}

TEST(ConditionalsGrammar, MetricsAndVariablesOf) {
  auto e = parse_expression("Ar40 > $min and average(Ar40, window=3) < Ar39/Ar36 or gauge.ig.pressure > $max");
  ASSERT_TRUE(e);
  const auto m = metrics_of(**e);
  ASSERT_EQ(m.size(), 3u);  // Ar40 once
  EXPECT_EQ(to_string(m[1]), "Ar39/Ar36");
  EXPECT_EQ(variables_of(**e), (std::vector<std::string>{"min", "max"}));
}

TEST(ConditionalsGrammar, WindowTransform) {
  auto e = parse_expression("Ar40 > 10 and slope(Ar39) < 1 and max(Ar36, window=2) < 5 and Ar40.cur > 0");
  ASSERT_TRUE(e);
  EXPECT_EQ(to_string(*apply_window(**e, 7)),
            "average(Ar40, window=7) > 10 and slope(Ar39, window=7) < 1 and max(Ar36, window=2) < 5 and "
            "Ar40.cur > 0");
}

TEST(ConditionalsGrammar, MapperTransform) {
  auto e = parse_expression("Ar40 > 900 and max(Ar39) < 5");
  ASSERT_TRUE(e);
  auto m = apply_mapper(**e, "x+1000");
  ASSERT_TRUE(m) << m.error().what;
  EXPECT_EQ(to_string(**m), "Ar40 + 1000 > 900 and max(Ar39) + 1000 < 5");
  auto m2 = apply_mapper(**e, "2*x");
  ASSERT_TRUE(m2);
  EXPECT_EQ(to_string(**m2), "2 * Ar40 > 900 and 2 * max(Ar39) < 5");
  for (const char* bad : {"y + 1", "x > 1", "1000", "x +"}) EXPECT_FALSE(apply_mapper(**e, bad)) << bad;
}

TEST(ConditionalsGrammar, CloneIsDeep) {
  auto e = parse_expression("between(Ar40 * 2, 1, 3) or not H1.inactive");
  ASSERT_TRUE(e);
  auto c = clone(**e);
  EXPECT_EQ(to_string(*c), to_string(**e));
  EXPECT_NE(c->children[0].get(), (*e)->children[0].get());
}

}  // namespace
