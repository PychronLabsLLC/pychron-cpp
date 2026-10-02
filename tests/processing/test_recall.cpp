#include "pychron/processing/recall.hpp"

#include <gtest/gtest.h>

#include <cmath>

#include "fixtures.hpp"

namespace pp = pychron::processing;
namespace r = pychron::reduction;
using pp::test::make_air;
using pp::test::make_unknown;

namespace {

const pp::RecallRow* row(const pp::RecallSection& s, const std::string& name) {
  for (const auto& r : s.rows)
    if (r.name == name) return &r;
  return nullptr;
}

TEST(Recall, UnknownHasComputedValuesAndBudget) {
  auto ra = pp::reduce_analysis(make_unknown(0), {});
  const auto m = pp::make_recall_model(*ra);
  EXPECT_EQ(m.title, "U1-01  air  unknown");
  ASSERT_NE(row(m.computed, "Age"), nullptr);
  ASSERT_TRUE(row(m.computed, "Age")->value);
  EXPECT_GT(row(m.computed, "Age")->value->value, 0.0);
  ASSERT_TRUE(row(m.computed, "Age w/o J")->value);
  EXPECT_LT(row(m.computed, "Age w/o J")->value->error, row(m.computed, "Age")->value->error);
  ASSERT_NE(row(m.ratios, "Ar40/Ar36"), nullptr);
  ASSERT_EQ(m.isotopes.size(), 5u);
  EXPECT_EQ(m.isotopes[0].key, "Ar40");
  EXPECT_EQ(m.isotopes[0].stages.count(pp::Stage::InterferenceCorrected), 1u);
  EXPECT_NEAR(m.isotopes[0].stages.at(pp::Stage::Intercept).value, 1000.0, 1e-12);
  ASSERT_FALSE(m.error_budget.empty());
  double total = 0;
  for (const auto& c : m.error_budget) total += c.percent;
  EXPECT_NEAR(total, 100.0, 1e-6);
  EXPECT_GE(m.error_budget.front().percent, m.error_budget.back().percent);
  ASSERT_NE(row(m.identity, "J"), nullptr);
}

TEST(Recall, AirWithoutFluxExplainsMissingAge) {
  auto ra = pp::reduce_analysis(make_air(0), {});
  const auto m = pp::make_recall_model(*ra);
  ASSERT_NE(row(m.computed, "Age"), nullptr);
  EXPECT_FALSE(row(m.computed, "Age")->value);
  EXPECT_TRUE(m.error_budget.empty());
  EXPECT_NEAR(row(m.ratios, "Ar40/Ar36")->value->value, 295.5, 1e-9);
  ASSERT_NE(row(m.extraction, "Extract value"), nullptr);
  EXPECT_EQ(row(m.extraction, "Extract value")->units, "W");
  ASSERT_NE(row(m.spectrometer, "H1 gain"), nullptr);
}

TEST(Recall, EvolutionSceneRefitsTheSignal) {
  auto a = make_air(0);
  r::FitSpec linear;
  linear.kind = r::FitKind::Linear;
  a->isotopes[0].fit = linear;
  a->isotopes[0].intercept = {100.0, 0.1};
  pp::RawData raw;
  pp::RawSeries s;
  s.kind = pp::SeriesKind::Signal;
  s.key = "Ar40";
  s.detector = "H1";
  for (int i = 0; i < 20; ++i) {
    s.t.push_back(i * 5.0);
    s.v.push_back(100.0 - 0.1 * i * 5.0 + (i % 2 ? 0.01 : -0.01));
  }
  raw.series.push_back(s);
  pp::RawSeries b = s;
  b.kind = pp::SeriesKind::Baseline;
  b.key = "H1";
  raw.series.push_back(b);

  const auto sig = pp::make_evolution_scene(*a, raw, pp::SeriesKind::Signal);
  ASSERT_EQ(sig.graphs.size(), 1u);
  ASSERT_EQ(sig.graphs[0].panels.size(), 1u);
  const auto& p = sig.graphs[0].panels[0];
  const pp::LineLayer* line = nullptr;
  const pp::TextLayer* text = nullptr;
  for (const auto& l : p.layers) {
    if (const auto* x = std::get_if<pp::LineLayer>(&l)) line = x;
    if (const auto* x = std::get_if<pp::TextLayer>(&l)) text = x;
  }
  ASSERT_NE(line, nullptr);
  EXPECT_NEAR(line->y.front(), 100.0, 0.02);  // the intercept at t = 0
  EXPECT_NEAR(line->y.back(), 100.0 - 0.1 * 95.0, 0.02);
  ASSERT_NE(text, nullptr);
  EXPECT_NE(text->lines[0].find("linear"), std::string::npos);

  const auto base = pp::make_evolution_scene(*a, raw, pp::SeriesKind::Baseline);
  ASSERT_EQ(base.graphs[0].panels.size(), 1u);
  EXPECT_EQ(base.graphs[0].panels[0].id, "H1");
  const auto sniff = pp::make_evolution_scene(*a, raw, pp::SeriesKind::Sniff);
  EXPECT_TRUE(sniff.graphs[0].panels.empty());
  EXPECT_EQ(sniff.warnings.size(), 1u);
}

}  // namespace
