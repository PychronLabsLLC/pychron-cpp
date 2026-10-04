#include <gtest/gtest.h>

#include <cmath>

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
  EXPECT_FALSE(eng.evaluate(ConditionalKind::Truncation, empty, {}, 1, 0));
  EXPECT_FALSE(eng.evaluate(ConditionalKind::Truncation, empty, {}, 2, 0));
  ASSERT_EQ(eng.errors().size(), 1u);  // deduplicated per conditional
  EXPECT_EQ(eng.errors()[0].count, 2);
  EXPECT_EQ(eng.errors()[0].name, "t");
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
  for (const char* k : {"skip_next", "skip_n", "skip_aliquot", "skip_to_last_in_aliquot", "repeat", "run_blank"})
    EXPECT_TRUE(parse_action(k)) << k;
  EXPECT_EQ(parse_action("skip_n")->count, 1);
  EXPECT_EQ(parse_action("skip_n 3")->count, 3);
  EXPECT_EQ(parse_action("skip_n=2")->count, 2);
  auto se = parse_action("set_extract=2.5");
  ASSERT_TRUE(se);
  EXPECT_EQ(se->steps, std::vector<double>{2.5});
  auto pct = parse_action("set_extract 10%, 20%");
  ASSERT_TRUE(pct);
  EXPECT_TRUE(pct->percent);
  EXPECT_EQ(pct->steps, (std::vector<double>{10, 20}));
  EXPECT_EQ(to_string(*pct), "set_extract 10%,20%");
  EXPECT_EQ(to_string(*parse_action("set_extract 1,2.5")), "set_extract 1,2.5");
  EXPECT_EQ(to_string(*parse_action("skip_n 4")), "skip_n 4");
  for (const char* bad : {"", "os.system('x')", "truncate:slow", "set_param x", "run_hook", "cancel now",
                          "set_extract=abc", "set_extract", "set_extract 1,10%", "skip_n 0", "skip_n 1.5",
                          "skip_next 2"})
    EXPECT_FALSE(parse_action(bad)) << bad;
  EXPECT_EQ(to_string(*parse_action("truncate:quick")), "truncate:quick");
}

TEST(ConditionalsEngine, StartFrequencyNtripsFollowLegacyGating) {
  // Evaluated after reading n when n > start and (n - start) % frequency == 0.
  auto ctx = make_ctx();
  Conditional c = item(ConditionalKind::Truncation, "t", "Ar40 > 8e5");
  c.start = 3;
  c.frequency = 2;
  c.ntrips = 2;
  ConditionalEngine eng(ConditionalSet{{c}, {}});
  constexpr auto K = ConditionalKind::Truncation;
  for (int n = 1; n <= 4; ++n) EXPECT_FALSE(eng.evaluate(K, ctx, {}, n, n)) << n;  // n=5 is the first check
  EXPECT_FALSE(eng.evaluate(K, ctx, {}, 5, 5));  // true, 1 of 2
  EXPECT_FALSE(eng.evaluate(K, ctx, {}, 6, 6));  // skipped by frequency
  auto trip = eng.evaluate(K, ctx, {}, 7, 7);   // 2 of 2
  ASSERT_TRUE(trip);
  EXPECT_EQ(trip->name, "t");
  EXPECT_EQ(trip->kind, ConditionalKind::Truncation);
  EXPECT_EQ(trip->count, 2);
  EXPECT_EQ(trip->value, 9e5);
  EXPECT_EQ(trip->ts, 7);
  EXPECT_EQ(trip->reading, 7);
  EXPECT_EQ(trip->check, "Ar40 > 800000");
  EXPECT_EQ(trip->id, c.id());
  ASSERT_EQ(trip->context.size(), 1u);
  EXPECT_EQ(trip->context[0], (MetricValue{"Ar40", 9e5}));
  EXPECT_FALSE(eng.evaluate(K, ctx, {}, 9, 9));  // fires once
  EXPECT_EQ(eng.trips().size(), 1u);
  eng.reset();
  EXPECT_TRUE(eng.trips().empty());
}

TEST(ConditionalsEngine, NtripsRequiresConsecutive) {
  MapContext ctx;
  Conditional c = item(ConditionalKind::Termination, "t", "Ar40 > 10");
  c.ntrips = 2;
  ConditionalEngine eng(ConditionalSet{{c}, {}});
  constexpr auto K = ConditionalKind::Termination;
  ctx.series_data["Ar40"] = {20};
  EXPECT_FALSE(eng.evaluate(K, ctx, {}, 1, 1));
  ctx.series_data["Ar40"] = {0};
  EXPECT_FALSE(eng.evaluate(K, ctx, {}, 2, 2));  // resets
  ctx.series_data["Ar40"] = {20};
  EXPECT_FALSE(eng.evaluate(K, ctx, {}, 3, 3));
  EXPECT_TRUE(eng.evaluate(K, ctx, {}, 4, 4));
}

TEST(ConditionalsEngine, FirstTripWinsInMeasurementOrder) {
  auto ctx = make_ctx();
  ConditionalSet s;
  for (auto k : {ConditionalKind::Equilibration, ConditionalKind::Cancelation, ConditionalKind::Termination,
                 ConditionalKind::Action, ConditionalKind::Truncation, ConditionalKind::Modification})
    s.items.push_back(item(k, std::string(to_string(k)), "Ar40 > 1"));
  s.items.push_back(item(ConditionalKind::PreRun, "pre", "Ar40 > 1"));
  ConditionalEngine eng(std::move(s));
  std::vector<std::string> order;
  for (int n = 1; n <= 8; ++n)
    if (auto t = eng.evaluate(kMeasurementOrder, ctx, {}, n, 0)) order.push_back(t->name);
  // One trip per reading; equilibration and pre_run are not in the measurement order.
  EXPECT_EQ(order, (std::vector<std::string>{"modification", "truncation", "action", "termination", "cancelation"}));
  EXPECT_TRUE(eng.evaluate(ConditionalKind::Equilibration, ctx, {}, 1, 0));
  EXPECT_TRUE(eng.check_now(ConditionalKind::PreRun, ctx, {}));
}

TEST(ConditionalsEngine, LaterKindsAreNotEvaluatedOnATrippedReading) {
  MapContext ctx;
  ctx.series_data["Ar40"] = {5};
  Conditional mod = item(ConditionalKind::Modification, "mod", "Ar40 > 1");
  mod.action.type = ActionSpec::Type::SkipNext;
  Conditional term = item(ConditionalKind::Termination, "term", "Ar40 > 1");
  term.ntrips = 2;
  ConditionalEngine eng(ConditionalSet{{mod, term}, {}});
  EXPECT_EQ(eng.evaluate(kMeasurementOrder, ctx, {}, 1, 0)->name, "mod");  // term not counted
  EXPECT_FALSE(eng.evaluate(kMeasurementOrder, ctx, {}, 2, 0));            // term 1 of 2
  EXPECT_EQ(eng.evaluate(kMeasurementOrder, ctx, {}, 3, 0)->name, "term");
}

TEST(ConditionalsEngine, ResumingActionsReArm) {
  MapContext ctx;
  ctx.series_data["Ar40"] = {5};
  Conditional a = item(ConditionalKind::Action, "a", "Ar40 > 1");
  a.action.type = ActionSpec::Type::Notify;
  a.resume = true;
  a.ntrips = 2;
  Conditional once = item(ConditionalKind::Action, "once", "Ar40 > 100");
  once.action.type = ActionSpec::Type::Notify;
  ConditionalEngine eng(ConditionalSet{{a, once}, {}});
  int fired = 0;
  for (int n = 1; n <= 6; ++n) fired += eng.evaluate(ConditionalKind::Action, ctx, {}, n, 0) ? 1 : 0;
  EXPECT_EQ(fired, 3);  // every second reading
}

TEST(ConditionalsEngine, AnalysisTypeFilter) {
  MapContext ctx;
  ctx.series_data["Ar40"] = {5};
  Conditional blanks = item(ConditionalKind::Termination, "blanks", "Ar40 > 1");
  blanks.analysis_types = {"blank"};
  Conditional air = item(ConditionalKind::Termination, "air", "Ar40 > 1");
  air.analysis_types = {"air", "cocktail"};
  EXPECT_TRUE(blanks.applies_to("blank_unknown"));
  EXPECT_TRUE(blanks.applies_to("Blank_Air"));
  EXPECT_FALSE(blanks.applies_to("unknown"));
  EXPECT_TRUE(air.applies_to("air"));
  EXPECT_TRUE(air.applies_to(""));  // unknown type: everything applies
  ConditionalEngine eng(ConditionalSet{{blanks, air}, {}}, "unknown");
  EXPECT_TRUE(eng.installed().empty());
  EXPECT_FALSE(eng.evaluate(ConditionalKind::Termination, ctx, {}, 1, 0));
  ConditionalEngine eng2(ConditionalSet{{blanks, air}, {}}, "blank_cocktail");
  ASSERT_EQ(eng2.installed().size(), 1u);
  EXPECT_EQ(eng2.evaluate(ConditionalKind::Termination, ctx, {}, 1, 0)->name, "blanks");
}

TEST(ConditionalsEngine, CheckNowCountsNtripsAcrossCalls) {
  MapContext ctx;
  ctx.series_data["Ar40"] = {5};
  Conditional c = item(ConditionalKind::PostRun, "low", "Ar40 > 1");
  c.ntrips = 2;
  c.action.type = ActionSpec::Type::RunBlank;
  ConditionalEngine eng(ConditionalSet{{c}, {}});
  EXPECT_FALSE(eng.check_now(ConditionalKind::PostRun, ctx, {}));
  EXPECT_TRUE(eng.check_now(ConditionalKind::PostRun, ctx, {}));
  EXPECT_FALSE(eng.check_now(ConditionalKind::PostRun, ctx, {}));  // counting again
  EXPECT_TRUE(eng.check_now(ConditionalKind::PostRun, ctx, {}));
}

TEST(ConditionalsModel, IdIsStableAndDefinitionSensitive) {
  Conditional a = item(ConditionalKind::Truncation, "a", "Ar40 > 8e5");
  Conditional b = item(ConditionalKind::Truncation, "b", "Ar40>800000");  // same canonical check
  EXPECT_EQ(a.id(), b.id());
  EXPECT_EQ(a.id().size(), 64u);
  b.start = 5;
  EXPECT_NE(a.id(), b.id());
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

TEST(ConditionalsToml, LegacyFields) {
  const char* text = R"(
[[truncations]]
name = "big"
check = "Ar40 > 900"
window = 5
mapper = "x + 1000"
abbreviated_count_ratio = 0.5
analysis_types = ["Unknown", "blank air"]

[[actions]]
check = "slope(Ar40) > 100"
action = "run_hook warn"
resume = true

[[modifications]]
check = "Ar40 < 1e3"
action = "set_extract 10%,20%"
truncate = true
abbreviated_count_ratio = 0.25

[[modifications]]
name = "default-skip"
check = "Ar40 < 1"

[[pre_run]]
check = "CDD.inactive"

[[post_run]]
check = "Ar40 < $MIN_INTENSITY"
analysis_types = ["air"]
)";
  auto set = parse_conditionals(text, "lab.toml");
  ASSERT_TRUE(set) << set.error().what;
  ASSERT_EQ(set->items.size(), 6u);
  auto find = [&](ConditionalKind k, int nth = 0) -> const Conditional& {
    for (const auto& c : set->items)
      if (c.kind == k && nth-- == 0) return c;
    ADD_FAILURE() << "missing " << to_string(k);
    return set->items[0];
  };
  const auto& big = find(ConditionalKind::Truncation);
  EXPECT_EQ(big.check, "Ar40 > 900");
  EXPECT_EQ(big.effective_check(), "average(Ar40, window=5) + 1000 > 900");
  EXPECT_EQ(big.window, 5);
  EXPECT_DOUBLE_EQ(big.abbreviated_count_ratio, 0.5);
  EXPECT_EQ(big.analysis_types, (std::vector<std::string>{"unknown", "blank_air"}));
  EXPECT_EQ(big.location, "lab.toml");
  EXPECT_TRUE(find(ConditionalKind::Action).resume);
  EXPECT_EQ(find(ConditionalKind::Action).action.type, ActionSpec::Type::RunHook);
  EXPECT_TRUE(find(ConditionalKind::Modification, 0).truncate);
  EXPECT_TRUE(find(ConditionalKind::Modification, 0).action.percent);
  EXPECT_EQ(find(ConditionalKind::Modification, 1).action.type, ActionSpec::Type::SkipNext);  // default
  EXPECT_EQ(find(ConditionalKind::PreRun).action.type, ActionSpec::Type::Cancel);             // default
  EXPECT_EQ(find(ConditionalKind::PostRun).action.type, ActionSpec::Type::Cancel);            // default
  set->stamp(ConditionalLevel::Queue, "queue/q1.toml");
  for (const auto& c : set->items) {
    EXPECT_EQ(c.level, ConditionalLevel::Queue);
    EXPECT_EQ(c.location, "queue/q1.toml");
  }
}

TEST(ConditionalsToml, PerKindRules) {
  auto bad = [](const std::string& body) {
    auto r = parse_conditionals(body);
    return r ? std::string() : r.error().what;
  };
  EXPECT_NE(bad("[[truncations]]\ncheck = \"Ar40 > 1\"\naction = \"cancel\"\n"), "");
  EXPECT_NE(bad("[[terminations]]\ncheck = \"Ar40 > 1\"\naction = \"cancel\"\n"), "");
  EXPECT_NE(bad("[[actions]]\ncheck = \"Ar40 > 1\"\n"), "");                            // needs action
  EXPECT_NE(bad("[[actions]]\ncheck = \"Ar40 > 1\"\naction = \"run_blank\"\n"), "");    // queue action
  EXPECT_NE(bad("[[modifications]]\ncheck = \"Ar40 > 1\"\naction = \"notify\"\n"), "");
  EXPECT_NE(bad("[[modifications]]\ncheck = \"Ar40 > 1\"\ntruncate = true\nterminate = true\n"), "");
  EXPECT_NE(bad("[[truncations]]\ncheck = \"Ar40 > 1\"\nresume = true\n"), "");
  EXPECT_NE(bad("[[truncations]]\ncheck = \"Ar40 > 1\"\nterminate = true\n"), "");
  EXPECT_NE(bad("[[terminations]]\ncheck = \"Ar40 > 1\"\nabbreviated_count_ratio = 0.5\n"), "");
  EXPECT_NE(bad("[[truncations]]\ncheck = \"Ar40 > 1\"\nabbreviated_count_ratio = 1.5\n"), "");
  EXPECT_NE(bad("[[truncations]]\ncheck = \"Ar40 > 1\"\nwindow = 0\n"), "");
  EXPECT_NE(bad("[[truncations]]\ncheck = \"Ar40 > 1\"\nmapper = \"y\"\n"), "");
  EXPECT_NE(bad("[[pre_run]]\ncheck = \"Ar40 > 1\"\naction = \"run_blank\"\n"), "");
  EXPECT_NE(bad("[[truncations]]\ncheck = \"Ar40 > 1\"\nanalysis_types = \"air\"\n"), "");
  EXPECT_NE(bad("[[truncations]]\ncheck = \"Ar40 > 1\"\nstart = 1.5\n"), "");
  EXPECT_EQ(bad("[[post_run]]\ncheck = \"Ar40 > 1\"\naction = \"skip_n 2\"\n"), "");
}

TEST(ConditionalKinds, FieldsOf) {
  EXPECT_TRUE(fields_of(ConditionalKind::Truncation).ratio);
  EXPECT_TRUE(fields_of(ConditionalKind::Action).resume);
  EXPECT_TRUE(fields_of(ConditionalKind::Modification).run_flags);
  EXPECT_TRUE(fields_of(ConditionalKind::Termination).actions.empty());
  EXPECT_FALSE(fields_of(ConditionalKind::PreRun).gating);
  EXPECT_EQ(fields_of(ConditionalKind::Modification).default_action, ActionSpec::Type::SkipNext);
  EXPECT_EQ(table_name(ConditionalKind::Truncation), "truncations");
  EXPECT_EQ(table_name(ConditionalKind::PreRun), "pre_run");
}

TEST(ConditionalKinds, FinalizeMatchesParser) {
  Conditional c;
  c.kind = ConditionalKind::Action;
  c.check = "Ar40 > 1";
  auto r = finalize(c);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().what, "an action conditional needs 'action'");
  c.kind = ConditionalKind::Truncation;
  auto ok = finalize(c);
  ASSERT_TRUE(ok) << ok.error().what;
  EXPECT_EQ(ok->name, default_name(ConditionalKind::Truncation, "Ar40 > 1"));
  EXPECT_EQ(ok->name, "truncation:Ar40 > 1");
  EXPECT_EQ(ok->action.type, ActionSpec::Type::Truncate);
  ASSERT_TRUE(ok->expr);
  c.resume = true;
  EXPECT_EQ(finalize(c).error().what, "'resume' applies to actions only");
  c = {};
  c.kind = ConditionalKind::Modification;
  c.check = "Ar40 < 1";
  c.truncate = c.terminate = true;
  EXPECT_EQ(finalize(c).error().what, "a modification may truncate or terminate, not both");
  c = {};
  c.check = "Ar40 >";
  EXPECT_FALSE(finalize(c));
  c = {};
  EXPECT_EQ(finalize(c).error().what, "missing 'check'");
}

TEST(ConditionalKinds, FinalizeRejectsWhatTheParserRejects) {
  auto err = [](Conditional c) {
    auto r = finalize(std::move(c));
    return r ? std::string() : r.error().what;
  };
  Conditional c;
  c.check = "Ar40 > 1";
  c.kind = ConditionalKind::Termination;
  c.abbreviated_count_ratio = 0.5;
  EXPECT_EQ(err(c), "'abbreviated_count_ratio' applies to truncations, modifications and equilibrations");
  c.abbreviated_count_ratio = 1.0;
  c.action.type = ActionSpec::Type::Cancel;
  EXPECT_EQ(err(c), "'action' is not allowed on terminations");
  c.action = {};
  c.kind = ConditionalKind::Truncation;
  c.abbreviated_count_ratio = 1.5;
  EXPECT_EQ(err(c), "'abbreviated_count_ratio' must be a number in (0, 1]");
  c.abbreviated_count_ratio = 1.0;
  c.window = 0;
  EXPECT_EQ(err(c), "'window' must be an integer >= 1");
  c.window.reset();
  c.frequency = 0;
  EXPECT_EQ(err(c), "'frequency' must be an integer >= 1");
  c.frequency = 1;
  c.action.type = ActionSpec::Type::Cancel;
  EXPECT_EQ(err(c), "a truncation's action must be truncate or truncate:quick");
  c.kind = ConditionalKind::Modification;
  EXPECT_EQ(err(c), "a modification's action must be a queue action (skip_next, run_blank, ...)");
  c.kind = ConditionalKind::Action;
  c.action.type = ActionSpec::Type::RunBlank;
  EXPECT_EQ(err(c), "queue actions belong in [[modifications]] or [[post_run]]");
  c.kind = ConditionalKind::PreRun;
  EXPECT_EQ(err(c), "a pre_run conditional's action must be cancel");
  c.kind = ConditionalKind::PostRun;
  EXPECT_EQ(err(c), "");
  c.action.type = ActionSpec::Type::Notify;
  EXPECT_EQ(err(c), "a post_run action must be cancel or a queue action");
}

TEST(ConditionalKinds, FinalizeChecksActionParameters) {
  using T = ActionSpec::Type;
  auto err = [](ConditionalKind kind, ActionSpec a) {
    Conditional c;
    c.kind = kind;
    c.check = "Ar40 > 1";
    c.action = std::move(a);
    auto r = finalize(std::move(c));
    return r ? std::string() : r.error().what;
  };
  // What to_toml would write for these does not parse; they must not get that far.
  EXPECT_NE(err(ConditionalKind::Action, {.type = T::SetParam}), "");
  EXPECT_NE(err(ConditionalKind::Action, {.type = T::SetParam, .name = "a=b", .value = 1}), "");
  EXPECT_NE(err(ConditionalKind::Action, {.type = T::RunHook}), "");
  EXPECT_NE(err(ConditionalKind::Action, {.type = T::RunHook, .name = "two words"}), "");
  EXPECT_NE(err(ConditionalKind::Modification, {.type = T::SetExtract}), "");
  EXPECT_NE(err(ConditionalKind::PostRun, {.type = T::SkipN, .count = 0}), "");
  EXPECT_NE(err(ConditionalKind::Action, {.type = T::SetParam, .name = "X", .value = std::nan("")}), "");
  EXPECT_NE(err(ConditionalKind::Action, {.type = T::SetParam, .name = "X", .value = HUGE_VAL}), "");
  EXPECT_EQ(err(ConditionalKind::Action, {.type = T::SetParam, .name = "X", .value = 0.1 + 0.2}), "");
  EXPECT_EQ(err(ConditionalKind::Action, {.type = T::RunHook, .name = "warn"}), "");
  EXPECT_EQ(err(ConditionalKind::Modification, {.type = T::SetExtract, .steps = {10, 20}, .percent = true}), "");
  EXPECT_EQ(err(ConditionalKind::PostRun, {.type = T::SkipN, .count = 3}), "");
}
