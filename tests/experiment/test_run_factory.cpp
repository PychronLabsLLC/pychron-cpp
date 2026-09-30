#include <gtest/gtest.h>

#include "pychron/experiment/factory/blocks.hpp"
#include "pychron/experiment/factory/defaults.hpp"
#include "pychron/experiment/factory/increments.hpp"
#include "pychron/experiment/factory/special_runs.hpp"
#include "pychron/experiment/model/positions.hpp"
#include "pychron/experiment/model/rules.hpp"

using namespace pychron::experiment;
using namespace std::chrono_literals;

namespace {

constexpr const char* kDefaults = R"(
[unknown.co2]
template = "thermo_argus/multicollect"
script = "felix_co2"
post_equilibration = "pump_extraction_line"
post_measurement = "pump_ms"
[unknown.co2.extraction]
units = "percent"
value = 2.5
duration = 30
cleanup = 15
[unknown.co2.overrides]
"main.counts" = 400
"peak_center.before" = true

[unknown."*"]
template = "thermo_argus/multicollect"
script = "generic_extract"

[blank_unknown."*"]
template = "thermo_argus/blank"
script = "blank_extract"
[blank_unknown."*".extraction]
duration = 30
value = 5

[air."*"]
template = "thermo_argus/air"
script = "air_pipette"
)";

DefaultsTable table() {
  auto t = DefaultsTable::from_toml(kDefaults);
  EXPECT_TRUE(t) << (t ? "" : t.error().what);
  return t ? *t : DefaultsTable{};
}

RunSpec unknown_run(std::string identifier = "20001") {
  RunSpec r;
  r.id.identifier = std::move(identifier);
  r.id.type = AnalysisType::Unknown;
  r.extraction.device = "co2";
  r.measurement.plan = "thermo_argus/multicollect";
  return r;
}

RunSpec special(AnalysisType type, std::string identifier) {
  RunSpec r;
  r.id.identifier = std::move(identifier);
  r.id.type = type;
  r.measurement.plan = "thermo_argus/blank";
  return r;
}

std::vector<std::string> ids_of(const std::vector<RunSpec>& runs) {
  std::vector<std::string> out;
  for (const auto& r : runs) out.push_back(r.id.identifier);
  return out;
}

}  // namespace

// --- defaults.toml ------------------------------------------------------------

TEST(RunDefaults, ParsesPerTypeAndDevice) {
  auto t = table();
  const RunDefaults* d = t.find(AnalysisType::Unknown, "co2");
  ASSERT_NE(d, nullptr);
  EXPECT_EQ(d->template_name, "thermo_argus/multicollect");
  EXPECT_EQ(d->script, "felix_co2");
  EXPECT_EQ(d->post_equilibration, "pump_extraction_line");
  EXPECT_EQ(d->post_measurement, "pump_ms");
  EXPECT_EQ(d->units, Unit::Percent);
  EXPECT_EQ(d->value, 2.5);
  EXPECT_EQ(d->duration, Duration(30));
  EXPECT_EQ(d->cleanup, Duration(15));
  EXPECT_FALSE(d->pre_cleanup);
  ASSERT_EQ(d->overrides.size(), 2u);
  EXPECT_EQ(std::get<std::int64_t>(d->overrides.at("main.counts")), 400);
  EXPECT_EQ(std::get<bool>(d->overrides.at("peak_center.before")), true);
}

TEST(RunDefaults, WildcardDeviceFallback) {
  auto t = table();
  const RunDefaults* d = t.find(AnalysisType::Unknown, "diode");
  ASSERT_NE(d, nullptr);
  EXPECT_EQ(d->script, "generic_extract");
  ASSERT_NE(t.find(AnalysisType::Air, "co2"), nullptr);
  EXPECT_EQ(t.find(AnalysisType::Cocktail, "co2"), nullptr);
}

TEST(RunDefaults, SchemaErrors) {
  EXPECT_FALSE(DefaultsTable::from_toml("[bogus_type.co2]\ntemplate = \"x\"\n"));
  EXPECT_FALSE(DefaultsTable::from_toml("[unknown.co2]\ntempl = \"x\"\n"));
  EXPECT_FALSE(DefaultsTable::from_toml("[unknown.co2]\ntemplate = 3\n"));
  EXPECT_FALSE(DefaultsTable::from_toml("[unknown]\ntemplate = \"x\"\n"));
  EXPECT_FALSE(DefaultsTable::from_toml("[unknown.co2.extraction]\nunits = \"furlongs\"\n"));
  EXPECT_FALSE(DefaultsTable::from_toml("[unknown.co2.extraction]\nbogus = 1\n"));
  EXPECT_FALSE(DefaultsTable::from_toml("[unknown.co2.overrides]\n\"a\" = [1, 2]\n"));
  EXPECT_FALSE(DefaultsTable::from_toml("not toml ="));
  EXPECT_FALSE(DefaultsTable::load("/nonexistent/defaults.toml"));
}

TEST(RunDefaults, SetAndReplace) {
  DefaultsTable t;
  t.set(AnalysisType::Degas, "co2", RunDefaults{.template_name = "a"});
  t.set(AnalysisType::Degas, "co2", RunDefaults{.template_name = "b"});
  ASSERT_NE(t.find(AnalysisType::Degas, "co2"), nullptr);
  EXPECT_EQ(t.find(AnalysisType::Degas, "co2")->template_name, "b");
}

TEST(RunDefaults, ApplyReplacesMeasurementAndScripts) {
  auto t = table();
  RunSpec r = unknown_run();
  r.measurement.overrides["main.counts"] = std::int64_t{10};
  apply_defaults(r, *t.find(AnalysisType::Unknown, "co2"));
  EXPECT_EQ(r.measurement.plan, "thermo_argus/multicollect");
  EXPECT_EQ(std::get<std::int64_t>(r.measurement.overrides.at("main.counts")), 400);
  EXPECT_EQ(r.extraction.script, "felix_co2");
  EXPECT_EQ(r.extraction.units, Unit::Percent);
  EXPECT_EQ(r.extraction.value, 2.5);
  EXPECT_EQ(r.extraction.duration, Duration(30));
  EXPECT_EQ(r.post_measurement, "pump_ms");
}

TEST(RunDefaults, StripForTypeMatchesValidationRules) {
  RunSpec r = unknown_run("a");
  r.id.type = AnalysisType::Air;
  r.extraction.value = 3;
  r.extraction.position = Position{{1}};
  r.extraction.pattern = "spiral";
  r.extraction.ramp = 5s;
  strip_for_type(r);
  EXPECT_EQ(r.extraction.value, 0);
  EXPECT_FALSE(r.extraction.position);
  EXPECT_FALSE(r.extraction.pattern);
  EXPECT_EQ(r.extraction.ramp, Duration(0));
  EXPECT_EQ(r.extraction.device, "co2");  // air keeps its extraction (pipette)
  EXPECT_TRUE(validate_run(r, IdentifierRules::defaults()).empty());

  RunSpec p = unknown_run("pa");
  p.id.type = AnalysisType::Pause;
  p.extraction.script = "x";
  p.extraction.duration = 10s;
  strip_for_type(p);
  EXPECT_TRUE(p.extraction.device.empty());
  EXPECT_TRUE(p.extraction.script.empty());
  EXPECT_TRUE(p.measurement.plan.empty());
  EXPECT_TRUE(validate_run(p, IdentifierRules::defaults()).empty());
}

TEST(RunDefaults, MakeRunClassifiesAndAppliesDefaults) {
  auto t = table();
  auto ids = IdentifierRules::defaults();

  auto u = make_run("20001", ids, "co2", t);
  ASSERT_TRUE(u);
  EXPECT_EQ(u->id.type, AnalysisType::Unknown);
  EXPECT_EQ(u->extraction.device, "co2");
  EXPECT_EQ(u->extraction.script, "felix_co2");

  auto b = make_run("bu", ids, "co2", t);
  ASSERT_TRUE(b);
  EXPECT_EQ(b->id.type, AnalysisType::BlankUnknown);
  EXPECT_EQ(b->measurement.plan, "thermo_argus/blank");
  EXPECT_EQ(b->extraction.value, 5);

  auto a = make_run("a", ids, "co2", t);
  ASSERT_TRUE(a);
  EXPECT_EQ(a->extraction.script, "air_pipette");
  EXPECT_TRUE(validate_run(*a, ids).empty());

  auto p = make_run("pa", ids, "co2", t);
  ASSERT_TRUE(p);
  EXPECT_TRUE(p->extraction.device.empty());

  EXPECT_FALSE(make_run("bad id", ids, "co2", t));
}

TEST(RunDefaults, MakeRunWithoutMatchingDefaultsStillBuildsRun) {
  DefaultsTable empty;
  auto r = make_run("c", IdentifierRules::defaults(), "co2", empty);
  ASSERT_TRUE(r);
  EXPECT_EQ(r->id.type, AnalysisType::Cocktail);
  EXPECT_TRUE(r->measurement.plan.empty());
}

TEST(RunDefaults, MakeSpecialRunUsesTypePrefix) {
  auto t = table();
  auto r = make_special_run(AnalysisType::Air, IdentifierRules::defaults(), "co2", t);
  ASSERT_TRUE(r);
  EXPECT_EQ(r->id.identifier, "a");
  EXPECT_EQ(r->id.type, AnalysisType::Air);
  EXPECT_FALSE(make_special_run(AnalysisType::Unknown, IdentifierRules::defaults(), "co2", t));
}

// --- increments / positions -----------------------------------------------------

TEST(Increments, NextStepIsBijectiveBase26) {
  EXPECT_EQ(next_step(""), "A");
  EXPECT_EQ(next_step("A"), "B");
  EXPECT_EQ(next_step("Z"), "AA");
  EXPECT_EQ(next_step("AZ"), "BA");
  EXPECT_EQ(next_step("ZZ"), "AAA");
  EXPECT_EQ(next_step("b"), "C");
  EXPECT_EQ(step_index("A"), 0);
  EXPECT_EQ(step_index("AA"), 26);
  EXPECT_EQ(step_index(""), -1);
  EXPECT_EQ(step_name(0), "A");
  EXPECT_EQ(step_name(27), "AB");
}

TEST(Increments, IdentifierKeepsPrefixAndPadding) {
  EXPECT_EQ(increment_identifier("20001").value(), "20002");
  EXPECT_EQ(increment_identifier("NM-205", 5).value(), "NM-210");
  EXPECT_EQ(increment_identifier("L009").value(), "L010");
  EXPECT_EQ(increment_identifier("L99").value(), "L100");
  EXPECT_EQ(increment_identifier("L010", -1).value(), "L009");
  EXPECT_FALSE(increment_identifier("abc"));
  EXPECT_FALSE(increment_identifier("L000", -1));
}

TEST(Increments, RunIdFormatting) {
  EXPECT_EQ(format_runid("20001", 1, ""), "20001-01");
  EXPECT_EQ(format_runid("20001", 12, "B"), "20001-12B");
  EXPECT_EQ(format_runid("20001", 123, "A"), "20001-123A");
  auto p = parse_runid("NM-205-03AB");
  ASSERT_TRUE(p);
  EXPECT_EQ(p->identifier, "NM-205");
  EXPECT_EQ(p->aliquot, 3);
  EXPECT_EQ(p->step, "AB");
  EXPECT_FALSE(parse_runid("20001"));
  EXPECT_FALSE(parse_runid("-01"));
}

TEST(Increments, NextTemplateAdvancesIdentifierAndPosition) {
  RunSpec r = unknown_run("20001");
  r.extraction.position = Position{{4, 5}};
  auto n = next_template(r, {.identifier = 1, .position = 2});
  ASSERT_TRUE(n);
  EXPECT_EQ(n->id.identifier, "20002");
  EXPECT_EQ(n->extraction.position->holes, (std::vector<int>{6, 7}));

  auto same = next_template(r, {});
  ASSERT_TRUE(same);
  EXPECT_EQ(*same, r);

  RunSpec s = special(AnalysisType::BlankUnknown, "bu");
  s.extraction.position = Position{{1}};
  auto ns = next_template(s, {.identifier = 1, .position = 1}, IdentifierRules::defaults());
  ASSERT_TRUE(ns);  // special identifiers are never incremented (pychron behavior)
  EXPECT_EQ(ns->id.identifier, "bu");
  EXPECT_EQ(ns->extraction.position->holes, std::vector<int>{2});

  RunSpec fixed = unknown_run("20001");
  fixed.id.aliquot = 3;
  fixed.id.step = "B";
  auto nf = next_template(fixed, {.identifier = 1});
  ASSERT_TRUE(nf);  // a new identifier drops the user-fixed aliquot and step
  EXPECT_FALSE(nf->id.aliquot);
  EXPECT_TRUE(nf->id.step.empty());
  EXPECT_FALSE(next_template(unknown_run("abc"), {.identifier = 1}));
}

TEST(Increments, ExpandPositionsOneRunPerHole) {
  RunSpec tmpl = unknown_run();
  auto pos = parse_position("3-5");
  ASSERT_TRUE(pos);
  auto runs = expand_positions(tmpl, *pos);
  ASSERT_EQ(runs.size(), 3u);
  EXPECT_EQ(runs[0].extraction.position->holes, std::vector<int>{3});
  EXPECT_EQ(runs[2].extraction.position->holes, std::vector<int>{5});
  EXPECT_EQ(runs[1].id.identifier, "20001");
}

TEST(Increments, ExpandPositionsCanAdvanceIdentifier) {
  auto runs = expand_positions(unknown_run("L009"), Position{{1, 2, 3}}, /*identifier_step=*/1);
  ASSERT_TRUE(runs);
  EXPECT_EQ(ids_of(*runs), (std::vector<std::string>{"L009", "L010", "L011"}));
  EXPECT_FALSE(expand_positions(unknown_run("abc"), Position{{1, 2}}, 1));
}

TEST(Increments, StepHeatAssignsStepsAndValues) {
  RunSpec tmpl = unknown_run();
  tmpl.id.aliquot = 4;
  auto runs = make_step_heat(tmpl, {1.0, 2.0, 3.5});
  ASSERT_EQ(runs.size(), 3u);
  EXPECT_EQ(runs[0].id.step, "A");
  EXPECT_EQ(runs[2].id.step, "C");
  EXPECT_EQ(runs[2].extraction.value, 3.5);
  EXPECT_EQ(runs[1].id.aliquot, 4);
  EXPECT_EQ(step_values(1.0, 0.5, 3), (std::vector<double>{1.0, 1.5, 2.0}));
  EXPECT_TRUE(step_values(1.0, 0.5, 0).empty());
}

// --- block templates -------------------------------------------------------------

namespace {
constexpr const char* kBlock = R"(
[block]
name = "blank_air_blank"
description = "bracket with blanks"

[[runs]]
identifier = "bu"
[runs.measurement]
plan = "thermo_argus/blank"

[[runs]]
identifier = "a"

[[runs]]
identifier = "bu"
device = "diode"
[runs.measurement]
plan = "thermo_argus/blank"
)";
}  // namespace

TEST(Blocks, ParseBlockTemplate) {
  auto b = parse_block(kBlock, IdentifierRules::defaults());
  ASSERT_TRUE(b) << b.error().what;
  EXPECT_EQ(b->name, "blank_air_blank");
  EXPECT_EQ(b->description, "bracket with blanks");
  ASSERT_EQ(b->runs.size(), 3u);
  EXPECT_EQ(b->runs[1].id.type, AnalysisType::Air);
  EXPECT_EQ(b->runs[2].extraction.device, "diode");
}

TEST(Blocks, ParseErrors) {
  auto ids = IdentifierRules::defaults();
  EXPECT_FALSE(parse_block("[[runs]]\nidentifier = \"bu\"\n", ids));  // missing [block]
  EXPECT_FALSE(parse_block("[block]\nname = \"x\"\n", ids));          // no runs
  EXPECT_FALSE(parse_block("[block]\nname = \"x\"\nbogus = 1\n[[runs]]\nidentifier = \"a\"\n", ids));
  EXPECT_FALSE(parse_block("[block]\nname = \"x\"\n[[runs]]\nidentifier = \"a\"\nnope = 1\n", ids));
  EXPECT_FALSE(parse_block("[block]\nname = \"x\"\n[queue]\n[[runs]]\nidentifier = \"a\"\n", ids));
  EXPECT_FALSE(load_block("/nonexistent/block.toml", ids));
}

TEST(Blocks, InstantiateFillsDeviceAndDefaults) {
  auto b = parse_block(kBlock, IdentifierRules::defaults());
  ASSERT_TRUE(b);
  auto t = table();
  auto runs = instantiate_block(*b, {.extract_device = "co2", .defaults = &t});
  ASSERT_EQ(runs.size(), 3u);
  EXPECT_EQ(runs[0].extraction.device, "co2");
  EXPECT_EQ(runs[0].extraction.script, "");  // plan was set in the block: defaults not applied
  EXPECT_EQ(runs[1].measurement.plan, "thermo_argus/air");
  EXPECT_EQ(runs[1].extraction.script, "air_pipette");
  EXPECT_EQ(runs[2].extraction.device, "diode");
}

TEST(Blocks, InsertBlockAtIndex) {
  std::vector<RunSpec> q{unknown_run("1"), unknown_run("2")};
  Block b{.name = "x", .runs = {special(AnalysisType::BlankUnknown, "bu"), special(AnalysisType::Air, "a")}};
  auto out = insert_runs(q, 1, instantiate_block(b, {}));
  EXPECT_EQ(ids_of(out), (std::vector<std::string>{"1", "bu", "a", "2"}));
  auto end = insert_runs(q, 99, b.runs);
  EXPECT_EQ(ids_of(end), (std::vector<std::string>{"1", "2", "bu", "a"}));
}

TEST(Blocks, RepeatBlock) {
  Block b{.name = "x", .runs = {unknown_run("1"), special(AnalysisType::BlankUnknown, "bu")}};
  auto out = repeat_block(b.runs, 3);
  EXPECT_EQ(ids_of(out), (std::vector<std::string>{"1", "bu", "1", "bu", "1", "bu"}));
  EXPECT_TRUE(repeat_block(b.runs, 0).empty());
}

// --- special-run insertion -------------------------------------------------------

TEST(SpecialRuns, EveryNUnknowns) {
  std::vector<RunSpec> q{unknown_run("1"), unknown_run("2"), unknown_run("3"), unknown_run("4"), unknown_run("5")};
  auto out = insert_frequency(q, special(AnalysisType::BlankUnknown, "bu"), {.every = 2});
  EXPECT_EQ(ids_of(out), (std::vector<std::string>{"1", "2", "bu", "3", "4", "bu", "5"}));
}

TEST(SpecialRuns, BeforeAndAfter) {
  std::vector<RunSpec> q{unknown_run("1"), unknown_run("2"), unknown_run("3"), unknown_run("4")};
  auto out = insert_frequency(q, special(AnalysisType::BlankUnknown, "bu"), {.every = 2, .before = true, .after = true});
  EXPECT_EQ(ids_of(out), (std::vector<std::string>{"bu", "1", "2", "bu", "3", "4", "bu"}));

  auto only = insert_frequency(q, special(AnalysisType::Air, "a"), {.every = 0, .before = true, .after = true});
  EXPECT_EQ(ids_of(only), (std::vector<std::string>{"a", "1", "2", "3", "4", "a"}));
}

TEST(SpecialRuns, SkippedAndSpecialRunsDoNotCount) {
  std::vector<RunSpec> q{unknown_run("1"), special(AnalysisType::Air, "a"), unknown_run("2"), unknown_run("3"),
                         unknown_run("4")};
  q[2].skip = true;
  auto out = insert_frequency(q, special(AnalysisType::BlankUnknown, "bu"), {.every = 2});
  EXPECT_EQ(ids_of(out), (std::vector<std::string>{"1", "a", "2", "3", "bu", "4"}));
}

TEST(SpecialRuns, AfterGoesAfterLastCountedRun) {
  std::vector<RunSpec> q{unknown_run("1"), unknown_run("2"), special(AnalysisType::Air, "a")};
  auto out = insert_frequency(q, special(AnalysisType::BlankUnknown, "bu"), {.every = 0, .after = true});
  EXPECT_EQ(ids_of(out), (std::vector<std::string>{"1", "2", "bu", "a"}));
}

TEST(SpecialRuns, CountAllTypesAndRange) {
  std::vector<RunSpec> q{unknown_run("1"), special(AnalysisType::Air, "a"), unknown_run("2"), unknown_run("3")};
  auto all = insert_frequency(q, special(AnalysisType::BlankUnknown, "bu"), {.every = 2, .count_all = true});
  EXPECT_EQ(ids_of(all), (std::vector<std::string>{"1", "a", "bu", "2", "3", "bu"}));

  // Only runs [2, 4) are considered.
  auto ranged = insert_frequency(q, special(AnalysisType::BlankUnknown, "bu"), {.every = 1, .first = 2, .last = 4});
  EXPECT_EQ(ids_of(ranged), (std::vector<std::string>{"1", "a", "2", "bu", "3", "bu"}));
}

TEST(SpecialRuns, NoCountedRunsNoInsertion) {
  std::vector<RunSpec> q{special(AnalysisType::Air, "a")};
  auto out = insert_frequency(q, special(AnalysisType::BlankUnknown, "bu"), {.every = 1, .before = true, .after = true});
  EXPECT_EQ(ids_of(out), (std::vector<std::string>{"a"}));
  EXPECT_TRUE(insert_frequency({}, special(AnalysisType::BlankUnknown, "bu"), {.every = 1}).empty());
}
