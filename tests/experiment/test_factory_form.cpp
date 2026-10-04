// The run factory form: parsing, building runs, defaults and auto-increment.

#include <gtest/gtest.h>

#include <filesystem>

#include "pychron/experiment/factory/form.hpp"
#include "pychron/experiment/lab/lab.hpp"
#include "pychron/experiment/model/positions.hpp"

namespace pychron::experiment {
namespace {

const IdentifierRules kIds = IdentifierRules::defaults();

FactoryForm unknown_form() {
  FactoryForm f;
  f.identifier = "20001";
  f.position = "4";
  f.value = 5;
  f.duration_s = 10;
  f.script = "sim_extract";
  f.plan = "sim_multicollect";
  f.post_measurement = "sim_pump";
  return f;
}

std::vector<std::string> ids_of(const std::vector<RunSpec>& runs) {
  std::vector<std::string> out;
  for (const auto& r : runs) out.push_back(r.id.identifier + (r.id.step.empty() ? "" : "/" + r.id.step));
  return out;
}

std::string pos(const RunSpec& r) { return r.extraction.position ? format_position(*r.extraction.position) : "-"; }

TEST(FactoryStepValues, ListsAndRanges) {
  EXPECT_EQ(*parse_step_values(""), std::vector<double>{});
  EXPECT_EQ(*parse_step_values("5, 10;15  20"), (std::vector<double>{5, 10, 15, 20}));
  EXPECT_EQ(*parse_step_values("5:2.5:3"), (std::vector<double>{5, 7.5, 10}));
  for (const char* bad : {"5, x", "5:2", "5:2:0", "5:2:1.5", "a:b:c", "5:1:1001"})
    EXPECT_FALSE(parse_step_values(bad)) << bad;
}

TEST(FactoryForm, OneRunWithEverythingFilledIn) {
  auto f = unknown_form();
  f.aliquot = 3;
  f.step = "B";
  f.comment = "first";
  f.units = Unit::Percent;
  f.cleanup_s = 30;
  auto runs = build_runs(f, kIds);
  ASSERT_TRUE(runs) << runs.error().what;
  ASSERT_EQ(runs->size(), 1u);
  const RunSpec& r = runs->front();
  EXPECT_EQ(r.id.identifier, "20001");
  EXPECT_EQ(r.id.type, AnalysisType::Unknown);
  EXPECT_EQ(r.id.aliquot, 3);
  EXPECT_EQ(r.id.step, "B");
  EXPECT_EQ(pos(r), "4");
  EXPECT_EQ(r.extraction.value, 5);
  EXPECT_EQ(r.extraction.units, Unit::Percent);
  EXPECT_EQ(r.extraction.duration.count(), 10);
  EXPECT_EQ(r.extraction.cleanup.count(), 30);
  EXPECT_EQ(r.extraction.script, "sim_extract");
  EXPECT_EQ(r.measurement.plan, "sim_multicollect");
  EXPECT_EQ(r.post_measurement, std::optional<std::string>("sim_pump"));
  EXPECT_FALSE(r.post_equilibration.has_value());
  EXPECT_EQ(r.comment, "first");

  // The run round-trips through the form.
  auto again = build_runs(form_from_run(r), kIds);
  ASSERT_TRUE(again);
  EXPECT_EQ(again->front(), r);
}

TEST(FactoryForm, MultiHolePositionsAndStepHeats) {
  auto f = unknown_form();
  f.position = "1-3";
  auto runs = build_runs(f, kIds);
  ASSERT_TRUE(runs);
  ASSERT_EQ(runs->size(), 3u);
  EXPECT_EQ(pos((*runs)[0]), "1");
  EXPECT_EQ(pos((*runs)[2]), "3");
  EXPECT_EQ(ids_of(*runs), (std::vector<std::string>{"20001", "20001", "20001"}));

  f.identifier_step = 1;
  runs = build_runs(f, kIds);
  ASSERT_TRUE(runs);
  EXPECT_EQ(ids_of(*runs), (std::vector<std::string>{"20001", "20002", "20003"}));

  f.one_run_per_hole = false;
  runs = build_runs(f, kIds);
  ASSERT_TRUE(runs);
  ASSERT_EQ(runs->size(), 1u);
  EXPECT_EQ(pos(runs->front()), "1-3");

  f = unknown_form();
  f.step_heat = "2:2:3";
  runs = build_runs(f, kIds);
  ASSERT_TRUE(runs) << runs.error().what;
  EXPECT_EQ(ids_of(*runs), (std::vector<std::string>{"20001/A", "20001/B", "20001/C"}));
  EXPECT_EQ((*runs)[2].extraction.value, 6);
  EXPECT_EQ(pos((*runs)[2]), "4");
}

TEST(FactoryForm, FieldsTheTypeDoesNotUseAreDropped) {
  auto f = unknown_form();
  f.identifier = "a";  // air: extraction but no heating or position
  EXPECT_FALSE(form_rules(f, kIds).heating);
  EXPECT_FALSE(form_rules(f, kIds).position);
  EXPECT_TRUE(form_rules(unknown_form(), kIds).heating);
  auto runs = build_runs(f, kIds);
  ASSERT_TRUE(runs);
  EXPECT_EQ(runs->front().id.type, AnalysisType::Air);
  EXPECT_EQ(runs->front().extraction.value, 0);
  EXPECT_FALSE(runs->front().extraction.position.has_value());
  EXPECT_EQ(runs->front().measurement.plan, "sim_multicollect");

  f.step_heat = "5, 10";
  auto heat = build_runs(f, kIds);
  ASSERT_FALSE(heat);
  EXPECT_NE(heat.error().what.find("heats"), std::string::npos) << heat.error().what;
}

TEST(FactoryForm, ConditionalsGoToEveryRun) {
  FactoryForm f = unknown_form();
  f.position = "1-3";
  f.conditionals = {"default_unknown", "run_cdd"};
  auto runs = build_runs(f, kIds);
  ASSERT_TRUE(runs) << runs.error().what;
  ASSERT_EQ(runs->size(), 3u);
  const std::vector<ConditionalRef> want{{"default_unknown", "action"}, {"run_cdd", "action"}};
  for (const auto& r : *runs) EXPECT_EQ(r.conditionals, want);

  RunSpec run = runs->front();
  run.conditionals = {{"a", "truncate"}, {"b", "action"}};
  EXPECT_EQ(form_from_run(run).conditionals, (std::vector<std::string>{"a", "b"}));
  EXPECT_TRUE(form_from_run(RunSpec{}).conditionals.empty());
}

TEST(FactoryForm, BadInputIsAConfigError) {
  auto f = unknown_form();
  f.identifier = " ";
  EXPECT_FALSE(build_runs(f, kIds));
  f = unknown_form();
  f.identifier = "bad id!";
  EXPECT_FALSE(build_runs(f, kIds));
  f = unknown_form();
  f.position = "x";
  EXPECT_FALSE(build_runs(f, kIds));
  f = unknown_form();
  f.step = "ABC";
  EXPECT_FALSE(build_runs(f, kIds));
  f = unknown_form();
  f.step_heat = "5, hot";
  auto r = build_runs(f, kIds);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Config);
}

TEST(FactoryForm, NextFormAdvancesPastWhatWasAdded) {
  auto f = unknown_form();
  f.aliquot = 2;
  f.step = "A";
  auto next = next_form(f, {1, 1}, kIds);
  ASSERT_TRUE(next) << next.error().what;
  EXPECT_EQ(next->identifier, "20002");
  EXPECT_EQ(next->position, "5");
  EXPECT_FALSE(next->aliquot.has_value());
  EXPECT_TRUE(next->step.empty());
  EXPECT_EQ(next->plan, f.plan);

  // Nothing to advance: the form is unchanged.
  EXPECT_EQ(*next_form(f, {0, 0}, kIds), f);

  // One run per hole with an identifier step: past the last run added.
  f = unknown_form();
  f.position = "1-4";
  f.identifier_step = 1;
  next = next_form(f, {1, 2}, kIds);
  ASSERT_TRUE(next);
  EXPECT_EQ(next->identifier, "20005");
  EXPECT_EQ(next->position, "6-9");

  // Specials keep their identifier.
  f = unknown_form();
  f.identifier = "bu";
  next = next_form(f, {1, 1}, kIds);
  ASSERT_TRUE(next);
  EXPECT_EQ(next->identifier, "bu");
}

TEST(FactoryForm, TheExampleLabsDefaultsBlocksAndListings) {
  const std::filesystem::path dir = PYCHRON_EXAMPLE_CONFIGS_DIR;
  const auto lab = lab::load_lab({dir, dir / "extraction_line.toml", dir / "spectrometer.sim-integrated.toml"});
  ASSERT_TRUE(lab.problems.empty()) << lab.problems.front();
  EXPECT_EQ(lab.plans->names(), std::vector<std::string>{"sim_multicollect"});
  EXPECT_EQ(lab.scripts->names(scripting::ScriptKind::Extraction), std::vector<std::string>{"sim_extract"});
  EXPECT_EQ(lab.scripts->names(scripting::ScriptKind::PostMeasurement), std::vector<std::string>{"sim_pump"});
  EXPECT_TRUE(lab.scripts->names(scripting::ScriptKind::PostEquilibration).empty());
  ASSERT_TRUE(lab.blocks.contains("blank_pair"));
  EXPECT_EQ(lab.blocks.at("blank_pair").runs.size(), 2u);

  FactoryForm f;
  f.identifier = "20001";
  auto d = with_lab_defaults(f, lab.ids, lab.defaults);
  ASSERT_TRUE(d.has_value());
  EXPECT_EQ(d->plan, "sim_multicollect");
  EXPECT_EQ(d->script, "sim_extract");
  EXPECT_EQ(d->post_measurement, "sim_pump");
  EXPECT_EQ(d->value, 5);
  EXPECT_EQ(d->duration_s, 10);
  EXPECT_EQ(d->identifier, "20001");
  f.identifier = "ic";
  EXPECT_FALSE(with_lab_defaults(f, lab.ids, lab.defaults).has_value());
}

}  // namespace
}  // namespace pychron::experiment
