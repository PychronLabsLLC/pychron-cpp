#include <gtest/gtest.h>

#include <set>

#include "pychron/experiment/model/queue_validation.hpp"

using namespace pychron::experiment;

namespace {
const IdentifierRules kIds = IdentifierRules::defaults();

struct Plans : IPlanResolver {
  std::set<std::string> names{"argon", "staged", "broken"};
  bool has_plan(std::string_view name) const override { return names.contains(std::string(name)); }
  std::optional<Duration> plan_duration(std::string_view name, const ParamOverrides& o, bool) const override {
    if (!has_plan(name)) return std::nullopt;
    return Duration(o.contains("counts") ? 200 : 100);
  }
  // "staged" prepares its gases on the line (spec 11.4).
  // "broken" is a plan that does not load.
  std::optional<bool> holds_extraction_line(std::string_view name, const ParamOverrides&) const override {
    if (name == "broken") return std::nullopt;
    return name == "staged";
  }
};

struct Scripts : IScriptResolver {
  std::set<std::string> names{"felix", "pe", "pm"};
  bool has_script(std::string_view name) const override { return names.contains(std::string(name)); }
};

struct Conds : IConditionalResolver {
  bool has_conditional(std::string_view name, std::string_view kind) const override {
    return name == "trunc1" && kind == "truncate";
  }
  bool has_conditional_set(std::string_view name) const override { return name == "queue_cond"; }
};

RunSpec unknown(const std::string& id) {
  RunSpec r;
  r.id.identifier = id;
  r.id.type = kIds.classify(id);
  r.extraction.device = "laser";
  r.extraction.duration = Duration(10);
  r.extraction.script = "felix";
  r.measurement.plan = "argon";
  return r;
}

QueueSpec good_queue() {
  QueueSpec q;
  q.mass_spectrometer = "jan";
  q.queue_conditionals = "queue_cond";
  q.delays.before_analyses = Duration(5);
  q.delays.between_analyses = Duration(1);
  q.runs = {unknown("100"), unknown("101")};
  q.runs[1].conditionals = {{"trunc1", "truncate"}};
  q.runs[1].post_equilibration = "pe";
  q.runs[1].post_measurement = "pm";
  return q;
}

bool has(const QueueReport& r, int run, const std::string& field, Severity sev = Severity::Error) {
  for (const auto& d : r.diagnostics)
    if (d.run == run && d.field == field && d.severity == sev) return true;
  return false;
}
}  // namespace

TEST(QueueValidation, CleanQueueReportsEta) {
  Plans p;
  Scripts s;
  Conds c;
  auto r = check_queue(good_queue(), kIds, {&p, &s, &c});
  EXPECT_TRUE(r.ok());
  EXPECT_TRUE(r.diagnostics.empty()) << r.diagnostics.front().field << ": " << r.diagnostics.front().message;
  ASSERT_EQ(r.run_estimates.size(), 2u);
  EXPECT_EQ(r.run_estimates[0], Duration(110));
  EXPECT_EQ(r.eta, Duration(5 + 110 + 1 + 110));
}

TEST(QueueValidation, MissingReferencesAreCollected) {
  Plans p;
  Scripts s;
  Conds c;
  auto q = good_queue();
  q.runs[0].measurement.plan = "nope";
  q.runs[0].extraction.script = "gone";
  q.runs[1].post_measurement = "gone";
  q.runs[1].measurement.hook = "gone_hook";
  q.runs[1].conditionals.push_back({"trunc1", "cancel"});
  q.queue_conditionals = "missing";
  auto r = check_queue(q, kIds, {&p, &s, &c});
  EXPECT_FALSE(r.ok());
  EXPECT_TRUE(has(r, 0, "measurement.plan"));
  EXPECT_TRUE(has(r, 0, "extraction.script"));
  EXPECT_TRUE(has(r, 1, "post_measurement"));
  EXPECT_TRUE(has(r, 1, "measurement.hook"));
  EXPECT_TRUE(has(r, 1, "conditionals"));
  EXPECT_TRUE(has(r, -1, "queue.queue_conditionals"));
  // Unknown plan contributes no measurement time.
  EXPECT_EQ(r.run_estimates[0], Duration(10));
}

TEST(QueueValidation, IncludesSchemaRules) {
  auto q = good_queue();
  q.runs.push_back(unknown("a"));
  q.runs.back().extraction.value = 5;  // air cannot heat
  q.runs.push_back(unknown("bad id!"));
  auto r = check_queue(q, kIds, {});
  EXPECT_TRUE(has(r, 2, "extraction.value"));
  EXPECT_TRUE(has(r, 3, "identifier"));
}

TEST(QueueValidation, NullResolversSkipReferenceChecks) {
  auto q = good_queue();
  q.runs[0].measurement.plan = "anything";
  auto r = check_queue(q, kIds, {});
  EXPECT_TRUE(r.ok());
}

TEST(QueueValidation, DuplicateFixedAliquotIsError) {
  auto q = good_queue();
  q.runs[0].id.aliquot = 2;
  q.runs[1].id.identifier = "100";
  q.runs[1].id.aliquot = 2;
  auto r = check_queue(q, kIds, {});
  EXPECT_TRUE(has(r, 1, "aliquot"));
  q.runs[1].id.step = "A";
  q.runs[0].id.step = "B";
  EXPECT_FALSE(has(check_queue(q, kIds, {}), 1, "aliquot"));
}

TEST(QueueValidation, WarningsDoNotFailQueue) {
  auto q = good_queue();
  q.runs[0].end_after = true;
  q.runs[1].end_after = true;  // runs after the first end_after never execute
  auto r = check_queue(q, kIds, {});
  EXPECT_TRUE(r.ok());
  EXPECT_TRUE(has(r, 1, "end_after", Severity::Warning));

  auto e = good_queue();
  e.runs = {};
  auto re = check_queue(e, kIds, {});
  EXPECT_TRUE(re.ok());
  EXPECT_TRUE(has(re, -1, "runs", Severity::Warning));
}

TEST(QueueValidation, SkippedRunsExcludedFromEta) {
  auto q = good_queue();
  q.runs[0].skip = true;
  Plans p;
  auto r = check_queue(q, kIds, {&p, nullptr, nullptr});
  EXPECT_EQ(r.run_estimates[0], Duration(0));
  EXPECT_EQ(r.eta, Duration(5 + 110));
}

// ---- a staged plan and overlap (measurement plan v2 spec 11.4) ----------------

namespace {
QueueSpec staged_queue(const std::string& first_plan, const std::string& second_plan, bool overlap) {
  QueueSpec q;
  q.mass_spectrometer = "jan";
  q.runs = {unknown("100"), unknown("101")};
  q.runs[0].measurement.plan = first_plan;
  q.runs[1].measurement.plan = second_plan;
  if (overlap) q.runs[0].overlap.duration = Duration(30);
  return q;
}
const Plans kPlans;
const QueueResolvers kRes{&kPlans, nullptr, nullptr};
}  // namespace

TEST(QueueValidation, OverlapWithStagedPlanIsRefused) {
  const auto r = check_queue(staged_queue("staged", "argon", true), kIds, kRes);
  EXPECT_FALSE(r.ok());
  ASSERT_TRUE(has(r, 0, "overlap"));
  EXPECT_FALSE(has(r, 1, "overlap"));
  for (const auto& d : r.diagnostics) {
    if (d.field == "overlap") {
      EXPECT_EQ(d.message, "row 0: plan 'staged' holds the extraction line until its last gas; overlap is not possible");
    }
  }
}

TEST(QueueValidation, OverlapBeforeStagedPlanIsFine) {
  const auto r = check_queue(staged_queue("argon", "staged", true), kIds, kRes);
  EXPECT_FALSE(has(r, 0, "overlap"));
  EXPECT_FALSE(has(r, 1, "overlap"));
}

TEST(QueueValidation, StagedPlanWithoutOverlapIsFine) {
  const auto r = check_queue(staged_queue("staged", "staged", false), kIds, kRes);
  EXPECT_FALSE(has(r, 0, "overlap"));
  // Nor as the last runnable row, which never overlaps.
  auto last = staged_queue("argon", "staged", false);
  last.runs[1].overlap.duration = Duration(30);
  EXPECT_FALSE(has(check_queue(last, kIds, kRes), 1, "overlap"));
}

TEST(QueueValidation, NullPlanResolverSkipsTheCheck) {
  const auto r = check_queue(staged_queue("staged", "argon", true), kIds, QueueResolvers{});
  EXPECT_FALSE(has(r, 0, "overlap"));
}

// A plan that does not load cannot be said to allow the overlap: said.
TEST(QueueValidation, OverlapWithAPlanThatDoesNotLoadIsReported) {
  const auto r = check_queue(staged_queue("broken", "argon", true), kIds, kRes);
  ASSERT_TRUE(has(r, 0, "overlap"));
  for (const auto& d : r.diagnostics) {
    if (d.field == "overlap") {
      EXPECT_NE(d.message.find("does not load"), std::string::npos) << d.message;
    }
  }
}
