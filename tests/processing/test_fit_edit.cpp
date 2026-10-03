// Fit edits and revision diffs (data browsing and visualization design,
// section 11.3): refits with points left out, outliers in full-series
// indices, evolution point references, edit messages, the evolution scene's
// left-out layer, and table diffs.

#include <gtest/gtest.h>

#include "fixtures.hpp"
#include "pychron/processing/fit_edit.hpp"
#include "pychron/processing/recall.hpp"
#include "pychron/processing/revisions.hpp"

namespace pychron::processing {
namespace {

namespace r = pychron::reduction;

// y = 100 - 0.5 t with a small alternating wiggle, 20 points; index 7 is a
// wild point.
RawSeries signal(const std::string& key = "Ar40") {
  RawSeries s;
  s.kind = SeriesKind::Signal;
  s.key = key;
  s.detector = "H1";
  for (int i = 0; i < 20; ++i) {
    s.t.push_back(i);
    s.v.push_back(100.0 - 0.5 * i + (i % 2 ? 0.01 : -0.01) + (i == 7 ? 50.0 : 0.0));
  }
  return s;
}

r::FitSpec linear(bool outliers = false) {
  r::FitSpec f;
  f.kind = r::FitKind::Linear;
  f.outliers.enabled = outliers;
  f.outliers.iterations = 1;
  f.outliers.std_devs = 2.0;
  return f;
}

TEST(FitSeries, LeavesOutPointsAndReportsOutliersInFullIndices) {
  const auto s = signal();
  auto with = fit_series(s, linear(), {7});
  ASSERT_TRUE(with) << with.error().what;
  EXPECT_NEAR(with->intercept.value, 100.0, 0.01);
  EXPECT_EQ(with->intercept.n_used, 19u);
  EXPECT_TRUE(with->outliers.empty());

  auto filtered = fit_series(s, linear(true), {2, 3, 99});  // 99 is out of range: ignored
  ASSERT_TRUE(filtered) << filtered.error().what;
  EXPECT_EQ(filtered->outliers, (std::vector<std::size_t>{7}));
  EXPECT_EQ(filtered->intercept.n_used, 17u);

  auto shifted = fit_series(s, linear(), {7}, 10.0);
  ASSERT_TRUE(shifted);
  EXPECT_NEAR(shifted->intercept.value, 95.0, 0.01);

  std::vector<std::size_t> all(20);
  for (std::size_t i = 0; i < all.size(); ++i) all[i] = i;
  EXPECT_FALSE(fit_series(s, linear(), all));
}

TEST(FitEdits, ApplyRefitsOnACopy) {
  auto a = test::make_air(0);
  RawData raw;
  raw.series.push_back(signal());
  r::FitSpec avg;
  avg.kind = r::FitKind::Average;
  auto result = apply_fit_edits(*a, raw, {FitEdit{SeriesKind::Signal, "Ar40", linear(), {7}}});
  ASSERT_TRUE(result) << result.error().what;
  ASSERT_EQ(result->fits.size(), 1u);
  const auto& e = result->fits[0];
  EXPECT_NEAR(e.value.value, 100.0, 0.01);
  EXPECT_EQ(e.n_points, 20);
  EXPECT_EQ(e.n_used, 19);
  const IsotopeData* edited = result->analysis->find_isotope("Ar40");
  ASSERT_TRUE(edited);
  EXPECT_EQ(edited->intercept, e.value);
  EXPECT_EQ(edited->n, 19);
  EXPECT_EQ(edited->user_excluded, (std::vector<std::size_t>{7}));
  ASSERT_TRUE(edited->fit);
  EXPECT_EQ(edited->fit->kind, r::FitKind::Linear);
  EXPECT_EQ(a->find_isotope("Ar40")->intercept.value, 10.0 * 295.5 + 0.01);  // original untouched

  EXPECT_FALSE(apply_fit_edits(*a, raw, {FitEdit{SeriesKind::Signal, "Ar99", linear(), {}}}));
  auto missing = apply_fit_edits(*a, raw, {FitEdit{SeriesKind::Signal, "Ar39", avg, {}}});
  ASSERT_FALSE(missing);
  EXPECT_NE(missing.error().what.find("Ar39"), std::string::npos);
}

TEST(FitEdits, RefsTogglesAndMessages) {
  EXPECT_EQ(evolution_ref("H1:Ar40", 12), "H1:Ar40#12");
  auto ref = parse_evolution_ref("H1:Ar40#12");
  ASSERT_TRUE(ref);
  EXPECT_EQ(ref->first, "H1:Ar40");
  EXPECT_EQ(ref->second, 12u);
  for (const char* bad : {"Ar40", "#3", "Ar40#", "Ar40#x", "Ar40#3x", ""}) EXPECT_FALSE(parse_evolution_ref(bad)) << bad;

  std::vector<std::size_t> v{2, 9};
  toggle_index(v, 5);
  EXPECT_EQ(v, (std::vector<std::size_t>{2, 5, 9}));
  toggle_index(v, 2);
  EXPECT_EQ(v, (std::vector<std::size_t>{5, 9}));

  auto a = test::make_air(0);
  Analysis& before = *a;
  before.isotopes[0].fit = linear();
  EditedFit e;
  e.key = "Ar40";
  e.fit = linear();
  e.fit.kind = r::FitKind::Parabolic;
  e.user_excluded = {1, 2};
  EXPECT_EQ(describe_fit_edits(before, {e}), "<ISOEVO> Ar40 linear -> parabolic 2 excluded");
  e.fit = linear(true);
  e.fit.error = r::ErrorType::Sd;
  e.user_excluded.clear();
  EXPECT_EQ(describe_fit_edits(before, {e}), "<ISOEVO> Ar40 SD outliers 1x");
}

TEST(EvolutionScene, LeftOutPointsAreAReferencedLayerOfTheirOwn) {
  auto a = std::make_shared<Analysis>(*test::make_air(0));
  a->isotopes[0].fit = linear(true);
  a->isotopes[0].user_excluded = {3};
  RawData raw;
  raw.series.push_back(signal());
  const Scene scene = make_evolution_scene(*a, raw, SeriesKind::Signal);
  ASSERT_EQ(scene.graphs.size(), 1u);
  ASSERT_EQ(scene.graphs[0].panels.size(), 1u);
  std::vector<const PointLayer*> layers;
  for (const auto& l : scene.graphs[0].panels[0].layers)
    if (const auto* p = std::get_if<PointLayer>(&l)) layers.push_back(p);
  ASSERT_EQ(layers.size(), 2u);
  const PointLayer& in = *layers[0];
  const PointLayer& out = *layers[1];
  ASSERT_EQ(in.x.size(), 19u);
  ASSERT_EQ(out.refs.size(), 1u);
  EXPECT_EQ(out.refs[0].analysis, "Ar40#3");
  EXPECT_EQ(in.refs[3].analysis, "Ar40#4");
  // The wild point (series index 7, layer index 6) is the filter's outlier.
  for (std::size_t i = 0; i < in.excluded.size(); ++i) EXPECT_EQ(in.excluded[i], i == 6) << i;
}

TEST(FitEdits, BaselineEditsApplyToEveryIsotopeOnTheDetector) {
  auto a = std::make_shared<Analysis>(*test::make_air(0));
  a->isotopes[2].detector = "H1";  // Ar38 shares Ar40's detector
  RawData raw;
  RawSeries bs = signal("H1");
  bs.kind = SeriesKind::Baseline;
  raw.series.push_back(bs);
  r::FitSpec avg;
  avg.kind = r::FitKind::Average;
  auto stored = stored_fit(*a, SeriesKind::Baseline, "H1");
  ASSERT_TRUE(stored);
  EXPECT_EQ(stored->value, (Value{0.01, 0.001}));
  EXPECT_FALSE(stored_fit(*a, SeriesKind::Baseline, "XX"));
  EXPECT_FALSE(stored_fit(*a, SeriesKind::Sniff, "Ar40"));

  auto result = apply_fit_edits(*a, raw, {FitEdit{SeriesKind::Baseline, "H1", linear(), {7}}});
  ASSERT_TRUE(result) << result.error().what;
  ASSERT_EQ(result->fits.size(), 1u);
  EXPECT_EQ(result->fits[0].kind, SeriesKind::Baseline);
  EXPECT_NEAR(result->fits[0].value.value, 100.0, 0.01);
  for (const char* key : {"Ar40", "Ar38"}) {
    const IsotopeData* iso = result->analysis->find_isotope(key);
    EXPECT_NEAR(iso->baseline.value, 100.0, 0.01) << key;
    EXPECT_EQ(iso->baseline_user_excluded, (std::vector<std::size_t>{7})) << key;
    EXPECT_EQ(iso->baseline_fit->kind, r::FitKind::Linear) << key;
    EXPECT_EQ(iso->intercept, a->find_isotope(key)->intercept) << key;  // signals untouched
  }
  EXPECT_EQ(result->analysis->find_isotope("Ar39")->baseline, a->find_isotope("Ar39")->baseline);
  EXPECT_EQ(describe_fit_edits(*a, result->fits), "<ISOEVO> H1 baseline ? -> linear SEM no outlier filter 1 excluded");

  EXPECT_FALSE(apply_fit_edits(*a, raw, {FitEdit{SeriesKind::Baseline, "XX", avg, {}}}));
  EXPECT_FALSE(apply_fit_edits(*a, raw, {FitEdit{SeriesKind::Baseline, "AX", avg, {}}}));  // no raw baseline
  EXPECT_FALSE(apply_fit_edits(*a, raw, {FitEdit{SeriesKind::Sniff, "Ar40", avg, {}}}));
}

TEST(FitEdits, SameAsStored) {
  auto a = std::make_shared<Analysis>(*test::make_air(0));
  a->isotopes[0].fit = linear(true);
  a->isotopes[0].user_excluded = {2};
  FitEdit e{SeriesKind::Signal, "Ar40", linear(true), {2}};
  EXPECT_TRUE(same_as_stored(*a, e));
  e.fit.outliers.std_devs = 3;
  EXPECT_FALSE(same_as_stored(*a, e));
  e = FitEdit{SeriesKind::Signal, "Ar40", linear(true), {}};
  EXPECT_FALSE(same_as_stored(*a, e));
  // Without the filter its settings do not matter.
  a->isotopes[0].fit = linear(false);
  e = FitEdit{SeriesKind::Signal, "Ar40", linear(false), {2}};
  e.fit.outliers.iterations = 4;
  EXPECT_TRUE(same_as_stored(*a, e));
  EXPECT_FALSE(same_as_stored(*a, FitEdit{SeriesKind::Baseline, "H1", linear(), {}}));  // no stored baseline fit
}

TEST(EvolutionScene, BaselinePanelsDrawTheDetectorsFit) {
  auto a = std::make_shared<Analysis>(*test::make_air(0));
  a->isotopes[0].baseline_fit = linear();
  a->isotopes[0].baseline_user_excluded = {7};
  RawData raw;
  RawSeries bs = signal("H1");
  bs.kind = SeriesKind::Baseline;
  raw.series.push_back(bs);
  const Scene scene = make_evolution_scene(*a, raw, SeriesKind::Baseline);
  const auto& layers = scene.graphs[0].panels[0].layers;
  int lines = 0, points = 0;
  std::string text;
  for (const auto& l : layers) {
    lines += std::holds_alternative<LineLayer>(l);
    points += std::holds_alternative<PointLayer>(l);
    if (const auto* t = std::get_if<TextLayer>(&l)) text = t->lines.at(0);
  }
  EXPECT_EQ(lines, 1);
  EXPECT_EQ(points, 2);  // in the fit, and the left-out point
  EXPECT_EQ(text.rfind("linear SEM  Bs = ", 0), 0u) << text;
}

TEST(RevisionDiff, ChangedAddedRemovedAndColumnUnion) {
  RevisionTable before{{"value", "fit"}, {{"Ar40", {"100", "linear"}}, {"Ar39", {"10", "linear"}}, {"Ar36", {"1", "average"}}}};
  RevisionTable after{{"value", "fit", "excluded"},
                      {{"Ar40", {"100.5", "linear", "[7]"}}, {"Ar39", {"10", "linear", ""}}, {"Ar38", {"2", "linear", ""}}}};
  const auto d = diff_revisions(before, after);
  EXPECT_EQ(d.columns, (std::vector<std::string>{"value", "fit", "excluded"}));
  ASSERT_EQ(d.rows.size(), 4u);
  EXPECT_EQ(d.rows[0].key, "Ar40");
  EXPECT_EQ(d.rows[0].state, DiffState::Changed);
  EXPECT_EQ(d.rows[0].changed, (std::vector<bool>{true, false, true}));
  EXPECT_EQ(d.rows[1].state, DiffState::Same);
  EXPECT_EQ(d.rows[2].key, "Ar36");
  EXPECT_EQ(d.rows[2].state, DiffState::Removed);
  EXPECT_EQ(d.rows[2].before[1], "average");
  EXPECT_EQ(d.rows[3].key, "Ar38");
  EXPECT_EQ(d.rows[3].state, DiffState::Added);
  EXPECT_EQ(d.changed_rows(), 3);
  EXPECT_EQ(diff_revisions(after, after).changed_rows(), 0);
}

TEST(RevisionKinds, SpellingsRoundTrip) {
  for (auto k : kRevisionKinds) EXPECT_EQ(parse_revision_kind(to_string(k)), k);
  EXPECT_FALSE(parse_revision_kind("refpins"));
  EXPECT_EQ(title(RevisionKind::IcFactors), "IC factors");
}

}  // namespace
}  // namespace pychron::processing
