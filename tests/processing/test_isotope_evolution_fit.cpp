// Batch isotope-evolution refits (design section 7.5, V3): every included
// analysis refitted with the configured fits, left-out points kept,
// goodness flags, reviewed intercepts kept, the unit in a pipeline.

#include <gtest/gtest.h>

#include <atomic>

#include "fixtures.hpp"
#include "pychron/processing/isotope_evolution_fit.hpp"
#include "pychron/processing/units.hpp"

namespace pychron::processing {
namespace {

namespace r = pychron::reduction;

// Signals of every isotope: v = 2 x intercept - 0.5 t (rising when
// `rising`), a small wiggle, and a wild point at index 7.
RawData raw_for(const Analysis& a, bool rising = false) {
  RawData raw;
  for (const auto& iso : a.isotopes) {
    RawSeries s;
    s.kind = SeriesKind::Signal;
    s.key = iso.key;
    s.detector = iso.detector;
    for (int k = 0; k < 20; ++k) {
      s.t.push_back(k);
      s.v.push_back(2 * iso.intercept.value + (rising ? 0.5 : -0.5) * k + (k % 2 ? 0.01 : -0.01) + (k == 7 ? 40.0 : 0.0));
    }
    raw.series.push_back(s);
  }
  return raw;
}

struct Fixture {
  Dataset dataset;
  std::map<std::string, RawData> raw;
  RawLoader loader() const {
    return [this](const std::string& uuid) -> Result<RawData> {
      auto it = raw.find(uuid);
      if (it == raw.end()) return fail(ErrorKind::Config, "no raw data");
      return it->second;
    };
  }
};

Fixture make(int n, bool rising = false) {
  Fixture f;
  for (int i = 0; i < n; ++i) {
    auto a = test::make_air(i);
    a->heads = {{"intercepts", "int-" + a->uuid}};
    f.raw[a->uuid] = raw_for(*a, rising);
    f.dataset.mutable_items().push_back(DatasetItem{reduce_analysis(a, {}), {}, {}});
  }
  return f;
}

Options with_row(std::function<void(Options&)> edit, std::size_t rows = 1) {
  Options o(isotope_evolution_fit_schema());
  auto list = o.rows("isotopes");
  list.resize(rows);
  edit(list[0]);
  EXPECT_TRUE(o.set_rows("isotopes", list));
  return o;
}

TEST(IsotopeEvolutionFits, RefitsEveryIncludedAnalysis) {
  auto f = make(3);
  f.dataset.mutable_items()[2].exclusion.user = true;
  Options o(isotope_evolution_fit_schema());
  auto fig = build_isotope_evolution_fits(f.dataset, o, f.loader());
  ASSERT_TRUE(fig) << fig.error().what;
  const auto& fits = fig->fits;
  ASSERT_EQ(fits.analyses.size(), 2u);
  const auto& first = fits.analyses[0];
  EXPECT_EQ(first.heads.at("intercepts"), "int-uuid-A1-0");
  ASSERT_EQ(first.isotopes.size(), 5u);
  const Analysis& a0 = *f.dataset.items()[0].analysis->analysis;
  const auto& ar40 = first.isotopes[0];
  EXPECT_EQ(ar40.fit.key, "Ar40");
  EXPECT_EQ(ar40.fit.fit.kind, r::FitKind::Linear);
  EXPECT_EQ(ar40.stored, a0.isotopes[0].intercept);
  EXPECT_EQ(ar40.fit.n_points, 20);
  EXPECT_LT(ar40.slope, 0.0);
  ASSERT_TRUE(first.edited);
  EXPECT_EQ(first.edited->find_isotope("Ar40")->intercept, ar40.fit.value);
  EXPECT_TRUE(first.good());
  EXPECT_EQ(fits.message(), "<ISOEVO> refit Ar40(linear),Ar39(linear),Ar38(linear),Ar37(linear),Ar36(linear)");
  EXPECT_EQ(first.fits().size(), 5u);
  ASSERT_EQ(fig->scene.graphs.at(0).panels.size(), 5u);
  EXPECT_EQ(fig->scene.graphs[0].x.format, AxisFormat::Time);
  const auto& layers = fig->scene.graphs[0].panels[0].layers;
  bool clickable = false;
  for (const auto& l : layers)
    if (const auto* p = std::get_if<PointLayer>(&l); p && p->label == "refit") clickable = p->refs.at(0).analysis == "uuid-A1-0";
  EXPECT_TRUE(clickable);
}

TEST(IsotopeEvolutionFits, KeepsLeftOutPointsUnlessToldNot) {
  auto f = make(1);
  auto a = std::make_shared<Analysis>(*f.dataset.items()[0].analysis->analysis);
  a->isotopes[0].user_excluded = {7};  // the wild point
  f.dataset.mutable_items()[0].analysis = reduce_analysis(a, {});
  Options o = with_row([](Options& row) { (void)row.set("isotope", std::string("Ar40")); });
  auto kept = build_isotope_evolution_fits(f.dataset, o, f.loader());
  ASSERT_TRUE(kept);
  ASSERT_EQ(kept->fits.analyses.at(0).isotopes.size(), 1u);
  EXPECT_EQ(kept->fits.analyses[0].isotopes[0].fit.user_excluded, (std::vector<std::size_t>{7}));
  EXPECT_NEAR(kept->fits.analyses[0].isotopes[0].fit.value.value, 2 * a->isotopes[0].intercept.value, 0.02);
  ASSERT_TRUE(o.set("keep_user_excluded", false));
  auto all = build_isotope_evolution_fits(f.dataset, o, f.loader());
  ASSERT_TRUE(all);
  EXPECT_TRUE(all->fits.analyses[0].isotopes[0].fit.user_excluded.empty());
  EXPECT_GT(all->fits.analyses[0].isotopes[0].fit.value.value, kept->fits.analyses[0].isotopes[0].fit.value.value);
}

TEST(IsotopeEvolutionFits, GoodnessFlags) {
  auto f = make(2);
  // The outlier filter removes the wild point; one outlier is too many.
  Options outliers = with_row([](Options& row) {
    (void)row.set("filter_outliers", true);
    (void)row.set("max_outliers", 0.0);
  });
  auto o1 = build_isotope_evolution_fits(f.dataset, outliers, f.loader());
  ASSERT_TRUE(o1);
  ASSERT_EQ(o1->fits.flagged(), 2);
  const auto& flag = o1->fits.analyses[0].flags.at(0);
  EXPECT_EQ(flag.key, "Ar40");
  EXPECT_EQ(flag.check, "outliers");
  EXPECT_EQ(flag.value, 1.0);
  EXPECT_EQ(o1->fits.analyses[0].isotopes[0].outliers, 1u);

  Options error = with_row([](Options& row) { (void)row.set("max_percent_error", 1e-9); });
  auto o2 = build_isotope_evolution_fits(f.dataset, error, f.loader());
  ASSERT_TRUE(o2);
  EXPECT_EQ(o2->fits.analyses[0].flags.at(0).check, "percent_error");

  Options slope = with_row([](Options& row) { (void)row.set("max_slope", 0.1); });
  EXPECT_EQ(build_isotope_evolution_fits(f.dataset, slope, f.loader())->fits.flagged(), 0);  // falling signals
  auto rising = make(2, true);
  auto o3 = build_isotope_evolution_fits(rising.dataset, slope, rising.loader());
  ASSERT_TRUE(o3);
  ASSERT_EQ(o3->fits.flagged(), 2);
  EXPECT_EQ(o3->fits.analyses[0].flags.at(0).check, "slope");
  // 0.5 fA/s, less the wild point's pull, 40 (7 - 9.5) / Sxx, plus the
  // wiggle's, 0.01 (sum of odd k - sum of even k) / Sxx; Sxx = 665.
  EXPECT_NEAR(o3->fits.analyses[0].flags.at(0).value, 0.5 - 100.0 / 665.0 + 0.1 / 665.0, 1e-9);
}

TEST(IsotopeEvolutionFits, ReviewedMissingAndCancelled) {
  auto f = make(2);
  auto a = std::make_shared<Analysis>(*f.dataset.items()[0].analysis->analysis);
  a->isotopes[0].intercept_reviewed = true;
  f.dataset.mutable_items()[0].analysis = reduce_analysis(a, {});
  Options o(isotope_evolution_fit_schema());
  ASSERT_TRUE(o.set("skip_reviewed", true));
  auto fig = build_isotope_evolution_fits(f.dataset, o, f.loader());
  ASSERT_TRUE(fig);
  EXPECT_EQ(fig->fits.reviewed_kept, 1);
  EXPECT_EQ(fig->fits.analyses[0].isotopes.size(), 4u);
  EXPECT_EQ(fig->fits.analyses[0].isotopes[0].fit.key, "Ar39");
  EXPECT_EQ(fig->scene.warnings.back(), "1 reviewed intercept(s) kept");

  f.raw.erase("uuid-A1-1");
  auto missing = build_isotope_evolution_fits(f.dataset, Options(isotope_evolution_fit_schema()), f.loader());
  ASSERT_TRUE(missing);
  EXPECT_EQ(missing->fits.analyses.size(), 1u);
  ASSERT_EQ(missing->fits.warnings.size(), 1u);
  EXPECT_EQ(missing->fits.warnings[0], "A1-02: no raw data");

  std::atomic<bool> cancel{true};
  auto stopped = build_isotope_evolution_fits(f.dataset, Options(isotope_evolution_fit_schema()), f.loader(), &cancel);
  ASSERT_FALSE(stopped);
  EXPECT_EQ(stopped.error().kind, ErrorKind::Cancelled);
}

TEST(IsotopeEvolutionFits, KeysAndNames) {
  auto f = make(1);
  auto a = std::make_shared<Analysis>(*f.dataset.items()[0].analysis->analysis);
  a->isotopes[1].key = "AX:Ar39";
  RawData raw = f.raw.begin()->second;
  raw.series[1].key = "AX:Ar39";
  f.raw.begin()->second = raw;
  f.dataset.mutable_items()[0].analysis = reduce_analysis(a, {});
  auto by_name = build_isotope_evolution_fits(
      f.dataset, with_row([](Options& row) { (void)row.set("isotope", std::string("Ar39")); }), f.loader());
  ASSERT_TRUE(by_name);
  EXPECT_EQ(by_name->fits.analyses.at(0).isotopes.at(0).fit.key, "AX:Ar39");
  auto none = build_isotope_evolution_fits(
      f.dataset, with_row([](Options& row) { (void)row.set("isotope", std::string("Ar99")); }), f.loader());
  ASSERT_TRUE(none);
  EXPECT_TRUE(none->fits.analyses.empty());
}

TEST(IsotopeEvolutionFits, UnitReadsRawFromTheSource) {
  MemorySource src;
  std::vector<std::string> ids;
  for (int i = 0; i < 2; ++i) {
    auto a = test::make_air(i);
    ids.push_back(a->uuid);
    src.add(a, raw_for(*a));
  }
  const auto& reg = UnitRegistry::builtin();
  Pipeline p;
  auto& sel = p.add(reg, "select", "select");
  ASSERT_TRUE(sel.options.set("uuids", ids));
  p.add(reg, "reduce", "reduce", {"select"});
  p.add(reg, "fit", "isotope_evolution_fit", {"reduce"});
  Runner runner(reg, &src);
  auto out = runner.run(p, "fit");
  ASSERT_TRUE(out) << out.error().what;
  ASSERT_EQ(out->size(), 2u);
  EXPECT_EQ(port_type((*out)[1]), PortType::IsotopeFits);
  EXPECT_EQ(std::get<IsotopeFitSetPtr>((*out)[1])->analyses.size(), 2u);
  EXPECT_EQ(to_string(PortType::IsotopeFits), "isotope_fits");
}

}  // namespace
}  // namespace pychron::processing
