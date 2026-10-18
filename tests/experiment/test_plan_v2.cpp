// Measurement plan v2 (docs/superpowers/specs/2026-10-05-measurement-plan-v2-design.md):
// the v1 desugar (section 4.1), one test per row of its table.

#include <gtest/gtest.h>

#include <map>
#include <optional>
#include <set>
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
