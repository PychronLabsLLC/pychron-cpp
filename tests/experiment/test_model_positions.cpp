#include <gtest/gtest.h>

#include "pychron/experiment/model/positions.hpp"

using namespace pychron::experiment;

TEST(Positions, ParsesListsAndRanges) {
  auto p = parse_position("1,3-5; 9");
  ASSERT_TRUE(p);
  EXPECT_EQ(p->holes, (std::vector<int>{1, 3, 4, 5, 9}));
  EXPECT_EQ(format_position(*p), "1,3-5,9");
}

TEST(Positions, DescendingRange) {
  auto p = parse_position("5-3");
  ASSERT_TRUE(p);
  EXPECT_EQ(p->holes, (std::vector<int>{5, 4, 3}));
}

TEST(Positions, RejectsBadInput) {
  EXPECT_FALSE(parse_position(""));
  EXPECT_FALSE(parse_position("a"));
  EXPECT_FALSE(parse_position("1-"));
  EXPECT_FALSE(parse_position("-3"));
  EXPECT_FALSE(parse_position("1-100000"));
}

TEST(Positions, SplitAndIncrement) {
  auto p = *parse_position("1-3");
  EXPECT_EQ(split_position(p).size(), 3u);
  EXPECT_EQ(split_position(p)[1].holes, (std::vector<int>{2}));
  auto inc = increment_position(p, 2);
  ASSERT_TRUE(inc);
  EXPECT_EQ(inc->holes, (std::vector<int>{3, 4, 5}));
  EXPECT_FALSE(increment_position(p, -2));
}
