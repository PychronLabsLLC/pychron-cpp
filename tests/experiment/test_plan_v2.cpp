// Measurement plan v2 (docs/superpowers/specs/2026-10-05-measurement-plan-v2-design.md):
// the v1 desugar (section 4.1), one test per row of its table, and the native
// v2 parse (sections 3.2-3.4, worked plans of section 7).

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "pychron/experiment/plan/plan.hpp"
#include "pychron/experiment/plan/plan_loader.hpp"

namespace plan = pychron::experiment::plan;

namespace {

// Detectors of an ARGUS VI; CDD and H2 have a configured isotope.
class Catalog final : public plan::ISpectrometerCatalog {
 public:
  bool has_detector(std::string_view name) const override {
    static const std::set<std::string, std::less<>> dets{"H2", "H1", "AX", "L1", "L2", "CDD"};
    return dets.contains(name);
  }
  std::optional<std::string> isotope_of(std::string_view detector) const override {
    static const std::map<std::string, std::string, std::less<>> isotopes{{"CDD", "Ar36"}, {"H2", "Ar41"}};
    if (const auto it = isotopes.find(detector); it != isotopes.end()) return it->second;
    return std::nullopt;
  }
};

const Catalog kCatalog;

constexpr std::string_view kHeader = R"(
[plan]
name = "desugar"
instrument_family = "thermo_argus"
)";

constexpr std::string_view kReferenceH1 = R"(
[detectors]
reference = "H1"
)";

plan::MeasurementPlan resolve(std::string_view body, const plan::ISpectrometerCatalog* catalog = &kCatalog,
                              std::string_view detectors = kReferenceH1) {
  const std::string text = std::string(kHeader) + std::string(detectors) + std::string(body);
  auto r = plan::resolve_plan(text, plan::PlanResolvers{nullptr, catalog}, "desugar");
  EXPECT_TRUE(r.has_value()) << (r ? "" : r.error().what);
  return r.value_or(plan::MeasurementPlan{});
}

// The segments of the one implicit gas, or empty (with a failure) when the
// plan does not have exactly one.
std::vector<plan::Segment> segments_of(const plan::MeasurementPlan& p) {
  if (p.gases.size() != 1) {
    ADD_FAILURE() << "expected one gas, got " << p.gases.size();
    return {};
  }
  return p.gases.front().segments;
}

std::vector<plan::Segment> of_kind(const std::vector<plan::Segment>& segs, plan::SegmentKind kind) {
  std::vector<plan::Segment> out;
  for (const auto& s : segs)
    if (s.kind == kind) out.push_back(s);
  return out;
}

plan::CollectEntry entry(std::string label, std::string detector) {
  plan::CollectEntry e;
  e.label = std::move(label);
  e.detector = std::move(detector);
  return e;
}

const plan::CollectEntry* find_entry(const plan::Position& pos, std::string_view detector) {
  for (const auto& e : pos.collect)
    if (e.detector == detector) return &e;
  return nullptr;
}

}  // namespace

TEST(PlanDesugar, MainBecomesOneSignalSegment) {
  const auto p = resolve(R"(
[main]
cycles = 3
integration_s = 2
[[main.hops]]
positions = { Ar40 = "H1", Ar36 = "CDD", Ar39 = "AX" }
counts = 10
settle_s = 4
[[main.hops]]
positions = { Ar38 = "H1" }
counts = 5
)");
  ASSERT_EQ(p.gases.size(), 1u);
  const auto& gas = p.gases[0];
  EXPECT_TRUE(gas.implicit);
  EXPECT_EQ(gas.equilibration, p.equilibration);
  ASSERT_TRUE(gas.fits);
  EXPECT_EQ(*gas.fits, p.fits);
  ASSERT_EQ(gas.segments.size(), 1u);
  const auto& s = gas.segments[0];
  EXPECT_EQ(s.kind, plan::SegmentKind::Signal);
  EXPECT_EQ(s.when, plan::When::AfterInlet);
  EXPECT_EQ(s.cycles, 3);
  EXPECT_DOUBLE_EQ(s.integration_s, 2);
  ASSERT_TRUE(s.time_zero);
  EXPECT_EQ(s.time_zero->kind, plan::TimeZeroKind::OnInletClose);
  ASSERT_EQ(s.positions.size(), 2u);
  // The hop's positions map, as entries in map (label) order.
  const std::vector<plan::CollectEntry> first{entry("Ar36", "CDD"), entry("Ar39", "AX"), entry("Ar40", "H1")};
  EXPECT_EQ(s.positions[0].collect, first);
  EXPECT_EQ(s.positions[0].kind, plan::PositionKind::Signal);
  EXPECT_EQ(s.positions[0].counts, 10);
  EXPECT_DOUBLE_EQ(s.positions[0].settle_s, 4);
  EXPECT_FALSE(s.positions[0].at);  // reference detector collects: the default target
  EXPECT_FALSE(s.positions[0].integration_s);
  EXPECT_EQ(s.positions[1].collect, (std::vector<plan::CollectEntry>{entry("Ar38", "H1")}));
  EXPECT_EQ(s.positions[1].counts, 5);
}

TEST(PlanDesugar, ExplicitHopPositionBecomesAtLabel) {
  const auto p = resolve(R"(
[main]
[[main.hops]]
positions = { Ar39 = "CDD" }
counts = 5
position = { isotope = "Ar39", detector = "CDD" }
)");
  const auto segs = segments_of(p);
  ASSERT_EQ(segs.size(), 1u);
  ASSERT_EQ(segs[0].positions.size(), 1u);
  ASSERT_TRUE(segs[0].positions[0].at);
  EXPECT_EQ(*segs[0].positions[0].at, plan::At(plan::AtLabel{"Ar39", "CDD"}));
}

TEST(PlanDesugar, ProtectBecomesEntriesAndInactiveEntries) {
  // CDD is collected by another hop: the added entry takes that hop's label.
  const auto p = resolve(R"(
[main]
[[main.hops]]
positions = { Ar40 = "H1" }
counts = 5
protect = ["H1", "CDD"]
[[main.hops]]
positions = { Ar39 = "CDD" }
counts = 5
position = { isotope = "Ar39", detector = "CDD" }
)");
  const auto segs = segments_of(p);
  ASSERT_EQ(segs.size(), 1u);
  const auto& pos = segs[0].positions.at(0);
  ASSERT_EQ(pos.collect.size(), 2u);
  const auto* h1 = find_entry(pos, "H1");
  ASSERT_NE(h1, nullptr);
  EXPECT_EQ(h1->label, "Ar40");
  EXPECT_TRUE(h1->active);
  EXPECT_TRUE(h1->protect);
  const auto* cdd = find_entry(pos, "CDD");
  ASSERT_NE(cdd, nullptr);
  EXPECT_EQ(*cdd, (plan::CollectEntry{"Ar39", "CDD", false, std::nullopt, true}));
  // The hop that collects CDD has no protect list: nothing protected there.
  EXPECT_FALSE(segs[0].positions.at(1).collect.at(0).protect);
}

TEST(PlanDesugar, ProtectedDetectorNoHopCollectsTakesTheCatalogIsotope) {
  constexpr std::string_view body = R"(
[main]
[[main.hops]]
positions = { Ar40 = "H1" }
counts = 5
protect = ["CDD", "L2"]
)";
  const auto with = resolve(body);
  const auto segs = segments_of(with);
  ASSERT_EQ(segs.size(), 1u);
  const auto& pos = segs[0].positions.at(0);
  const auto* cdd = find_entry(pos, "CDD");
  ASSERT_NE(cdd, nullptr);
  EXPECT_EQ(*cdd, (plan::CollectEntry{"Ar36", "CDD", false, std::nullopt, true}));
  // L2 has no configured isotope: its name stands in.
  const auto* l2 = find_entry(pos, "L2");
  ASSERT_NE(l2, nullptr);
  EXPECT_EQ(l2->label, "L2");
  EXPECT_FALSE(l2->active);

  // Without a catalog the detector's name is the label.
  const auto without = resolve(body, nullptr);
  const auto segs2 = segments_of(without);
  ASSERT_EQ(segs2.size(), 1u);
  const auto* cdd2 = find_entry(segs2[0].positions.at(0), "CDD");
  ASSERT_NE(cdd2, nullptr);
  EXPECT_EQ(cdd2->label, "CDD");
}

TEST(PlanDesugar, BaselineHopBecomesBaselinePosition) {
  const auto p = resolve(R"(
[main]
[[main.hops]]
positions = { Ar40 = "H1", Ar39 = "AX" }
counts = 10
[[main.hops]]
positions = { Ar40 = "H1", Ar39 = "AX" }
counts = 20
baseline = true
mass = 34.2
)");
  const auto segs = segments_of(p);
  ASSERT_EQ(segs.size(), 1u);
  ASSERT_EQ(segs[0].positions.size(), 2u);
  EXPECT_EQ(segs[0].positions[0].kind, plan::PositionKind::Signal);
  const auto& bs = segs[0].positions[1];
  EXPECT_EQ(bs.kind, plan::PositionKind::Baseline);
  ASSERT_TRUE(bs.at);
  EXPECT_EQ(*bs.at, plan::At(plan::AtMass{34.2, "H1"}));
  EXPECT_EQ(bs.counts, 20);
}

TEST(PlanDesugar, BaselineBeforeAfterBecomeSegments) {
  const auto p = resolve(R"(
[baseline]
before = true
after = true
counts = 30
mass = 34.2
settle_s = 15
integration_s = 2
[main]
[[main.hops]]
positions = { Ar40 = "H1", Ar39 = "AX" }
counts = 10
[[main.hops]]
positions = { Ar36 = "CDD", Ar38 = "H1" }
counts = 10
)");
  const auto segs = segments_of(p);
  const auto baselines = of_kind(segs, plan::SegmentKind::Baseline);
  ASSERT_EQ(baselines.size(), 2u);
  EXPECT_EQ(baselines[0].when, plan::When::BeforeInlet);
  EXPECT_EQ(baselines[1].when, plan::When::AfterMain);
  // Every detector of any signal position, each once, under its first label.
  const std::vector<plan::CollectEntry> every{entry("Ar39", "AX"), entry("Ar40", "H1"), entry("Ar36", "CDD")};
  for (const auto& b : baselines) {
    EXPECT_DOUBLE_EQ(b.integration_s, 2);
    ASSERT_EQ(b.positions.size(), 1u);
    EXPECT_EQ(b.positions[0].collect, every);
    ASSERT_TRUE(b.positions[0].at);
    EXPECT_EQ(*b.positions[0].at, plan::At(plan::AtMass{34.2, "H1"}));
    EXPECT_EQ(b.positions[0].counts, 30);
    EXPECT_DOUBLE_EQ(b.positions[0].settle_s, 15);
  }
  // Block order: baseline before, signal, baseline after.
  ASSERT_EQ(segs.size(), 3u);
  EXPECT_EQ(segs[0].kind, plan::SegmentKind::Baseline);
  EXPECT_EQ(segs[1].kind, plan::SegmentKind::Signal);
  EXPECT_EQ(segs[2].kind, plan::SegmentKind::Baseline);
}

TEST(PlanDesugar, PeakCenterBecomesSteps) {
  const auto p = resolve(R"(
[peak_center]
before = true
after = true
isotope = "Ar40"
min_signal = 5
[main]
[[main.hops]]
positions = { Ar40 = "H1" }
counts = 10
)");
  const auto segs = segments_of(p);
  const auto pcs = of_kind(segs, plan::SegmentKind::PeakCenter);
  ASSERT_EQ(pcs.size(), 2u);
  EXPECT_TRUE(plan::is_step(pcs[0].kind));
  EXPECT_EQ(pcs[0].when, plan::When::BeforeInlet);
  EXPECT_EQ(pcs[1].when, plan::When::AfterRun);
  ASSERT_TRUE(pcs[0].peak_center);
  ASSERT_TRUE(pcs[1].peak_center);
  EXPECT_EQ(pcs[0].peak_center->label, "Ar40");
  EXPECT_EQ(pcs[0].peak_center->detectors, (std::vector<std::string>{"H1"}));  // the reference
  EXPECT_EQ(pcs[0].peak_center->config, "default");
  EXPECT_DOUBLE_EQ(pcs[0].peak_center->min_signal, 0);
  EXPECT_DOUBLE_EQ(pcs[1].peak_center->min_signal, 5);
  EXPECT_TRUE(pcs[0].positions.empty());
  ASSERT_EQ(segs.size(), 3u);
  EXPECT_EQ(segs.front().kind, plan::SegmentKind::PeakCenter);
  EXPECT_EQ(segs.back().kind, plan::SegmentKind::PeakCenter);
}

TEST(PlanDesugar, SniffAndWhiffBecomeSegments) {
  const auto p = resolve(R"(
[sniff]
enabled = true
counts = 12
integration_s = 0.5
[whiff]
enabled = true
counts = 4
integration_s = 2
[[whiff.checks]]
check = "Ar40 > 100"
action = "pump"
[main]
[[main.hops]]
positions = { Ar40 = "H1", Ar39 = "AX" }
counts = 10
protect = ["H1"]
[[main.hops]]
positions = { Ar36 = "H1" }
counts = 10
)");
  const auto segs = segments_of(p);
  ASSERT_EQ(segs.size(), 3u);
  const std::vector<plan::CollectEntry> first{entry("Ar39", "AX"), entry("Ar40", "H1")};

  const auto& sniff = segs[0];
  EXPECT_EQ(sniff.kind, plan::SegmentKind::Sniff);
  EXPECT_EQ(sniff.when, plan::When::DuringEquilibration);
  EXPECT_DOUBLE_EQ(sniff.integration_s, 0.5);
  ASSERT_EQ(sniff.positions.size(), 1u);
  EXPECT_EQ(sniff.positions[0].collect, first);
  EXPECT_EQ(sniff.positions[0].counts, 12);

  const auto& whiff = segs[1];
  EXPECT_EQ(whiff.kind, plan::SegmentKind::Whiff);
  EXPECT_EQ(whiff.when, plan::When::AfterInlet);
  EXPECT_DOUBLE_EQ(whiff.integration_s, 2);
  ASSERT_EQ(whiff.positions.size(), 1u);
  EXPECT_EQ(whiff.positions[0].collect, first);
  EXPECT_EQ(whiff.positions[0].counts, 4);
  EXPECT_EQ(whiff.checks, p.whiff.checks);
  ASSERT_EQ(whiff.checks.size(), 1u);

  // The whiff runs before the signal segment.
  EXPECT_EQ(segs[2].kind, plan::SegmentKind::Signal);
}

TEST(PlanDesugar, SniffAndWhiffSkipALeadingBaselineHop) {
  const auto p = resolve(R"(
[sniff]
enabled = true
counts = 12
[whiff]
enabled = true
counts = 4
[[whiff.checks]]
check = "Ar40 > 100"
action = "pump"
[main]
[[main.hops]]
positions = { Ar40 = "H1", Ar36 = "CDD" }
counts = 10
baseline = true
mass = 34.2
[[main.hops]]
positions = { Ar39 = "H1" }
counts = 10
)");
  const auto segs = segments_of(p);
  ASSERT_EQ(segs.size(), 3u);
  const std::vector<plan::CollectEntry> signal{entry("Ar39", "H1")};
  ASSERT_EQ(segs[0].positions.size(), 1u);
  EXPECT_EQ(segs[0].positions[0].collect, signal);
  ASSERT_EQ(segs[1].positions.size(), 1u);
  EXPECT_EQ(segs[1].positions[0].collect, signal);
}

TEST(PlanDesugar, ExcludeRemovesFromEveryCollect) {
  const auto p = resolve(R"(
[baseline]
before = true
after = true
counts = 30
[sniff]
enabled = true
counts = 5
[whiff]
enabled = true
counts = 4
[[whiff.checks]]
check = "Ar40 > 100"
action = "run_remainder"
[main]
[[main.hops]]
positions = { Ar40 = "H1", Ar36 = "CDD" }
counts = 10
protect = ["CDD"]
[[main.hops]]
positions = { Ar39 = "H1" }
counts = 10
protect = ["CDD"]
)",
                         &kCatalog, R"(
[detectors]
reference = "H1"
exclude = ["CDD"]
)");
  const auto segs = segments_of(p);
  ASSERT_EQ(segs.size(), 5u);  // baseline, sniff, whiff, signal, baseline
  std::size_t entries = 0;
  for (const auto& s : segs) {
    for (const auto& pos : s.positions) {
      EXPECT_FALSE(pos.collect.empty());
      for (const auto& e : pos.collect) {
        ++entries;
        EXPECT_NE(e.detector, "CDD") << plan::to_string(s.kind);
      }
    }
  }
  EXPECT_EQ(entries, 6u);  // H1 in each of the five segments, once more in the second hop
}

TEST(PlanDesugar, NamesRoundTrip) {
  for (const auto w : {plan::When::DuringExtraction, plan::When::BeforeInlet, plan::When::DuringEquilibration,
                       plan::When::AfterInlet, plan::When::AfterMain, plan::When::AfterRun})
    EXPECT_EQ(plan::parse_when(plan::to_string(w)), w);
  EXPECT_EQ(plan::to_string(plan::When::DuringEquilibration), "during_equilibration");
  EXPECT_FALSE(plan::parse_when("during_inlet"));
  for (const auto k : {plan::SegmentKind::Signal, plan::SegmentKind::Baseline, plan::SegmentKind::Sniff,
                       plan::SegmentKind::Whiff, plan::SegmentKind::PeakCenter, plan::SegmentKind::Set,
                       plan::SegmentKind::BlankBeam, plan::SegmentKind::Park, plan::SegmentKind::Delay,
                       plan::SegmentKind::TimeZero})
    EXPECT_EQ(plan::parse_segment_kind(plan::to_string(k)), k);
  EXPECT_EQ(plan::to_string(plan::SegmentKind::PeakCenter), "peak_center");
  EXPECT_EQ(plan::to_string(plan::SegmentKind::BlankBeam), "blank_beam");
  EXPECT_EQ(plan::to_string(plan::SegmentKind::TimeZero), "time_zero");
  EXPECT_FALSE(plan::is_step(plan::SegmentKind::Whiff));
  EXPECT_TRUE(plan::is_step(plan::SegmentKind::Delay));
}

// ---- native v2 parse (Task 2) ----------------------------------------------------

namespace {

namespace fs = std::filesystem;

// Given by the build, not taken from __FILE__ (see test_plan_schema.cpp).
const fs::path kPlanFixtures = PYCHRON_PLAN_FIXTURES_DIR;

std::string read_text(const fs::path& p) {
  std::ifstream in(p);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// The extraction-line aliases the worked plans name.
class Line final : public plan::IAliasResolver {
 public:
  std::optional<pychron::experiment::ParamValue> resolve_alias(std::string_view key) const override {
    static const std::map<std::string, pychron::experiment::ParamValue, std::less<>> aliases{
        {"valves.inlet", std::string("V12")},     {"valves.outlet", std::string("V13")},
        {"valves.he_inlet", std::string("V20")},  {"valves.ne_inlet", std::string("V21")},
        {"valves.ar_inlet", std::string("V22")},  {"extraction.eqtime", std::int64_t{20}}};
    if (const auto it = aliases.find(key); it != aliases.end()) return it->second;
    return std::nullopt;
  }
};

const Line kLine;
// No spectrometer until the v2 validation (plan Task 3) checks detectors and masses.
const plan::PlanResolvers kWorkedResolvers{&kLine, nullptr};

plan::MeasurementPlan load_worked(const std::string& name) {
  const auto text = read_text(kPlanFixtures / "valid" / name);
  auto t = plan::parse_plan_template(text, name);
  EXPECT_TRUE(t.has_value()) << (t ? "" : t.error().what);
  if (!t) return {};
  auto r = plan::load_plan(*t, {}, kWorkedResolvers);
  EXPECT_TRUE(r.has_value()) << (r ? "" : r.error().what);
  return r ? r->plan : plan::MeasurementPlan{};
}

// The error text of a plan that must not resolve (empty, with a failure, when it does).
std::string problems(std::string_view body, std::string_view detectors = kReferenceH1) {
  const std::string text = std::string(kHeader) + std::string(detectors) + std::string(body);
  auto r = plan::resolve_plan(text, plan::PlanResolvers{nullptr, &kCatalog}, "v2");
  if (r) {
    ADD_FAILURE() << "plan resolved but should not:\n" << body;
    return {};
  }
  return r.error().what;
}

}  // namespace

TEST(PlanV2Parse, EveryWorkedPlanResolves) {
  std::size_t n = 0;
  for (const auto& entry : fs::directory_iterator(kPlanFixtures / "valid")) {
    const auto name = entry.path().filename().string();
    if (entry.path().extension() != ".toml" || !name.starts_with("v2_7")) continue;
    ++n;
    SCOPED_TRACE(name);
    const auto p = load_worked(name);
    EXPECT_FALSE(p.gases.empty());
    for (const auto& gas : p.gases) EXPECT_FALSE(gas.segments.empty()) << gas.name;
  }
  EXPECT_EQ(n, 7u);
}

TEST(PlanV2Parse, WorkedPlanSegmentsAndPositions) {
  // 7.1: a during_extraction baseline at a mass, a signal segment with its
  // time zero, a peak center step after the run.
  const auto p = load_worked("v2_71_argus_syn_baseline.toml");
  ASSERT_EQ(p.gases.size(), 1u);
  const auto& gas = p.gases[0];
  EXPECT_TRUE(gas.implicit);
  EXPECT_EQ(gas.equilibration.inlet, "V12");
  EXPECT_DOUBLE_EQ(gas.equilibration.time_s, 20);
  ASSERT_EQ(gas.segments.size(), 3u);
  const auto& bs = gas.segments[0];
  EXPECT_EQ(bs.kind, plan::SegmentKind::Baseline);
  EXPECT_EQ(bs.when, plan::When::DuringExtraction);
  ASSERT_EQ(bs.positions.size(), 1u);
  EXPECT_EQ(bs.positions[0].collect.size(), 5u);
  EXPECT_EQ(bs.positions[0].at, std::optional<plan::At>(plan::AtMass{34.2, "H1"}));
  EXPECT_EQ(bs.positions[0].counts, 120);
  EXPECT_DOUBLE_EQ(bs.positions[0].settle_s, 20);
  const auto& sig = gas.segments[1];
  EXPECT_EQ(sig.when, plan::When::AfterInlet);
  ASSERT_TRUE(sig.time_zero);
  EXPECT_EQ(sig.time_zero->kind, plan::TimeZeroKind::OnInletClose);
  const auto& pc = gas.segments[2];
  EXPECT_EQ(pc.kind, plan::SegmentKind::PeakCenter);
  EXPECT_EQ(pc.when, plan::When::AfterRun);
  ASSERT_TRUE(pc.peak_center);
  EXPECT_EQ(pc.peak_center->detectors, (std::vector<std::string>{"H1"}));
  EXPECT_EQ(pc.peak_center->extra_detectors, (std::vector<std::string>{"AX", "L2", "CDD"}));

  // 7.4: an inactive protected entry with a deflection, explicit `at`.
  const auto h = load_worked("v2_74_helix_hydride_hcl.toml");
  ASSERT_EQ(h.gases.size(), 1u);
  const auto& pos = h.gases[0].segments.at(0).positions.at(1);
  EXPECT_EQ(pos.collect.at(0), (plan::CollectEntry{"Ar40", "H2(CDD)", false, 3250.0, true}));
  EXPECT_EQ(pos.collect.at(2), (plan::CollectEntry{"Ar38", "AX(CDD)", true, std::nullopt, false}));
  EXPECT_EQ(pos.at, std::optional<plan::At>(plan::AtLabel{"Ar38", "AX(CDD)"}));

  // 7.5: optics and integration per position.
  const auto nb = load_worked("v2_75_noblesse_optics.toml");
  ASSERT_EQ(nb.gases.size(), 1u);
  const auto& nps = nb.gases[0].segments.at(0).positions;
  ASSERT_EQ(nps.size(), 2u);
  EXPECT_EQ(nps[0].optics, "ar_config_a");
  EXPECT_EQ(nps[0].integration_s, std::optional<double>(1));
  EXPECT_EQ(nps[1].integration_s, std::optional<double>(2));
  EXPECT_EQ(nb.gases[0].segments[0].cycles, 15);

  // 7.3: the segment's table is the default of its positions.
  const auto ic = load_worked("v2_73_argus_ic_table.toml");
  ASSERT_EQ(ic.gases.size(), 1u);
  EXPECT_EQ(ic.gases[0].segments.at(2).table, "ic_run");
  EXPECT_TRUE(ic.gases[0].segments.at(2).positions.at(0).table.empty());
}

TEST(PlanV2Parse, CollectShorthandEqualsListForm) {
  // The shorthand map is read in the order toml++ iterates a table, which is
  // key order: Ar36 before Ar40 whatever order the file writes them in.
  const auto p = resolve(R"(
[[segment]]
[[segment.positions]]
collect = { Ar40 = "H1", Ar36 = "CDD" }
counts = 5
settle_s = 2
[[segment.positions]]
collect = [ { label = "Ar36", detector = "CDD" }, { label = "Ar40", detector = "H1" } ]
counts = 5
settle_s = 2
)");
  const auto segs = segments_of(p);
  ASSERT_EQ(segs.size(), 1u);
  ASSERT_EQ(segs[0].positions.size(), 2u);
  EXPECT_EQ(segs[0].positions[0], segs[0].positions[1]);
  EXPECT_EQ(segs[0].positions[0].collect,
            (std::vector<plan::CollectEntry>{entry("Ar36", "CDD"), entry("Ar40", "H1")}));
}

TEST(PlanV2Parse, AtForms) {
  const auto p = resolve(R"(
[[segment]]
[[segment.positions]]
collect = { Ar39 = "CDD" }
at = { label = "Ar39", detector = "CDD" }
counts = 1
[[segment.positions]]
collect = { mass35 = "CDD" }
at = { mass = 35.0, detector = "CDD" }
counts = 1
[[segment.positions]]
collect = { Ar40 = "H1" }
at = { dac = 4.25 }
counts = 1
[[segment.positions]]
collect = { Ar40 = "H1" }
at = { hv = 4500, detector = "H1" }
counts = 1
[[segment.positions]]
collect = { Ar40 = "H1" }
counts = 1
)");
  const auto segs = segments_of(p);
  ASSERT_EQ(segs.size(), 1u);
  const auto& ps = segs[0].positions;
  ASSERT_EQ(ps.size(), 5u);
  EXPECT_EQ(ps[0].at, std::optional<plan::At>(plan::AtLabel{"Ar39", "CDD"}));
  EXPECT_EQ(ps[1].at, std::optional<plan::At>(plan::AtMass{35.0, "CDD"}));
  EXPECT_EQ(ps[2].at, std::optional<plan::At>(plan::AtDac{4.25}));
  EXPECT_EQ(ps[3].at, std::optional<plan::At>(plan::AtHv{4500, "H1"}));
  EXPECT_FALSE(ps[4].at);

  const std::string two = problems(R"(
[[segment]]
[[segment.positions]]
collect = { Ar40 = "H1" }
at = { mass = 35.0, dac = 3.0, detector = "H1" }
counts = 1
)");
  EXPECT_NE(two.find("segment[0].positions[0].at: expected exactly one of 'label', 'mass', 'dac' or 'hv'"),
            std::string::npos)
      << two;
  const std::string none = problems(R"(
[[segment]]
[[segment.positions]]
collect = { Ar40 = "H1" }
at = { detector = "H1" }
counts = 1
)");
  EXPECT_NE(none.find("expected exactly one of"), std::string::npos) << none;
  const std::string dac_det = problems(R"(
[[segment]]
[[segment.positions]]
collect = { Ar40 = "H1" }
at = { dac = 3.0, detector = "H1" }
counts = 1
)");
  EXPECT_NE(dac_det.find("segment[0].positions[0].at.detector: not allowed with 'dac'"), std::string::npos)
      << dac_det;
  const std::string no_det = problems(R"(
[[segment]]
[[segment.positions]]
collect = { Ar40 = "H1" }
at = { mass = 35.0 }
counts = 1
)");
  EXPECT_NE(no_det.find("segment[0].positions[0].at.detector: required"), std::string::npos) << no_det;
}

TEST(PlanV2Parse, StepTables) {
  const auto p = resolve(R"(
[[segment]]
kind = "peak_center"
when = "before_inlet"
peak_center = { label = "Ar40", detectors = ["H1", "AX", "L2"], extra_detectors = ["CDD"], config = "fast", write_table = "ic_run", min_signal = 2 }
[[segment]]
kind = "peak_center"
peak_center = { label = "Ar40" }
[[segment]]
kind = "set"
set = { deflection = { CDD = 2000 }, gain = { H1 = 1.0 }, hv = 4500, cdd_voltage = 1800, source = { trap = 250 }, integration_s = 2 }
[[segment]]
kind = "blank_beam"
blank_beam = true
[[segment]]
kind = "park"
park = { mass = 35.5, detector = "CDD" }
[[segment]]
kind = "park"
park = { label = "Ar40", detector = "H1" }
[[segment]]
kind = "delay"
delay = { seconds = 30 }
[[segment]]
kind = "time_zero"
time_zero = "now"
[[segment]]
kind = "time_zero"
time_zero = { offset_s = 5 }
)");
  const auto segs = segments_of(p);
  ASSERT_EQ(segs.size(), 9u);
  for (const auto& s : segs) {
    EXPECT_TRUE(plan::is_step(s.kind)) << plan::to_string(s.kind);
    EXPECT_TRUE(s.positions.empty());
  }

  ASSERT_TRUE(segs[0].peak_center);
  EXPECT_EQ(segs[0].when, plan::When::BeforeInlet);
  const auto& pc = *segs[0].peak_center;
  EXPECT_EQ(pc.label, "Ar40");
  EXPECT_EQ(pc.detectors, (std::vector<std::string>{"H1", "AX", "L2"}));
  EXPECT_EQ(pc.extra_detectors, (std::vector<std::string>{"CDD"}));
  EXPECT_EQ(pc.config, "fast");
  EXPECT_EQ(pc.write_table, "ic_run");
  EXPECT_DOUBLE_EQ(pc.min_signal, 2);
  // No detector: the reference, as v1.
  ASSERT_TRUE(segs[1].peak_center);
  EXPECT_EQ(segs[1].when, plan::When::AfterInlet);
  EXPECT_EQ(segs[1].peak_center->detectors, (std::vector<std::string>{"H1"}));
  EXPECT_EQ(segs[1].peak_center->config, "default");

  ASSERT_TRUE(segs[2].set);
  const auto& set = *segs[2].set;
  EXPECT_DOUBLE_EQ(set.deflection.at("CDD"), 2000);
  EXPECT_DOUBLE_EQ(set.gain.at("H1"), 1.0);
  EXPECT_DOUBLE_EQ(set.source.at("trap"), 250);
  EXPECT_EQ(set.hv, std::optional<double>(4500));
  EXPECT_EQ(set.cdd_voltage, std::optional<double>(1800));
  EXPECT_EQ(set.integration_s, std::optional<double>(2));

  EXPECT_EQ(segs[3].blank_beam, std::optional<bool>(true));

  ASSERT_TRUE(segs[4].park);
  EXPECT_EQ(*segs[4].park, (plan::ParkStep{"", 35.5, "CDD"}));
  ASSERT_TRUE(segs[5].park);
  EXPECT_EQ(*segs[5].park, (plan::ParkStep{"Ar40", std::nullopt, "H1"}));

  EXPECT_EQ(segs[6].delay_s, std::optional<double>(30));

  ASSERT_TRUE(segs[7].time_zero);
  EXPECT_EQ(segs[7].time_zero->kind, plan::TimeZeroKind::Now);
  ASSERT_TRUE(segs[8].time_zero);
  EXPECT_EQ(segs[8].time_zero->kind, plan::TimeZeroKind::Offset);
  EXPECT_DOUBLE_EQ(segs[8].time_zero->offset_s, 5);
}

TEST(PlanV2Parse, SegmentFitsConditionalsAndChecks) {
  const auto p = resolve(R"(
[[segment]]
kind = "whiff"
checks = [{ check = "Ar40 > 100", action = "pump" }]
[[segment.positions]]
collect = { Ar40 = "H1" }
counts = 4
[[segment]]
cycles = 3
integration_s = 2
fits = { "Ar40@H1" = { fit = "parabolic" }, default = { error = "sd", outliers = { enabled = true, iterations = 2, std_devs = 3 } } }
conditionals = { truncations = [{ check = "Ar40 > 8e5", start = 20 }] }
[[segment.positions]]
collect = { Ar40 = "H1" }
counts = 10
kind = "baseline"
integration_s = 0.5
)");
  const auto segs = segments_of(p);
  ASSERT_EQ(segs.size(), 2u);
  EXPECT_EQ(segs[0].kind, plan::SegmentKind::Whiff);
  ASSERT_EQ(segs[0].checks.size(), 1u);
  EXPECT_EQ(segs[0].checks[0], (plan::WhiffCheck{"Ar40 > 100", "pump"}));
  const auto& s = segs[1];
  EXPECT_EQ(s.cycles, 3);
  EXPECT_DOUBLE_EQ(s.integration_s, 2);
  ASSERT_EQ(s.fits.size(), 2u);
  EXPECT_EQ(s.fits.at("Ar40@H1").fit, std::optional(pychron::reduction::FitKind::Parabolic));
  EXPECT_FALSE(s.fits.at("Ar40@H1").error);
  const auto& d = s.fits.at("default");
  EXPECT_EQ(d.error, std::optional(pychron::reduction::ErrorType::Sd));
  ASSERT_TRUE(d.outliers);
  EXPECT_TRUE(d.outliers->enabled);
  EXPECT_EQ(d.outliers->iterations, 2);
  EXPECT_DOUBLE_EQ(d.outliers->std_devs, 3);
  EXPECT_EQ(s.truncations, (std::vector<plan::Truncation>{{"Ar40 > 8e5", 20}}));
  ASSERT_EQ(s.positions.size(), 1u);
  EXPECT_EQ(s.positions[0].kind, plan::PositionKind::Baseline);
  EXPECT_EQ(s.positions[0].integration_s, std::optional<double>(0.5));
}

TEST(PlanV2Parse, GasesInFileOrder) {
  const auto p = load_worked("v2_76_helix_sft_he_ne_ar.toml");
  ASSERT_EQ(p.gases.size(), 3u);
  EXPECT_EQ(p.gases[0].name, "He");
  EXPECT_EQ(p.gases[1].name, "Ne");
  EXPECT_EQ(p.gases[2].name, "Ar");
  for (const auto& g : p.gases) EXPECT_FALSE(g.implicit) << g.name;
  const auto& he = p.gases[0];
  ASSERT_TRUE(he.time_zero);
  EXPECT_EQ(he.time_zero->kind, plan::TimeZeroKind::OnInletClose);
  EXPECT_EQ(he.equilibration.inlet, "V20");
  EXPECT_EQ(he.equilibration.outlet, "V13");
  EXPECT_DOUBLE_EQ(he.equilibration.time_s, 20);
  ASSERT_EQ(he.segments.size(), 2u);
  EXPECT_EQ(he.segments[1].when, plan::When::AfterMain);
  const auto& ne = p.gases[1];
  EXPECT_DOUBLE_EQ(ne.equilibration.time_s, 30);
  EXPECT_EQ(ne.equilibration.inlet, "V21");
  EXPECT_FALSE(ne.time_zero);
  ASSERT_EQ(ne.segments.size(), 1u);
  EXPECT_EQ(ne.segments[0].positions.size(), 5u);
  EXPECT_EQ(ne.segments[0].cycles, 10);
  // A gas without its own equilibration takes the top-level table's values.
  const auto& ar = p.gases[2];
  EXPECT_EQ(ar.equilibration.inlet, "V22");
  EXPECT_DOUBLE_EQ(ar.equilibration.time_s, p.equilibration.time_s);
  EXPECT_DOUBLE_EQ(ar.equilibration.inlet_delay_s, 3);
  ASSERT_TRUE(ar.fits);
  EXPECT_EQ(*ar.fits, p.fits);  // no gas fits: the plan's [fits]
}

TEST(PlanV2Parse, UnknownKeysAtEveryLevel) {
  struct Case {
    std::string_view body;
    std::string_view path;
  };
  const std::vector<Case> cases{
      {R"(
[[gas]]
name = "He"
bogus = 1
[[gas.segment]]
[[gas.segment.positions]]
collect = { He4 = "H1" }
counts = 1
)",
       "gas[0]"},
      {R"(
[[gas]]
name = "He"
equilibration = { time_s = 5, bogus = 1 }
[[gas.segment]]
[[gas.segment.positions]]
collect = { He4 = "H1" }
counts = 1
)",
       "gas[0].equilibration"},
      {R"(
[[gas]]
name = "He"
[[gas.segment]]
[[gas.segment.positions]]
collect = [{ label = "He4", detector = "H1", bogus = 1 }]
counts = 1
)",
       "gas[0].segment[0].positions[0].collect[0]"},
      {R"(
[[segment]]
bogus = 1
[[segment.positions]]
collect = { Ar40 = "H1" }
counts = 1
)",
       "segment[0]"},
      {R"(
[[segment]]
[[segment.positions]]
collect = { Ar40 = "H1" }
counts = 1
[[segment.positions]]
collect = { Ar39 = "H1" }
counts = 1
bogus = 1
)",
       "segment[0].positions[1]"},
      {R"(
[[segment]]
[[segment.positions]]
collect = [{ label = "Ar40", detector = "H1" }, { label = "Ar39", detector = "AX", bogus = 1 }]
counts = 1
)",
       "segment[0].positions[0].collect[1]"},
      {R"(
[[segment]]
[[segment.positions]]
collect = { Ar40 = "H1" }
at = { mass = 34, detector = "H1", bogus = 1 }
counts = 1
)",
       "segment[0].positions[0].at"},
      {R"(
[[segment]]
fits = { Ar40 = { fit = "linear", bogus = 1 } }
[[segment.positions]]
collect = { Ar40 = "H1" }
counts = 1
)",
       "segment[0].fits.Ar40"},
      {R"(
[[segment]]
conditionals = { bogus = 1 }
[[segment.positions]]
collect = { Ar40 = "H1" }
counts = 1
)",
       "segment[0].conditionals"},
      {R"(
[[segment]]
kind = "peak_center"
peak_center = { label = "Ar40", bogus = 1 }
)",
       "segment[0].peak_center"},
      {R"(
[[segment]]
kind = "set"
set = { hv = 4500, bogus = 1 }
)",
       "segment[0].set"},
      {R"(
[[segment]]
kind = "park"
park = { mass = 35, detector = "CDD", bogus = 1 }
)",
       "segment[0].park"},
      {R"(
[[segment]]
kind = "delay"
delay = { seconds = 3, bogus = 1 }
)",
       "segment[0].delay"},
      {R"(
[[segment]]
kind = "time_zero"
time_zero = { offset_s = 3, bogus = 1 }
)",
       "segment[0].time_zero"},
  };
  for (const auto& c : cases) {
    SCOPED_TRACE(c.path);
    const auto what = problems(c.body);
    EXPECT_NE(what.find("v2: " + std::string(c.path) + ": unknown key 'bogus'"), std::string::npos) << what;
  }
}

TEST(PlanV2Parse, ShapeErrors) {
  struct Case {
    std::string_view body;
    std::string_view expect;
  };
  const std::vector<Case> cases{
      {"[[segment]]\nkind = \"sideways\"\n[[segment.positions]]\ncollect = { Ar40 = \"H1\" }\ncounts = 1\n",
       "segment[0].kind: expected signal, baseline, sniff, whiff, peak_center, set, blank_beam, park, delay or "
       "time_zero"},
      {"[[segment]]\nwhen = \"later\"\n[[segment.positions]]\ncollect = { Ar40 = \"H1\" }\ncounts = 1\n",
       "segment[0].when: expected during_extraction, before_inlet, during_equilibration, after_inlet, after_main "
       "or after_run"},
      {"[[segment]]\nkind = \"signal\"\n", "segment[0].positions: at least one position required"},
      {"[[segment]]\nkind = \"delay\"\ndelay = { seconds = 1 }\n[[segment.positions]]\ncollect = { Ar40 = \"H1\" "
       "}\ncounts = 1\n",
       "segment[0].positions: not allowed on a delay step"},
      {"[[segment]]\nkind = \"delay\"\n", "segment[0].delay: required"},
      {"[[segment]]\nkind = \"set\"\nset = { hv = 1 }\ndelay = { seconds = 1 }\n",
       "segment[0].delay: only allowed on a delay step"},
      {"[[segment]]\nkind = \"time_zero\"\ntime_zero = \"on_inlet_close\"\n",
       "segment[0].time_zero: expected 'now' or { offset_s = N }"},
      {"[[segment]]\ntime_zero = \"now\"\n[[segment.positions]]\ncollect = { Ar40 = \"H1\" }\ncounts = 1\n",
       "segment[0].time_zero: expected 'on_inlet_close', 'on_first_count' or { offset_s = N }"},
      {"[[segment]]\nkind = \"peak_center\"\npeak_center = { label = \"Ar40\", detector = \"H1\", detectors = "
       "[\"AX\"] }\n",
       "segment[0].peak_center: 'detector' and 'detectors' cannot both appear"},
      {"[[segment]]\nkind = \"peak_center\"\npeak_center = { detector = \"H1\" }\n",
       "segment[0].peak_center.label: required"},
      {"[[segment]]\nkind = \"park\"\npark = { detector = \"H1\" }\n",
       "segment[0].park: expected exactly one of 'mass' or 'label'"},
      {"[[segment]]\nchecks = [{ check = \"Ar40 > 1\", action = \"pump\" }]\n[[segment.positions]]\ncollect = { "
       "Ar40 = \"H1\" }\ncounts = 1\n",
       "segment[0].checks: only allowed on whiff segments"},
      {"[[segment]]\n[[segment.positions]]\ncounts = 1\n", "segment[0].positions[0].collect: required"},
      {"[[segment]]\n[[segment.positions]]\ncollect = { Ar40 = \"H1\" }\n",
       "segment[0].positions[0].counts: must be >= 1"},
      {"[[segment]]\n[[segment.positions]]\ncollect = [{ label = \"Ar40\" }]\ncounts = 1\n",
       "segment[0].positions[0].collect[0].detector: required"},
      {"[[segment]]\n[[segment.positions]]\ncollect = { Ar40 = \"H1\" }\nkind = \"sniff\"\ncounts = 1\n",
       "segment[0].positions[0].kind: expected 'signal' or 'baseline'"},
      {"[[gas]]\n[[gas.segment]]\n[[gas.segment.positions]]\ncollect = { Ar40 = \"H1\" }\ncounts = 1\n",
       "gas[0].name: required"},
      {"[[gas]]\nname = \"Ar\"\n", "gas[0].segment: at least one segment required"},
      {"[[gas]]\nname = \"Ar\"\n[[gas.segment]]\n[[gas.segment.positions]]\ncollect = { Ar40 = \"H1\" }\ncounts = "
       "1\n[[gas]]\nname = \"Ar\"\n[[gas.segment]]\n[[gas.segment.positions]]\ncollect = { Ar39 = \"H1\" }\ncounts "
       "= 1\n",
       "gas[1].name: gas 'Ar' named twice"},
      {"[main]\n[[main.hops]]\npositions = { Ar40 = \"H1\" }\ncounts = 1\n[[segment]]\n[[segment.positions]]\ncollect "
       "= { Ar40 = \"H1\" }\ncounts = 1\n",
       "[main] and [[segment]] cannot both appear"},
      {"[[segment]]\n[[segment.positions]]\ncollect = { Ar40 = \"\" }\ncounts = 1\n",
       "segment[0].positions[0].collect.Ar40: required"},
      {"[main]\n[[main.hops]]\npositions = { Ar40 = \"H1\" }\ncounts = 1\n[[gas]]\nname = "
       "\"Ar\"\n[[gas.segment]]\n[[gas.segment.positions]]\ncollect = { Ar40 = \"H1\" }\ncounts = 1\n",
       "[main] and [[gas]] cannot both appear"},
      {"[[segment]]\n[[segment.positions]]\ncollect = { Ar40 = \"H1\" }\ncounts = 1\n[[gas]]\nname = "
       "\"Ar\"\n[[gas.segment]]\n[[gas.segment.positions]]\ncollect = { Ar40 = \"H1\" }\ncounts = 1\n",
       "[[gas]] and top-level [[segment]] cannot both appear"},
      {"[baseline]\nafter = true\ncounts = 5\n[[segment]]\n[[segment.positions]]\ncollect = { Ar40 = \"H1\" "
       "}\ncounts = 1\n",
       "baseline.after: only with [main]; in a plan of segments write a baseline segment"},
      {"[sniff]\nenabled = true\ncounts = 5\n[[segment]]\n[[segment.positions]]\ncollect = { Ar40 = \"H1\" "
       "}\ncounts = 1\n",
       "sniff.enabled: only with [main]; in a plan of segments write a sniff segment"},
  };
  for (const auto& c : cases) {
    SCOPED_TRACE(c.expect);
    const auto what = problems(c.body);
    EXPECT_NE(what.find(c.expect), std::string::npos) << what;
  }
}

TEST(PlanV2Parse, GasFitsOverrideThePlansKeyByKey) {
  const std::string text = std::string(kHeader) + R"(
[fits]
signal = { default = "linear", Ar40 = "parabolic" }
[[gas]]
name = "Ar"
fits = { error = "sd", signal = { Ar36 = "average" } }
[[gas.segment]]
[[gas.segment.positions]]
collect = { Ar40 = "H1" }
counts = 1
)";
  auto r = plan::resolve_plan(text, plan::PlanResolvers{nullptr, &kCatalog}, "v2");
  ASSERT_TRUE(r.has_value()) << r.error().what;
  ASSERT_EQ(r->gases.size(), 1u);
  ASSERT_TRUE(r->gases[0].fits);
  const auto& f = *r->gases[0].fits;
  EXPECT_EQ(f.error, pychron::reduction::ErrorType::Sd);
  EXPECT_EQ(f.signal.at("Ar40"), pychron::reduction::FitKind::Parabolic);
  EXPECT_EQ(f.signal.at("Ar36"), pychron::reduction::FitKind::Average);
  EXPECT_EQ(f.signal.at("default"), pychron::reduction::FitKind::Linear);
  EXPECT_EQ(r->fits.error, pychron::reduction::ErrorType::Sem);  // the plan's own stays
}
