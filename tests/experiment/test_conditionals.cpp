#include <gtest/gtest.h>

#include "pychron/experiment/conditionals/conditional.hpp"

using namespace pychron::experiment;

namespace {

std::string canon(const std::string& s) {
  auto e = parse_expression(s);
  EXPECT_TRUE(e) << s << ": " << (e ? "" : e.error().what);
  return e ? to_string(**e) : "";
}

MapContext make_ctx() {
  MapContext c;
  c.series_data["Ar40"] = {1e5, 2e5, 4e5, 9e5};
  c.series_data["Ar36"] = {1, -1, -2, -3};
  c.series_data["Ar40/Ar39"] = {10};
  c.series_data["Ar40.bs"] = {1, 2, 3};
  c.series_data["gauge.ion_pump.pressure"] = {2e-6};
  c.series_data["H1.deflection"] = {5};
  c.elapsed_s = 42;
  return c;
}

Conditional item(ConditionalKind kind, const std::string& name, const std::string& check) {
  Conditional c;
  c.kind = kind;
  c.name = name;
  c.check = check;
  c.expr = std::shared_ptr<const Expr>(std::move(*parse_expression(check)));
  return c;
}

}  // namespace

TEST(ConditionalsParser, RoundTripsPychronExamples) {
  for (const char* s : {"Ar40 > 8e5", "Ar40>8e5", "Ar40/Ar39 < 10", "average(Ar36, window=10) < 0", "slope(Ar40) < -1",
                        "between(Ar40, 1e5, 2e5)", "not (Ar40 > 1 and Ar39 < 2) or age > $limit", "H1.deflection > 10",
                        "device.chiller_temp >= 5.5", "gauge.ion_pump.pressure > 1e-6", "param.foo != 3", "kca < 1 and radiogenic_yield > 50",
                        "Ar40.bs_corrected > 0", "rsd(Ar40, window=5) < 2", "elapsed() > 30", "count(Ar40) >= 10",
                        "Ar40 < 1 or Ar39 < 2 and Ar38 < 3", "(Ar40 < 1 or Ar39 < 2) and Ar38 < 3", "not not Ar40 > 1"}) {
    const std::string once = canon(s);
    ASSERT_FALSE(once.empty()) << s;
    EXPECT_EQ(canon(once), once) << s;
  }
  EXPECT_EQ(canon("Ar40>8e5"), "Ar40 > 800000");
}

TEST(ConditionalsParser, PrecedenceAndOrNot) {
  auto e = parse_expression("a1 < 1 or a2 < 2 and not a3 < 3");
  ASSERT_TRUE(e);
  EXPECT_EQ((*e)->kind, Expr::Kind::Or);
  EXPECT_EQ((*e)->children[1]->kind, Expr::Kind::And);
  EXPECT_EQ((*e)->children[1]->children[1]->kind, Expr::Kind::Not);
}

TEST(ConditionalsParser, RejectsBadInput) {
  for (const char* s : {"", "Ar40 >", "eval(1)", "Ar40 > 1 )", "slope(1) < 2", "average(Ar40, window=0) < 1",
                        "between(1, 2) < 1", "elapsed(1)", "Ar40.nope > 1", "gauge.x > 1", "min(Ar40, window=x)",
                        "Ar40 @ 1", "__import__('os')", "between(1,2,3, window=4)"})
    EXPECT_FALSE(parse_expression(s)) << s;
}

TEST(ConditionalsParser, SeriesVersusScalarWithoutReducerIsError) {
  auto bad = parse_expression("Ar40.bs > 5");
  ASSERT_FALSE(bad);
  EXPECT_NE(bad.error().what.find("without a reducer"), std::string::npos);
  EXPECT_FALSE(parse_expression("Ar40.bs > average(Ar39.bs)"));
  EXPECT_TRUE(parse_expression("average(Ar40.bs) > 5"));
  EXPECT_TRUE(parse_expression("slope(Ar40.bs, window=3) < (average(Ar40.bs))"));
}

TEST(ConditionalsEval, ScalarsAndSeriesFunctions) {
  auto ctx = make_ctx();
  Variables vars;
  auto ev = [&](const char* s) {
    auto e = parse_expression(s);
    EXPECT_TRUE(e) << s;
    auto r = evaluate(**e, ctx, vars);
    EXPECT_TRUE(r) << s << (r ? "" : r.error().what);
    return r ? *r : -999.0;
  };
  EXPECT_EQ(ev("Ar40 > 8e5"), 1);
  EXPECT_EQ(ev("Ar40 < 8e5"), 0);
  EXPECT_EQ(ev("Ar40/Ar39 == 10"), 1);
  EXPECT_EQ(ev("max(Ar40)"), 9e5);
  EXPECT_EQ(ev("min(Ar40, window=2)"), 4e5);
  EXPECT_DOUBLE_EQ(ev("average(Ar36, window=2)"), -2.5);
  EXPECT_DOUBLE_EQ(ev("slope(Ar36)"), -1.3);
  EXPECT_DOUBLE_EQ(ev("average(Ar40.bs)"), 2);
  EXPECT_EQ(ev("count(Ar40, window=3)"), 3);
  EXPECT_EQ(ev("elapsed() >= 42"), 1);
  EXPECT_EQ(ev("between(Ar40, 8e5, 1e6)"), 1);
  EXPECT_EQ(ev("gauge.ion_pump.pressure > 1e-6"), 1);
  EXPECT_EQ(ev("H1.deflection != 5"), 0);
  EXPECT_EQ(ev("not (Ar40 > 1e9) and Ar36 < 0"), 1);
  EXPECT_NEAR(ev("std(Ar36)"), 1.47902, 1e-4);  // population std of {1,-1,-2,-3}
  EXPECT_GT(ev("rsd(Ar40)"), 0);
}

TEST(ConditionalsEval, VariablesResolveScriptOptionsThenParams) {
  auto ctx = make_ctx();
  Variables vars;
  vars.params["limit"] = 1e6;
  vars.params["thr"] = 1;
  vars.script_options["limit"] = 5e5;
  auto e = parse_expression("Ar40 > $limit");
  auto r = evaluate(**e, ctx, vars);
  ASSERT_TRUE(r);
  EXPECT_EQ(*r, 1);  // script option 5e5 wins over param 1e6
  EXPECT_EQ(*vars.lookup("thr"), 1);
  auto missing = parse_expression("Ar40 > $nope");
  EXPECT_FALSE(evaluate(**missing, ctx, vars));
}

TEST(ConditionalsEval, MissingMetricIsErrorNotTrip) {
  MapContext empty;
  auto e = parse_expression("Ar40 > 1");
  EXPECT_FALSE(evaluate(**e, empty, {}));
  ConditionalEngine eng(ConditionalSet{{item(ConditionalKind::Truncation, "t", "Ar40 > 1")}, {}});
  EXPECT_TRUE(eng.evaluate(empty, {}, 1, 0).empty());
  EXPECT_EQ(eng.errors().size(), 1u);
}

TEST(ConditionalsActions, ParseEnumActions) {
  using T = ActionSpec::Type;
  EXPECT_EQ(parse_action("truncate")->type, T::Truncate);
  EXPECT_TRUE(parse_action("truncate:quick")->quick);
  EXPECT_EQ(parse_action("terminate")->type, T::Terminate);
  EXPECT_EQ(parse_action("cancel")->type, T::Cancel);
  auto sp = parse_action("set_param NAME=1.5");
  ASSERT_TRUE(sp);
  EXPECT_EQ(sp->name, "NAME");
  EXPECT_EQ(sp->value, 1.5);
  EXPECT_EQ(parse_action("run_hook warm")->name, "warm");
  EXPECT_EQ(parse_action("notify")->type, T::Notify);
  for (const char* k : {"skip_n", "skip_aliquot", "repeat", "run_blank"}) EXPECT_TRUE(parse_action(k)) << k;
  EXPECT_EQ(parse_action("set_extract=2.5")->value, 2.5);
  for (const char* bad : {"", "os.system('x')", "truncate:slow", "set_param x", "run_hook", "cancel now", "set_extract=abc"})
    EXPECT_FALSE(parse_action(bad)) << bad;
  EXPECT_EQ(to_string(*parse_action("truncate:quick")), "truncate:quick");
}

TEST(ConditionalsEngine, StartFrequencyNtrips) {
  auto ctx = make_ctx();
  Conditional c = item(ConditionalKind::Truncation, "t", "Ar40 > 8e5");
  c.start = 3;
  c.frequency = 2;
  c.ntrips = 2;
  ConditionalEngine eng(ConditionalSet{{c}, {}});
  EXPECT_TRUE(eng.evaluate(ctx, {}, 1, 1).empty());  // before start
  EXPECT_TRUE(eng.evaluate(ctx, {}, 2, 2).empty());
  EXPECT_TRUE(eng.evaluate(ctx, {}, 3, 3).empty());  // true, count 1 of 2
  EXPECT_TRUE(eng.evaluate(ctx, {}, 4, 4).empty());  // skipped by frequency
  auto trips = eng.evaluate(ctx, {}, 5, 5);          // count 2 -> trip
  ASSERT_EQ(trips.size(), 1u);
  EXPECT_EQ(trips[0].name, "t");
  EXPECT_EQ(trips[0].kind, ConditionalKind::Truncation);
  EXPECT_EQ(trips[0].count, 2);
  EXPECT_EQ(trips[0].value, 9e5);
  EXPECT_EQ(trips[0].ts, 5);
  EXPECT_TRUE(eng.evaluate(ctx, {}, 7, 7).empty());  // fires once
  EXPECT_EQ(eng.trips().size(), 1u);
  eng.reset();
  EXPECT_TRUE(eng.trips().empty());
}

TEST(ConditionalsEngine, NtripsRequiresConsecutive) {
  MapContext ctx;
  Conditional c = item(ConditionalKind::Termination, "t", "Ar40 > 10");
  c.ntrips = 2;
  ConditionalEngine eng(ConditionalSet{{c}, {}});
  ctx.series_data["Ar40"] = {20};
  EXPECT_TRUE(eng.evaluate(ctx, {}, 1, 1).empty());
  ctx.series_data["Ar40"] = {0};
  EXPECT_TRUE(eng.evaluate(ctx, {}, 2, 2).empty());  // resets
  ctx.series_data["Ar40"] = {20};
  EXPECT_TRUE(eng.evaluate(ctx, {}, 3, 3).empty());
  EXPECT_EQ(eng.evaluate(ctx, {}, 4, 4).size(), 1u);
}

TEST(ConditionalsEngine, EvaluationOrder) {
  auto ctx = make_ctx();
  ConditionalSet s;
  for (auto k : {ConditionalKind::Equilibration, ConditionalKind::Cancelation, ConditionalKind::Termination,
                 ConditionalKind::Action, ConditionalKind::Truncation, ConditionalKind::Modification})
    s.items.push_back(item(k, std::string(to_string(k)), "Ar40 > 1"));
  s.items.push_back(item(ConditionalKind::PreRun, "pre", "Ar40 > 1"));
  ConditionalEngine eng(std::move(s));
  auto trips = eng.evaluate(ctx, {}, 1, 0);
  ASSERT_EQ(trips.size(), 6u);  // pre_run excluded
  std::vector<std::string> order;
  for (auto& t : trips) order.push_back(t.name);
  EXPECT_EQ(order, (std::vector<std::string>{"modification", "truncation", "action", "termination", "cancelation",
                                             "equilibration"}));
  EXPECT_EQ(eng.evaluate_kind(ConditionalKind::PreRun, ctx, {}, 1, 0).size(), 1u);
}

TEST(ConditionalsMerge, LaterLevelsAddAndRunDisables) {
  ConditionalSet sys{{item(ConditionalKind::Cancelation, "pump", "Ar40 > 1"), item(ConditionalKind::Truncation, "t", "Ar40 > 2")}, {}};
  ConditionalSet queue{{item(ConditionalKind::Termination, "q", "Ar40 > 3")}, {}};
  ConditionalSet plan{{item(ConditionalKind::Truncation, "t", "Ar40 > 20")}, {}};  // replaces by name
  ConditionalSet run{{item(ConditionalKind::Termination, "r", "Ar40 > 4")}, {"pump"}};
  auto m = merge_levels({sys, queue, plan, run});
  ASSERT_EQ(m.items.size(), 3u);
  EXPECT_EQ(m.items[0].name, "t");
  EXPECT_EQ(m.items[0].check, "Ar40 > 20");
  EXPECT_EQ(m.items[1].name, "q");
  EXPECT_EQ(m.items[2].name, "r");
}

TEST(ConditionalsToml, LoadsAllKinds) {
  const char* text = R"(
disable = ["old"]
[[truncations]]
check = "Ar40 > 8e5"
start = 20
frequency = 5
[[terminations]]
check = "average(Ar36, window=10) < 0"
start = 30
ntrips = 3
[[cancelations]]
check = "gauge.ion_pump.pressure > 1e-6"
[[actions]]
check = "slope(Ar40) > 100"
start = 10
action = "truncate:quick"
resume = false
[[modifications]]
check = "Ar40 < 1e3"
action = "skip_aliquot"
[[equilibrations]]
check = "slope(Ar40) < 50"
start = 5
[[pre_run]]
check = "gauge.spec.pressure < 5e-9"
[[post_run]]
check = "Ar40 > 1e6"
action = "run_blank"
name = "blank-after"
)";
  auto set = parse_conditionals(text);
  ASSERT_TRUE(set) << set.error().what;
  EXPECT_EQ(set->items.size(), 8u);
  EXPECT_EQ(set->disable, std::vector<std::string>{"old"});
  const Conditional* tr = nullptr;
  const Conditional* pr = nullptr;
  for (auto& c : set->items) {
    if (c.kind == ConditionalKind::Truncation) tr = &c;
    if (c.name == "blank-after") pr = &c;
  }
  ASSERT_TRUE(tr && pr);
  EXPECT_EQ(tr->start, 20);
  EXPECT_EQ(tr->frequency, 5);
  EXPECT_EQ(tr->action.type, ActionSpec::Type::Truncate);
  EXPECT_EQ(pr->action.type, ActionSpec::Type::RunBlank);
  EXPECT_EQ(pr->kind, ConditionalKind::PostRun);
}

TEST(ConditionalsToml, Errors) {
  EXPECT_FALSE(parse_conditionals("[[truncations]]\ncheck = \"Ar40 >\"\n"));
  EXPECT_FALSE(parse_conditionals("[[truncations]]\nstart = 1\n"));
  EXPECT_FALSE(parse_conditionals("[[truncations]]\ncheck = \"Ar40 > 1\"\nbogus = 1\n"));
  EXPECT_FALSE(parse_conditionals("[[bogus]]\ncheck = \"Ar40 > 1\"\n"));
  EXPECT_FALSE(parse_conditionals("[[actions]]\ncheck = \"Ar40 > 1\"\naction = \"eval(x)\"\n"));
  EXPECT_FALSE(parse_conditionals("[[truncations]]\ncheck = \"Ar40 > 1\"\nfrequency = 0\n"));
  auto series = parse_conditionals("[[truncations]]\ncheck = \"Ar40.bs > 1\"\n");
  ASSERT_FALSE(series);
  EXPECT_NE(series.error().what.find("without a reducer"), std::string::npos);
  EXPECT_FALSE(parse_conditionals("[[truncations]]\ncheck = \"Ar40 > 1\"\n[[truncations]]\ncheck = \"Ar40 > 1\"\n"));  // duplicate name
}

TEST(ConditionalsWhiff, ParseAndEvaluate) {
  auto w = parse_whiff(R"(
[whiff]
sniff = 4
[[whiff.checks]]
check = "Ar40 > 1e9"
action = "abort"
[[whiff.checks]]
check = "Ar40 > 8e5"
action = "pump"
)");
  ASSERT_TRUE(w) << w.error().what;
  EXPECT_EQ(w->sniff, 4);
  ASSERT_EQ(w->checks.size(), 2u);
  auto ctx = make_ctx();
  EXPECT_EQ(evaluate_whiff(*w, ctx, {}), WhiffCheck::Action::Pump);
  EXPECT_FALSE(parse_whiff("[whiff]\n[[whiff.checks]]\ncheck = \"Ar40 > 1\"\naction = \"explode\"\n"));
  EXPECT_FALSE(parse_whiff("x = 1"));
}
