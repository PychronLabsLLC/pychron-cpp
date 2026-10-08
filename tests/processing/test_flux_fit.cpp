// fit_level on a hand-built level: the ring monitors of the flux golden data
// (three analyses each, F chosen so the arithmetic mean J is the golden J)
// and four unknowns at the golden prediction points.
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "flux_level_inputs.hpp"
#include "pychron/processing/flux_fit.hpp"

namespace pychron::processing {
namespace {

namespace pr = pychron::reduction;

using flux_test::analysis;
using flux_test::fc2;
using flux_test::level;
using flux_test::three;

FluxOptions plane(bool weighted, pr::MeanErrorKind error = pr::MeanErrorKind::Msem) {
  FluxOptions o;
  o.fit.kind = pr::ModelKind::Plane;
  o.fit.weighted = weighted;
  o.fit.error = error;
  return o;
}

LevelPosition& at_hole(LevelInputs& in, int hole) {
  return *std::find_if(in.positions.begin(), in.positions.end(), [&](auto& p) { return p.hole == hole; });
}
const FittedPosition& at_hole(const LevelFit& fit, int hole) {
  return *std::find_if(fit.positions.begin(), fit.positions.end(), [&](auto& p) { return p.hole == hole; });
}
bool has(const FittedPosition& p, PositionNote n) {
  return std::find(p.notes.begin(), p.notes.end(), n) != p.notes.end();
}

TEST(FluxFitLevel, TablesEqualTheMathLayer) {
  const auto in = level();
  auto fit = fit_level(in, plane(true), {});
  ASSERT_TRUE(fit) << fit.error().what;
  ASSERT_EQ(fit->positions.size(), 12u);

  // The math layer on the monitors' own mean J, as fit_level reports it.
  std::vector<pr::Monitor> monitors;
  std::vector<pr::Point> points;
  for (const auto& p : fit->positions) {
    points.push_back({p.x, p.y});
    if (p.monitor) {
      ASSERT_TRUE(p.mean_j && p.mean_j_err);
      EXPECT_NEAR(*p.mean_j, flux_golden::kRing[p.hole - 1].j, flux_golden::kRing[p.hole - 1].j * 1e-12);
      monitors.push_back({std::to_string(p.hole), {p.x, p.y}, *p.mean_j, *p.mean_j_err});
    }
  }
  const auto direct = pr::fit_flux(monitors, points, plane(true).fit);
  ASSERT_TRUE(direct) << direct.error().what;
  for (std::size_t i = 0; i < fit->positions.size(); ++i) {
    EXPECT_EQ(fit->positions[i].j, direct->at[i].j) << i;
    EXPECT_EQ(fit->positions[i].j_err, direct->at[i].j_err) << i;
  }
  EXPECT_EQ(fit->mswd, direct->mswd);
  EXPECT_EQ(fit->dof, direct->dof);
  EXPECT_EQ(fit->dof, 5);
  EXPECT_EQ(fit->parameters, direct->parameters);
  // The unknowns sit where the golden plane predicts, to the accuracy of the
  // mean errors (the fit weights differ from the golden sigma).
  EXPECT_NEAR(at_hole(*fit, 101).j, 0.0010001015, 1e-7);
  EXPECT_EQ(fit->options, plane(true));
  EXPECT_EQ(fit->monitor_set, in.monitor_set);
  EXPECT_EQ(fit->holder, "24-hole");
}

TEST(FluxFitLevel, MonitorsGetAPredictedJToo) {
  auto in = level();
  at_hole(in, 1).saved = SavedFlux{};
  at_hole(in, 1).saved->revision = "rev-1";
  at_hole(in, 1).saved->j = 0.00102;
  auto fit = fit_level(in, plane(false), {});
  ASSERT_TRUE(fit) << fit.error().what;
  const auto& m = at_hole(*fit, 1);
  EXPECT_TRUE(m.monitor);
  EXPECT_TRUE(m.used_in_fit);
  EXPECT_EQ(m.n, 3);
  ASSERT_TRUE(m.mean_j);
  EXPECT_NE(m.j, *m.mean_j);  // the model's J, not its own mean
  EXPECT_GT(m.j, 0);
  ASSERT_TRUE(m.dev_percent);
  EXPECT_NEAR(*m.dev_percent, (0.00102 - m.j) / m.j * 100.0, 1e-9);
  ASSERT_TRUE(m.saved_j);
  EXPECT_EQ(*m.saved_revision, "rev-1");
  EXPECT_FALSE(at_hole(*fit, 2).dev_percent);  // nothing saved there
  EXPECT_FALSE(at_hole(*fit, 2).saved_revision);
}

TEST(FluxFitLevel, TagsStartAnAnalysisOmitted) {
  auto in = level();
  auto& a = at_hole(in, 1).analyses;
  a[0].tag = "omit";
  a[1].tag = "Invalid";
  a[2].tag = "ok";
  at_hole(in, 2).analyses[0].tag = "outlier";
  at_hole(in, 3).analyses[0].tag = "skip";
  auto fit = fit_level(in, plane(false), {});
  ASSERT_TRUE(fit) << fit.error().what;
  const auto& p = at_hole(*fit, 1);
  EXPECT_EQ(p.n, 1);
  EXPECT_TRUE(p.analyses[0].omitted);
  EXPECT_TRUE(p.analyses[1].omitted);
  EXPECT_FALSE(p.analyses[2].omitted);
  EXPECT_EQ(at_hole(*fit, 2).n, 2);
  EXPECT_EQ(at_hole(*fit, 3).n, 2);
  EXPECT_EQ(at_hole(*fit, 4).n, 3);
}

TEST(FluxFitLevel, IncludeOverridesATagAndASavedOmission) {
  auto in = level();
  at_hole(in, 1).analyses[0].tag = "omit";
  at_hole(in, 2).saved = SavedFlux{};
  at_hole(in, 2).saved->omitted = {"M2-02"};
  Edits e;
  e.include = {"M1-01", "M2-02"};
  auto fit = fit_level(in, plane(false), e);
  ASSERT_TRUE(fit) << fit.error().what;
  EXPECT_EQ(at_hole(*fit, 1).n, 3);
  EXPECT_EQ(at_hole(*fit, 2).n, 3);

  Edits omit;  // an include wins over an omit for the same record
  omit.omit = {"M1-02"};
  omit.include = {"M1-02"};
  fit = fit_level(in, plane(false), omit);
  ASSERT_TRUE(fit) << fit.error().what;
  EXPECT_EQ(at_hole(*fit, 1).n, 2);  // M1-01 stays tagged out, M1-02 is back in
}

TEST(FluxFitLevel, SavedOmissionsAndExclusionsApplyUnlessReset) {
  auto in = level();
  at_hole(in, 1).saved = SavedFlux{};
  at_hole(in, 1).saved->omitted = {"M1-01"};
  at_hole(in, 2).saved = SavedFlux{};
  at_hole(in, 2).saved->used_in_fit = false;
  at_hole(in, 2).saved->excluded = true;

  auto fit = fit_level(in, plane(false), {});
  ASSERT_TRUE(fit) << fit.error().what;
  EXPECT_EQ(at_hole(*fit, 1).n, 2);
  EXPECT_TRUE(at_hole(*fit, 1).analyses[0].omitted);
  EXPECT_FALSE(at_hole(*fit, 1).excluded);
  EXPECT_FALSE(at_hole(*fit, 2).used_in_fit);
  EXPECT_TRUE(at_hole(*fit, 2).excluded);  // carried, so the next save says so again
  EXPECT_TRUE(has(at_hole(*fit, 2), PositionNote::LeftOutOfFit));

  Edits reset;
  reset.reset_omits = true;
  fit = fit_level(in, plane(false), reset);
  ASSERT_TRUE(fit) << fit.error().what;
  EXPECT_EQ(at_hole(*fit, 1).n, 3);
  EXPECT_TRUE(at_hole(*fit, 2).used_in_fit);
  EXPECT_FALSE(at_hole(*fit, 2).excluded);
  EXPECT_FALSE(has(at_hole(*fit, 2), PositionNote::LeftOutOfFit));
}

// Edits::include_positions: a monitor the saved fit excluded is fitted again,
// without forgetting the rest of what that fit left out.
TEST(FluxFitLevel, IncludePositionsOverridesACarriedExclusion) {
  auto in = level();
  at_hole(in, 1).saved = SavedFlux{};
  at_hole(in, 1).saved->omitted = {"M1-01"};
  at_hole(in, 2).saved = SavedFlux{};
  at_hole(in, 2).saved->used_in_fit = false;
  at_hole(in, 2).saved->excluded = true;
  // Saved before `excluded` existed: read as excluded, and overridden the same way.
  at_hole(in, 3).saved = SavedFlux{};
  at_hole(in, 3).saved->used_in_fit = false;
  at_hole(in, 3).saved->mean_j = 1.0e-3;

  auto carried = fit_level(in, plane(false), {});
  ASSERT_TRUE(carried) << carried.error().what;
  EXPECT_TRUE(at_hole(*carried, 2).excluded);
  EXPECT_TRUE(at_hole(*carried, 3).excluded);

  Edits e;
  e.include_positions = {2, 3};
  auto fit = fit_level(in, plane(false), e);
  ASSERT_TRUE(fit) << fit.error().what;
  for (int hole : {2, 3}) {
    EXPECT_TRUE(at_hole(*fit, hole).used_in_fit) << hole;
    EXPECT_FALSE(at_hole(*fit, hole).excluded) << hole;
    EXPECT_FALSE(has(at_hole(*fit, hole), PositionNote::LeftOutOfFit)) << hole;
  }
  EXPECT_EQ(fit->dof, carried->dof + 2);
  // The saved omission of hole 1 still applies: this is not reset_omits.
  EXPECT_EQ(at_hole(*fit, 1).n, 2);
  EXPECT_TRUE(at_hole(*fit, 1).analyses[0].omitted);

  // A hole in both sets is included, as `include` wins over `omit`.
  e.exclude_positions = {2, 4};
  fit = fit_level(in, plane(false), e);
  ASSERT_TRUE(fit) << fit.error().what;
  EXPECT_FALSE(at_hole(*fit, 2).excluded);
  EXPECT_TRUE(at_hole(*fit, 4).excluded);

  // Including a monitor nobody excluded, or an unknown's hole, changes nothing.
  Edits plain;
  plain.include_positions = {5, 101};
  auto same = fit_level(level(), plane(false), plain);
  auto base = fit_level(level(), plane(false), {});
  ASSERT_TRUE(same && base);
  EXPECT_EQ(same->dof, base->dof);
  EXPECT_EQ(same->parameters, base->parameters);

  // A hole that is not a position of the level is refused, with the holes.
  Edits wrong;
  wrong.include_positions = {99};
  auto bad = fit_level(in, plane(false), wrong);
  ASSERT_FALSE(bad);
  EXPECT_NE(bad.error().what.find("hole 99 is not a position of"), std::string::npos) << bad.error().what;
  EXPECT_NE(bad.error().what.find("1, 2, 3, 4, 5, 6, 7, 8, 101, 102, 103, 104"), std::string::npos) << bad.error().what;
}

// R15: `used_in_fit: false` is what a save says of every position that took
// no part, whoever decided it. Only `excluded` is the user's word.
TEST(FluxFitLevel, AMonitorSavedUnusedForWantOfAnalysesIsUsedOnceItHasThem) {
  auto in = level();
  // Saved when it had no analyses: not used, no mean, and (an older save) no `excluded`.
  at_hole(in, 2).saved = SavedFlux{};
  at_hole(in, 2).saved->used_in_fit = false;
  auto fit = fit_level(in, plane(false), {});
  ASSERT_TRUE(fit) << fit.error().what;
  EXPECT_TRUE(at_hole(*fit, 2).used_in_fit);
  EXPECT_FALSE(at_hole(*fit, 2).excluded);
  EXPECT_FALSE(has(at_hole(*fit, 2), PositionNote::LeftOutOfFit));
  EXPECT_EQ(fit->dof, 5);  // all 8 monitors, 3 parameters

  // A save that knows the key and says the position was not excluded: used,
  // even though it had a mean then and was not used.
  at_hole(in, 2).saved->mean_j = 1.0e-3;
  at_hole(in, 2).saved->excluded = false;
  fit = fit_level(in, plane(false), {});
  ASSERT_TRUE(fit) << fit.error().what;
  EXPECT_TRUE(at_hole(*fit, 2).used_in_fit);
  EXPECT_FALSE(at_hole(*fit, 2).excluded);
}

// A revision saved before `excluded` existed: a monitor that had its mean and
// still was not used was left out by the user.
TEST(FluxFitLevel, AnOlderSavedExclusionIsReadFromUsedInFitAndTheMean) {
  auto in = level();
  at_hole(in, 2).saved = SavedFlux{};
  at_hole(in, 2).saved->used_in_fit = false;
  at_hole(in, 2).saved->mean_j = 1.0e-3;
  auto fit = fit_level(in, plane(false), {});
  ASSERT_TRUE(fit) << fit.error().what;
  EXPECT_FALSE(at_hole(*fit, 2).used_in_fit);
  EXPECT_TRUE(at_hole(*fit, 2).excluded);
  EXPECT_EQ(fit->dof, 4);

  Edits reset;
  reset.reset_omits = true;
  fit = fit_level(in, plane(false), reset);
  ASSERT_TRUE(fit) << fit.error().what;
  EXPECT_TRUE(at_hole(*fit, 2).used_in_fit);
  EXPECT_FALSE(at_hole(*fit, 2).excluded);
}

TEST(FluxFitLevel, ExcludedIsOnlyTheUsersWord) {
  auto in = level();
  for (auto& a : at_hole(in, 4).analyses) a.tag = "omit";  // no usable analysis: left out, not excluded
  Edits e;
  e.exclude_positions = {3, 101};  // 101 is an unknown: nothing to exclude
  auto fit = fit_level(in, plane(false), e);
  ASSERT_TRUE(fit) << fit.error().what;
  for (const auto& p : fit->positions) EXPECT_EQ(p.excluded, p.hole == 3) << p.hole;
  EXPECT_FALSE(at_hole(*fit, 4).used_in_fit);
}

TEST(FluxFitLevel, AnExcludedMonitorStillGetsAPredictedJ) {
  const auto in = level();
  Edits e;
  e.exclude_positions = {3};
  auto fit = fit_level(in, plane(false), e);
  ASSERT_TRUE(fit) << fit.error().what;
  const auto& p = at_hole(*fit, 3);
  EXPECT_FALSE(p.used_in_fit);
  EXPECT_TRUE(has(p, PositionNote::LeftOutOfFit));
  EXPECT_GT(p.j, 0);
  EXPECT_GT(p.j_err, 0);
  ASSERT_TRUE(p.mean_j);  // its own mean is still shown
  EXPECT_EQ(fit->dof, 4);  // 7 monitors, 3 parameters

  // The prediction there comes from the other seven.
  std::vector<pr::Monitor> others;
  for (const auto& q : fit->positions)
    if (q.monitor && q.hole != 3) others.push_back({std::to_string(q.hole), {q.x, q.y}, *q.mean_j, *q.mean_j_err});
  const auto direct = pr::fit_flux(others, std::vector<pr::Point>{{p.x, p.y}}, plane(false).fit);
  ASSERT_TRUE(direct) << direct.error().what;
  EXPECT_EQ(p.j, direct->at[0].j);
}

TEST(FluxFitLevel, AMonitorWithNoUsableAnalysisIsLeftOutAndNoted) {
  auto in = level();
  for (auto& a : at_hole(in, 4).analyses) a.tag = "omit";
  auto fit = fit_level(in, plane(false), {});
  ASSERT_TRUE(fit) << fit.error().what;
  const auto& p = at_hole(*fit, 4);
  EXPECT_TRUE(has(p, PositionNote::NoUsableAnalysis));
  EXPECT_FALSE(p.used_in_fit);
  EXPECT_FALSE(p.mean_j);
  EXPECT_EQ(p.n, 0);
  EXPECT_GT(p.j, 0);
  EXPECT_EQ(fit->dof, 4);
}

TEST(FluxFitLevel, AFailedReductionTakesNoPartAndIsNoted) {
  auto in = level();
  at_hole(in, 5).analyses[1].f.reset();
  at_hole(in, 5).analyses[1].reduction_error = "no isotopes";
  auto fit = fit_level(in, plane(false), {});
  ASSERT_TRUE(fit) << fit.error().what;
  const auto& p = at_hole(*fit, 5);
  EXPECT_EQ(p.n, 2);
  EXPECT_TRUE(has(p, PositionNote::AnalysisNotReduced));
  // Not usable is not omitted (R15): a save must not carry it as an omission.
  ASSERT_EQ(p.analyses.size(), 3u);
  EXPECT_FALSE(p.analyses[1].omitted);
  EXPECT_TRUE(p.used_in_fit);
  EXPECT_FALSE(has(at_hole(*fit, 6), PositionNote::AnalysisNotReduced));
}

TEST(FluxFitLevel, AnAnalysisWithNoJIsRejectedAndNamed) {
  auto in = level();
  at_hole(in, 5).analyses[2].f = pr::UFloat(-1.0);  // F <= 0 gives no J
  auto fit = fit_level(in, plane(false), {});
  ASSERT_TRUE(fit) << fit.error().what;
  const auto& p = at_hole(*fit, 5);
  EXPECT_EQ(p.n, 2);
  EXPECT_TRUE(has(p, PositionNote::AnalysisRejected));
  ASSERT_EQ(p.rejected.size(), 1u);
  EXPECT_EQ(p.rejected[0], "M5-03");
}

TEST(FluxFitLevel, LevelSummary) {
  auto fit = fit_level(level(), plane(false), {});
  ASSERT_TRUE(fit) << fit.error().what;
  double lo = 1e9, hi = -1e9;
  for (const auto& p : fit->positions) {
    lo = std::min(lo, p.j);
    hi = std::max(hi, p.j);
  }
  EXPECT_EQ(fit->min_j, lo);
  EXPECT_EQ(fit->max_j, hi);
  EXPECT_NEAR(fit->delta_j_percent, (hi - lo) / hi * 100.0, 1e-12);
  EXPECT_GT(fit->delta_j_percent, 0);
}

TEST(FluxFitLevel, NoMonitorsIsAnError) {
  auto in = level();
  for (auto& p : in.positions) p.monitor = false;
  auto fit = fit_level(in, plane(false), {});
  ASSERT_FALSE(fit);
  EXPECT_NE(fit.error().what.find("no monitor positions"), std::string::npos) << fit.error().what;
}

TEST(FluxFitLevel, TooFewUsedMonitorsIsTheModelsError) {
  auto in = level();
  FluxOptions bowl;
  bowl.fit.kind = pr::ModelKind::Bowl;
  Edits e;
  e.exclude_positions = {1, 2, 3};  // 5 left, a bowl needs 6
  auto fit = fit_level(in, bowl, e);
  ASSERT_FALSE(fit);
  EXPECT_NE(fit.error().what.find("bowl needs 6 monitor positions, 5 used"), std::string::npos) << fit.error().what;
}

TEST(FluxFitLevel, UnknownRecordIdOrHoleIsAnError) {
  const auto in = level();
  Edits omit;
  omit.omit = {"99999-01"};
  auto fit = fit_level(in, plane(false), omit);
  ASSERT_FALSE(fit);
  EXPECT_NE(fit.error().what.find("99999-01"), std::string::npos) << fit.error().what;
  EXPECT_NE(fit.error().what.find("not an analysis of"), std::string::npos) << fit.error().what;
  EXPECT_NE(fit.error().what.find("M3-02"), std::string::npos) << fit.error().what;

  Edits include;
  include.include = {"99999-01"};
  fit = fit_level(in, plane(false), include);
  ASSERT_FALSE(fit);
  EXPECT_NE(fit.error().what.find("99999-01"), std::string::npos) << fit.error().what;
  EXPECT_NE(fit.error().what.find("not an analysis of"), std::string::npos) << fit.error().what;
  EXPECT_NE(fit.error().what.find("M8-03"), std::string::npos) << fit.error().what;

  Edits hole;
  hole.exclude_positions = {99};
  fit = fit_level(in, plane(false), hole);
  ASSERT_FALSE(fit);
  EXPECT_NE(fit.error().what.find("99"), std::string::npos) << fit.error().what;
  EXPECT_NE(fit.error().what.find("1, 2, 3, 4, 5, 6, 7, 8, 101, 102, 103, 104"), std::string::npos) << fit.error().what;

  // An unknown's hole is a position of the level: excluding it changes nothing.
  Edits unknown_hole;
  unknown_hole.exclude_positions = {101};
  auto same = fit_level(in, plane(false), unknown_hole);
  auto base = fit_level(in, plane(false), {});
  ASSERT_TRUE(same && base);
  EXPECT_EQ(same->dof, base->dof);
  EXPECT_EQ(at_hole(*same, 101).j, at_hole(*base, 101).j);
}

TEST(FluxFitLevel, AMonitorWhoseEveryAnalysisGivesNoJIsLeftOut) {
  auto in = level();
  for (auto& a : at_hole(in, 4).analyses) a.f = pr::UFloat(0.0);
  auto fit = fit_level(in, plane(false), {});
  ASSERT_TRUE(fit) << fit.error().what;
  const auto& p = at_hole(*fit, 4);
  EXPECT_TRUE(has(p, PositionNote::NoUsableAnalysis));
  EXPECT_FALSE(p.used_in_fit);
  EXPECT_FALSE(p.mean_j);
  EXPECT_EQ(p.rejected.size(), 3u);
  EXPECT_GT(p.j, 0);
  EXPECT_EQ(fit->dof, 4);
}

TEST(FluxFitLevel, AnUnknownBeyondTheEndMonitorsIsExtrapolated) {
  auto in = level();
  for (auto& p : in.positions) p.y = 0;  // monitors spread along x only
  at_hole(in, 101).x = 50;               // beyond the end monitors (x in [-10, 10])
  FluxOptions o;
  o.fit.kind = pr::ModelKind::Bracketing1D;
  o.fit.axis = pr::Axis::X;
  auto fit = fit_level(in, o, {});
  ASSERT_TRUE(fit) << fit.error().what;
  EXPECT_TRUE(has(at_hole(*fit, 101), PositionNote::Extrapolated));
  EXPECT_FALSE(has(at_hole(*fit, 102), PositionNote::Extrapolated));
}

TEST(FluxFitLevel, ModelNames) {
  using pr::ModelKind;
  const std::pair<ModelKind, const char*> legacy[] = {
      {ModelKind::Plane, "Plane"},
      {ModelKind::Bowl, "Bowl"},
      {ModelKind::WeightedMean, "Weighted Mean"},
      {ModelKind::Matching, "Matching"},
      {ModelKind::NearestNeighbors, "Nearest Neighbors"},
      {ModelKind::Bracketing, "Bracketing"},
      {ModelKind::LeastSquares1D, "LeastSquares1D"},
      {ModelKind::WeightedMean1D, "WeightedMean1D"},
      {ModelKind::Bracketing1D, "Bracketing1D"},
  };
  for (const auto& [kind, name] : legacy) {
    EXPECT_EQ(legacy_model_name(kind), name);
    ASSERT_TRUE(parse_model_kind(name)) << name;
    EXPECT_EQ(*parse_model_kind(name), kind) << name;
  }
  const std::pair<const char*, ModelKind> cli[] = {
      {"plane", ModelKind::Plane},
      {"bowl", ModelKind::Bowl},
      {"weighted-mean", ModelKind::WeightedMean},
      {"matching", ModelKind::Matching},
      {"nearest", ModelKind::NearestNeighbors},
      {"bracketing", ModelKind::Bracketing},
      {"ls1d", ModelKind::LeastSquares1D},
      {"mean1d", ModelKind::WeightedMean1D},
      {"bracketing1d", ModelKind::Bracketing1D},
  };
  for (const auto& [name, kind] : cli) {
    ASSERT_TRUE(parse_model_kind(name)) << name;
    EXPECT_EQ(*parse_model_kind(name), kind) << name;
  }
  EXPECT_FALSE(parse_model_kind("RBF"));
  EXPECT_FALSE(parse_model_kind(""));
}

const FittedPosition::UsedAnalysis& by_record(const FittedPosition& p, const std::string& id) {
  return *std::find_if(p.analyses.begin(), p.analyses.end(), [&](auto& a) { return a.record_id == id; });
}

TEST(FluxFitLevel, EachAnalysisCarriesItsJ) {
  auto in = level();
  auto fit = fit_level(in, plane(true), {});
  ASSERT_TRUE(fit) << fit.error().what;
  const auto& p = at_hole(*fit, 1);
  const auto& src = at_hole(in, 1);
  ASSERT_EQ(p.analyses.size(), 3u);
  for (std::size_t i = 0; i < 3; ++i) {
    const auto j = pr::j_of(*src.analyses[i].f, in.monitor_set.constants());
    ASSERT_TRUE(j);
    const auto& a = p.analyses[i];
    EXPECT_EQ(a.state, AnalysisState::Used);
    EXPECT_EQ(a.tag, "ok");
    ASSERT_TRUE(a.j && a.j_err);
    EXPECT_NEAR(*a.j, j->nominal(), std::abs(j->nominal()) * 1e-15);
    EXPECT_NEAR(*a.j_err, j->std_dev(), std::abs(j->std_dev()) * 1e-15);
  }
}

TEST(FluxFitLevel, AnalysisStateSaysWhyItIsOut) {
  auto in = level();
  at_hole(in, 1).analyses[0].tag = "outlier";
  at_hole(in, 2).saved = SavedFlux{};
  at_hole(in, 2).saved->revision = "rev-2";
  at_hole(in, 2).saved->omitted = {"M2-02"};
  at_hole(in, 3).analyses[0].f.reset();
  at_hole(in, 3).analyses[0].reduction_error = "no peaks";
  at_hole(in, 4).analyses[1].f = pr::UFloat::variable(0.0, 0.0);
  Edits edits;
  edits.omit = {"M5-03"};
  auto fit = fit_level(in, plane(false), edits);
  ASSERT_TRUE(fit) << fit.error().what;

  const auto& tagged = by_record(at_hole(*fit, 1), "M1-01");
  EXPECT_EQ(tagged.state, AnalysisState::OmittedByTag);
  EXPECT_EQ(tagged.tag, "outlier");
  EXPECT_TRUE(tagged.omitted);
  EXPECT_TRUE(tagged.j && tagged.j_err);  // an omitted analysis with an F still has its J

  const auto& saved = by_record(at_hole(*fit, 2), "M2-02");
  EXPECT_EQ(saved.state, AnalysisState::OmittedBySavedFit);
  EXPECT_TRUE(saved.omitted && saved.j);

  const auto& unreduced = by_record(at_hole(*fit, 3), "M3-01");
  EXPECT_EQ(unreduced.state, AnalysisState::NotReduced);
  EXPECT_EQ(unreduced.reduction_error, "no peaks");
  EXPECT_FALSE(unreduced.omitted);
  EXPECT_FALSE(unreduced.j || unreduced.j_err);

  const auto& noj = by_record(at_hole(*fit, 4), "M4-02");
  EXPECT_EQ(noj.state, AnalysisState::NoJ);
  EXPECT_FALSE(noj.omitted);
  EXPECT_FALSE(noj.j || noj.j_err);  // F gives no J: nothing to draw

  const auto& edited = by_record(at_hole(*fit, 5), "M5-03");
  EXPECT_EQ(edited.state, AnalysisState::OmittedByEdit);
  EXPECT_TRUE(edited.omitted && edited.j);

  // Out by rule first, and it also failed to reduce: the omission is named.
  auto both = level();
  at_hole(both, 1).analyses[0].tag = "omit";
  at_hole(both, 1).analyses[0].f.reset();
  auto fit2 = fit_level(both, plane(false), {});
  ASSERT_TRUE(fit2) << fit2.error().what;
  const auto& a = by_record(at_hole(*fit2, 1), "M1-01");
  EXPECT_EQ(a.state, AnalysisState::OmittedByTag);
  EXPECT_FALSE(a.j);

  EXPECT_EQ(to_string(AnalysisState::Used), "used");
  EXPECT_EQ(to_string(AnalysisState::OmittedByTag), "omitted by tag");
  EXPECT_EQ(to_string(AnalysisState::OmittedBySavedFit), "omitted by saved fit");
  EXPECT_EQ(to_string(AnalysisState::OmittedByEdit), "omitted here");
  EXPECT_EQ(to_string(AnalysisState::NotReduced), "not reduced");
  EXPECT_EQ(to_string(AnalysisState::NoJ), "no J");
}

TEST(FluxFitLevel, StatePrecedenceIsEditThenSavedThenTag) {
  auto in = level();
  at_hole(in, 1).analyses[0].tag = "outlier";
  at_hole(in, 1).analyses[1].tag = "outlier";
  at_hole(in, 1).saved = SavedFlux{};
  at_hole(in, 1).saved->revision = "rev-1";
  at_hole(in, 1).saved->omitted = {"M1-01", "M1-02"};
  Edits edits;
  edits.omit = {"M1-01"};
  auto fit = fit_level(in, plane(false), edits);
  ASSERT_TRUE(fit) << fit.error().what;
  EXPECT_EQ(by_record(at_hole(*fit, 1), "M1-01").state, AnalysisState::OmittedByEdit);
  EXPECT_EQ(by_record(at_hole(*fit, 1), "M1-02").state, AnalysisState::OmittedBySavedFit);
}

TEST(FluxFitLevel, IncludeClearsTheState) {
  auto in = level();
  at_hole(in, 1).analyses[0].tag = "outlier";
  Edits edits;
  edits.include = {"M1-01"};
  auto fit = fit_level(in, plane(false), edits);
  ASSERT_TRUE(fit) << fit.error().what;
  const auto& a = by_record(at_hole(*fit, 1), "M1-01");
  EXPECT_EQ(a.state, AnalysisState::Used);
  EXPECT_FALSE(a.omitted);
  EXPECT_TRUE(a.j);
}

TEST(FluxFitLevel, OmittedIsUnchangedByTheNewFields) {
  auto in = level();
  at_hole(in, 1).analyses[0].tag = "outlier";
  at_hole(in, 2).analyses[0].f.reset();
  at_hole(in, 3).analyses[1].f = pr::UFloat::variable(0.0, 0.0);
  Edits edits;
  edits.omit = {"M4-01"};
  auto fit = fit_level(in, plane(false), edits);
  ASSERT_TRUE(fit) << fit.error().what;
  int seen = 0;
  for (const auto& p : fit->positions)
    for (const auto& a : p.analyses) {
      const bool out = a.state == AnalysisState::OmittedByTag || a.state == AnalysisState::OmittedBySavedFit ||
                       a.state == AnalysisState::OmittedByEdit;
      EXPECT_EQ(a.omitted, out) << a.record_id;
      ++seen;
    }
  EXPECT_EQ(seen, 24);
}

TEST(FluxFitLevel, AWeightedMeanRefusesAJWithNoError) {
  auto in = level();
  auto& a = at_hole(in, 1).analyses[1];
  const double f = a.f->nominal();
  a.f = pr::UFloat(f);  // finite F, exact: J has no error
  FluxOptions o = plane(false);
  o.mean = pr::MeanKind::Weighted;
  auto weighted = fit_level(in, o, {});
  ASSERT_TRUE(weighted) << weighted.error().what;
  const auto& p = at_hole(*weighted, 1);
  EXPECT_EQ(p.rejected, std::vector<std::string>{"M1-02"});
  EXPECT_EQ(p.n, 2);
  const auto& r = by_record(p, "M1-02");
  EXPECT_EQ(r.state, AnalysisState::NoJ);
  EXPECT_FALSE(r.omitted);
  const auto j = pr::j_of(*a.f, in.monitor_set.constants());
  ASSERT_TRUE(j);
  ASSERT_TRUE(r.j && r.j_err);
  EXPECT_EQ(*r.j, j->nominal());
  EXPECT_EQ(*r.j_err, 0.0);
}

}  // namespace
}  // namespace pychron::processing
