// Releases, embargoes and external identifiers (data publishing spec,
// sections 4.2 to 4.5), and loading an analysis as of a bookmark.

#include <gtest/gtest.h>

#include <algorithm>

#include "pychron/persistence/publish.hpp"
#include "store_fixture.hpp"

using namespace pychron;
using namespace pychron::persistence;
using namespace pychron::persistence::testing;

namespace {

UtcTime at(const char* iso) { return *UtcTime::parse(iso); }

// Two samples of one project and one of another, each with one analysis; a
// group and a bookmark over all three, as a release pins them.
class PublishStoreTest : public StoreTest {
 protected:
  void SetUp() override {
    StoreTest::SetUp();
    const Uuid c = lab_.reduction_client;
    project_ = *store_->add_project(c, {"Alpha"});
    project2_ = *store_->add_project(c, {"Beta"});
    material_ = *store_->add_material(c, {"sanidine"});
    sample1_ = *store_->add_sample(c, {"FC-1", project_, material_});
    sample2_ = *store_->add_sample(c, {"FC-2", project_, material_});
    sample3_ = *store_->add_sample(c, {"BT-1", project2_, material_});
    analysis1_ = ingest("70001", sample1_);
    analysis2_ = ingest("70002", sample2_);
    analysis3_ = ingest("70003", sample3_);
    group_ = *store_->create_group(reducer(), "release:r@v1", {analysis1_, analysis2_, analysis3_});
    bookmark_ = *store_->create_bookmark(reducer(), BookmarkSpec{"release r v1", std::nullopt, std::nullopt, group_});
  }

  Actor reducer() const { return Actor{lab_.reducer, lab_.reduction_client}; }

  Uuid ingest(const std::string& identifier, Uuid sample) {
    IdentifierSpec spec;
    spec.identifier = identifier;
    spec.sample = sample;
    EXPECT_TRUE(store_->add_identifier(lab_.acquisition_client, spec));
    const auto item = analysis_item(lab_, 1, series(1), series(0), identifier);
    auto ack = store_->ingest(item);
    EXPECT_TRUE(ack) << (ack ? "" : to_string(ack.error()));
    return std::get<AnalysisIngest>(item.body).analysis;
  }

  ReleaseInsert insert(const std::string& name, int version = 1) const {
    ReleaseInsert r;
    r.uuid = Uuid::v7();
    r.name = name;
    r.title = "Release " + name;
    r.version = version;
    r.group = group_;
    r.bookmark = bookmark_;
    r.licence = "CC-BY-4.0";
    r.authors_json = R"([{"name": "J. Ross"}])";
    r.options_json = "{}";
    r.reduction_version = "arar-1";
    r.software_version = "pychron-cpp/0.4.0";
    r.samples = {sample1_, sample2_, sample3_};
    return r;
  }

  Uuid project_, project2_, material_, sample1_, sample2_, sample3_, analysis1_, analysis2_, analysis3_, group_,
      bookmark_;
};

}  // namespace

TEST_P(PublishStoreTest, AddReleaseWritesRowSamplesAndInterpretedAges) {
  const ChangeSeq before = *store_->latest_change_seq();
  auto spec = insert("r");
  auto ia = store_->add_interpreted_age(lab_.reduction_client, {"FC-1 plateau"});
  ASSERT_TRUE(ia);
  spec.interpreted_ages = {{*ia, **store_->head(analysis1_, Kind::Tags)}};
  auto id = store_->add_release(reducer(), spec);
  ASSERT_TRUE(id) << to_string(id.error());
  EXPECT_EQ(*id, spec.uuid);
  auto row = store_->release(*id);
  ASSERT_TRUE(row && *row);
  EXPECT_EQ((*row)->name, "r");
  EXPECT_EQ((*row)->title, "Release r");
  EXPECT_EQ((*row)->state, ReleaseState::Draft);
  EXPECT_EQ((*row)->visibility, Visibility::Private);
  EXPECT_EQ((*row)->licence, "CC-BY-4.0");
  EXPECT_EQ((*row)->bookmark, bookmark_);
  EXPECT_FALSE((*row)->published);
  auto samples = store_->release_samples(*id);
  ASSERT_TRUE(samples);
  EXPECT_EQ(samples->size(), 3u);
  auto ias = store_->release_interpreted_ages(*id);
  ASSERT_TRUE(ias);
  ASSERT_EQ(ias->size(), 1u);
  EXPECT_EQ((*ias)[0].ia, *ia);
  EXPECT_EQ(*store_->latest_change_seq(), before + 2) << "the interpreted age and the release";
}

TEST_P(PublishStoreTest, FindReleaseGivesNewestVersion) {
  ASSERT_TRUE(store_->add_release(reducer(), insert("r", 1)));
  auto v2 = insert("r", 2);
  ASSERT_TRUE(store_->add_release(reducer(), v2));
  auto newest = store_->find_release("r", std::nullopt);
  ASSERT_TRUE(newest && *newest);
  EXPECT_EQ((*newest)->version, 2);
  EXPECT_EQ((*newest)->uuid, v2.uuid);
  auto first = store_->find_release("r", 1);
  ASSERT_TRUE(first && *first);
  EXPECT_EQ((*first)->version, 1);
  auto none = store_->find_release("s", std::nullopt);
  ASSERT_TRUE(none);
  EXPECT_FALSE(*none);
  auto all = store_->releases({});
  ASSERT_TRUE(all);
  EXPECT_EQ(all->size(), 2u);
  EXPECT_EQ((*all)[0].version, 1);
  auto drafts = store_->releases({ReleaseState::Draft, "r"});
  ASSERT_TRUE(drafts);
  EXPECT_EQ(drafts->size(), 2u);
}

TEST_P(PublishStoreTest, DraftToPublishedSetsPublishedUtc) {
  const auto spec = insert("r");
  ASSERT_TRUE(store_->add_release(reducer(), spec));
  ReleaseUpdate u;
  u.release = spec.uuid;
  u.state = ReleaseState::Published;
  u.manifest_sha256 = sha256(std::string_view{"manifest"});
  ASSERT_TRUE(store_->update_release(reducer(), u));
  auto row = store_->release(spec.uuid);
  ASSERT_TRUE(row && *row);
  EXPECT_EQ((*row)->state, ReleaseState::Published);
  EXPECT_TRUE((*row)->published);
  EXPECT_EQ((*row)->visibility, Visibility::Private);
  ASSERT_TRUE((*row)->manifest_sha256);
  EXPECT_EQ(*(*row)->manifest_sha256, sha256(std::string_view{"manifest"}));
}

TEST_P(PublishStoreTest, PublishedCannotGoPrivateOrChangeTitle) {
  const auto spec = insert("r");
  ASSERT_TRUE(store_->add_release(reducer(), spec));
  ReleaseUpdate publish;
  publish.release = spec.uuid;
  publish.state = ReleaseState::Published;
  publish.visibility = Visibility::Public;
  ASSERT_TRUE(store_->update_release(reducer(), publish));
  ReleaseUpdate hide;
  hide.release = spec.uuid;
  hide.visibility = Visibility::Private;
  auto refused = store_->update_release(reducer(), hide);
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().kind, ErrorKind::Config);
  ReleaseUpdate draft_public;
  draft_public.release = spec.uuid;
  draft_public.state = ReleaseState::Draft;
  EXPECT_FALSE(store_->update_release(reducer(), draft_public));
}

TEST_P(PublishStoreTest, DraftVisibilityCannotChange) {
  const auto spec = insert("r");
  ASSERT_TRUE(store_->add_release(reducer(), spec));
  ReleaseUpdate u;
  u.release = spec.uuid;
  u.visibility = Visibility::Public;
  auto refused = store_->update_release(reducer(), u);
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().kind, ErrorKind::Config);
}

TEST_P(PublishStoreTest, WithdrawNeedsAReason) {
  const auto spec = insert("r");
  ASSERT_TRUE(store_->add_release(reducer(), spec));
  ReleaseUpdate publish;
  publish.release = spec.uuid;
  publish.state = ReleaseState::Published;
  publish.visibility = Visibility::Public;
  ASSERT_TRUE(store_->update_release(reducer(), publish));
  ReleaseUpdate withdraw;
  withdraw.release = spec.uuid;
  withdraw.state = ReleaseState::Withdrawn;
  EXPECT_FALSE(store_->update_release(reducer(), withdraw));
  withdraw.withdrawn_reason = "wrong J";
  withdraw.visibility = Visibility::Private;
  ASSERT_TRUE(store_->update_release(reducer(), withdraw));
  auto row = store_->release(spec.uuid);
  ASSERT_TRUE(row && *row);
  EXPECT_EQ((*row)->state, ReleaseState::Withdrawn);
  EXPECT_EQ((*row)->visibility, Visibility::Private);
  EXPECT_TRUE((*row)->withdrawn);
  EXPECT_EQ((*row)->withdrawn_reason, "wrong J");
  ReleaseUpdate again;
  again.release = spec.uuid;
  again.doi = "10.1/x";
  EXPECT_FALSE(store_->update_release(reducer(), again)) << "a withdrawn release never changes";
}

TEST_P(PublishStoreTest, DeleteOnlyADraft) {
  const auto spec = insert("r");
  ASSERT_TRUE(store_->add_release(reducer(), spec));
  ASSERT_TRUE(store_->delete_release(reducer(), spec.uuid));
  auto gone = store_->release(spec.uuid);
  ASSERT_TRUE(gone);
  EXPECT_FALSE(*gone);
  EXPECT_TRUE(store_->release_samples(spec.uuid)->empty());
  const auto published = insert("p");
  ASSERT_TRUE(store_->add_release(reducer(), published));
  ReleaseUpdate u;
  u.release = published.uuid;
  u.state = ReleaseState::Published;
  ASSERT_TRUE(store_->update_release(reducer(), u));
  auto refused = store_->delete_release(reducer(), published.uuid);
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().kind, ErrorKind::Config);
}

TEST_P(PublishStoreTest, EmbargoOnProjectCoversItsSamplesAndAnalyses) {
  const ChangeSeq before = *store_->latest_change_seq();
  auto e = store_->add_embargo(reducer(), {{EmbargoScopeType::Project, project_}, std::nullopt, "paper in review"});
  ASSERT_TRUE(e) << to_string(e.error());
  EXPECT_EQ(*store_->latest_change_seq(), before + 1);
  const UtcTime now = at("2026-10-18T12:00:00Z");
  const std::vector<Uuid> samples{sample1_, sample2_, sample3_};
  auto es = store_->embargoed_samples(samples, now);
  ASSERT_TRUE(es);
  EXPECT_EQ(*es, (std::vector<Uuid>{std::min(sample1_, sample2_), std::max(sample1_, sample2_)}));
  const std::vector<Uuid> analyses{analysis1_, analysis2_, analysis3_};
  auto ea = store_->embargoed_analyses(analyses, now);
  ASSERT_TRUE(ea);
  EXPECT_EQ(ea->size(), 2u);
  EXPECT_EQ(std::count(ea->begin(), ea->end(), analysis3_), 0);
  auto live = store_->embargoes(true, now);
  ASSERT_TRUE(live);
  ASSERT_EQ(live->size(), 1u);
  EXPECT_EQ((*live)[0].scope.type, EmbargoScopeType::Project);
  EXPECT_EQ((*live)[0].reason, "paper in review");
  EXPECT_FALSE((*live)[0].lifted);

  ASSERT_TRUE(store_->lift_embargo(reducer(), *e));
  EXPECT_TRUE(store_->embargoed_samples(samples, now)->empty());
  EXPECT_TRUE(store_->embargoed_analyses(analyses, now)->empty());
  EXPECT_TRUE(store_->embargoes(true, now)->empty());
  EXPECT_EQ(store_->embargoes(false, now)->size(), 1u);
  auto twice = store_->lift_embargo(reducer(), *e);
  ASSERT_FALSE(twice);
  EXPECT_EQ(twice.error().kind, ErrorKind::Config);
}

TEST_P(PublishStoreTest, EmbargoOnAnalysisCoversOnlyIt) {
  ASSERT_TRUE(store_->add_embargo(reducer(), {{EmbargoScopeType::Analysis, analysis1_}, std::nullopt, std::nullopt}));
  const UtcTime now = at("2026-10-18T12:00:00Z");
  const std::vector<Uuid> samples{sample1_, sample2_, sample3_}, analyses{analysis1_, analysis2_, analysis3_};
  EXPECT_TRUE(store_->embargoed_samples(samples, now)->empty());
  auto ea = store_->embargoed_analyses(analyses, now);
  ASSERT_TRUE(ea);
  EXPECT_EQ(*ea, std::vector<Uuid>{analysis1_});
}

TEST_P(PublishStoreTest, EmbargoExpiresAtUntil) {
  const UtcTime until = at("2027-01-01T00:00:00Z");
  ASSERT_TRUE(store_->add_embargo(reducer(), {{EmbargoScopeType::Sample, sample1_}, until, std::nullopt}));
  const std::vector<Uuid> one{sample1_};
  EXPECT_EQ(store_->embargoed_samples(one, at("2026-12-31T23:59:59Z"))->size(), 1u);
  EXPECT_TRUE(store_->embargoed_samples(one, until)->empty());
  EXPECT_EQ(store_->embargoes(true, at("2026-12-31T23:59:59Z"))->size(), 1u);
  EXPECT_TRUE(store_->embargoes(true, until)->empty());
}

TEST_P(PublishStoreTest, ExternalIdUniquePerSchemeAndValue) {
  const auto spec = insert("r");
  ASSERT_TRUE(store_->add_release(reducer(), spec));
  ExternalIdInsert id;
  id.entity_type = "release";
  id.entity = spec.uuid;
  id.scheme = "doi";
  id.value = "10.1234/abcd";
  id.registrar = "datacite-test";
  id.state = "draft";
  id.request_sha256 = sha256(std::string_view{"request"});
  id.response_json = R"({"data": {"id": "10.1234/abcd"}})";
  auto first = store_->add_external_id(reducer(), id);
  ASSERT_TRUE(first) << to_string(first.error());
  auto rows = store_->external_ids("release", spec.uuid);
  ASSERT_TRUE(rows);
  ASSERT_EQ(rows->size(), 1u);
  EXPECT_EQ((*rows)[0].value, "10.1234/abcd");
  EXPECT_EQ((*rows)[0].state, "draft");
  EXPECT_EQ((*rows)[0].request_sha256, sha256(std::string_view{"request"}));
  EXPECT_FALSE(store_->add_external_id(reducer(), id)) << "same scheme and value twice";
}

INSTANTIATE_TEST_SUITE_P(Engines, PublishStoreTest, ::testing::ValuesIn(engines()),
                         [](const ::testing::TestParamInfo<std::string>& i) { return i.param; });
