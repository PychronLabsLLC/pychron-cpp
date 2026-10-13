// Plan parameters: what the measurement panel lists and edits.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>

#include "pychron/experiment/plan/parameters.hpp"

namespace pychron::experiment::plan {
namespace {

PlanTemplate example() {
  std::ifstream in(std::filesystem::path(PYCHRON_EXAMPLE_CONFIGS_DIR) / "plans" / "sim_multicollect.toml");
  std::stringstream ss;
  ss << in.rdbuf();
  auto t = parse_plan_template(ss.str(), "sim_multicollect.toml");
  EXPECT_TRUE(t) << t.error().what;
  return *t;
}

constexpr const char* kOther = R"(
[plan]
name = "argus_hop"
instrument_family = "argus"
analysis_types = ["unknown"]

[detectors]
reference = "H1"
exclude = ["CDD", "L2"]

[equilibration]
time_s = 20.0
close_inlet = true

[baseline]
after = true
counts = 30
integration_s = 1

[main]
cycles = 3
integration_s = 1

[[main.hops]]
positions = { Ar40 = "H1" }
counts = 10

[parameters]
expose = [{ path = "main.cycles", label = "Cycles" }, "baseline", "detectors.exclude"]
)";

PlanTemplate other() {
  auto t = parse_plan_template(kOther, "argus_hop.toml");
  EXPECT_TRUE(t) << t.error().what;
  return *t;
}

const PlanParameter* find(const std::vector<PlanParameter>& ps, const std::string& path) {
  for (const auto& p : ps)
    if (p.path == path) return &p;
  return nullptr;
}

TEST(PlanParameters, ExposedInExposeOrderWithTypesAndDefaults) {
  auto ps = plan_parameters(example());
  ASSERT_TRUE(ps) << ps.error().what;
  ASSERT_EQ(ps->size(), 6u);
  EXPECT_EQ((*ps)[0].path, "main.cycles");
  EXPECT_EQ((*ps)[0].kind, ParamKind::Int);
  EXPECT_EQ((*ps)[0].value, ParamValue(std::int64_t{2}));
  EXPECT_TRUE((*ps)[0].exposed);
  EXPECT_EQ((*ps)[1].path, "main.hops[0].counts");
  EXPECT_EQ((*ps)[3].path, "baseline.counts");
  EXPECT_EQ((*ps)[4].path, "main.time_zero");
  EXPECT_EQ((*ps)[4].label, "Time zero");
  EXPECT_EQ((*ps)[4].kind, ParamKind::String);
  EXPECT_EQ((*ps)[4].value, ParamValue(std::string("on_inlet_close")));
  EXPECT_EQ((*ps)[5].path, "peak_center.min_signal");
  EXPECT_EQ((*ps)[5].label, "Peak center minimum signal");
  EXPECT_EQ((*ps)[5].kind, ParamKind::Float);
  EXPECT_EQ((*ps)[5].value, ParamValue(3.0));

  // An exposed table lists every value under it; labels come from expose.
  ps = plan_parameters(other());
  ASSERT_TRUE(ps);
  EXPECT_EQ(find(*ps, "main.cycles")->label, "Cycles");
  ASSERT_NE(find(*ps, "baseline.after"), nullptr);
  EXPECT_EQ(find(*ps, "baseline.after")->kind, ParamKind::Bool);
  EXPECT_EQ(find(*ps, "baseline.integration_s")->kind, ParamKind::Int);  // as authored
  const auto* exclude = find(*ps, "detectors.exclude");
  ASSERT_NE(exclude, nullptr);
  EXPECT_EQ(exclude->kind, ParamKind::List);
  EXPECT_EQ(exclude->value, ParamValue(std::string("CDD, L2")));
  EXPECT_EQ(find(*ps, "equilibration.time_s"), nullptr);  // not exposed
}

// Time zero a number of seconds after the inlet opens is a table in the plan:
// exposed, it is shown as that number, and a run may change it.
TEST(PlanParameters, ATimeZeroOffsetIsShownAsItsSeconds) {
  std::string text = kOther;
  const std::string main_table = "[main]\ncycles = 3\n";
  const auto at = text.find(main_table);
  ASSERT_NE(at, std::string::npos);
  text.insert(at + main_table.size(), "time_zero = { offset_s = 20.0 }\n");
  const std::string expose = R"("detectors.exclude"])";
  const auto end = text.find(expose);
  ASSERT_NE(end, std::string::npos);
  text.replace(end, expose.size(), R"("detectors.exclude", { path = "main.time_zero", label = "Time zero" }])");
  auto tmpl = parse_plan_template(text, "argus_hop.toml");
  ASSERT_TRUE(tmpl) << tmpl.error().what;

  auto ps = plan_parameters(*tmpl);
  ASSERT_TRUE(ps) << ps.error().what;
  const auto* offset = find(*ps, "main.time_zero.offset_s");
  ASSERT_NE(offset, nullptr);
  EXPECT_EQ(offset->kind, ParamKind::Float);  // as authored: written 20, it would take whole seconds only
  EXPECT_EQ(offset->value, ParamValue(20.0));
  EXPECT_TRUE(offset->exposed);

  auto changed = render_effective_plan(*tmpl, {{"main.time_zero.offset_s", 35.5}}, LoadOptions{false});
  ASSERT_TRUE(changed) << changed.error().what;
  EXPECT_NE(changed->find("35.5"), std::string::npos) << *changed;
}

TEST(PlanParameters, AllForAdvancedMode) {
  auto ps = plan_parameters(example(), true);
  ASSERT_TRUE(ps);
  EXPECT_EQ(find(*ps, "plan.name"), nullptr);
  EXPECT_EQ(find(*ps, "parameters.expose"), nullptr);
  const auto* eq = find(*ps, "equilibration.time_s");
  ASSERT_NE(eq, nullptr);
  EXPECT_EQ(eq->kind, ParamKind::Alias);
  EXPECT_EQ(eq->value, ParamValue(std::string("@extraction.eqtime")));
  EXPECT_FALSE(eq->exposed);
  ASSERT_NE(find(*ps, "main.hops[1].positions.Ar36"), nullptr);
  EXPECT_EQ(find(*ps, "main.hops[1].positions.Ar36")->value, ParamValue(std::string("CDD")));
  EXPECT_TRUE(find(*ps, "main.hops[1].counts")->exposed);
  EXPECT_EQ(find(*ps, "sniff.enabled")->kind, ParamKind::Bool);
  EXPECT_EQ(find(*ps, "equilibration.inlet_delay_s")->kind, ParamKind::Int);
}

TEST(PlanParameters, InfoFamiliesAndMatching) {
  PlanLibrary lib;
  lib.add(example());
  lib.add(other());
  const PlanInfo info = plan_info(*lib.find("sim_multicollect"));
  EXPECT_EQ(info.instrument_family, "sim");
  EXPECT_EQ(info.analysis_types, (std::vector<std::string>{"unknown", "blank_unknown", "air", "blank_air"}));
  EXPECT_EQ(plan_families(lib), (std::vector<std::string>{"argus", "sim"}));
  EXPECT_EQ(matching_plans(lib, "", AnalysisType::Unknown), (std::vector<std::string>{"argus_hop", "sim_multicollect"}));
  EXPECT_EQ(matching_plans(lib, "argus", AnalysisType::Unknown), std::vector<std::string>{"argus_hop"});
  EXPECT_EQ(matching_plans(lib, "", AnalysisType::Air), std::vector<std::string>{"sim_multicollect"});
  // The example's air queue (experiment.sim-air.toml) runs its blanks on it.
  EXPECT_EQ(matching_plans(lib, "", AnalysisType::BlankAir), std::vector<std::string>{"sim_multicollect"});
  EXPECT_TRUE(matching_plans(lib, "", AnalysisType::Cocktail).empty());
}

TEST(PlanParameters, TypedTextAndComparison) {
  EXPECT_EQ(parse_param(ParamKind::Int, " 12 "), ParamValue(std::int64_t{12}));
  EXPECT_FALSE(parse_param(ParamKind::Int, "1.5"));
  EXPECT_EQ(parse_param(ParamKind::Float, "3"), ParamValue(3.0));
  EXPECT_FALSE(parse_param(ParamKind::Float, "fast"));
  EXPECT_EQ(parse_param(ParamKind::Bool, "Yes"), ParamValue(true));
  EXPECT_FALSE(parse_param(ParamKind::Bool, "maybe"));
  EXPECT_EQ(parse_param(ParamKind::List, "CDD, L2"), ParamValue(std::string("CDD, L2")));
  EXPECT_EQ(parse_param(ParamKind::Alias, "25"), ParamValue(std::int64_t{25}));
  EXPECT_EQ(parse_param(ParamKind::Alias, "12.5"), ParamValue(12.5));
  EXPECT_EQ(parse_param(ParamKind::Alias, "@other.key"), ParamValue(std::string("@other.key")));
  EXPECT_FALSE(parse_param(ParamKind::Alias, " "));

  EXPECT_EQ(format_param(ParamValue(std::int64_t{3})), "3");
  EXPECT_EQ(format_param(ParamValue(2.5)), "2.5");
  EXPECT_EQ(format_param(ParamValue(true)), "true");
  EXPECT_TRUE(same_param(ParamValue(std::int64_t{2}), ParamValue(2.0)));
  EXPECT_FALSE(same_param(ParamValue(std::int64_t{2}), ParamValue(2.5)));
  EXPECT_FALSE(same_param(ParamValue(std::string("2")), ParamValue(std::int64_t{2})));
}

TEST(PlanParameters, OverridesThatSurviveAPlanChange) {
  const ParamOverrides overrides{{"main.cycles", std::int64_t{4}},
                                 {"main.hops[1].counts", std::int64_t{20}},
                                 {"sniff.counts", std::int64_t{5}}};
  // argus_hop exposes main.cycles but has one hop and no sniff.
  EXPECT_EQ(overrides_for(other(), overrides, false), (ParamOverrides{{"main.cycles", std::int64_t{4}}}));
  // sim_multicollect: sniff.counts is not exposed unless advanced.
  EXPECT_EQ(overrides_for(example(), overrides, false).size(), 2u);
  EXPECT_EQ(overrides_for(example(), overrides, true).size(), 3u);
}

}  // namespace
}  // namespace pychron::experiment::plan
