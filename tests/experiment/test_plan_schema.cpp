#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>

#include "pychron/experiment/plan/duration.hpp"
#include "pychron/experiment/plan/plan_library.hpp"
#include "pychron/experiment/plan/plan_loader.hpp"

namespace fs = std::filesystem;
using namespace pychron;
using namespace pychron::experiment;
using namespace pychron::experiment::plan;

namespace {

const fs::path kFixtures = fs::path(__FILE__).parent_path() / "fixtures" / "plans";

std::string read_file(const fs::path& p) {
  std::ifstream in(p);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

class FakeExtractionLine : public IAliasResolver {
 public:
  std::optional<ParamValue> resolve_alias(std::string_view key) const override {
    static const std::map<std::string, ParamValue, std::less<>> aliases{
        {"valves.inlet", std::string("V12")}, {"valves.outlet", std::string("V13")},
        {"extraction.eqtime", std::int64_t{20}}};
    if (auto it = aliases.find(key); it != aliases.end()) return it->second;
    return std::nullopt;
  }
};

class FakeSpectrometer : public ISpectrometerCatalog {
 public:
  bool has_detector(std::string_view name) const override {
    static const std::set<std::string, std::less<>> dets{"H2", "H1", "AX", "L1", "L2", "CDD"};
    return dets.count(name) > 0;
  }
};

const FakeExtractionLine kLine;
const FakeSpectrometer kSpec;
const PlanResolvers kResolvers{&kLine, &kSpec};

PlanTemplate fixture_template(const std::string& name) {
  auto t = parse_plan_template(read_file(kFixtures / "valid" / name), name);
  EXPECT_TRUE(t.has_value()) << (t ? "" : t.error().what);
  return t.value_or(PlanTemplate{});
}

MeasurementPlan load_fixture(const std::string& name, const ParamOverrides& ov = {}) {
  auto r = load_plan(fixture_template(name), ov, kResolvers);
  EXPECT_TRUE(r.has_value()) << (r ? "" : r.error().what);
  return r ? r->plan : MeasurementPlan{};
}

const char* kMinimal = R"(
[plan]
name = "single"
instrument_family = "generic"
[detectors]
reference = "H1"
[main]
cycles = CYCLES
integration_s = 1
[[main.hops]]
positions = { Ar40 = "H1" }
counts = COUNTS
settle_s = 3
)";

MeasurementPlan minimal(int cycles, int counts) {
  std::string text = kMinimal;
  text.replace(text.find("CYCLES"), 6, std::to_string(cycles));
  text.replace(text.find("COUNTS"), 6, std::to_string(counts));
  auto p = resolve_plan(text, kResolvers);
  EXPECT_TRUE(p.has_value()) << (p ? "" : p.error().what);
  return p.value_or(MeasurementPlan{});
}

}  // namespace

// ---- valid templates --------------------------------------------------------

TEST(PlanSchema, MulticollectResolvesAliasesAndSections) {
  const auto p = load_fixture("multicollect.toml");
  EXPECT_EQ(p.info.name, "argus_multicollect");
  EXPECT_EQ(p.info.instrument_family, "thermo_argus");
  EXPECT_EQ(p.info.analysis_types.size(), 4u);
  EXPECT_EQ(p.detectors.reference, "H1");
  EXPECT_EQ(p.equilibration.inlet, "V12");
  EXPECT_EQ(p.equilibration.outlet, "V13");
  EXPECT_DOUBLE_EQ(p.equilibration.time_s, 20);  // "@extraction.eqtime"
  EXPECT_DOUBLE_EQ(p.equilibration.inlet_delay_s, 3);
  EXPECT_TRUE(p.sniff.enabled);
  EXPECT_EQ(p.sniff.counts, 20);  // "@equilibration.time_s" -> "@extraction.eqtime"
  EXPECT_TRUE(p.peak_center.after);
  EXPECT_EQ(p.peak_center.isotope, "Ar40");
  EXPECT_EQ(p.baseline.counts, 120);
  ASSERT_TRUE(p.baseline.mass);
  EXPECT_DOUBLE_EQ(*p.baseline.mass, 34.2);
  EXPECT_EQ(p.main.cycles, 1);
  EXPECT_EQ(p.main.time_zero.kind, TimeZeroKind::OnInletClose);
  ASSERT_EQ(p.main.hops.size(), 1u);
  EXPECT_EQ(p.main.hops[0].positions.size(), 5u);
  EXPECT_EQ(p.main.hops[0].positions.at("Ar36"), "CDD");
  EXPECT_EQ(p.main.hops[0].counts, 400);
  EXPECT_EQ(p.fits.signal.at("Ar40"), reduction::FitKind::Parabolic);
  EXPECT_EQ(signal_fit(p.fits, "Ar39").kind, reduction::FitKind::Linear);
  EXPECT_EQ(signal_fit(p.fits, "Ar40").kind, reduction::FitKind::Parabolic);
  EXPECT_TRUE(signal_fit(p.fits, "Ar40").outliers.enabled);
  EXPECT_EQ(baseline_fit(p.fits, "H1").kind, reduction::FitKind::Average);
  ASSERT_EQ(p.conditionals.include.size(), 1u);
  EXPECT_EQ(p.conditionals.include[0], "@conditionals.default_unknown");  // kept verbatim
  ASSERT_EQ(p.conditionals.truncations.size(), 1u);
  EXPECT_EQ(p.conditionals.truncations[0].start, 20);
  EXPECT_FALSE(p.whiff.enabled);
  ASSERT_TRUE(p.hook);
  EXPECT_EQ(*p.hook, "scripts/measurement_hooks/whiff.py");
  ASSERT_EQ(p.expose.size(), 9u);
  EXPECT_EQ(p.expose[0], (ExposeEntry{"main.hops[0].counts", "Counts"}));
  EXPECT_EQ(p.expose[1], (ExposeEntry{"main.integration_s", "main.integration_s"}));
}

TEST(PlanSchema, PeakHopTargetsFollowReferenceOrExplicitPosition) {
  const auto p = load_fixture("peak_hop_cdd.toml");
  EXPECT_EQ(p.main.cycles, 20);
  EXPECT_EQ(p.main.time_zero.kind, TimeZeroKind::Offset);
  EXPECT_DOUBLE_EQ(p.main.time_zero.offset_s, 5);
  ASSERT_EQ(p.main.hops.size(), 3u);
  EXPECT_EQ(hop_target(p, p.main.hops[0]), (HopTarget{"Ar40", std::nullopt, "H1"}));
  EXPECT_EQ(hop_target(p, p.main.hops[1]), (HopTarget{"Ar39", std::nullopt, "CDD"}));
  EXPECT_EQ(hop_target(p, p.main.hops[2]), (HopTarget{"", 34.2, "CDD"}));
  EXPECT_TRUE(p.main.hops[2].baseline);
  EXPECT_EQ(p.main.hops[0].protect, std::vector<std::string>{"CDD"});
  EXPECT_EQ(baseline_target(p), (HopTarget{"", 34.2, "H1"}));
}

TEST(PlanSchema, ExcludedDetectorsDropOutOfEveryHop) {
  const auto p = load_fixture("multicollect.toml", {{"detectors.exclude", std::string("CDD, L2")}});
  EXPECT_EQ(p.detectors.exclude, (std::vector<std::string>{"CDD", "L2"}));
  const auto act = active_detectors(p, p.main.hops[0]);
  ASSERT_EQ(act.size(), 3u);
  EXPECT_EQ(act[0], (ActiveDetector{"Ar38", "L1"}));
  EXPECT_EQ(act[1], (ActiveDetector{"Ar39", "AX"}));
  EXPECT_EQ(act[2], (ActiveDetector{"Ar40", "H1"}));
}

TEST(PlanSchema, NullSpectrometerSkipsDetectorChecks) {
  const auto text = read_file(kFixtures / "invalid" / "unknown_detector.toml");
  EXPECT_TRUE(resolve_plan(text, PlanResolvers{&kLine, nullptr}).has_value());
}

// ---- one failing fixture per rule --------------------------------------------

TEST(PlanSchema, EveryInvalidFixtureFailsWithItsRule) {
  int n = 0;
  for (const auto& entry : fs::directory_iterator(kFixtures / "invalid")) {
    if (entry.path().extension() != ".toml") continue;
    ++n;
    const auto text = read_file(entry.path());
    const std::string prefix = "# expect: ";
    ASSERT_EQ(text.rfind(prefix, 0), 0u) << entry.path();
    const auto expect = text.substr(prefix.size(), text.find('\n') - prefix.size());
    SCOPED_TRACE(entry.path().filename().string());

    const auto source = entry.path().filename().string();
    std::string what;
    if (auto t = parse_plan_template(text, source); !t) {
      what = t.error().what;
      EXPECT_EQ(t.error().kind, ErrorKind::Config);
    } else if (auto r = load_plan(*t, {}, kResolvers); !r) {
      what = r.error().what;
      EXPECT_EQ(r.error().kind, ErrorKind::Config);
    } else {
      ADD_FAILURE() << "fixture loaded but should fail with: " << expect;
      continue;
    }
    EXPECT_NE(what.find(expect), std::string::npos) << "expected '" << expect << "' in:\n" << what;
    EXPECT_NE(what.find(source), std::string::npos) << "message names the source";
  }
  EXPECT_GE(n, 30);
}

TEST(PlanSchema, CollectsAllProblemsNotJustTheFirst) {
  auto r = resolve_plan(R"(
[plan]
instrument_family = "x"
[main]
cycles = 0
[[main.hops]]
positions = { Ar40 = "H1" }
counts = 0
)",
                        kResolvers, "multi.toml");
  ASSERT_FALSE(r);
  EXPECT_NE(r.error().what.find("plan.name: required"), std::string::npos);
  EXPECT_NE(r.error().what.find("main.cycles: must be >= 1"), std::string::npos);
  EXPECT_NE(r.error().what.find("main.hops[0].counts: must be >= 1"), std::string::npos);
}

// ---- parameters.expose / overrides -------------------------------------------

TEST(PlanOverrides, ExposedIndexedPathIsSettable) {
  const auto p = load_fixture("multicollect.toml", {{"main.hops[0].counts", std::int64_t{200}}});
  EXPECT_EQ(p.main.hops[0].counts, 200);
}

TEST(PlanOverrides, NonExposedPathRejectedUnlessAdvanced) {
  const auto t = fixture_template("multicollect.toml");
  const ParamOverrides ov{{"main.hops[0].settle_s", 5.0}};
  auto r = load_plan(t, ov, kResolvers);
  ASSERT_FALSE(r);
  EXPECT_NE(r.error().what.find("main.hops[0].settle_s: not exposed"), std::string::npos) << r.error().what;

  auto adv = load_plan(t, ov, kResolvers, LoadOptions{.advanced = true});
  ASSERT_TRUE(adv) << adv.error().what;
  EXPECT_DOUBLE_EQ(adv->plan.main.hops[0].settle_s, 5.0);
}

TEST(PlanOverrides, MissingPathRejectedEvenWhenAdvanced) {
  const auto t = fixture_template("multicollect.toml");
  auto r = render_effective_plan(t, {{"main.hops[4].counts", std::int64_t{1}}}, LoadOptions{.advanced = true});
  ASSERT_FALSE(r);
  EXPECT_NE(r.error().what.find("main.hops[4].counts: index 4 out of range"), std::string::npos) << r.error().what;

  r = render_effective_plan(t, {{"nosuch.key", std::int64_t{1}}}, LoadOptions{.advanced = true});
  ASSERT_FALSE(r);
  EXPECT_NE(r.error().what.find("nosuch.key: no such path"), std::string::npos) << r.error().what;
}

TEST(PlanOverrides, TypeMustMatchTemplate) {
  const auto t = fixture_template("multicollect.toml");
  auto r = render_effective_plan(t, {{"main.cycles", std::string("many")}});
  ASSERT_FALSE(r);
  EXPECT_NE(r.error().what.find("main.cycles: expected an integer"), std::string::npos) << r.error().what;

  r = render_effective_plan(t, {{"peak_center.before", std::int64_t{1}}});
  ASSERT_FALSE(r);
  EXPECT_NE(r.error().what.find("peak_center.before: expected a boolean"), std::string::npos) << r.error().what;

  r = render_effective_plan(t, {{"main.cycles", 2.5}});
  ASSERT_FALSE(r);

  // An int sets a float; an integral double sets an int.
  const auto p = load_fixture("multicollect.toml",
                              {{"main.integration_s", std::int64_t{2}}, {"main.cycles", 3.0}});
  EXPECT_DOUBLE_EQ(p.main.integration_s, 2.0);
  EXPECT_EQ(p.main.cycles, 3);
}

TEST(PlanOverrides, ChildOfExposedTableMaySetOrAddKey) {
  const auto p = load_fixture("multicollect.toml", {{"fits.signal.Ar36", std::string("cubic")}});
  EXPECT_EQ(p.fits.signal.at("Ar36"), reduction::FitKind::Cubic);
  EXPECT_EQ(p.fits.signal.at("Ar40"), reduction::FitKind::Parabolic);
}

TEST(PlanOverrides, OverriddenValueIsStillValidated) {
  auto r = load_plan(fixture_template("multicollect.toml"), {{"main.cycles", std::int64_t{0}}}, kResolvers);
  ASSERT_FALSE(r);
  EXPECT_NE(r.error().what.find("main.cycles: must be >= 1"), std::string::npos);

  r = load_plan(fixture_template("multicollect.toml"), {{"detectors.exclude", std::string("H9")}}, kResolvers);
  ASSERT_FALSE(r);
  EXPECT_NE(r.error().what.find("detectors.exclude[0]: unknown detector 'H9'"), std::string::npos);
}

TEST(PlanOverrides, ScalarMayReplaceAlias) {
  const auto t = fixture_template("multicollect.toml");
  auto r = load_plan(t, {{"equilibration.time_s", std::int64_t{30}}}, kResolvers, LoadOptions{.advanced = true});
  ASSERT_TRUE(r) << r.error().what;
  EXPECT_DOUBLE_EQ(r->plan.equilibration.time_s, 30);
  EXPECT_EQ(r->plan.sniff.counts, 30);  // self reference follows the override
}

TEST(PlanOverrides, IndexedExposeOnArrayValue) {
  const auto t = fixture_template("peak_hop_cdd.toml");
  auto r = load_plan(t, {{"main.hops[0].protect", std::string("CDD,L2")}, {"main.hops[1].counts", std::int64_t{25}}},
                     kResolvers);
  ASSERT_TRUE(r) << r.error().what;
  EXPECT_EQ(r->plan.main.hops[0].protect, (std::vector<std::string>{"CDD", "L2"}));
  EXPECT_EQ(r->plan.main.hops[1].counts, 25);
  EXPECT_EQ(r->plan.main.hops[2].counts, 10);
}

// ---- effective-plan rendering ------------------------------------------------

TEST(PlanRender, EffectivePlanCarriesOverridesAndRoundTrips) {
  const auto t = fixture_template("multicollect.toml");
  const ParamOverrides ov{{"main.hops[0].counts", std::int64_t{250}}, {"baseline.counts", std::int64_t{60}}};
  auto loaded = load_plan(t, ov, kResolvers);
  ASSERT_TRUE(loaded) << loaded.error().what;
  EXPECT_NE(loaded->effective_toml.find("250"), std::string::npos);
  EXPECT_NE(loaded->effective_toml.find("@valves.inlet"), std::string::npos);  // aliases unresolved

  auto again = resolve_plan(loaded->effective_toml, kResolvers);
  ASSERT_TRUE(again) << again.error().what;
  EXPECT_EQ(*again, loaded->plan);
  EXPECT_EQ(again->main.hops[0].counts, 250);
  EXPECT_EQ(again->baseline.counts, 60);

  // Deterministic: same inputs, same text; rendering the effective plan with
  // no overrides reproduces it.
  auto text2 = render_effective_plan(t, ov);
  ASSERT_TRUE(text2);
  EXPECT_EQ(*text2, loaded->effective_toml);
  auto t2 = parse_plan_template(loaded->effective_toml, "effective");
  ASSERT_TRUE(t2) << t2.error().what;
  auto text3 = render_effective_plan(*t2, {});
  ASSERT_TRUE(text3);
  EXPECT_EQ(*text3, loaded->effective_toml);
}

TEST(PlanRender, NoOverridesStillRendersTemplate) {
  const auto t = fixture_template("peak_hop_cdd.toml");
  auto text = render_effective_plan(t, {});
  ASSERT_TRUE(text);
  auto p = resolve_plan(*text, kResolvers);
  ASSERT_TRUE(p) << p.error().what;
  EXPECT_EQ(p->main.hops.size(), 3u);
  ASSERT_TRUE(p->baseline.mass);
  EXPECT_EQ(*p->baseline.mass, 34.2);  // floats render at full precision
}

// ---- duration ------------------------------------------------------------------

TEST(PlanDuration, MulticollectMainIsSettlePlusCounts) {
  const auto p = load_fixture("multicollect.toml");
  EXPECT_DOUBLE_EQ(main_duration(p).count(), 3 + 400 * 1.0);
}

TEST(PlanDuration, OneHopTimesNCyclesEqualsOneHopWithNTimesCounts) {
  const auto a = minimal(4, 100);
  const auto b = minimal(1, 400);
  EXPECT_DOUBLE_EQ(main_duration(a).count(), 403);
  EXPECT_DOUBLE_EQ(main_duration(a).count(), main_duration(b).count());
}

TEST(PlanDuration, PeakHopSettlesEveryMove) {
  const auto p = load_fixture("peak_hop_cdd.toml");
  EXPECT_DOUBLE_EQ(main_duration(p).count(), 20 * 3 * (3 + 10));
}

TEST(PlanDuration, IntegrationScalesCounts) {
  const auto p = load_fixture("multicollect.toml", {{"main.integration_s", 2.0}});
  EXPECT_DOUBLE_EQ(main_duration(p).count(), 3 + 400 * 2.0);
}

TEST(PlanDuration, FullSequence) {
  const auto p = load_fixture("multicollect.toml");
  // eq 20 (sniff concurrent) + main 403 + baseline.after (moves: 15 + 120) + peak_center.after 30
  EXPECT_DOUBLE_EQ(estimate_duration(p, DurationOptions{Duration(30)}).count(), 20 + 403 + 135 + 30);

  const auto h = load_fixture("peak_hop_cdd.toml");
  // eq 15 + main 780 + baseline.after at 34.2 on H1 (last hop was 34.2 on CDD: move) 10 + 30
  EXPECT_DOUBLE_EQ(estimate_duration(h).count(), 15 + 780 + 40);
}

TEST(PlanLibrary, ResolvesPlansForQueueValidation) {
  PlanLibrary lib(kResolvers, DurationOptions{Duration(30)});
  lib.add(fixture_template("multicollect.toml"));
  EXPECT_TRUE(lib.has_plan("argus_multicollect"));
  EXPECT_FALSE(lib.has_plan("nope"));
  const IPlanResolver& r = lib;
  auto d = r.plan_duration("argus_multicollect", {});
  ASSERT_TRUE(d);
  EXPECT_DOUBLE_EQ(d->count(), 588);
  d = r.plan_duration("argus_multicollect", {{"main.hops[0].counts", std::int64_t{100}}});
  ASSERT_TRUE(d);
  EXPECT_DOUBLE_EQ(d->count(), 288);
  EXPECT_FALSE(r.plan_duration("argus_multicollect", {{"main.hops[0].settle_s", 1.0}}));  // not exposed
  EXPECT_FALSE(r.plan_duration("nope", {}));
  EXPECT_FALSE(lib.load("nope", {}));
}

TEST(PlanDuration, BaselineAtSamePositionSkipsSettle) {
  auto p = minimal(1, 10);
  p.baseline.after = true;
  p.baseline.counts = 5;
  p.baseline.settle_s = 7;
  p.equilibration.time_s = 0;
  // Baseline at the last hop's position (Ar40 on H1) -> no move, no settle.
  p.baseline.mass.reset();
  EXPECT_DOUBLE_EQ(estimate_duration(p).count(), 13 + 5);
}
