#include <gtest/gtest.h>

#include "pychron/core/log_match.hpp"

using pychron::log_name_matches;
using pychron::log_rule_specificity;

TEST(LogMatch, BareNameMatchesSelfAndChildren) {
  EXPECT_TRUE(log_name_matches("scheduler", "scheduler"));
  EXPECT_TRUE(log_name_matches("scheduler", "scheduler.jobs"));
  EXPECT_FALSE(log_name_matches("scheduler", "scheduler2"));
}

TEST(LogMatch, StarMatchesAnySequenceIncludingDots) {
  EXPECT_TRUE(log_name_matches("transport.serial.*", "transport.serial.ig1"));
  EXPECT_TRUE(log_name_matches("transport.serial.*", "transport.serial.ig1.wire"));
  EXPECT_FALSE(log_name_matches("transport.serial.*", "transport.serial"));
  EXPECT_TRUE(log_name_matches("*", "anything.at.all"));
  EXPECT_TRUE(log_name_matches("a*c", "abc"));
  EXPECT_FALSE(log_name_matches("a*c", "ab"));
}

TEST(LogMatch, SpecificityOrdersRules) {
  EXPECT_GT(log_rule_specificity("transport.serial.ig1"), log_rule_specificity("transport.serial.*"));
  EXPECT_GT(log_rule_specificity("transport.serial.*"), log_rule_specificity("transport.*"));
  EXPECT_GT(log_rule_specificity("transport.*"), log_rule_specificity("*"));
  // Bare name outranks a glob with the same literal length.
  EXPECT_GT(log_rule_specificity("abc"), log_rule_specificity("abc*"));
}

TEST(LogMatch, EmptyPatternMatchesNothing) {
  EXPECT_FALSE(log_name_matches("", ""));
  EXPECT_FALSE(log_name_matches("", "anything"));
}
