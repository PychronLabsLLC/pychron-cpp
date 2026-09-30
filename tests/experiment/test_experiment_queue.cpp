#include <gtest/gtest.h>

#include <algorithm>

#include "pychron/experiment/model/experiment_queue.hpp"

using namespace pychron::experiment;

namespace {
const IdentifierRules kIds = IdentifierRules::defaults();

RunSpec run(const std::string& identifier, double value = 0, const std::string& device = "laser") {
  RunSpec r;
  r.id.identifier = identifier;
  r.id.type = kIds.classify(identifier);
  r.extraction.device = device;
  r.extraction.value = value;
  return r;
}

std::vector<std::string> ids(const ExperimentQueue& q) {
  std::vector<std::string> out;
  for (const auto& r : q.runs()) out.push_back(r.id.identifier);
  return out;
}

using V = std::vector<std::string>;

ExperimentQueue abcde() {
  ExperimentQueue q;
  for (const char* s : {"1", "2", "3", "4", "5"}) q.append(run(s));
  return q;
}
}  // namespace

TEST(ExperimentQueue, InsertAndRemove) {
  auto q = abcde();
  ASSERT_TRUE(q.insert(0, run("0")));
  ASSERT_TRUE(q.insert(6, run("6")));
  EXPECT_FALSE(q.insert(8, run("x")));
  EXPECT_EQ(ids(q), (V{"0", "1", "2", "3", "4", "5", "6"}));
  ASSERT_TRUE(q.remove({0, 6, 3}));
  EXPECT_EQ(ids(q), (V{"1", "2", "4", "5"}));
  EXPECT_FALSE(q.remove({9}));
}

TEST(ExperimentQueue, MoveSelectionKeepsRelativeOrder) {
  auto q = abcde();
  ASSERT_TRUE(q.move({3, 1}, 0));  // "2","4" to the front
  EXPECT_EQ(ids(q), (V{"2", "4", "1", "3", "5"}));
  q = abcde();
  ASSERT_TRUE(q.move({0}, 5));  // to the end
  EXPECT_EQ(ids(q), (V{"2", "3", "4", "5", "1"}));
  q = abcde();
  ASSERT_TRUE(q.move({0, 1}, 3));  // before original row 3 ("4")
  EXPECT_EQ(ids(q), (V{"3", "1", "2", "4", "5"}));
  EXPECT_FALSE(q.move({7}, 0));
  EXPECT_FALSE(q.move({0}, 6));
}

TEST(ExperimentQueue, CopyInsertsDuplicates) {
  auto q = abcde();
  ASSERT_TRUE(q.copy({0, 2}, 5));
  EXPECT_EQ(ids(q), (V{"1", "2", "3", "4", "5", "1", "3"}));
}

TEST(ExperimentQueue, RepeatBlock) {
  auto q = abcde();
  ASSERT_TRUE(q.repeat_block(1, 2, 2));  // "2","3" twice more after the block
  EXPECT_EQ(ids(q), (V{"1", "2", "3", "2", "3", "2", "3", "4", "5"}));
  EXPECT_FALSE(q.repeat_block(8, 3, 1));
  EXPECT_FALSE(q.repeat_block(0, 0, 1));
}

TEST(ExperimentQueue, RandomizeIsSeededPermutation) {
  auto a = abcde();
  auto b = abcde();
  a.randomize(42);
  b.randomize(42);
  EXPECT_EQ(ids(a), ids(b));
  auto sorted = ids(a);
  std::sort(sorted.begin(), sorted.end());
  EXPECT_EQ(sorted, (V{"1", "2", "3", "4", "5"}));

  // Selection only: unselected rows stay put.
  auto c = abcde();
  ASSERT_TRUE(c.randomize(7, {1, 2, 3}));
  EXPECT_EQ(c.runs()[0].id.identifier, "1");
  EXPECT_EQ(c.runs()[4].id.identifier, "5");
}

TEST(ExperimentQueue, GroupByExtractionIsStable) {
  ExperimentQueue q;
  q.append(run("1", 5));
  q.append(run("2", 10));
  q.append(run("3", 5));
  q.append(run("4", 10, "furnace"));
  q.append(run("5", 10));
  q.group_by_extraction();
  EXPECT_EQ(ids(q), (V{"1", "3", "2", "5", "4"}));
}

TEST(ExperimentQueue, ToggleSkipAndEndAfter) {
  auto q = abcde();
  ASSERT_TRUE(q.toggle_skip({0, 2}));
  EXPECT_TRUE(q.runs()[0].skip);
  EXPECT_TRUE(q.runs()[2].skip);
  ASSERT_TRUE(q.toggle_skip({0}));
  EXPECT_FALSE(q.runs()[0].skip);

  ASSERT_TRUE(q.toggle_end_after(1));
  EXPECT_TRUE(q.runs()[1].end_after);
  ASSERT_TRUE(q.toggle_end_after(3));  // only one end-after at a time
  EXPECT_FALSE(q.runs()[1].end_after);
  EXPECT_TRUE(q.runs()[3].end_after);
  ASSERT_TRUE(q.toggle_end_after(3));
  EXPECT_FALSE(q.runs()[3].end_after);
  EXPECT_FALSE(q.toggle_end_after(9));
}

TEST(FrequencyExpansion, EveryNUnknowns) {
  ExperimentQueue q;
  for (const char* s : {"1", "2", "3", "4", "5"}) q.append(run(s));
  FrequencySpec f;
  f.run = run("bu");
  f.every = 2;
  auto n = q.add_frequency_runs(f);
  ASSERT_TRUE(n);
  EXPECT_EQ(*n, 2u);
  EXPECT_EQ(ids(q), (V{"1", "2", "bu", "3", "4", "bu", "5"}));
}

TEST(FrequencyExpansion, BeforeAndAfter) {
  ExperimentQueue q;
  q.append(run("a"));  // air is not counted
  for (const char* s : {"1", "2", "3", "4"}) q.append(run(s));
  FrequencySpec f;
  f.run = run("bu");
  f.every = 2;
  f.before = true;
  f.after = true;
  ASSERT_TRUE(q.add_frequency_runs(f));
  // after-last coincides with the every-2 insert: no duplicate.
  EXPECT_EQ(ids(q), (V{"a", "bu", "1", "2", "bu", "3", "4", "bu"}));
}

TEST(FrequencyExpansion, CountedTypesAndRange) {
  ExperimentQueue q;
  for (const char* s : {"1", "a", "2", "a", "3"}) q.append(run(s));
  FrequencySpec f;
  f.run = run("ba");
  f.every = 1;
  f.counted = {AnalysisType::Air};
  ASSERT_TRUE(q.add_frequency_runs(f));
  EXPECT_EQ(ids(q), (V{"1", "a", "ba", "2", "a", "ba", "3"}));

  ExperimentQueue r;
  for (const char* s : {"1", "2", "3", "4"}) r.append(run(s));
  FrequencySpec g;
  g.run = run("bu");
  g.every = 1;
  g.first = 2;  // only rows 2..end
  ASSERT_TRUE(r.add_frequency_runs(g));
  EXPECT_EQ(ids(r), (V{"1", "2", "3", "bu", "4", "bu"}));
}

TEST(FrequencyExpansion, SkippedRunsAreNotCounted) {
  ExperimentQueue q;
  for (const char* s : {"1", "2", "3"}) q.append(run(s));
  ASSERT_TRUE(q.toggle_skip({1}));
  FrequencySpec f;
  f.run = run("bu");
  f.every = 2;
  ASSERT_TRUE(q.add_frequency_runs(f));
  EXPECT_EQ(ids(q), (V{"1", "2", "3", "bu"}));
}

TEST(FrequencyExpansion, RejectsNothingToDo) {
  auto q = abcde();
  FrequencySpec f;
  f.run = run("bu");
  EXPECT_FALSE(q.add_frequency_runs(f));  // every = 0, no before/after
  f.every = -1;
  EXPECT_FALSE(q.add_frequency_runs(f));
}

TEST(ExperimentQueue, SpecRoundTripsThroughQueue) {
  QueueSpec spec;
  spec.name = "q";
  spec.runs = {run("1"), run("2")};
  ExperimentQueue q(spec);
  EXPECT_EQ(q.spec(), spec);
  q.header().name = "renamed";
  EXPECT_EQ(q.spec().name, "renamed");
  EXPECT_EQ(q.size(), 2u);
}
