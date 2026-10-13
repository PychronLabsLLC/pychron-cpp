#include <gtest/gtest.h>

#include <random>

#include "catalog_fixture.hpp"
#include "pychron/entry/identifier_plan.hpp"

using namespace pychron;
using namespace pychron::entry;
using namespace pychron::persistence;
using namespace pychron::persistence::testing;

namespace {

PositionRow pos(int n, bool sample, std::optional<std::string> identifier = std::nullopt, int analyses = 0) {
  PositionRow p;
  p.uuid = Uuid::v7();
  p.position = n;
  if (sample) {
    p.sample = Uuid::v7();
    p.sample_name = "s" + std::to_string(n);
  }
  p.identifier = identifier;
  if (identifier) p.identifier_uuid = Uuid::v7();
  p.n_analyses = analyses;
  return p;
}

LevelSheet level(const std::string& name, std::vector<PositionRow> positions) {
  LevelSheet s;
  s.level.uuid = Uuid::v7();
  s.level.name = name;
  s.positions = std::move(positions);
  return s;
}

std::vector<std::int64_t> numbers(const IdentifierPlan& p) {
  std::vector<std::int64_t> out;
  for (const auto& a : p.assignments) out.push_back(a.number);
  return out;
}

}  // namespace

TEST(IdentifierPlan, SequentialAcrossLevelsInNameOrder) {
  // B is given first; A is numbered first.
  const auto p = plan_identifiers({level("B", {pos(2, true), pos(1, true)}), level("A", {pos(3, true), pos(1, false), pos(2, true)})},
                                  100, false);
  ASSERT_EQ(p.assignments.size(), 4u);
  EXPECT_EQ(numbers(p), (std::vector<std::int64_t>{101, 102, 103, 104}));
  EXPECT_EQ(p.assignments[0].level, "A");
  EXPECT_EQ(p.assignments[0].position_number, 2);
  EXPECT_EQ(p.assignments[1].position_number, 3);
  EXPECT_EQ(p.assignments[2].level, "B");
  EXPECT_EQ(p.assignments[2].position_number, 1);
  EXPECT_EQ(p.expected_last, 100);
  EXPECT_EQ(p.last, 104);
}

TEST(IdentifierPlan, OverwriteAndAnalyzed) {
  const std::vector<LevelSheet> sheets = {
      level("A", {pos(1, true, "50"), pos(2, true, "51", 3), pos(3, true)})};
  auto p = plan_identifiers(sheets, 60, false);
  EXPECT_EQ(numbers(p), (std::vector<std::int64_t>{61}));
  p = plan_identifiers(sheets, 60, true);
  EXPECT_EQ(numbers(p), (std::vector<std::int64_t>{61, 62}));  // the analyzed one is kept
  EXPECT_EQ(p.assignments[0].current, "50");
  EXPECT_TRUE(p.assignments[0].replaces);
  EXPECT_FALSE(p.assignments[1].replaces);
  // A loaded identifier is kept too.
  auto loaded = sheets;
  loaded[0].positions[0].in_load = true;
  EXPECT_EQ(plan_identifiers(loaded, 60, true).assignments.size(), 1u);
  EXPECT_TRUE(plan_identifiers({}, 5, true).assignments.empty());
}

TEST(IdentifierPlan, AllocationCarriesThePlan) {
  const auto p = plan_identifiers({level("A", {pos(1, true), pos(2, true, "9")})}, 10, true);
  const auto a = p.allocation();
  EXPECT_EQ(a.expected_last, 10);
  ASSERT_EQ(a.assignments.size(), 2u);
  EXPECT_EQ(a.assignments[1].number, 12);
  EXPECT_EQ(a.assignments[1].replaces, p.assignments[1].replaces);
}

TEST(IdentifierPlan, PropertySequentialAndIdempotent) {
  std::mt19937 rng(20261004);
  for (int round = 0; round < 500; ++round) {
    std::vector<LevelSheet> sheets;
    const int nlevels = static_cast<int>(rng() % 4);
    for (int l = 0; l < nlevels; ++l) {
      std::vector<PositionRow> ps;
      const int n = static_cast<int>(rng() % 8);
      for (int i = 1; i <= n; ++i) ps.push_back(pos(i, rng() % 3 != 0, rng() % 4 == 0 ? std::optional<std::string>("x") : std::nullopt, rng() % 5 == 0 ? 1 : 0));
      std::shuffle(ps.begin(), ps.end(), rng);
      sheets.push_back(level(std::string(1, static_cast<char>('A' + (nlevels - 1 - l))), ps));
    }
    const auto last = static_cast<std::int64_t>(rng() % 1000);
    const bool overwrite = rng() % 2 == 0;
    const auto p = plan_identifiers(sheets, last, overwrite);
    for (std::size_t i = 0; i < p.assignments.size(); ++i)
      ASSERT_EQ(p.assignments[i].number, last + 1 + static_cast<std::int64_t>(i));
    for (std::size_t i = 1; i < p.assignments.size(); ++i) {
      const auto& a = p.assignments[i - 1];
      const auto& b = p.assignments[i];
      ASSERT_TRUE(a.level < b.level || (a.level == b.level && a.position_number < b.position_number));
    }
    // Apply the plan; re-planning without overwrite assigns nothing.
    for (auto& s : sheets)
      for (auto& q : s.positions)
        for (const auto& a : p.assignments)
          if (a.position == q.uuid) q.identifier = std::to_string(a.number);
    ASSERT_TRUE(plan_identifiers(sheets, p.last, false).assignments.empty());
  }
}

TEST(IdentifierPlan, HumanErrorChecks) {
  EntrySettings s;
  auto monitor = pos(1, true);
  monitor.sample_name = s.monitor_sample;
  monitor.material = s.monitor_material;
  monitor.project = "Elsewhere";
  const auto warnings = human_error_checks({level("A", {monitor, pos(2, true)}), level("B", {pos(1, true)}), level("C", {})}, s, "NM-301");
  ASSERT_EQ(warnings.size(), 2u);
  EXPECT_NE(warnings[0].find("Irradiation-NM-301"), std::string::npos);
  EXPECT_NE(warnings[1].find("level B has no monitor"), std::string::npos);
}

namespace {
class PlanStoreTest : public EntryTest {};
}  // namespace

TEST_P(PlanStoreTest, PlanAllocateReadBack) {
  auto sheets = package_sheets(*store_, cat_.nm301);
  ASSERT_TRUE(sheets);
  auto last = current_last(*store_);
  ASSERT_TRUE(last);
  EXPECT_EQ(*last, 66574);
  const auto plan = plan_identifiers(*sheets, *last, false);
  ASSERT_EQ(plan.assignments.size(), 3u);
  auto out = store_->allocate_identifiers(client(), plan.allocation());
  ASSERT_TRUE(out && std::holds_alternative<CatalogApplied>(*out));
  sheets = package_sheets(*store_, cat_.nm301);
  for (const auto& a : plan.assignments)
    for (const auto& s : *sheets)
      for (const auto& p : s.positions)
        if (p.uuid == a.position) {
          EXPECT_EQ(p.identifier, std::to_string(a.number));
        }
  EXPECT_EQ(*current_last(*store_), 66577);
  EXPECT_TRUE(plan_identifiers(*sheets, 66577, false).assignments.empty());
}

INSTANTIATE_TEST_SUITE_P(Engines, PlanStoreTest, ::testing::ValuesIn(engines()),
                         [](const auto& p) { return p.param; });
