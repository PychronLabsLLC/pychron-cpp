#include <gtest/gtest.h>

#include "pychron/experiment/model/identifiers.hpp"

using namespace pychron::experiment;

TEST(Identifiers, DefaultsClassifyPychronPrefixes) {
  auto ids = IdentifierRules::defaults();
  EXPECT_EQ(ids.classify("u"), AnalysisType::Unknown);
  EXPECT_EQ(ids.classify("b"), AnalysisType::BlankUnknown);
  EXPECT_EQ(ids.classify("bu"), AnalysisType::BlankUnknown);
  EXPECT_EQ(ids.classify("ba"), AnalysisType::BlankAir);
  EXPECT_EQ(ids.classify("bc"), AnalysisType::BlankCocktail);
  EXPECT_EQ(ids.classify("be"), AnalysisType::BlankExtractionLine);
  EXPECT_EQ(ids.classify("bg"), AnalysisType::Background);
  EXPECT_EQ(ids.classify("c"), AnalysisType::Cocktail);
  EXPECT_EQ(ids.classify("a"), AnalysisType::Air);
  EXPECT_EQ(ids.classify("pa"), AnalysisType::Pause);
  EXPECT_EQ(ids.classify("dg"), AnalysisType::Degas);
  EXPECT_EQ(ids.classify("IC"), AnalysisType::DetectorIC);
  EXPECT_EQ(ids.classify("20001"), AnalysisType::Unknown);
}

TEST(Identifiers, ValidatesIdentifierAndStep) {
  auto ids = IdentifierRules::defaults();
  EXPECT_TRUE(ids.validate_identifier("bu"));
  EXPECT_TRUE(ids.validate_identifier("NM-205"));
  EXPECT_FALSE(ids.validate_identifier(""));
  EXPECT_FALSE(ids.validate_identifier("bad id"));
  EXPECT_TRUE(ids.valid_step(""));
  EXPECT_TRUE(ids.valid_step("A"));
  EXPECT_FALSE(ids.valid_step("1"));
}

TEST(Identifiers, TomlOverridesPrefixes) {
  auto ids = IdentifierRules::from_toml(R"(
[prefixes]
unknown = "u"
blank_unknown = "blk"
air = "air"
[patterns]
unknown = "^[0-9]+$"
)");
  ASSERT_TRUE(ids);
  EXPECT_EQ(ids->classify("blk"), AnalysisType::BlankUnknown);
  EXPECT_EQ(ids->classify("bu"), AnalysisType::Unknown);
  EXPECT_EQ(ids->prefix_for(AnalysisType::Air), "air");
  EXPECT_TRUE(ids->validate_identifier("123"));
  EXPECT_FALSE(ids->validate_identifier("abc"));
}

TEST(Identifiers, TomlErrors) {
  EXPECT_FALSE(IdentifierRules::from_toml("[prefixes]\nbogus = \"x\"\n"));
  EXPECT_FALSE(IdentifierRules::from_toml("[prefixes]\nair = \"x\"\nunknown = \"x\"\n"));
  EXPECT_FALSE(IdentifierRules::from_toml("[patterns]\nunknown = \"[\"\n[prefixes]\nair=\"a\"\n"));
  EXPECT_FALSE(IdentifierRules::from_toml("not toml ="));
  EXPECT_FALSE(IdentifierRules::load("/nonexistent/identifiers.toml"));
}

TEST(Identifiers, TypeNamesRoundTrip) {
  for (auto t : {AnalysisType::Unknown, AnalysisType::BlankExtractionLine, AnalysisType::DetectorIC}) {
    EXPECT_EQ(parse_analysis_type(to_string(t)), t);
  }
  EXPECT_FALSE(parse_analysis_type("nope"));
}
