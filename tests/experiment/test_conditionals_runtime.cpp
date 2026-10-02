// Between-run conditionals (conditionals spec 6.2): queue actions, the
// conditional library and its levels, pre/post-run checks, record provenance.

#include <gtest/gtest.h>

#include "pychron/experiment/conditionals/library.hpp"
#include "pychron/experiment/conditionals/metrics.hpp"
#include "pychron/experiment/conditionals/queue_actions.hpp"
#include "pychron/experiment/conditionals/validate.hpp"
#include "pychron/experiment/measurement/results.hpp"

using namespace pychron;
using namespace pychron::experiment;

namespace {

RunSpec run(std::string identifier, AnalysisType type = AnalysisType::Unknown, double value = 1.0,
            std::optional<int> aliquot = std::nullopt) {
  RunSpec r;
  r.id.identifier = std::move(identifier);
  r.id.type = type;
  r.id.aliquot = aliquot;
  r.extraction.value = value;
  r.measurement.plan = "multicollect";
  return r;
}

// 0: 1000 (current)  1-3: 1000 steps  4: blank  5: 2000  6: 1000
ExperimentQueue queue() {
  QueueSpec q;
  q.runs = {run("1000"), run("1000", AnalysisType::Unknown, 2), run("1000", AnalysisType::Unknown, 3),
            run("1000", AnalysisType::Unknown, 4), run("bu", AnalysisType::BlankUnknown), run("2000"), run("1000")};
  return ExperimentQueue(q);
}

ActionSpec act(const std::string& text) { return *parse_action(text); }

std::vector<bool> skips(const ExperimentQueue& q) {
  std::vector<bool> out;
  for (const auto& r : q.runs()) out.push_back(r.skip);
  return out;
}

TEST(QueueActions, SkipNextAndSkipN) {
  auto q = queue();
  auto c = apply_queue_action(q, 0, act("skip_next"));
  ASSERT_TRUE(c) << c.error().what;
  EXPECT_EQ(c->skipped, std::vector<std::size_t>{1});
  c = apply_queue_action(q, 0, act("skip_n 2"));  // already-skipped rows are passed over
  ASSERT_TRUE(c);
  EXPECT_EQ(c->skipped, (std::vector<std::size_t>{2, 3}));
  EXPECT_EQ(skips(q), (std::vector<bool>{false, true, true, true, false, false, false}));
  c = apply_queue_action(q, 5, act("skip_n 5"));  // runs out of rows
  ASSERT_TRUE(c);
  EXPECT_EQ(c->skipped, std::vector<std::size_t>{6});
}

TEST(QueueActions, SkipAliquotAndToLast) {
  auto q = queue();
  auto c = apply_queue_action(q, 0, act("skip_aliquot"));
  ASSERT_TRUE(c);
  // Consecutive unknowns of the aliquot only: the blank ends the run of 1000s.
  EXPECT_EQ(c->skipped, (std::vector<std::size_t>{1, 2, 3}));
  auto q2 = queue();
  c = apply_queue_action(q2, 0, act("skip_to_last_in_aliquot"));
  ASSERT_TRUE(c);
  EXPECT_EQ(c->skipped, (std::vector<std::size_t>{1, 2}));
  EXPECT_FALSE(q2.runs()[3].skip);
  // A different aliquot is not part of it.
  QueueSpec spec;
  spec.runs = {run("1000", AnalysisType::Unknown, 1, 1), run("1000", AnalysisType::Unknown, 1, 2)};
  ExperimentQueue q3(spec);
  EXPECT_FALSE(apply_queue_action(q3, 0, act("skip_aliquot"))->changed());
}

TEST(QueueActions, SetExtractAbsoluteAndPercent) {
  auto q = queue();
  auto c = apply_queue_action(q, 0, act("set_extract 0.5,1"));
  ASSERT_TRUE(c);
  EXPECT_EQ(c->modified, (std::vector<std::size_t>{1, 2}));  // steps run out
  EXPECT_DOUBLE_EQ(q.runs()[1].extraction.value, 2.5);
  EXPECT_DOUBLE_EQ(q.runs()[2].extraction.value, 4);
  EXPECT_DOUBLE_EQ(q.runs()[3].extraction.value, 4);
  auto q2 = queue();
  ASSERT_TRUE(apply_queue_action(q2, 0, act("set_extract 10%,50%,100%")));
  EXPECT_DOUBLE_EQ(q2.runs()[1].extraction.value, 2.2);
  EXPECT_DOUBLE_EQ(q2.runs()[2].extraction.value, 4.5);
  EXPECT_DOUBLE_EQ(q2.runs()[3].extraction.value, 8);
}

TEST(QueueActions, RepeatAndRunBlank) {
  auto q = queue();
  RunSpec cur = q.runs()[0];
  cur.id.aliquot = 7;
  ASSERT_TRUE(q.replace(0, cur));
  auto c = apply_queue_action(q, 0, act("repeat"));
  ASSERT_TRUE(c);
  EXPECT_EQ(c->inserted, 1u);
  EXPECT_EQ(q.size(), 8u);
  EXPECT_EQ(q.runs()[1].id.identifier, "1000");
  EXPECT_FALSE(q.runs()[1].id.aliquot);  // gets a new aliquot

  c = apply_queue_action(q, 0, act("run_blank"));
  ASSERT_TRUE(c);
  EXPECT_EQ(q.runs()[1].id.type, AnalysisType::BlankUnknown);
  EXPECT_EQ(q.runs()[1].id.identifier, "bu");
  EXPECT_EQ(q.runs()[1].measurement.plan, "multicollect");

  QueueSpec spec;
  spec.runs = {run("a", AnalysisType::Air)};
  ExperimentQueue air(spec);
  ASSERT_TRUE(apply_queue_action(air, 0, act("run_blank")));
  EXPECT_EQ(air.runs()[1].id.type, AnalysisType::BlankAir);
  EXPECT_EQ(air.runs()[1].id.identifier, "ba");
  EXPECT_EQ(blank_type_for(AnalysisType::Cocktail), AnalysisType::BlankCocktail);
  EXPECT_EQ(blank_type_for(AnalysisType::BlankExtractionLine), AnalysisType::BlankExtractionLine);
}

TEST(QueueActions, Errors) {
  auto q = queue();
  EXPECT_FALSE(apply_queue_action(q, 0, act("truncate")));
  EXPECT_FALSE(apply_queue_action(q, 99, act("skip_next")));
  EXPECT_FALSE(apply_queue_action(q, 0, act("run_blank"), BlankFactory{}));
}

MapConditionalSource lab() {
  MapConditionalSource src;
  src.add("system", R"(
[[cancelations]]
name = "gauge"
check = "gauge.spec.pressure > 1e-6"
[[truncations]]
name = "big"
check = "Ar40 > 1e7"
[[pre_run]]
name = "cdd"
check = "CDD.inactive"
)");
  src.add("q1", R"(
disable = ["gauge"]
[[terminations]]
name = "q-term"
check = "Ar36 < 0"
[[post_run]]
name = "low"
check = "Ar40 < 100"
action = "run_blank"
analysis_types = ["unknown"]
)");
  src.add("default_unknown", R"(
[[truncations]]
name = "big"
check = "Ar40 > 5e6"
)");
  src.add("strict", R"(
[[terminations]]
name = "r-term"
check = "slope(Ar40) < -10"
)");
  return src;
}

plan::MeasurementPlan plan_with(std::vector<std::string> include) {
  plan::MeasurementPlan p;
  p.conditionals.include = std::move(include);
  p.conditionals.truncations = {{"Ar40 > 9e6", 20}};
  return p;
}

TEST(ConditionalLibrary, MergesLevelsAndStampsProvenance) {
  const auto src = lab();
  ConditionalLibrary lib(src);
  QueueSpec qs;
  qs.queue_conditionals = "q1";
  RunSpec r = run("1000");
  r.conditionals = {{"strict", "termination"}};
  auto set = lib.for_run(qs, r, plan_with({"@conditionals.default_unknown"}));
  ASSERT_TRUE(set) << set.error().what;
  std::map<std::string, const Conditional*> by;
  for (const auto& c : set->items) by[c.name] = &c;
  EXPECT_FALSE(by.contains("gauge"));  // disabled by the queue
  ASSERT_TRUE(by.contains("big"));
  EXPECT_EQ(by["big"]->level, ConditionalLevel::Plan);  // plan include replaces system's "big"
  EXPECT_EQ(by["big"]->check, "Ar40 > 5e6");
  EXPECT_EQ(by["big"]->location, "default_unknown.toml");
  EXPECT_EQ(by["q-term"]->level, ConditionalLevel::Queue);
  EXPECT_EQ(by["r-term"]->level, ConditionalLevel::Run);
  EXPECT_EQ(by["plan.truncation[0]"]->level, ConditionalLevel::Plan);
  EXPECT_EQ(by["plan.truncation[0]"]->location, "multicollect");
  EXPECT_EQ(by["cdd"]->level, ConditionalLevel::System);
}

TEST(ConditionalLibrary, MissingAndOptionalSources) {
  MapConditionalSource empty;
  ConditionalLibrary lib(empty);
  auto sys = lib.system();
  ASSERT_TRUE(sys);
  EXPECT_TRUE(sys->items.empty());  // no system.toml is fine
  QueueSpec qs;
  EXPECT_TRUE(lib.queue(qs)->items.empty());
  qs.queue_conditionals = "nope";
  auto q = lib.queue(qs);
  ASSERT_FALSE(q);
  EXPECT_NE(q.error().what.find("not found"), std::string::npos);
  DirectoryConditionalSource dir("/nonexistent");
  EXPECT_FALSE(dir.text("../etc/passwd"));
  EXPECT_FALSE(dir.text("a/b"));
  EXPECT_FALSE(*dir.text("system"));
}

TEST(RunChecks, PreRunFiltersByTypeAndCountsAcrossRuns) {
  const auto src = lab();
  ConditionalLibrary lib(src);
  QueueSpec qs;
  qs.queue_conditionals = "q1";
  auto set = lib.for_queue(qs);
  ASSERT_TRUE(set);
  RunChecks checks(*set);
  MapContext ok, bad;
  ok.series_data["CDD.inactive"] = {0};
  bad.series_data["CDD.inactive"] = {1};
  EXPECT_FALSE(checks.pre_run(run("1000"), ok));
  auto trip = checks.pre_run(run("1000"), bad);
  ASSERT_TRUE(trip);
  EXPECT_EQ(trip->name, "cdd");
  EXPECT_EQ(trip->action.type, ActionSpec::Type::Cancel);
}

TEST(RunChecks, PostRunAppliesQueueActionsAndCancel) {
  const auto src = lab();
  ConditionalLibrary lib(src);
  QueueSpec qs;
  qs.queue_conditionals = "q1";
  RunChecks checks(*lib.for_queue(qs));
  auto q = queue();

  record::AnalysisRecord low;
  record::DataSeries s;
  s.iso = "Ar40";
  s.det = "H1";
  s.kind = "signal";
  s.trace.t = {1, 2};
  s.trace.v = {50, 50};
  low.data.series.push_back(s);
  low.results.intercepts["Ar40"] = {reduction::Intercept{50, 1, 2, {}, 0}, {}};
  RecordMetrics metrics(low);

  auto out = checks.post_run(q.runs()[0], metrics, q, 0);
  ASSERT_TRUE(out) << out.error().what;
  ASSERT_TRUE(*out);
  EXPECT_EQ((*out)->trip.name, "low");
  EXPECT_FALSE((*out)->cancel_queue);
  ASSERT_TRUE((*out)->change);
  EXPECT_EQ((*out)->change->inserted, 1u);
  EXPECT_EQ(q.runs()[1].id.type, AnalysisType::BlankUnknown);

  // analysis_types = ["unknown"]: an air run is not checked.
  auto air = run("a", AnalysisType::Air);
  auto none = checks.post_run(air, metrics, q, 0);
  ASSERT_TRUE(none);
  EXPECT_FALSE(*none);

  MapConditionalSource cancel;
  cancel.add("system", "[[post_run]]\ncheck = \"Ar40 < 100\"\n");
  ConditionalLibrary lib2(cancel);
  RunChecks c2(*lib2.for_queue(QueueSpec{}));
  auto r = c2.post_run(q.runs()[0], metrics, q, 0);
  ASSERT_TRUE(r && *r);
  EXPECT_TRUE((*r)->cancel_queue);
  EXPECT_FALSE((*r)->change);
}

TEST(RecordProvenance, ConditionalsToRecord) {
  auto set = parse_conditionals(R"(
[[truncations]]
name = "big"
check = "Ar40 > 900"
window = 3
abbreviated_count_ratio = 0.5
analysis_types = ["unknown"]
)",
                                "lab/system.toml");
  ASSERT_TRUE(set);
  set->stamp(ConditionalLevel::System, "lab/system.toml");
  Trip t;
  t.name = "big";
  t.kind = ConditionalKind::Truncation;
  t.id = set->items[0].id();
  t.check = set->items[0].effective_check();
  t.action = set->items[0].action;
  t.reading = 12;
  t.count = 1;
  t.ts = 30.5;
  t.value = 950;
  t.context = {{"Ar40", 950}};
  auto rec = measurement::to_record_conditionals(set->items, {t}, {{"x", "unavailable", 3}});
  ASSERT_EQ(rec.installed.size(), 1u);
  const auto& i = rec.installed[0];
  EXPECT_EQ(i.name, "big");
  EXPECT_EQ(i.kind, "truncation");
  EXPECT_EQ(i.level, "system");
  EXPECT_EQ(i.location, "lab/system.toml");
  EXPECT_EQ(i.check, "average(Ar40, window=3) > 900");
  EXPECT_EQ(i.window, 3);
  EXPECT_EQ(i.action, "truncate");
  EXPECT_DOUBLE_EQ(i.abbreviated_count_ratio, 0.5);
  EXPECT_EQ(i.id, t.id);
  ASSERT_EQ(rec.tripped.size(), 1u);
  EXPECT_EQ(rec.tripped[0].reading, 12);
  EXPECT_EQ(rec.tripped[0].context.at("Ar40"), 950);
  ASSERT_EQ(rec.errors.size(), 1u);
  EXPECT_EQ(rec.errors[0].count, 3);
}

}  // namespace

namespace {

TEST(ConditionalValidation, ChecksNamesAgainstTheLab) {
  auto set = parse_conditionals(R"(
[[truncations]]
name = "ok"
check = "Ar40 > 1 and H1.deflection < 10 and gauge.IG1.pressure < 1e-8 and Ar40/Ar36 > 1"
[[terminations]]
name = "bad-names"
check = "Ar99 > 1 or IC9.inactive or gauge.nope.pressure > 1 or device.chiller > 1 or Ar40/Ar98 > 1"
[[actions]]
name = "computed"
check = "age > 1 or kcl > 2"
action = "notify"
[[pre_run]]
name = "pre"
check = "Ar40 < 1 or CDD.inactive"
[[post_run]]
name = "var"
check = "Ar40 < $MIN"
)");
  ASSERT_TRUE(set) << set.error().what;
  MetricCatalog cat;
  cat.isotopes = {"Ar36", "Ar37", "Ar38", "Ar39", "Ar40"};
  cat.detectors = {"H1", "CDD"};
  cat.gauges = {"IG1"};
  cat.devices = {"pump"};
  cat.variables = {"MAX"};
  auto d = validate_conditionals(*set, cat);
  auto messages = [&](const std::string& name) {
    std::vector<std::string> out;
    for (const auto& x : d)
      if (x.conditional == name) out.push_back(x.message);
    return out;
  };
  EXPECT_TRUE(messages("ok").empty()) << testing::PrintToString(messages("ok"));
  EXPECT_EQ(messages("bad-names").size(), 5u) << testing::PrintToString(messages("bad-names"));
  const auto computed = messages("computed");
  ASSERT_EQ(computed.size(), 2u);
  EXPECT_NE(computed[0].find("Ar-Ar constants"), std::string::npos);
  EXPECT_NE(computed[1].find("chlorine"), std::string::npos);
  ASSERT_EQ(messages("pre").size(), 1u);
  EXPECT_NE(messages("pre")[0].find("pre-run"), std::string::npos);
  ASSERT_EQ(messages("var").size(), 1u);
  for (const auto& x : d) {
    if (x.conditional == "var") {
      EXPECT_FALSE(x.error);  // a warning
    }
  }

  cat.computed = true;
  d = validate_conditionals(*set, cat);
  EXPECT_EQ(messages("computed").size(), 1u);  // kcl still unavailable

  // An empty catalog checks no names: only kcl and the pre-run isotope remain.
  MetricCatalog unchecked;
  unchecked.computed = true;
  d = validate_conditionals(*set, unchecked);
  EXPECT_EQ(d.size(), 2u) << testing::PrintToString(d.size());
}

TEST(ConditionalsValidate, ChlorineCatalogAllowsKcl) {
  auto set = parse_conditionals(R"(
[[actions]]
name = "computed"
check = "kcl > 2 or clk < 1 or cl36 > 0"
action = "notify"
)");
  ASSERT_TRUE(set) << set.error().what;
  MetricCatalog cat;
  cat.computed = true;
  auto d = validate_conditionals(*set, cat);
  ASSERT_EQ(d.size(), 3u);  // default: no chlorine
  for (const auto& x : d) EXPECT_NE(x.message.find("chlorine"), std::string::npos) << x.message;

  cat.chlorine = true;
  d = validate_conditionals(*set, cat);
  EXPECT_TRUE(d.empty()) << testing::PrintToString(d.size());

  // Chlorine alone still needs the Ar-Ar constants.
  cat.computed = false;
  d = validate_conditionals(*set, cat);
  ASSERT_EQ(d.size(), 3u);
  for (const auto& x : d) EXPECT_NE(x.message.find("Ar-Ar constants"), std::string::npos) << x.message;
}

}  // namespace
