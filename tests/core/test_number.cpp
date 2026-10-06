// parse_double / parse_double_prefix: the same answer on every platform and
// in every locale, the whole text or nothing, finite only.

#include "pychron/core/number.hpp"

#include <gtest/gtest.h>

#include <clocale>

using pychron::parse_double;
using pychron::parse_double_prefix;

TEST(Number, WholeTextOnlyAndFinite) {
  EXPECT_EQ(parse_double("34.5"), 34.5);
  EXPECT_EQ(parse_double("-106.905"), -106.905);
  EXPECT_EQ(parse_double("+1"), 1.0);
  EXPECT_EQ(parse_double("1e-6"), 1e-6);
  EXPECT_EQ(parse_double(".5"), 0.5);
  EXPECT_EQ(parse_double("7"), 7.0);
  EXPECT_FALSE(parse_double(""));
  EXPECT_FALSE(parse_double(" 1"));
  EXPECT_FALSE(parse_double("1 "));
  EXPECT_FALSE(parse_double("1,5"));
  EXPECT_FALSE(parse_double("abc"));
  EXPECT_FALSE(parse_double("1x"));
  EXPECT_FALSE(parse_double("nan"));
  EXPECT_FALSE(parse_double("inf"));
  EXPECT_FALSE(parse_double("-inf"));
  EXPECT_FALSE(parse_double("1e999"));
  EXPECT_FALSE(parse_double("0x10"));
}

TEST(Number, IgnoresTheProcessLocale) {
  // A locale with a comma for the decimal point, when the machine has one.
  const char* set = std::setlocale(LC_NUMERIC, "de_DE.UTF-8");
  if (set == nullptr) set = std::setlocale(LC_NUMERIC, "de_DE");
  EXPECT_EQ(parse_double("2.5"), 2.5);
  EXPECT_FALSE(parse_double("2,5"));
  std::setlocale(LC_NUMERIC, "C");
}

TEST(Number, PrefixTakesTheLongestNumber) {
  auto p = parse_double_prefix("12.5, \"x\": 3");
  ASSERT_TRUE(p);
  EXPECT_EQ(p->first, 12.5);
  EXPECT_EQ(p->second, 4u);
  p = parse_double_prefix("-1e3}");
  ASSERT_TRUE(p);
  EXPECT_EQ(p->first, -1000.0);
  EXPECT_EQ(p->second, 4u);
  p = parse_double_prefix("2e");  // the marker without digits is not part of the number
  ASSERT_TRUE(p);
  EXPECT_EQ(p->first, 2.0);
  EXPECT_EQ(p->second, 1u);
  EXPECT_FALSE(parse_double_prefix("x1"));
  EXPECT_FALSE(parse_double_prefix(""));
  EXPECT_FALSE(parse_double_prefix("-"));
}
