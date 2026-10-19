// Blank and IC factor fits from references (design section 7.5, V3): the
// legacy interpolations, means and regressions, the figure and fit set for
// blanks and IC factors, the units in a pipeline, and finding references.

#include <gtest/gtest.h>

#include <cmath>

#include "fixtures.hpp"
#include "pychron/processing/reference_fit.hpp"
#include "pychron/processing/units.hpp"
#include "pychron/reduction/stats.hpp"

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

TEST(ReferenceModel, WeightedPolynomials) {
  // Three references on y = 1 + x (hours) with small errors and a wild one
  // with a huge error: the weighted fit follows the three.
  std::vector<ReferencePoint> pts{{0, {1, 0.1}, "a", "", false},
                                  {3600, {2, 0.1}, "b", "", false},
                                  {7200, {3, 0.1}, "c", "", false},
                                  {10800, {10, 100}, "d", "", false}};
  auto w = ReferenceModel::make(pts, ReferenceFitKind::Linear, ReferenceErrorKind::Sem);
  ASSERT_TRUE(w) << w.error().what;
  EXPECT_TRUE(w->weighted());
  EXPECT_NEAR(w->at(14400)->value, 5.0, 1e-3);
  // A zero error anywhere: unweighted, pulled towards the wild point.
  pts[0].value.error = 0;
  auto ols = ReferenceModel::make(pts, ReferenceFitKind::Linear, ReferenceErrorKind::Sem);
  ASSERT_TRUE(ols);
  EXPECT_FALSE(ols->weighted());
  EXPECT_GT(ols->at(14400)->value, 6.0);
  // Exponential fits are never weighted.
  pts[0].value.error = 0.1;
  auto ex = ReferenceModel::make(pts, ReferenceFitKind::Exponential, ReferenceErrorKind::Sem);
  if (ex) {
    EXPECT_FALSE(ex->weighted());
  }
}

TEST(ReferenceModel, WeightedErrorKinds) {
  // On y = 1 + x exactly, sigma 0.1: at the center, SEM = 0.1 / sqrt(3).
  std::vector<ReferencePoint> pts{{0, {1, 0.1}, "a", "", false}, {3600, {2, 0.1}, "b", "", false},
                                  {7200, {3, 0.1}, "c", "", false}};
  auto err = [&](ReferenceErrorKind k) { return ReferenceModel::make(pts, ReferenceFitKind::Linear, k)->at(3600)->error; };
  const double sem = 0.1 / std::sqrt(3.0);
  EXPECT_NEAR(err(ReferenceErrorKind::Sem), sem, 1e-12);
  EXPECT_NEAR(err(ReferenceErrorKind::Msem), sem, 1e-12);  // MSWD 0
  EXPECT_NEAR(err(ReferenceErrorKind::Sd), sem, 1e-12);    // no scatter
  EXPECT_NEAR(err(ReferenceErrorKind::Ci), reduction::student_t_quantile(0.975, 1) * sem, 1e-9);
  // Away from the center: sigma^2 (1/n + (x - xbar)^2 / Sxx), x - xbar = 2 h.
  EXPECT_NEAR(ReferenceModel::make(pts, ReferenceFitKind::Linear, ReferenceErrorKind::Sem)->at(10800)->error,
              0.1 * std::sqrt(1.0 / 3.0 + 4.0 / 2.0), 1e-12);
  // Monte Carlo agrees with the propagated SEM within its sampling error,
  // and is reproducible.
  const double mc = err(ReferenceErrorKind::MonteCarlo);
  EXPECT_NEAR(mc, sem, 0.15 * sem);
  EXPECT_EQ(mc, err(ReferenceErrorKind::MonteCarlo));
  // Scatter: SD includes the residuals, MSEM scales by sqrt(MSWD).
  pts[1].value.value = 2.5;
  auto scattered = [&](ReferenceErrorKind k) {
    return ReferenceModel::make(pts, ReferenceFitKind::Linear, k);
  };
  const auto m = scattered(ReferenceErrorKind::Msem);
  ASSERT_TRUE(m && m->mswd());
  EXPECT_GT(*m->mswd(), 1.0);
  EXPECT_NEAR(m->at(3600)->error, sem * std::sqrt(*m->mswd()), 1e-12);
  EXPECT_GT(scattered(ReferenceErrorKind::Sd)->at(3600)->error, sem);
}

TEST(ReferenceModel, MeanErrorKinds) {
  std::vector<double> v{1, 3, 5}, e{0.1, 0.2, 0.1};
  const auto msem = reduction::arithmetic_mean(v, e, reduction::MeanErrorKind::Msem)->error;
  EXPECT_NEAR(at(ReferenceFitKind::Average, 0, ReferenceErrorKind::Ci).error,
              reduction::student_t_quantile(0.975, 2) * msem, 1e-12);
  const double wsem = reduction::weighted_mean(v, e, reduction::MeanErrorKind::Sem)->error;
  const double mc = at(ReferenceFitKind::WeightedMean, 0, ReferenceErrorKind::MonteCarlo).error;
  EXPECT_NEAR(mc, wsem, 0.15 * wsem);
  // Interpolations keep the references' errors whatever the kind.
  EXPECT_EQ(at(ReferenceFitKind::Preceding, 12, ReferenceErrorKind::MonteCarlo), (Value{3, 0.2}));
}

TEST(ReferenceModel, Spellings) {
  for (const char* k : {"preceding", "succeeding", "bracketing_average", "bracketing_interpolate", "average",
                        "weighted_mean", "linear", "parabolic", "cubic", "exponential"})
    EXPECT_EQ(to_string(*parse_reference_fit(k)), k);
  EXPECT_FALSE(parse_reference_fit("Linear"));
  EXPECT_EQ(parse_reference_error("MSEM"), ReferenceErrorKind::Msem);
  EXPECT_EQ(parse_reference_error("CI"), ReferenceErrorKind::Ci);
  EXPECT_EQ(parse_reference_error("MC"), ReferenceErrorKind::MonteCarlo);
  EXPECT_FALSE(parse_reference_error("sem"));
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

TEST(ReferenceFigure, SourceCorrectionGivesIcFactorsByMass) {
  Dataset airs;
  for (int i : {0, 4}) {
    auto a = test::make_air(i, i == 0 ? 295.5 : 300.0);
    airs.mutable_items().push_back(DatasetItem{reduce_analysis(a, {}), {}, {}});
  }
  Options o(icfactor_fit_schema());
  auto rows = o.rows("ratios");
  ASSERT_TRUE(rows[0].set("mode", std::string("source_correction")));
  ASSERT_TRUE(o.set_rows("ratios", rows));
  auto fig = build_reference_figure(ReferenceFitTarget::IcFactors, unknowns_dataset(), airs, o);
  ASSERT_TRUE(fig) << fig.error().what;
  const auto& out = fig->fits.analyses.at(0).rows;
  ASSERT_EQ(out.size(), 4u);  // Ar36 CDD, Ar37 L2, Ar38 L1, Ar39 AX
  const double v = (1.0 + 300.0 / 295.5) / 2.0;
  const double l = std::log(39.9624 / 35.9675);
  auto row = [&](const std::string& det) {
    return *std::find_if(out.begin(), out.end(), [&](const ReferenceRowFit& r) { return r.key == det; });
  };
  EXPECT_NEAR(row("CDD").value.value, v, 1e-12);  // k = 1 for Ar36
  EXPECT_NEAR(row("AX").value.value, std::pow(v, std::log(39.9624 / 38.964) / l), 1e-12);
  EXPECT_NEAR(row("L2").value.value, std::pow(v, std::log(39.9624 / 36.9668) / l), 1e-12);
  const double k38 = std::log(39.9624 / 37.9627) / l;
  EXPECT_NEAR(row("L1").value.error, k38 * std::pow(v, k38 - 1.0) * row("CDD").value.error, 1e-15);
  EXPECT_TRUE(row("AX").source_correction);
  EXPECT_EQ(row("AX").reference_detector, "H1");
}

TEST(ReferenceFigure, SkipReviewedKeepsReviewedValues) {
  Dataset unknowns = unknowns_dataset();
  auto reviewed = std::make_shared<Analysis>(*unknowns.items()[0].analysis->analysis);
  reviewed->isotopes[0].blank_reviewed = true;  // Ar40
  unknowns.mutable_items()[0].analysis = reduce_analysis(reviewed, {});
  Options o(blank_fit_schema());
  auto all = build_reference_figure(ReferenceFitTarget::Blanks, unknowns, blanks_dataset(), o);
  ASSERT_TRUE(all);
  EXPECT_EQ(all->fits.analyses[0].rows.size(), 5u);
  ASSERT_TRUE(o.set("skip_reviewed", true));
  auto skip = build_reference_figure(ReferenceFitTarget::Blanks, unknowns, blanks_dataset(), o);
  ASSERT_TRUE(skip);
  ASSERT_EQ(skip->fits.analyses[0].rows.size(), 4u);
  EXPECT_EQ(skip->fits.analyses[0].rows[0].key, "Ar39");
  EXPECT_EQ(skip->fits.analyses[1].rows.size(), 5u);
  bool noted = false;
  for (const auto& l : skip->scene.graphs[0].panels[0].layers)
    if (const auto* t = std::get_if<TextLayer>(&l))
      for (const auto& line : t->lines) noted = noted || line == "1 reviewed value(s) kept";
  EXPECT_TRUE(noted);
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

// The style fields the fit figures share with the other figures
// (figure_common.hpp, fit_figure_fields).
void set_a_style(Options& o) {
  ASSERT_TRUE(o.set("background", std::string("#101010")));
  ASSERT_TRUE(o.set("plot_background", std::string("#202020")));
  ASSERT_TRUE(o.set("show_grid", false));
  ASSERT_TRUE(o.set("show_legend", false));
  ASSERT_TRUE(o.set("legend_location", std::string("bottom_left")));
  ASSERT_TRUE(o.set("font.family", std::string("Courier")));
  ASSERT_TRUE(o.set("font.title", 20.0));
  ASSERT_TRUE(o.set("font.axis", 18.0));
  ASSERT_TRUE(o.set("font.tick", 16.0));
  ASSERT_TRUE(o.set("font.annotation", 14.0));
  ASSERT_TRUE(o.set("panel_spacing", std::int64_t{0}));
}

void expect_that_style(const SceneStyle& s) {
  EXPECT_EQ(s.background, (Color{0x10, 0x10, 0x10, 255}));
  EXPECT_EQ(s.plot_background, (Color{0x20, 0x20, 0x20, 255}));
  EXPECT_FALSE(s.grid);
  EXPECT_FALSE(s.legend);
  EXPECT_EQ(s.legend_corner, Corner::BottomLeft);
  EXPECT_EQ(s.fonts.family, "Courier");
  EXPECT_EQ(s.fonts.title, 20.0);
  EXPECT_EQ(s.fonts.axis_title, 18.0);
  EXPECT_EQ(s.fonts.tick, 16.0);
  EXPECT_EQ(s.fonts.annotation, 14.0);
  EXPECT_EQ(s.panel_spacing, 0);
}

void expect_the_default_style(const SceneStyle& s) {
  const SceneStyle d;
  EXPECT_EQ(s.background, d.background);
  EXPECT_EQ(s.plot_background, d.plot_background);
  EXPECT_EQ(s.grid, d.grid);
  EXPECT_EQ(s.legend, d.legend);
  EXPECT_EQ(s.legend_corner, d.legend_corner);
  EXPECT_EQ(s.fonts.family, d.fonts.family);
  EXPECT_EQ(s.fonts.title, d.fonts.title);
  EXPECT_EQ(s.fonts.axis_title, d.fonts.axis_title);
  EXPECT_EQ(s.fonts.tick, d.fonts.tick);
  EXPECT_EQ(s.fonts.annotation, d.fonts.annotation);
  EXPECT_EQ(s.panel_spacing, d.panel_spacing);
}

// Airs with Ar40/Ar36 = 295.5 and 300.0, as IcFactorsFromAirRatios has them.
Dataset airs_dataset() {
  Dataset airs;
  for (int i : {0, 4}) {
    auto a = test::make_air(i, i == 0 ? 295.5 : 300.0);
    airs.mutable_items().push_back(DatasetItem{reduce_analysis(a, {}), {}, {}});
  }
  return airs;
}

TEST(ReferenceFigure, TheSharedStyleOptionsReachTheScene) {
  for (const auto target : {ReferenceFitTarget::Blanks, ReferenceFitTarget::IcFactors}) {
    const bool blanks = target == ReferenceFitTarget::Blanks;
    Options o(blanks ? blank_fit_schema() : icfactor_fit_schema());
    set_a_style(o);
    auto fig = build_reference_figure(target, unknowns_dataset(), blanks ? blanks_dataset() : airs_dataset(), o);
    ASSERT_TRUE(fig) << fig.error().what;
    expect_that_style(fig->scene.style);
  }
}

TEST(ReferenceFigure, WithNoStyleSetTheSceneIsStyledAsItWas) {
  for (const auto target : {ReferenceFitTarget::Blanks, ReferenceFitTarget::IcFactors}) {
    const bool blanks = target == ReferenceFitTarget::Blanks;
    const Options o(blanks ? blank_fit_schema() : icfactor_fit_schema());
    auto fig = build_reference_figure(target, unknowns_dataset(), blanks ? blanks_dataset() : airs_dataset(), o);
    ASSERT_TRUE(fig) << fig.error().what;
    expect_the_default_style(fig->scene.style);
  }
}

// A preset written before the style fields existed has none of their keys.
TEST(ReferenceFigure, APresetSavedBeforeTheStyleFieldsStillLoads) {
  for (const auto& schema : {blank_fit_schema(), icfactor_fit_schema()}) {
    auto loaded = options_from_toml(schema, "nsigma = 2\nshow_current = false\n");
    ASSERT_TRUE(loaded) << loaded.error().what;
    EXPECT_TRUE(loaded->warnings.empty());
    EXPECT_EQ(loaded->options.get_int("nsigma"), 2);
    EXPECT_FALSE(loaded->options.get_bool("show_current"));
    EXPECT_TRUE(loaded->options.get_bool("show_grid"));
    EXPECT_EQ(loaded->options.get_string("legend_location"), "top_right");
  }
}


// Measurement plan v2, worked plans 7.3 and 7.7: one label measured on
// several detectors in one run gives that run's IC factors.
Analysis two_detector_run() {
  Analysis a;
  a.uuid = "u-ic";
  a.runid = "ic-01";
  a.timestamp = 1000;
  IsotopeData h1;
  h1.key = "H1:Ar40";
  h1.isotope = "Ar40";
  h1.detector = "H1";
  h1.intercept = {101, 0.3};
  h1.baseline = {1, 0.4};
  h1.optics = "ar_config_a";
  IsotopeData ax = h1;
  ax.key = "AX:Ar40";
  ax.detector = "AX";
  ax.intercept = {100, 0.6};
  ax.baseline = {2, 0.8};
  ax.optics = "ar_config_b";
  IsotopeData ar36;
  ar36.key = "Ar36";
  ar36.isotope = "Ar36";
  ar36.detector = "CDD";
  ar36.intercept = {1, 0.01};
  a.isotopes = {h1, ax, ar36};
  return a;
}

TEST(ReferenceFit, SameRunIcFactorFromTwoDetectors) {
  const auto pts = same_run_references(two_detector_run(), "Ar40", "H1");
  ASSERT_EQ(pts.size(), 1u);
  const auto& p = pts.front();
  EXPECT_EQ(p.detector, "AX");
  EXPECT_DOUBLE_EQ(p.value.value, 100.0 / 98.0);
  // Corrected H1 100 +- 0.5, AX 98 +- 1.0; relative errors add in quadrature.
  EXPECT_NEAR(p.value.error, 100.0 / 98.0 * std::hypot(0.5 / 100.0, 1.0 / 98.0), 1e-12);
  EXPECT_DOUBLE_EQ(p.t, 1000);
  EXPECT_EQ(p.uuid, "u-ic");
  EXPECT_EQ(p.runid, "ic-01");
  EXPECT_FALSE(p.excluded);

  EXPECT_TRUE(same_run_references(two_detector_run(), "Ar40", "L2").empty());  // reference not measured
  EXPECT_TRUE(same_run_references(two_detector_run(), "Ar36", "CDD").empty());  // one detector only
}

TEST(ReferenceFit, SameRunIcFactorKeepsOptics) {
  const auto pts = same_run_references(two_detector_run(), "Ar40", "H1");
  ASSERT_EQ(pts.size(), 1u);
  EXPECT_EQ(pts.front().optics, "ar_config_b");  // the state the AX series was measured under

  // A model through such points keeps them, optics and all.
  auto m = ReferenceModel::make(pts, ReferenceFitKind::Average, ReferenceErrorKind::Sem);
  ASSERT_TRUE(m) << m.error().what;
  EXPECT_EQ(m->included().front().optics, "ar_config_b");
}

}  // namespace
}  // namespace pychron::processing
