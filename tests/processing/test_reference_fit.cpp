// Blank and IC factor fits from references (design section 7.5, V3): the
// legacy interpolations, means and regressions, the figure and fit set for
// blanks and IC factors, the units in a pipeline, and finding references.

#include <gtest/gtest.h>

#include <cmath>

#include "fixtures.hpp"
#include "pychron/processing/reference_fit.hpp"
#include "pychron/processing/units.hpp"

namespace pychron::processing {
namespace {

std::vector<ReferencePoint> points() {
  return {{0, {1, 0.1}, "a", "r-1", false},
          {10, {3, 0.2}, "b", "r-2", false},
          {15, {100, 1}, "x", "r-x", true},  // excluded
          {20, {5, 0.1}, "c", "r-3", false}};
}

Value at(ReferenceFitKind kind, double t, ReferenceErrorKind e = ReferenceErrorKind::Sem) {
  auto m = ReferenceModel::make(points(), kind, e);
  EXPECT_TRUE(m) << (m ? "" : m.error().what);
  auto v = m->at(t);
  EXPECT_TRUE(v);
  return *v;
}

TEST(ReferenceModel, Interpolations) {
  using K = ReferenceFitKind;
  EXPECT_EQ(at(K::Preceding, 12), (Value{3, 0.2}));
  EXPECT_EQ(at(K::Preceding, 10), (Value{3, 0.2}));
  EXPECT_EQ(at(K::Preceding, -5), (Value{1, 0.1}));  // before the first: the first
  EXPECT_EQ(at(K::Preceding, 25), (Value{5, 0.1}));
  EXPECT_EQ(at(K::Succeeding, 12), (Value{5, 0.1}));
  EXPECT_EQ(at(K::Succeeding, 25), (Value{5, 0.1}));  // after the last: the last
  const Value avg = at(K::BracketingAverage, 12);
  EXPECT_DOUBLE_EQ(avg.value, 4.0);
  EXPECT_DOUBLE_EQ(avg.error, std::hypot(0.2, 0.1) / 2.0);
  const Value lin = at(K::BracketingInterpolate, 12);
  EXPECT_DOUBLE_EQ(lin.value, 3.4);
  EXPECT_DOUBLE_EQ(lin.error, std::hypot(0.8 * 0.2, 0.2 * 0.1));
  EXPECT_EQ(at(K::BracketingInterpolate, 10), (Value{3, 0.2}));
  EXPECT_EQ(at(K::BracketingInterpolate, -5), (Value{1, 0.1}));
  EXPECT_EQ(at(K::BracketingAverage, 30), (Value{5, 0.1}));
}

TEST(ReferenceModel, Means) {
  const Value sem = at(ReferenceFitKind::Average, 0);
  EXPECT_DOUBLE_EQ(sem.value, 3.0);
  EXPECT_NEAR(sem.error, 2.0 / std::sqrt(3.0), 1e-12);
  EXPECT_NEAR(at(ReferenceFitKind::Average, 0, ReferenceErrorKind::Sd).error, 2.0, 1e-12);
  const Value w = at(ReferenceFitKind::WeightedMean, 0);
  const double w1 = 100, w2 = 25, w3 = 100;
  EXPECT_NEAR(w.value, (1 * w1 + 3 * w2 + 5 * w3) / (w1 + w2 + w3), 1e-12);
  EXPECT_NEAR(w.error, 1.0 / std::sqrt(w1 + w2 + w3), 1e-12);
  auto m = ReferenceModel::make(points(), ReferenceFitKind::WeightedMean, ReferenceErrorKind::Msem);
  ASSERT_TRUE(m && m->mswd());
  EXPECT_GT(*m->mswd(), 1.0);
  EXPECT_NEAR(m->at(0)->error, w.error * std::sqrt(*m->mswd()), 1e-12);
}

TEST(ReferenceModel, RegressionsAndFailures) {
  std::vector<ReferencePoint> line{{0, {1, 0.1}, "a", "", false}, {3600, {2, 0.1}, "b", "", false},
                                   {7200, {3, 0.1}, "c", "", false}};
  auto m = ReferenceModel::make(line, ReferenceFitKind::Linear, ReferenceErrorKind::Sem);
  ASSERT_TRUE(m) << m.error().what;
  EXPECT_NEAR(m->at(10800)->value, 4.0, 1e-9);
  EXPECT_NEAR(m->at(1800)->value, 1.5, 1e-9);
  EXPECT_FALSE(ReferenceModel::make({line[0], line[1]}, ReferenceFitKind::Cubic, ReferenceErrorKind::Sem));
  auto all_out = points();
  for (auto& p : all_out) p.excluded = true;
  auto none = ReferenceModel::make(all_out, ReferenceFitKind::Preceding, ReferenceErrorKind::Sem);
  ASSERT_FALSE(none);
  EXPECT_NE(none.error().what.find("no references"), std::string::npos);
  std::vector<ReferencePoint> zero{{0, {1, 0}, "a", "", false}};
  EXPECT_FALSE(ReferenceModel::make(zero, ReferenceFitKind::WeightedMean, ReferenceErrorKind::Sem));
}

TEST(ReferenceModel, Spellings) {
  for (const char* k : {"preceding", "succeeding", "bracketing_average", "bracketing_interpolate", "average",
                        "weighted_mean", "linear", "parabolic", "cubic", "exponential"})
    EXPECT_EQ(to_string(*parse_reference_fit(k)), k);
  EXPECT_FALSE(parse_reference_fit("Linear"));
  EXPECT_EQ(parse_reference_error("MSEM"), ReferenceErrorKind::Msem);
  EXPECT_TRUE(is_interpolation(ReferenceFitKind::Succeeding));
  EXPECT_FALSE(is_interpolation(ReferenceFitKind::Average));
}

// Blanks 0 and 4 (hours 0 and 4) bracket unknowns 1..3; blank i has Ar40 =
// 10 x (295.5 + i) + 0.01, so its baseline-corrected Ar40 is 2955 + 10 i.
Dataset blanks_dataset(bool exclude_first = false) {
  Dataset d;
  for (int i : {0, 4}) {
    auto a = test::make_air(i, 295.5 + i, "blank_unknown", "bu");
    DatasetItem item{reduce_analysis(a, {}), {}, {}};
    if (i == 0 && exclude_first) item.exclusion.user = true;
    d.mutable_items().push_back(item);
  }
  return d;
}

Dataset unknowns_dataset() {
  Dataset d;
  for (int i : {1, 2, 3}) {
    auto a = test::make_unknown(i);
    a->heads = {{"blanks", "bk-" + a->uuid}, {"icfactors", "ic-" + a->uuid}};
    a->isotopes[0].blank = {0.5, 0.05};
    d.mutable_items().push_back(DatasetItem{reduce_analysis(a, {}), {}, {}});
  }
  d.mutable_items()[2].exclusion.user = true;  // not fitted
  return d;
}

TEST(ReferenceFigure, BlanksPredictEachUnknown) {
  Options o(blank_fit_schema());
  auto fig = build_reference_figure(ReferenceFitTarget::Blanks, unknowns_dataset(), blanks_dataset(), o);
  ASSERT_TRUE(fig) << fig.error().what;
  ASSERT_EQ(fig->scene.graphs.size(), 1u);
  ASSERT_EQ(fig->scene.graphs[0].panels.size(), 5u);
  EXPECT_EQ(fig->scene.graphs[0].panels[0].y.title, "Ar40 blank (fA)");
  const auto& fits = fig->fits;
  EXPECT_EQ(fits.target, ReferenceFitTarget::Blanks);
  ASSERT_EQ(fits.analyses.size(), 2u);  // the excluded unknown is not fitted
  const auto& first = fits.analyses[0];
  EXPECT_EQ(first.heads.at("blanks"), "bk-uuid-U1-1");
  ASSERT_EQ(first.rows.size(), 5u);
  EXPECT_EQ(first.rows[0].key, "Ar40");
  EXPECT_EQ(first.rows[0].fit, ReferenceFitKind::Preceding);
  EXPECT_NEAR(first.rows[0].value.value, 2955.0, 1e-9);  // blank 0
  ASSERT_EQ(first.rows[0].references.size(), 2u);
  EXPECT_FALSE(first.rows[0].references[0].excluded);
  EXPECT_EQ(fits.message(), "<BLANKS> fits=Ar40(preceding),Ar39(preceding),Ar38(preceding),Ar37(preceding),Ar36(preceding)");

  // References are clickable by uuid; the unknowns' current values show.
  const auto& layers = fig->scene.graphs[0].panels[0].layers;
  int with_refs = 0, current = 0;
  for (const auto& l : layers)
    if (const auto* p = std::get_if<PointLayer>(&l)) {
      if (p->label == "references") {
        ++with_refs;
        ASSERT_EQ(p->refs.size(), 2u);
        EXPECT_EQ(p->refs[0].analysis, "uuid-bu-0");
        EXPECT_DOUBLE_EQ(p->x[1], 0.0);  // the newest run is hour 0
        EXPECT_DOUBLE_EQ(p->x[0], -4.0);
      }
      if (p->label == "current") {
        ++current;
        EXPECT_EQ(p->y[0], 0.5);
      }
    }
  EXPECT_EQ(with_refs, 1);
  EXPECT_EQ(current, 1);

  // Excluding blank 0: preceding falls back to the first included, blank 4.
  auto ex = build_reference_figure(ReferenceFitTarget::Blanks, unknowns_dataset(), blanks_dataset(true), o);
  ASSERT_TRUE(ex);
  EXPECT_NEAR(ex->fits.analyses[0].rows[0].value.value, 2995.0, 1e-9);
  EXPECT_TRUE(ex->fits.analyses[0].rows[0].references[0].excluded);

  // An interpolation between the two.
  auto rows = o.rows("isotopes");
  ASSERT_TRUE(rows[0].set("fit", std::string("bracketing_interpolate")));
  rows.resize(1);
  ASSERT_TRUE(o.set_rows("isotopes", rows));
  auto interp = build_reference_figure(ReferenceFitTarget::Blanks, unknowns_dataset(), blanks_dataset(), o);
  ASSERT_TRUE(interp);
  EXPECT_NEAR(interp->fits.analyses[0].rows[0].value.value, 2955.0 + 40.0 / 4.0, 1e-9);  // hour 1 of 4
  EXPECT_NEAR(interp->fits.analyses[1].rows[0].value.value, 2955.0 + 40.0 / 2.0, 1e-9);
  EXPECT_EQ(interp->fits.message(), "<BLANKS> fits=Ar40(bracketing_interpolate)");

  // Too few references for the fit: a warning, nothing to save.
  ASSERT_TRUE(rows[0].set("fit", std::string("cubic")));
  ASSERT_TRUE(o.set_rows("isotopes", rows));
  auto few = build_reference_figure(ReferenceFitTarget::Blanks, unknowns_dataset(), blanks_dataset(), o);
  ASSERT_TRUE(few);
  EXPECT_TRUE(few->fits.analyses.empty());
  ASSERT_EQ(few->scene.warnings.size(), 1u);
  EXPECT_NE(few->scene.warnings[0].find("Ar40: cubic needs 4"), std::string::npos) << few->scene.warnings[0];
}

TEST(ReferenceFigure, IcFactorsFromAirRatios) {
  // Airs with Ar40/Ar36 = 295.5 and 300.0 on H1 / CDD.
  Dataset airs;
  for (int i : {0, 4}) {
    auto a = test::make_air(i, i == 0 ? 295.5 : 300.0);
    airs.mutable_items().push_back(DatasetItem{reduce_analysis(a, {}), {}, {}});
  }
  Options o(icfactor_fit_schema());
  auto fig = build_reference_figure(ReferenceFitTarget::IcFactors, unknowns_dataset(), airs, o);
  ASSERT_TRUE(fig) << fig.error().what;
  ASSERT_EQ(fig->fits.analyses.size(), 2u);
  const auto& row = fig->fits.analyses[0].rows.at(0);
  EXPECT_EQ(row.key, "CDD");
  EXPECT_EQ(row.reference_detector, "H1");
  EXPECT_EQ(row.standard_ratio, 295.5);
  EXPECT_EQ(row.fit, ReferenceFitKind::Average);
  // (N/D)/295.5 of the two airs: 1 and 300/295.5; their average.
  EXPECT_NEAR(row.value.value, (1.0 + 300.0 / 295.5) / 2.0, 1e-9);
  EXPECT_EQ(fig->fits.message(), "<ICFactor> fits=CDD(average)");
  EXPECT_EQ(fig->scene.graphs[0].panels[0].quantity, "H1/CDD");

  // A detector no unknown has: nothing to save, no failure.
  auto rows = o.rows("ratios");
  ASSERT_TRUE(rows[0].set("denominator", std::string("XX")));
  ASSERT_TRUE(o.set_rows("ratios", rows));
  auto none = build_reference_figure(ReferenceFitTarget::IcFactors, unknowns_dataset(), airs, o);
  ASSERT_TRUE(none);
  EXPECT_TRUE(none->fits.analyses.empty());
}

TEST(ReferenceFitUnits, RunInAPipeline) {
  MemorySource src;
  std::vector<std::string> unknown_ids, ref_ids;
  for (int i : {1, 2}) {
    auto a = test::make_unknown(i);
    unknown_ids.push_back(a->uuid);
    src.add(a);
  }
  for (int i : {0, 3}) {
    auto a = test::make_air(i, 295.5, "blank_unknown", "bu");
    ref_ids.push_back(a->uuid);
    src.add(a);
  }
  const auto& reg = UnitRegistry::builtin();
  Pipeline p;
  auto& u = p.add(reg, "unknowns", "select");
  ASSERT_TRUE(u.options.set("uuids", unknown_ids));
  p.add(reg, "reduce_u", "reduce", {"unknowns"});
  auto& r = p.add(reg, "references", "select");
  ASSERT_TRUE(r.options.set("uuids", ref_ids));
  p.add(reg, "reduce_r", "reduce", {"references"});
  auto& e = p.add(reg, "reference_edits", "edits", {"reduce_r"});
  ASSERT_TRUE(e.options.set("exclude", std::vector<std::string>{ref_ids[0]}));
  p.add(reg, "fit", "blank_fit", {"reduce_u", "reference_edits"});
  ASSERT_TRUE(p.validate(reg)) << p.validate(reg).error().what;
  Runner runner(reg, &src);
  auto out = runner.run(p, "fit");
  ASSERT_TRUE(out) << out.error().what;
  ASSERT_EQ(out->size(), 2u);
  EXPECT_EQ(port_type((*out)[1]), PortType::ReferenceFits);
  const auto& fits = *std::get<ReferenceFitSetPtr>((*out)[1]);
  ASSERT_EQ(fits.analyses.size(), 2u);
  EXPECT_TRUE(fits.analyses[0].rows[0].references[0].excluded);
  EXPECT_EQ(to_string(PortType::ReferenceFits), "reference_fits");
  EXPECT_TRUE(reg.find("icfactor_fit"));
}

TEST(FindReferences, WindowsTypesSpectrometersAndTags) {
  MemorySource src;
  auto add = [&](int hour, const std::string& type, const std::string& ms, const std::string& tag = "ok") {
    auto a = test::make_air(hour, 295.5, type, type + std::to_string(hour));
    a->mass_spectrometer = ms;
    a->tag = tag;
    src.add(a);
    return a->uuid;
  };
  const auto u1 = add(10, "unknown", "jan");
  const auto u2 = add(100, "unknown", "jan");
  const auto near1 = add(5, "blank_unknown", "jan");
  const auto near2 = add(104, "blank_air", "jan");
  add(50, "blank_unknown", "jan");             // between the windows
  add(11, "blank_unknown", "obama");           // another spectrometer
  add(12, "blank_unknown", "jan", "invalid");  // invalid
  add(9, "air", "jan");                        // not a blank
  std::vector<AnalysisPtr> unknowns{*src.load(u1), *src.load(u2)};
  ReferenceQuery q;
  q.analysis_types = default_reference_types(ReferenceFitTarget::Blanks);
  q.hours = 6;
  auto found = find_references(src, unknowns, q);
  ASSERT_TRUE(found) << found.error().what;
  EXPECT_EQ(*found, (std::vector<std::string>{near2, near1}));
  q.same_mass_spectrometer = false;
  EXPECT_EQ(find_references(src, unknowns, q)->size(), 3u);
  q.analysis_types = default_reference_types(ReferenceFitTarget::IcFactors);
  EXPECT_EQ(find_references(src, unknowns, q)->size(), 1u);
  q.analysis_types = {"unknown"};
  EXPECT_TRUE(find_references(src, unknowns, q)->empty());  // the unknowns themselves are skipped
  EXPECT_TRUE(find_references(src, {}, q)->empty());
}

}  // namespace
}  // namespace pychron::processing
