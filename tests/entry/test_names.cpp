#include <gtest/gtest.h>

#include "pychron/entry/names.hpp"

using namespace pychron::entry;

TEST(Names, PrincipalInvestigator) {
  EXPECT_EQ(*parse_pi("Ross"), (PiName{"Ross", ""}));
  EXPECT_EQ(*parse_pi("Ross, J"), (PiName{"Ross", "J"}));
  EXPECT_EQ(*parse_pi(" Ross,J "), (PiName{"Ross", "J"}));
  EXPECT_EQ(*parse_pi("O'Brien-Smith, K"), (PiName{"O'Brien-Smith", "K"}));
  EXPECT_FALSE(parse_pi("Jake Ross"));
  EXPECT_FALSE(parse_pi("ross"));
  EXPECT_FALSE(parse_pi("Ross, Jake"));
  EXPECT_FALSE(parse_pi("R"));
  EXPECT_FALSE(parse_pi("NMGRL lab"));
  EXPECT_EQ(*parse_pi("NMGRL lab", {"NMGRL lab"}), (PiName{"NMGRL lab", ""}));
  EXPECT_EQ(display_name({"Ross", "J"}), "Ross, J");
  EXPECT_EQ(display_name({"Ross", ""}), "Ross");
}

TEST(Names, Project) {
  EXPECT_TRUE(valid_project_name("IR-1010_a"));
  EXPECT_TRUE(valid_project_name("a"));
  EXPECT_FALSE(valid_project_name("1abc"));
  EXPECT_FALSE(valid_project_name("a b"));
  EXPECT_FALSE(valid_project_name(""));
}

TEST(Names, PackageIncrement) {
  EXPECT_EQ(next_package_name({"NM-001"}, "NM-"), "NM-002");
  EXPECT_EQ(next_package_name({"NM-299", "NM-300", "NM-298"}, "NM-"), "NM-301");
  EXPECT_EQ(next_package_name({"NM-999"}, "NM-"), "NM-1000");
  EXPECT_EQ(next_package_name({"NM001"}, "NM"), "NM002");
  EXPECT_EQ(next_package_name({"NM-ABC-001", "NM-002"}, "NM-ABC-"), "NM-ABC-002");
  EXPECT_EQ(next_package_name({"NM-1", "NM-x7"}, "NM-"), "NM-002");
  EXPECT_EQ(next_package_name({"Other-5"}, "NM-"), "NM-001");
  EXPECT_EQ(next_package_name({}, "NM-"), "NM-001");
  EXPECT_TRUE(valid_package_name("NM-300"));
  EXPECT_FALSE(valid_package_name("NM 300"));
  EXPECT_FALSE(valid_package_name(""));
}

TEST(Names, LevelLetters) {
  EXPECT_EQ(next_letters("A"), "B");
  EXPECT_EQ(next_letters("Z"), "AA");
  EXPECT_EQ(next_letters("AZ"), "BA");
  EXPECT_EQ(next_letters("ZZ"), "AAA");
  EXPECT_EQ(next_level_name({"A", "C", "B"}), "D");
  EXPECT_EQ(next_level_name({"Z", "AA"}), "AB");
  EXPECT_EQ(next_level_name({}), "A");
  EXPECT_EQ(next_level_name({"top"}), "A");
}

TEST(Names, Packets) {
  EXPECT_TRUE(valid_packet("P7"));
  EXPECT_TRUE(valid_packet("12"));
  EXPECT_FALSE(valid_packet("P"));
  EXPECT_FALSE(valid_packet("7P"));
  EXPECT_FALSE(valid_packet(""));
  EXPECT_EQ(next_packet("P7"), "P8");
  EXPECT_EQ(next_packet("9"), "10");
  EXPECT_EQ(next_packet("P09"), "P10");
  EXPECT_EQ(next_packet("P99"), "P100");
  EXPECT_FALSE(next_packet("x"));
}
