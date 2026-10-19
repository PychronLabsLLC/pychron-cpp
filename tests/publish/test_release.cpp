// Creating, publishing and withdrawing a release (data publishing design,
// sections 4.3 and 4.4).

#include "pychron/publish/release.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <set>

#include "release_fixture.hpp"

namespace ps = pychron::persistence;
using namespace pychron;
using namespace pychron::publish;
using pychron::publish::testing::ReleaseFixture;

namespace {

class ReleaseTest : public ReleaseFixture {
 protected:
  ReleaseSpec spec(const std::string& name = "r") const {
    ReleaseSpec s;
    s.name = name;
    s.title = "Release " + name;
    s.authors = {{"J. Ross", "0000-0002-1825-0097", "NMT"}};
    s.analyses = analysis_texts();
    s.options_json = "{}";
    s.reduction_version = "arar-1";
    s.software_version = "pychron-cpp/0.4.0";
    return s;
  }
};

}  // namespace

TEST_P(ReleaseTest, CreatePinsEveryHeadAndRef) {
  auto created = create_release(*store_, reducer(), spec());
  ASSERT_TRUE(created) << to_string(created.error());
  EXPECT_EQ(created->members, 3u);
  EXPECT_EQ(created->samples, 3u);
  EXPECT_EQ(created->version, 1);
  auto row = store_->release(created->release);
  ASSERT_TRUE(row && *row);
  EXPECT_EQ((*row)->state, ps::ReleaseState::Draft);
  EXPECT_EQ((*row)->visibility, ps::Visibility::Private);
  EXPECT_EQ((*row)->licence, "CC-BY-4.0");
  EXPECT_NE((*row)->authors_json.find("0000-0002-1825-0097"), std::string::npos);
  auto pins = store_->bookmark_heads((*row)->bookmark);
  ASSERT_TRUE(pins);
  std::set<ps::Uuid> subjects;
  std::size_t values = 0;
  for (const auto& p : *pins) {
    subjects.insert(p.subject);
    if (p.kind == ps::Kind::RefValue) ++values;
  }
  for (const auto& a : analyses()) EXPECT_TRUE(subjects.contains(a));
  // 6 collection kinds per member; the two positioned members resolve flux,
  // level production, production and chronology (the production once each).
  EXPECT_EQ(pins->size() - values, 3u * 6u);
  EXPECT_EQ(values, 2u + 1u + 1u + 1u) << "flux x2, level production, production, chronology";
  // Loading as of the bookmark works for every member.
  for (const auto& a : analyses()) {
    auto view = store_->load_analysis_at(a, (*row)->bookmark);
    ASSERT_TRUE(view && *view) << a.str();
    EXPECT_EQ((*view)->heads.size(), 6u);
  }
}

TEST_P(ReleaseTest, TagFilterLeavesOutInvalidByDefault) {
  retag(analysis2_, "invalid");
  auto created = create_release(*store_, reducer(), spec());
  ASSERT_TRUE(created) << to_string(created.error());
  EXPECT_EQ(created->members, 2u);
  EXPECT_EQ(created->samples, 2u);
  auto samples = store_->release_samples(created->release);
  ASSERT_TRUE(samples);
  EXPECT_EQ(std::count(samples->begin(), samples->end(), sample2_), 0);

  ReleaseSpec only_ok = spec("ok-only");
  retag(analysis3_, "omit");
  only_ok.tags = {"ok"};
  auto ok = create_release(*store_, reducer(), only_ok);
  ASSERT_TRUE(ok);
  EXPECT_EQ(ok->members, 1u);

  ReleaseSpec none = spec("none");
  none.tags = {"nothing"};
  auto refused = create_release(*store_, reducer(), none);
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().kind, ErrorKind::Config);
}

TEST_P(ReleaseTest, RetagAfterCreationDoesNotChangeMembersOrPins) {
  auto created = create_release(*store_, reducer(), spec());
  ASSERT_TRUE(created);
  const auto bookmark = (*store_->release(created->release))->bookmark;
  const auto before = *store_->bookmark_heads(bookmark);
  retag(analysis1_, "invalid");
  EXPECT_EQ(*store_->bookmark_heads(bookmark), before);
  EXPECT_EQ(store_->release_samples(created->release)->size(), 3u);
  auto view = store_->load_analysis_at(analysis1_, bookmark);
  ASSERT_TRUE(view && *view);
  EXPECT_EQ(std::get<ps::TagValue>((*view)->payloads.at(ps::Kind::Tags)).name, "ok");
}

TEST_P(ReleaseTest, VersionBumpsPerName) {
  auto first = create_release(*store_, reducer(), spec());
  ASSERT_TRUE(first);
  auto second = create_release(*store_, reducer(), spec());
  ASSERT_TRUE(second) << to_string(second.error());
  EXPECT_EQ(second->version, 2);
  auto row = store_->release(second->release);
  ASSERT_TRUE(row && *row);
  EXPECT_EQ((*row)->supersedes, first->release);
  ReleaseSpec explicit_super = spec();
  explicit_super.supersedes = "r@v1";
  auto third = create_release(*store_, reducer(), explicit_super);
  ASSERT_TRUE(third);
  EXPECT_EQ(third->version, 3);
  EXPECT_EQ((*store_->release(third->release))->supersedes, first->release);
}

TEST_P(ReleaseTest, EmbargoedCountsAreReported) {
  ASSERT_TRUE(store_->add_embargo(reducer(), {{ps::EmbargoScopeType::Sample, sample1_}, std::nullopt, "paper"}));
  auto created = create_release(*store_, reducer(), spec());
  ASSERT_TRUE(created);
  EXPECT_EQ(created->members, 3u) << "an embargo hides, it does not exclude";
  EXPECT_EQ(created->embargoed_samples, 1u);
  EXPECT_EQ(created->embargoed_analyses, 1u);
}

TEST_P(ReleaseTest, InterpretedAgesWhoseMembersAreAllInTheReleaseAreCarried) {
  const auto make_ia = [&](const std::string& name, const std::vector<ps::Uuid>& members) {
    auto ia = *store_->add_interpreted_age(lab_.reduction_client, {name});
    auto uow = *store_->begin(reducer());
    ps::InterpretedAgeValue v;
    v.age = 28.2;
    v.age_kind = "Plateau";
    for (const auto& m : members) v.members.push_back({m, std::nullopt, std::nullopt, std::nullopt});
    EXPECT_TRUE(uow->add_revision(ia, ps::Kind::InterpretedAge, v, std::nullopt));
    auto outcome = uow->commit(ps::ChangesetKind::Reduction, "ia");
    EXPECT_TRUE(outcome && std::holds_alternative<ps::Committed>(*outcome));
    return ia;
  };
  const ps::Uuid outside = ingest("70001", 2, "2026-10-03T10:00:00Z");  // not in the release's selection
  const ps::Uuid inside = make_ia("FC-1 plateau", {analysis1_});
  make_ia("mixed", {analysis1_, outside});
  auto created = create_release(*store_, reducer(), spec());
  ASSERT_TRUE(created);
  EXPECT_EQ(created->interpreted_ages, 1u);
  auto ias = store_->release_interpreted_ages(created->release);
  ASSERT_TRUE(ias);
  ASSERT_EQ(ias->size(), 1u);
  EXPECT_EQ((*ias)[0].ia, inside);
  EXPECT_EQ((*ias)[0].revision, **store_->head(inside, ps::Kind::InterpretedAge));

  ReleaseSpec none = spec("none");
  none.interpreted_ages = std::vector<std::string>{};
  EXPECT_EQ(create_release(*store_, reducer(), none)->interpreted_ages, 0u);
  ReleaseSpec named = spec("named");
  named.interpreted_ages = std::vector<std::string>{"mixed"};
  auto by_name = create_release(*store_, reducer(), named);
  ASSERT_TRUE(by_name);
  EXPECT_EQ(by_name->interpreted_ages, 1u);
}

TEST_P(ReleaseTest, PublishThenWithdraw) {
  auto created = create_release(*store_, reducer(), spec());
  ASSERT_TRUE(created);
  const Sha256Digest manifest = sha256(std::string_view{"manifest"});
  ASSERT_TRUE(publish_release(*store_, reducer(), created->release, manifest, false));
  auto row = store_->release(created->release);
  ASSERT_TRUE(row && *row);
  EXPECT_EQ((*row)->state, ps::ReleaseState::Published);
  EXPECT_EQ((*row)->visibility, ps::Visibility::Private);
  EXPECT_TRUE((*row)->published);
  EXPECT_EQ((*row)->manifest_sha256, manifest);
  ASSERT_TRUE(set_release_public(*store_, reducer(), created->release));
  EXPECT_EQ((*store_->release(created->release))->visibility, ps::Visibility::Public);
  auto no_reason = withdraw_release(*store_, reducer(), created->release, "");
  ASSERT_FALSE(no_reason);
  EXPECT_EQ(no_reason.error().kind, ErrorKind::Config);
  ASSERT_TRUE(withdraw_release(*store_, reducer(), created->release, "wrong J"));
  row = store_->release(created->release);
  EXPECT_EQ((*row)->state, ps::ReleaseState::Withdrawn);
  EXPECT_EQ((*row)->visibility, ps::Visibility::Private);
  EXPECT_EQ((*row)->withdrawn_reason, "wrong J");
  EXPECT_FALSE(delete_release(*store_, reducer(), created->release));
}

TEST_P(ReleaseTest, DeleteADraft) {
  auto created = create_release(*store_, reducer(), spec());
  ASSERT_TRUE(created);
  ASSERT_TRUE(delete_release(*store_, reducer(), created->release));
  EXPECT_FALSE(*store_->release(created->release));
}

TEST_P(ReleaseTest, FindReleaseByNameVersionAndUuid) {
  auto first = create_release(*store_, reducer(), spec());
  auto second = create_release(*store_, reducer(), spec());
  ASSERT_TRUE(first && second);
  EXPECT_EQ(find_release(*store_, "r")->uuid, second->release);
  EXPECT_EQ(find_release(*store_, "r@v1")->uuid, first->release);
  EXPECT_EQ(find_release(*store_, "r@v2")->uuid, second->release);
  EXPECT_EQ(find_release(*store_, first->release.str())->uuid, first->release);
  auto none = find_release(*store_, "s");
  ASSERT_FALSE(none);
  EXPECT_EQ(none.error().kind, ErrorKind::Config);
  EXPECT_FALSE(find_release(*store_, "r@v9"));
}

TEST(Authors, JsonRoundTrip) {
  const std::vector<Author> authors{{"J. Ross", "0000-0002-1825-0097", "NMT"}, {"A. N. Other", std::nullopt, std::nullopt}};
  const std::string json = authors_json(authors);
  EXPECT_EQ(json, R"([{"name":"J. Ross","orcid":"0000-0002-1825-0097","affiliation":"NMT"},{"name":"A. N. Other"}])");
  auto back = parse_authors(json);
  ASSERT_TRUE(back);
  EXPECT_EQ(*back, authors);
  EXPECT_FALSE(parse_authors("nope"));
}

INSTANTIATE_TEST_SUITE_P(Engines, ReleaseTest, ::testing::ValuesIn(ps::testing::engines()),
                         [](const ::testing::TestParamInfo<std::string>& i) { return i.param; });
