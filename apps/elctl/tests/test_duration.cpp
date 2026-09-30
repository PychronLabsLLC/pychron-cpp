#include "duration.hpp"

#include <chrono>

#include <gtest/gtest.h>

using namespace std::chrono_literals;

namespace elctl {
namespace {

TEST(ParseDuration, AcceptsUnits) {
  EXPECT_EQ(*parse_duration("250ms"), pychron::Duration(250ms));
  EXPECT_EQ(*parse_duration("3s"), pychron::Duration(3s));
  EXPECT_EQ(*parse_duration("2m"), pychron::Duration(2min));
  EXPECT_EQ(*parse_duration("1h"), pychron::Duration(1h));
}

TEST(ParseDuration, BareNumberIsSeconds) {
  EXPECT_EQ(*parse_duration("5"), pychron::Duration(5s));
  EXPECT_EQ(*parse_duration("1.5"), pychron::Duration(1500ms));
}

TEST(ParseDuration, RejectsGarbage) {
  for (const char* bad : {"", "s", "abc", "5x", "-1s", "1.2.3s"}) {
    auto d = parse_duration(bad);
    ASSERT_FALSE(d) << bad;
    EXPECT_EQ(d.error().kind, pychron::ErrorKind::Config) << bad;
  }
}

}  // namespace
}  // namespace elctl
