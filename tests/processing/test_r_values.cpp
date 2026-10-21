#include "pychron/processing/r_values.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "fixtures.hpp"
#include "pychron/processing/arar_figures.hpp"
#include "pychron/reduction/stats.hpp"
#include "pychron/reduction/ufloat.hpp"

namespace pp = pychron::processing;
namespace r = pychron::reduction;
using pp::test::make_step;

namespace {

// A reduced step whose F is set by hand, in `group` of `graph`.
pp::DatasetItem with_f(double f, double e, int group, int graph = 0) {
  static const pp::ReducedPtr base = pp::reduce_analysis(make_step(0, 20, 0.1), {});
  auto a = std::make_shared<pp::ReducedAnalysis>(*base);
  a->arar->f.f = r::UFloat::variable(f, e);
  pp::DatasetItem item;
  item.analysis = std::move(a);
  item.path.group = group;
  item.path.graph = graph;
  return item;
}

// A reduced step with no argon result, so no F.
pp::DatasetItem without_f(int group) {
  auto item = with_f(10, 0.1, group);
  auto a = std::make_shared<pp::ReducedAnalysis>(*item.analysis);
  a->arar.reset();
  a->reduction_error = "no argon";
  item.analysis = std::move(a);
  return item;
}

pp::Dataset dataset(std::vector<pp::DatasetItem> items, std::vector<std::string> names) {
  pp::Dataset d(std::move(items));
  d.group_names = std::move(names);
  return d;
}

r::Mean wmean(const std::vector<double>& v, const std::vector<double>& e, r::MeanErrorKind k = r::MeanErrorKind::Msem) {
  return *r::weighted_mean(v, e, k);
}

double ratio_err(double fi, double si, double fj, double sj) {
  const double rr = fj / fi;
  return std::abs(rr) * std::sqrt((si / fi) * (si / fi) + (sj / fj) * (sj / fj));
}

pp::Options options() { return pp::Options(pp::ideogram_schema()); }

}  // namespace

TEST(RValues, TwoGroupsWeightedSem) {
  auto d = dataset({with_f(10.0, 0.1, 0), with_f(10.2, 0.1, 0), with_f(12.0, 0.2, 1), with_f(12.1, 0.1, 1)},
                   {"A", "B"});
  auto o = options();
  ASSERT_TRUE(o.set("error_kind", std::string("sem")));
  auto m = pp::r_value_matrices(d, o);
  ASSERT_TRUE(m) << m.error().what;
  ASSERT_EQ(m->size(), 1u);
  const auto& x = (*m)[0];
  ASSERT_EQ(x.groups.size(), 2u);
  EXPECT_EQ(x.groups[0].name, "A");
  const auto a = wmean({10.0, 10.2}, {0.1, 0.1}, r::MeanErrorKind::Sem);
  const auto b = wmean({12.0, 12.1}, {0.2, 0.1}, r::MeanErrorKind::Sem);
  ASSERT_TRUE(x.groups[0].f && x.groups[1].f);
  EXPECT_DOUBLE_EQ(x.groups[0].f->value, a.value);
  EXPECT_DOUBLE_EQ(x.groups[0].f->error, a.error);
  ASSERT_TRUE(x.cells[0][1]);
  EXPECT_NEAR(x.cells[0][1]->r, b.value / a.value, 1e-12);
  EXPECT_NEAR(x.cells[0][1]->r_err, ratio_err(a.value, a.error, b.value, b.error), 1e-12);
  EXPECT_FALSE(x.cells[0][0]);
  EXPECT_FALSE(x.cells[1][1]);
}

TEST(RValues, Reciprocal) {
  auto d = dataset({with_f(10.0, 0.1, 0), with_f(12.0, 0.2, 1)}, {"A", "B"});
  auto m = pp::r_value_matrices(d, options());
  ASSERT_TRUE(m);
  const auto& c = (*m)[0].cells;
  ASSERT_TRUE(c[0][1] && c[1][0]);
  EXPECT_NEAR(c[0][1]->r * c[1][0]->r, 1.0, 1e-12);
  EXPECT_NEAR(c[0][1]->r_err / c[0][1]->r, c[1][0]->r_err / c[1][0]->r, 1e-12);
}

TEST(RValues, MsemExpandsWhenMswdAboveOne) {
  // Group 0 scattered (MSWD > 1), group 1 tight (MSWD < 1).
  auto d = dataset({with_f(10.0, 0.05, 0), with_f(11.0, 0.05, 0), with_f(10.5, 0.05, 0), with_f(12.0, 0.5, 1),
                    with_f(12.01, 0.5, 1)},
                   {"A", "B"});
  auto m = pp::r_value_matrices(d, options());  // msem by default
  ASSERT_TRUE(m);
  const auto& a = *(*m)[0].groups[0].f;
  const auto& b = *(*m)[0].groups[1].f;
  ASSERT_GT(a.mswd, 1.0);
  EXPECT_NEAR(a.error, a.sem * std::sqrt(a.mswd), 1e-15);
  ASSERT_LE(b.mswd, 1.0);
  EXPECT_DOUBLE_EQ(b.error, b.sem);
}

TEST(RValues, ArithmeticMeanFollowed) {
  auto d = dataset({with_f(10.0, 0.1, 0), with_f(11.0, 0.5, 0), with_f(12.0, 0.2, 1)}, {"A", "B"});
  auto o = options();
  ASSERT_TRUE(o.set("mean", std::string("arithmetic")));
  auto m = pp::r_value_matrices(d, o);
  ASSERT_TRUE(m);
  EXPECT_DOUBLE_EQ((*m)[0].groups[0].f->value, 10.5);
}

TEST(RValues, ExcludedAnalysisLeftOut) {
  auto d = dataset({with_f(10.0, 0.1, 0), with_f(10.0, 0.1, 0), with_f(15.0, 0.1, 0), with_f(12.0, 0.2, 1)},
                   {"A", "B"});
  d.mutable_items()[2].exclusion.user = true;
  auto m = pp::r_value_matrices(d, options());
  ASSERT_TRUE(m);
  EXPECT_DOUBLE_EQ((*m)[0].groups[0].f->value, 10.0);
  EXPECT_EQ((*m)[0].groups[0].f->n, 2u);
}

TEST(RValues, GroupWithoutF) {
  auto d = dataset({with_f(10.0, 0.1, 0), without_f(1), with_f(12.0, 0.2, 2)}, {"A", "B", "C"});
  auto m = pp::r_value_matrices(d, options());
  ASSERT_TRUE(m);
  const auto& x = (*m)[0];
  ASSERT_EQ(x.groups.size(), 3u);
  EXPECT_FALSE(x.groups[1].f);
  EXPECT_FALSE(x.groups[1].error.empty());
  for (int k = 0; k < 3; ++k) {
    EXPECT_FALSE(x.cells[1][k]);
    EXPECT_FALSE(x.cells[k][1]);
  }
  EXPECT_TRUE(x.cells[0][2]);
  EXPECT_TRUE(x.cells[2][0]);
}

TEST(RValues, UsesFWhileIdeogramPlotsAge) {
  pp::Dataset d;
  for (int i = 0; i < 3; ++i) {
    pp::DatasetItem it;
    it.analysis = pp::reduce_analysis(make_step(i, 20, 0.1, 10.0), {});
    d.mutable_items().push_back(it);
  }
  for (int i = 0; i < 3; ++i) {
    pp::DatasetItem it;
    it.analysis = pp::reduce_analysis(make_step(i, 20, 0.1, 12.0, "S2"), {});
    it.path.group = 1;
    d.mutable_items().push_back(it);
  }
  auto o = options();
  ASSERT_EQ(o.get_string("quantity"), "age");
  auto m = pp::r_value_matrices(d, o);
  ASSERT_TRUE(m);
  EXPECT_NEAR((*m)[0].groups[0].f->value, 10.0, 1e-6);
  EXPECT_NEAR((*m)[0].groups[1].f->value, 12.0, 1e-6);
  EXPECT_NEAR((*m)[0].cells[0][1]->r, 1.2, 1e-6);
}

TEST(RValues, TwoGraphs) {
  auto d = dataset({with_f(10.0, 0.1, 0, 0), with_f(12.0, 0.1, 1, 0), with_f(11.0, 0.1, 2, 1)}, {"A", "B", "C"});
  d.graph_names = {"G0", "G1"};
  auto m = pp::r_value_matrices(d, options());
  ASSERT_TRUE(m);
  ASSERT_EQ(m->size(), 2u);
  EXPECT_EQ((*m)[0].graph_name, "G0");
  EXPECT_EQ((*m)[0].groups.size(), 2u);
  EXPECT_EQ((*m)[1].graph, 1);
  EXPECT_EQ((*m)[1].groups.size(), 1u);
  EXPECT_EQ((*m)[1].groups[0].name, "C");
}

TEST(RValues, NegativeFKeepsAPositiveError) {
  auto d = dataset({with_f(-2.0, 0.5, 0), with_f(10.0, 0.1, 1)}, {"A", "B"});
  auto m = pp::r_value_matrices(d, options());
  ASSERT_TRUE(m);
  const auto& c = *(*m)[0].cells[0][1];
  EXPECT_LT(c.r, 0.0);
  EXPECT_GT(c.r_err, 0.0);
  EXPECT_TRUE(std::isfinite(c.r_err));
  EXPECT_NEAR(c.r_err, ratio_err(-2.0, 0.5, 10.0, 0.1), 1e-12);
}

TEST(RValues, ZeroErrorsLeaveTheGroupWithoutF) {
  auto d = dataset({with_f(10.0, 0.0, 0), with_f(10.0, 0.0, 0), with_f(12.0, 0.2, 1), with_f(11.0, 0.2, 2)},
                   {"A", "B", "C"});
  auto m = pp::r_value_matrices(d, options());
  ASSERT_TRUE(m);
  const auto& x = (*m)[0];
  EXPECT_FALSE(x.groups[0].f);
  EXPECT_FALSE(x.groups[0].error.empty());
  EXPECT_FALSE(x.cells[0][1]);
  EXPECT_FALSE(x.cells[1][0]);
  EXPECT_TRUE(x.cells[1][2]);
}

TEST(RValues, OneAnalysisGroup) {
  auto d = dataset({with_f(10.0, 0.3, 0), with_f(12.0, 0.2, 1)}, {"A", "B"});
  auto m = pp::r_value_matrices(d, options());
  ASSERT_TRUE(m);
  const auto& a = *(*m)[0].groups[0].f;
  EXPECT_DOUBLE_EQ(a.value, 10.0);
  EXPECT_NEAR(a.error, 0.3, 1e-15);
}

TEST(RValues, ZeroFDenominatorLeavesCellEmpty) {
  auto d = dataset({with_f(0.0, 0.1, 0), with_f(12.0, 0.2, 1)}, {"A", "B"});
  auto m = pp::r_value_matrices(d, options());
  ASSERT_TRUE(m);
  const auto& c = (*m)[0].cells;
  EXPECT_FALSE(c[0][1]);
  ASSERT_TRUE(c[1][0]);
  EXPECT_DOUBLE_EQ(c[1][0]->r, 0.0);
  // A zero numerator still has an error: sigma_num / F_den.
  EXPECT_NEAR(c[1][0]->r_err, 0.1 / 12.0, 1e-15);
}
