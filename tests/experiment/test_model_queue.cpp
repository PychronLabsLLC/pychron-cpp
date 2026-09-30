#include <gtest/gtest.h>

#include "pychron/experiment/model/queue_toml.hpp"
#include "pychron/experiment/model/rules.hpp"

using namespace pychron::experiment;

namespace {
const IdentifierRules kIds = IdentifierRules::defaults();

constexpr const char* kQueue = R"(
[queue]
schema_version = 1
name = "test"
mass_spectrometer = "jan"
extract_device = "FusionsDiode"
tray = "24-well"

[queue.delays]
before_analyses = 30

[[runs]]
identifier = "bu"
[runs.measurement]
plan = "argon"

[[runs]]
identifier = "20001"
e_value = 5.5
extract_units = "w"
duration = 30
cleanup = 120
position = "1-3"
s_opt = "foo"
truncate = "tr1"
[runs.measurement]
plan = "argon"
[runs.measurement.overrides]
counts = 20
peak_center = true
[runs.sample]
sample = "NM-1"
)";
}  // namespace

TEST(QueueToml, ParsesAndNormalizesAliases) {
  auto q = parse_queue(kQueue, kIds);
  ASSERT_TRUE(q) << q.error().what;
  EXPECT_EQ(q->mass_spectrometer, "jan");
  EXPECT_EQ(q->delays.before_analyses, Duration(30));
  ASSERT_EQ(q->runs.size(), 2u);
  EXPECT_EQ(q->runs[0].id.type, AnalysisType::BlankUnknown);
  EXPECT_EQ(q->runs[0].extraction.device, "FusionsDiode");
  const auto& r = q->runs[1];
  EXPECT_EQ(r.id.type, AnalysisType::Unknown);
  EXPECT_DOUBLE_EQ(r.extraction.value, 5.5);
  EXPECT_EQ(r.extraction.units, Unit::Watts);
  EXPECT_EQ(r.extraction.options, "foo");
  EXPECT_EQ(r.extraction.position->holes, (std::vector<int>{1, 2, 3}));
  ASSERT_EQ(r.conditionals.size(), 1u);
  EXPECT_EQ(r.conditionals[0].kind, "truncate");
  EXPECT_EQ(std::get<std::int64_t>(r.measurement.overrides.at("counts")), 20);
  EXPECT_EQ(r.sample.sample, "NM-1");
}

TEST(QueueToml, AliasConflictIsError) {
  auto q = parse_queue("[queue]\n[[runs]]\nidentifier=\"1\"\ne_value=1\nextract_value=2\n", kIds);
  ASSERT_FALSE(q);
  EXPECT_NE(q.error().what.find("duplicates"), std::string::npos);
}

TEST(QueueToml, UnknownKeyAndBadTypeAreErrors) {
  EXPECT_FALSE(parse_queue("[queue]\nbogus = 1\n", kIds));
  EXPECT_FALSE(parse_queue("[queue]\n[[runs]]\nidentifier = 5\n", kIds));
  EXPECT_FALSE(parse_queue("[queue]\n[[runs]]\nidentifier = \"1\"\nposition = \"x\"\n", kIds));
  EXPECT_FALSE(parse_queue("[queue]\nschema_version = 2\n", kIds));
  EXPECT_FALSE(parse_queue("[[runs]]\nidentifier=\"1\"\n", kIds));
}

TEST(Rules, ValidQueueHasNoIssues) {
  auto q = parse_queue(kQueue, kIds);
  ASSERT_TRUE(q);
  // bu: heating allowed, plan given -> fine.
  auto issues = validate_queue(*q, kIds);
  EXPECT_TRUE(issues.empty()) << issues.front().field << ": " << issues.front().message;
}

TEST(Rules, AirCannotHeat) {
  auto q = parse_queue(R"(
[queue]
mass_spectrometer = "jan"
[[runs]]
identifier = "a"
e_value = 5
pattern = "spiral"
[runs.measurement]
plan = "argon"
)", kIds);
  ASSERT_TRUE(q);
  auto issues = validate_queue(*q, kIds);
  ASSERT_EQ(issues.size(), 2u);
  EXPECT_EQ(issues[0].run, 0);
}

TEST(Rules, PauseAndMissingPlan) {
  auto q = parse_queue(R"(
[queue]
mass_spectrometer = "jan"
extract_device = "dev"
[[runs]]
identifier = "pa"
[[runs]]
identifier = "123"
)", kIds);
  ASSERT_TRUE(q);
  auto issues = validate_queue(*q, kIds);
  // pause inherits queue device (forbidden); unknown run has no plan.
  bool device = false, plan = false;
  for (auto& i : issues) {
    device |= i.run == 0 && i.field == "extraction.device";
    plan |= i.run == 1 && i.field == "measurement.plan";
  }
  EXPECT_TRUE(device);
  EXPECT_TRUE(plan);
}

TEST(Rules, SpecialIdentifierRejectsFixedAliquot) {
  auto q = parse_queue("[queue]\nmass_spectrometer=\"j\"\n[[runs]]\nidentifier=\"bu\"\naliquot=3\n[runs.measurement]\nplan=\"p\"\n", kIds);
  ASSERT_TRUE(q);
  EXPECT_FALSE(validate_queue(*q, kIds).empty());
}

TEST(Rules, EtaSkipsSkippedRuns) {
  QueueSpec q;
  q.delays.before_analyses = Duration(10);
  q.delays.between_analyses = Duration(5);
  RunSpec a, b, c;
  a.extraction.duration = Duration(30);
  b.extraction.duration = Duration(100);
  b.skip = true;
  c.extraction.cleanup = Duration(20);
  q.runs = {a, b, c};
  auto eta = queue_eta(q, [](const RunSpec&) { return Duration(60); });
  EXPECT_EQ(eta, Duration(10 + 90 + 5 + 80));
}
