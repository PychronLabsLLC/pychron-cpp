#include "pychron/processing/units.hpp"

#include <gtest/gtest.h>

#include "fixtures.hpp"
#include "pychron/processing/time_series.hpp"

namespace pp = pychron::processing;
using pp::test::make_air;
using pp::test::make_unknown;

namespace {

std::unique_ptr<pp::MemorySource> source() {
  auto s = std::make_unique<pp::MemorySource>();
  for (int i = 0; i < 6; ++i) s->add(make_air(i, 295.0 + i));
  for (int i = 0; i < 3; ++i) s->add(make_unknown(10 + i));
  return s;
}

const pp::UnitRegistry& reg() { return pp::UnitRegistry::builtin(); }

pp::Pipeline air_pipeline() {
  pp::Pipeline p;
  auto& sel = p.add(reg(), "select", "select");
  EXPECT_TRUE(sel.options.set("analysis_types", std::vector<std::string>{"air"}));
  p.add(reg(), "reduce", "reduce", {"select"});
  p.add(reg(), "group", "group", {"reduce"});
  p.add(reg(), "edits", "edits", {"group"});
  p.add(reg(), "figure", "time_series", {"edits"});
  p.add(reg(), "stats", "group_stats", {"edits"});
  return p;
}

const pp::Dataset& dataset(const std::vector<pp::PortValue>& v) { return *std::get<pp::DatasetPtr>(v.at(0)); }

TEST(Units, BuiltinsRegistered) {
  for (const char* k : {"select", "reduce", "filter", "group", "edits", "group_stats", "time_series"})
    EXPECT_NE(reg().find(k), nullptr) << k;
}

TEST(Units, ValidateNamesTheProblem) {
  pp::Pipeline p;
  p.add(reg(), "a", "select");
  p.add(reg(), "b", "reduce", {"nope"});
  auto v = p.validate(reg());
  ASSERT_FALSE(v);
  EXPECT_NE(v.error().what.find("'b'"), std::string::npos);

  pp::Pipeline wrong_type;
  wrong_type.add(reg(), "s", "select");
  wrong_type.add(reg(), "fig", "time_series", {"s"});
  wrong_type.add(reg(), "r", "reduce", {"fig"});  // a scene into a dataset port
  EXPECT_FALSE(wrong_type.validate(reg()));

  pp::Pipeline cycle;
  cycle.add(reg(), "a", "reduce", {"b"});
  cycle.add(reg(), "b", "reduce", {"a"});
  EXPECT_FALSE(cycle.validate(reg()));

  pp::Pipeline arity;
  arity.add(reg(), "s", "select", {"x"});
  EXPECT_FALSE(arity.validate(reg()));
}

TEST(Units, SelectReduceGroup) {
  auto src = source();
  pp::Runner runner(reg(), src.get());
  auto p = air_pipeline();
  ASSERT_TRUE(p.find("group")->options.set("key", std::string("analysis_type")));
  auto out = runner.run(p, "group");
  ASSERT_TRUE(out) << out.error().what;
  const auto& d = dataset(*out);
  EXPECT_EQ(d.size(), 6u);
  EXPECT_TRUE(d.items().front().analysis->arar.has_value());
  EXPECT_LT(d.items().front().analysis->analysis->timestamp, d.items().back().analysis->analysis->timestamp);
  ASSERT_EQ(d.group_names.size(), 1u);
  EXPECT_EQ(d.group_names[0], "air");
}

TEST(Units, ChangingAnOptionRecomputesOnlyDownstream) {
  auto src = source();
  pp::Runner runner(reg(), src.get());
  auto p = air_pipeline();
  ASSERT_TRUE(runner.run(p, "figure"));
  EXPECT_EQ(runner.executions(), 5u);

  // Same pipeline: everything cached.
  ASSERT_TRUE(runner.run(p, "figure"));
  EXPECT_EQ(runner.executions(), 5u);

  // Exclude a point: edits and figure rerun, select/reduce/group do not.
  ASSERT_TRUE(p.find("edits")->options.set("exclude", std::vector<std::string>{"uuid-A1-0"}));
  ASSERT_TRUE(runner.run(p, "figure"));
  EXPECT_EQ(runner.executions(), 7u);
  for (const auto& n : runner.last_run()) {
    const bool should = n.id == "edits" || n.id == "figure";
    EXPECT_EQ(n.executed, should) << n.id;
  }

  // A figure option: only the figure.
  auto row = p.find("figure")->options.new_row("panels");
  ASSERT_TRUE(row.set("quantity", std::string("Ar40/Ar36")));
  ASSERT_TRUE(p.find("figure")->options.set_rows("panels", {row}));
  ASSERT_TRUE(runner.run(p, "figure"));
  EXPECT_EQ(runner.executions(), 8u);

  // New data in the source invalidates select and everything after it.
  src->add(make_air(20));
  auto out = runner.run(p, "figure");
  ASSERT_TRUE(out);
  EXPECT_EQ(runner.executions(), 13u);
}

TEST(Units, StatsHonourExclusions) {
  auto src = source();
  pp::Runner runner(reg(), src.get());
  auto p = air_pipeline();
  ASSERT_TRUE(p.find("stats")->options.set("quantity", std::string("Ar40/Ar36")));
  auto all = runner.run(p, "stats");
  ASSERT_TRUE(all);
  const auto& rows = std::get<pp::GroupResultsPtr>(all->at(0))->rows;
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(rows[0].n_included, 6u);
  ASSERT_TRUE(rows[0].mean);
  const double with_all = rows[0].mean->value;

  ASSERT_TRUE(p.find("edits")->options.set("exclude", std::vector<std::string>{"uuid-A1-5"}));
  auto fewer = runner.run(p, "stats");
  ASSERT_TRUE(fewer);
  const auto& r2 = std::get<pp::GroupResultsPtr>(fewer->at(0))->rows[0];
  EXPECT_EQ(r2.n_included, 5u);
  EXPECT_EQ(r2.n_total, 6u);
  EXPECT_LT(r2.mean->value, with_all);  // the highest ratio left out
}

TEST(Units, FilterOmitsOrRemoves) {
  auto src = source();
  pp::Runner runner(reg(), src.get());
  pp::Pipeline p;
  p.add(reg(), "select", "select");
  p.add(reg(), "reduce", "reduce", {"select"});
  auto& f = p.add(reg(), "filter", "filter", {"reduce"});
  auto rule = f.options.new_row("rules");
  ASSERT_TRUE(rule.set("quantity", std::string("Ar40/Ar36")));
  ASSERT_TRUE(rule.set("comparator", std::string("<")));
  ASSERT_TRUE(rule.set("value", 298.0));
  ASSERT_TRUE(f.options.set_rows("rules", {rule}));
  auto out = runner.run(p, "filter");
  ASSERT_TRUE(out);
  std::size_t omitted = 0;
  for (const auto& it : dataset(*out).items()) omitted += it.exclusion.filter;
  EXPECT_EQ(dataset(*out).size(), 9u);
  // airs at 298, 299, 300 fail; unknowns have Ar40/Ar36 = 2000 and fail too.
  EXPECT_EQ(omitted, 6u);

  ASSERT_TRUE(p.find("filter")->options.set("mode", std::string("removed")));
  auto removed = runner.run(p, "filter");
  ASSERT_TRUE(removed);
  EXPECT_EQ(dataset(*removed).size(), 3u);
}

TEST(Units, ErrorsStopDependants) {
  pp::Runner runner(reg(), nullptr);  // select needs a source
  auto p = air_pipeline();
  auto out = runner.run(p, "figure");
  ASSERT_FALSE(out);
  EXPECT_EQ(out.error().device, "select");
  for (const auto& n : runner.last_run()) EXPECT_TRUE(n.error) << n.id;
}

TEST(Units, CancelStops) {
  auto src = source();
  pp::Runner runner(reg(), src.get());
  std::atomic<bool> cancel{true};
  auto out = runner.run(air_pipeline(), "figure", &cancel);
  ASSERT_FALSE(out);
  EXPECT_EQ(out.error().kind, pychron::ErrorKind::Cancelled);
}

TEST(Units, PipelineTomlRoundTrip) {
  auto p = air_pipeline();
  p.name = "Air time series";
  ASSERT_TRUE(p.find("group")->options.set("key", std::string("mass_spectrometer")));
  auto row = p.find("figure")->options.new_row("panels");
  ASSERT_TRUE(row.set("quantity", std::string("Ar40/Ar36")));
  ASSERT_TRUE(row.set("fit", std::string("weighted_mean")));
  ASSERT_TRUE(p.find("figure")->options.set_rows("panels", {row}));
  p.find("figure")->preset = "Air monitor";
  const std::string text = pp::pipeline_to_toml(p);
  std::vector<std::string> warnings;
  auto back = pp::pipeline_from_toml(reg(), text, &warnings);
  ASSERT_TRUE(back) << back.error().what << "\n" << text;
  EXPECT_TRUE(warnings.empty());
  EXPECT_EQ(back->name, "Air time series");
  ASSERT_EQ(back->nodes.size(), p.nodes.size());
  for (std::size_t i = 0; i < p.nodes.size(); ++i) {
    EXPECT_EQ(back->nodes[i].id, p.nodes[i].id);
    EXPECT_EQ(back->nodes[i].inputs, p.nodes[i].inputs);
    EXPECT_EQ(back->nodes[i].options, p.nodes[i].options) << p.nodes[i].id;
  }
  EXPECT_EQ(back->find("figure")->preset, "Air monitor");

  EXPECT_FALSE(pp::pipeline_from_toml(reg(), "schema = \"pipeline\"\n[[units]]\nid = \"a\"\nkind = \"warp\"\n"));
}

}  // namespace
