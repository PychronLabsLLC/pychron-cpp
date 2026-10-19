// Embargoes by name (data publishing design, section 4.2).

#include "pychron/publish/embargo.hpp"

#include <gtest/gtest.h>

#include "release_fixture.hpp"

namespace ps = pychron::persistence;
using namespace pychron;
using namespace pychron::publish;
using pychron::publish::testing::ReleaseFixture;

namespace {

class EmbargoTest : public ReleaseFixture {
 protected:
  static UtcTime at(const char* iso) { return *UtcTime::parse(iso); }
};

}  // namespace

TEST(RunId, ParsesIdentifierAliquotAndStep) {
  EXPECT_EQ(parse_runid("70001-01"), (RunId{"70001", 1, -1}));
  EXPECT_EQ(parse_runid("70001-12A"), (RunId{"70001", 12, 0}));
  EXPECT_EQ(parse_runid("70001-01Z"), (RunId{"70001", 1, 25}));
  EXPECT_EQ(parse_runid("70001-01AA"), (RunId{"70001", 1, 26}));
  EXPECT_EQ(parse_runid("bu-FD-J-03"), (RunId{"bu-FD-J", 3, -1}));
  EXPECT_FALSE(parse_runid("70001"));
  EXPECT_FALSE(parse_runid("70001-"));
  EXPECT_FALSE(parse_runid("70001-x"));
  EXPECT_FALSE(parse_runid("-01"));
}

TEST_P(EmbargoTest, SetBySampleNameRefusesAmbiguity) {
  ASSERT_TRUE(store_->add_sample(lab_.reduction_client, {"FC-1", project2_, material_}));  // a second FC-1, in Beta
  auto ambiguous = set_embargo(*store_, reducer(), {ps::EmbargoScopeType::Sample, "FC-1", std::nullopt, std::nullopt});
  ASSERT_FALSE(ambiguous);
  EXPECT_EQ(ambiguous.error().kind, ErrorKind::Config);
  EXPECT_NE(ambiguous.error().what.find("Alpha"), std::string::npos) << ambiguous.error().what;
  EXPECT_NE(ambiguous.error().what.find("Beta"), std::string::npos) << ambiguous.error().what;
  auto qualified = set_embargo(*store_, reducer(), {ps::EmbargoScopeType::Sample, "FC-1@Alpha", std::nullopt, "paper"});
  ASSERT_TRUE(qualified) << to_string(qualified.error());
  const std::vector<ps::Uuid> samples{sample1_, sample2_, sample3_};
  auto hidden = store_->embargoed_samples(samples, at("2026-10-18T00:00:00Z"));
  ASSERT_TRUE(hidden);
  EXPECT_EQ(*hidden, std::vector<ps::Uuid>{sample1_});
  auto unknown = set_embargo(*store_, reducer(), {ps::EmbargoScopeType::Sample, "nope", std::nullopt, std::nullopt});
  ASSERT_FALSE(unknown);
  EXPECT_EQ(unknown.error().kind, ErrorKind::Config);
  auto by_uuid = set_embargo(*store_, reducer(), {ps::EmbargoScopeType::Sample, sample2_.str(), std::nullopt, std::nullopt});
  ASSERT_TRUE(by_uuid);
  EXPECT_EQ(store_->embargoed_samples(samples, at("2026-10-18T00:00:00Z"))->size(), 2u);
}

TEST_P(EmbargoTest, SetByRunidResolvesTheAnalysis) {
  auto e = set_embargo(*store_, reducer(), {ps::EmbargoScopeType::Analysis, "70001-01", std::nullopt, std::nullopt});
  ASSERT_TRUE(e) << to_string(e.error());
  const std::vector<ps::Uuid> all = analyses();
  auto hidden = store_->embargoed_analyses(all, at("2026-10-18T00:00:00Z"));
  ASSERT_TRUE(hidden);
  EXPECT_EQ(*hidden, std::vector<ps::Uuid>{analysis1_});
  EXPECT_FALSE(set_embargo(*store_, reducer(), {ps::EmbargoScopeType::Analysis, "70001-09", std::nullopt, std::nullopt}));
  EXPECT_FALSE(set_embargo(*store_, reducer(), {ps::EmbargoScopeType::Analysis, "nonsense", std::nullopt, std::nullopt}));
  auto by_uuid = set_embargo(*store_, reducer(), {ps::EmbargoScopeType::Analysis, analysis2_.str(), std::nullopt, std::nullopt});
  ASSERT_TRUE(by_uuid);
}

TEST_P(EmbargoTest, ListNamesTheScope) {
  ASSERT_TRUE(set_embargo(*store_, reducer(), {ps::EmbargoScopeType::Project, "Alpha", at("2027-01-01T00:00:00Z"), "review"}));
  ASSERT_TRUE(set_embargo(*store_, reducer(), {ps::EmbargoScopeType::Sample, "BT-1", std::nullopt, std::nullopt}));
  ASSERT_TRUE(set_embargo(*store_, reducer(), {ps::EmbargoScopeType::Analysis, "70002-01", std::nullopt, std::nullopt}));
  auto listed = list_embargoes(*store_, true, at("2026-10-18T00:00:00Z"));
  ASSERT_TRUE(listed) << to_string(listed.error());
  ASSERT_EQ(listed->size(), 3u);
  EXPECT_EQ((*listed)[0].scope_name, "project Alpha");
  EXPECT_EQ((*listed)[0].row.reason, "review");
  EXPECT_EQ((*listed)[0].row.until, at("2027-01-01T00:00:00Z"));
  EXPECT_EQ((*listed)[1].scope_name, "sample BT-1 (Beta)");
  EXPECT_EQ((*listed)[2].scope_name, "analysis 70002-01");
  EXPECT_TRUE(list_embargoes(*store_, true, at("2027-06-01T00:00:00Z"))->size() == 2u) << "the dated one is over";
}

TEST_P(EmbargoTest, LiftTwiceIsConfig) {
  auto e = set_embargo(*store_, reducer(), {ps::EmbargoScopeType::Project, "Beta", std::nullopt, std::nullopt});
  ASSERT_TRUE(e);
  ASSERT_TRUE(lift_embargo(*store_, reducer(), e->str()));
  auto again = lift_embargo(*store_, reducer(), e->str());
  ASSERT_FALSE(again);
  EXPECT_EQ(again.error().kind, ErrorKind::Config);
  EXPECT_FALSE(lift_embargo(*store_, reducer(), "not-a-uuid"));
  EXPECT_TRUE(list_embargoes(*store_, true, at("2026-10-18T00:00:00Z"))->empty());
  EXPECT_EQ(list_embargoes(*store_, false, at("2026-10-18T00:00:00Z"))->size(), 1u);
}

TEST_P(EmbargoTest, LiftByScopeLiftsEveryLiveOne) {
  ASSERT_TRUE(set_embargo(*store_, reducer(), {ps::EmbargoScopeType::Project, "Alpha", std::nullopt, "one"}));
  ASSERT_TRUE(set_embargo(*store_, reducer(), {ps::EmbargoScopeType::Project, "Alpha", at("2027-01-01T00:00:00Z"), "two"}));
  ASSERT_TRUE(set_embargo(*store_, reducer(), {ps::EmbargoScopeType::Project, "Beta", std::nullopt, "other"}));
  auto lifted = lift_embargoes_on(*store_, reducer(), ps::EmbargoScopeType::Project, "Alpha");
  ASSERT_TRUE(lifted) << to_string(lifted.error());
  EXPECT_EQ(*lifted, 2u);
  auto live = list_embargoes(*store_, true, at("2026-10-18T00:00:00Z"));
  ASSERT_TRUE(live);
  ASSERT_EQ(live->size(), 1u);
  EXPECT_EQ((*live)[0].scope_name, "project Beta");
  EXPECT_EQ(*lift_embargoes_on(*store_, reducer(), ps::EmbargoScopeType::Project, "Alpha"), 0u);
}

INSTANTIATE_TEST_SUITE_P(Engines, EmbargoTest, ::testing::ValuesIn(ps::testing::engines()),
                         [](const ::testing::TestParamInfo<std::string>& i) { return i.param; });
