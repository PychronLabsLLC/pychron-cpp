// The members of a release at their pins, for a destination that wants the
// analyses rather than a directory (publication workflow design, section 5.2).

#include <gtest/gtest.h>

#include <algorithm>
#include <set>

#include "pychron/processing/store_source.hpp"
#include "pychron/publish/build.hpp"
#include "pychron/publish/earthbank_source.hpp"
#include "pychron/publish/release.hpp"
#include "release_fixture.hpp"

namespace ps = pychron::persistence;
namespace pp = pychron::processing;
using namespace pychron;
using namespace pychron::publish;
using pychron::publish::testing::ReleaseFixture;

namespace {

class ReleaseDatasetTest : public ReleaseFixture {
 protected:
  void SetUp() override {
    ReleaseFixture::SetUp();
    auto source =
        pp::StoreSource::open(ps::StoreConfig{db_->url(), false}, pychron::publish::testing::source_options());
    ASSERT_TRUE(source) << to_string(source.error());
    source_ = std::move(*source);
  }
  void TearDown() override {
    source_.reset();
    ReleaseFixture::TearDown();
  }

  Uuid create() {
    ReleaseSpec s;
    s.name = "r";
    s.title = "Release r";
    s.description = "Three unknowns";
    s.authors = {{"J. Ross", "0000-0002-1825-0097", "NMT"}};
    s.analyses = analysis_texts();
    s.options_json = release_options_json(ReleaseOptions{});
    s.reduction_version = "arar-1";
    s.software_version = "pychron-cpp/0.4.0";
    auto created = create_release(*store_, reducer(), s);
    EXPECT_TRUE(created) << (created ? "" : to_string(created.error()));
    return created ? created->release : Uuid{};
  }

  std::unique_ptr<pp::StoreSource> source_;
};

}  // namespace

TEST_P(ReleaseDatasetTest, LoadsEveryMemberOnce) {
  auto loaded = release_dataset(*store_, *source_, create());
  ASSERT_TRUE(loaded) << to_string(loaded.error());
  ASSERT_EQ(loaded->size(), 3u);
  std::set<std::string> got;
  for (const auto& a : *loaded) got.insert(a->uuid);
  EXPECT_EQ(got, (std::set<std::string>{analysis1_.str(), analysis2_.str(), analysis3_.str()}));
}

TEST_P(ReleaseDatasetTest, KeepsPinnedRevisionAfterLaterEdit) {
  const Uuid release = create();
  auto before = release_dataset(*store_, *source_, release);
  ASSERT_TRUE(before) << to_string(before.error());
  const auto pick = [&](const std::vector<pp::AnalysisPtr>& v) {
    return *std::find_if(v.begin(), v.end(), [&](const pp::AnalysisPtr& a) { return a->uuid == analysis1_.str(); });
  };
  const auto intercept_of = [](const pp::AnalysisPtr& a, const std::string& isotope) {
    const auto* iso = a->find_by_isotope(isotope);
    return iso ? iso->intercept.value : -1.0;
  };

  const auto head = store_->head(analysis1_, ps::Kind::Intercepts);
  ASSERT_TRUE(head && *head);
  auto payload = store_->load_payload(**head);
  ASSERT_TRUE(payload && *payload);
  auto rows = std::get<ps::Intercepts>(**payload);
  ASSERT_FALSE(rows.empty());
  const std::string isotope = rows.front().isotope;
  const double pinned = intercept_of(pick(*before), isotope);
  ASSERT_EQ(pinned, rows.front().value.value_or(-2.0));
  rows.front().value = pinned + 1000.0;
  auto uow = *store_->begin(reducer());
  ASSERT_TRUE(uow->add_revision(analysis1_, ps::Kind::Intercepts, ps::RevisionPayload{rows}, *head));
  auto outcome = uow->commit(ps::ChangesetKind::Reduction, "edit");
  ASSERT_TRUE(outcome && std::holds_alternative<ps::Committed>(*outcome));

  auto after = release_dataset(*store_, *source_, release);
  ASSERT_TRUE(after) << to_string(after.error());
  EXPECT_EQ(intercept_of(pick(*after), isotope), pinned);
  auto head_now = source_->load(analysis1_.str());
  ASSERT_TRUE(head_now) << to_string(head_now.error());
  EXPECT_EQ(intercept_of(*head_now, isotope), pinned + 1000.0);
}

TEST_P(ReleaseDatasetTest, ReturnsMembersInMemberOrder) {
  const Uuid release = create();
  std::vector<std::string> expected{analysis1_.str(), analysis2_.str(), analysis3_.str()};
  std::sort(expected.begin(), expected.end());
  auto loaded = release_dataset(*store_, *source_, release);
  ASSERT_TRUE(loaded) << to_string(loaded.error());
  std::vector<std::string> got;
  for (const auto& a : *loaded) got.push_back(a->uuid);
  EXPECT_EQ(got, expected);
}

TEST_P(ReleaseDatasetTest, KeepsEmbargoedMembers) {
  ASSERT_TRUE(store_->add_embargo(reducer(), {{ps::EmbargoScopeType::Sample, sample1_}, std::nullopt, "paper"}));
  auto loaded = release_dataset(*store_, *source_, create());
  ASSERT_TRUE(loaded) << to_string(loaded.error());
  EXPECT_EQ(loaded->size(), 3u);
}

TEST_P(ReleaseDatasetTest, UnknownReleaseIsConfig) {
  auto loaded = release_dataset(*store_, *source_, ps::UuidV7Generator().next());
  ASSERT_FALSE(loaded);
  EXPECT_EQ(loaded.error().kind, ErrorKind::Config);
}

INSTANTIATE_TEST_SUITE_P(Engines, ReleaseDatasetTest, ::testing::ValuesIn(ps::testing::engines()),
                         [](const ::testing::TestParamInfo<std::string>& i) { return i.param; });
