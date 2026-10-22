// The plan of a Publish run: order, dependencies and the rules it is checked
// by (publication workflow design, sections 4 and 5.1).

#include "pychron/publish/publication.hpp"

#include <gtest/gtest.h>

#include "pychron/core/sha256.hpp"
#include "pychron/publish/embargo.hpp"
#include "ingest/forwarding_store.hpp"
#include "release_fixture.hpp"

namespace ps = pychron::persistence;
using namespace pychron;
using namespace pychron::publish;
using pychron::publish::testing::ReleaseFixture;

namespace {

UtcTime at(const char* iso) { return *UtcTime::parse(iso); }
const UtcTime kNow = at("2026-10-21T00:00:00Z");

class CheckPlanTest : public ReleaseFixture {
 protected:
  ReleaseSpec spec(const char* name = "r") {
    ReleaseSpec s;
    s.name = name;
    s.title = "Release";
    s.authors = {{"J. Ross", std::nullopt, std::nullopt}};
    s.analyses = analysis_texts();
    s.options_json = release_options_json(ReleaseOptions{});
    s.reduction_version = "arar-1";
    s.software_version = "pychron-cpp/0.4.0";
    return s;
  }
  Uuid create(std::optional<UtcTime> embargo_until = std::nullopt) {
    auto s = spec();
    s.embargo_until = embargo_until;
    auto created = create_release(*store_, reducer(), s);
    EXPECT_TRUE(created) << (created ? "" : to_string(created.error()));
    return created ? created->release : Uuid{};
  }
  void publish(Uuid release, bool make_public) {
    ASSERT_TRUE(publish_release(*store_, reducer(), release, sha256("x"), make_public));
  }
  PublicationPlan plan_for(const std::string& name = "r") {
    PublicationPlan p;
    p.release = ExistingRelease{name};
    return p;
  }
  PublicationPlan upload_plan(bool store_step = false, bool make_public = true) {
    auto p = plan_for();
    p.earthbank = EarthBankStep{EarthBankMode::Upload};
    if (store_step) p.store = StoreStep{make_public, std::nullopt};
    p.folder = FolderStep{};
    return p;
  }
  Result<std::vector<PlanRefusal>> check(const PublicationPlan& p) { return check_plan(*store_, p, kNow); }
  bool refused_with(const PublicationPlan& p, Destination d, const char* text) {
    auto r = check(p);
    EXPECT_TRUE(r);
    if (!r) return false;
    for (const auto& x : *r)
      if (x.destination == d && x.reason.find(text) != std::string::npos) return true;
    return false;
  }
  bool allowed(const PublicationPlan& p) {
    auto r = check(p);
    EXPECT_TRUE(r);
    if (r && !r->empty()) ADD_FAILURE() << (*r)[0].reason;
    return r && r->empty();
  }
  void embargo(ps::EmbargoScopeType type, const std::string& name, std::optional<UtcTime> until = std::nullopt) {
    ASSERT_TRUE(set_embargo(*store_, reducer(), {type, name, until, std::nullopt}));
  }
};

}  // namespace

TEST(RunOrder, IsFixedWhateverTheTickOrder) {
  PublicationPlan p;
  p.earthbank = EarthBankStep{};
  p.store = StoreStep{};
  p.folder = FolderStep{};
  EXPECT_EQ(run_order(p), (std::vector<Destination>{Destination::Folder, Destination::Store, Destination::EarthBank}));
  p.hosted = p.doi = true;
  EXPECT_EQ(run_order(p).back(), Destination::Hosted);
  EXPECT_EQ(destination_name(Destination::Store), "Publish in store");
}

TEST_P(CheckPlanTest, DraftUploadWithoutStoreRefused) {
  create();
  EXPECT_TRUE(refused_with(upload_plan(), Destination::EarthBank, "published"));
}

TEST_P(CheckPlanTest, DraftUploadWithStorePublicAllowed) {
  create();
  EXPECT_TRUE(allowed(upload_plan(true, true)));
}

TEST_P(CheckPlanTest, DraftUploadWithStorePrivateRefused) {
  create();
  EXPECT_TRUE(refused_with(upload_plan(true, false), Destination::EarthBank, "public"));
}

TEST_P(CheckPlanTest, PrivatePublishedUploadRefused) {
  publish(create(), false);
  auto p = upload_plan();
  EXPECT_TRUE(refused_with(p, Destination::EarthBank, "private"));
}

TEST_P(CheckPlanTest, PublicPublishedUploadAllowed) {
  publish(create(), true);
  EXPECT_TRUE(allowed(upload_plan()));
}

TEST_P(CheckPlanTest, LiveMemberEmbargoRefusesUpload) {
  publish(create(), true);
  embargo(ps::EmbargoScopeType::Sample, "FC-1");
  EXPECT_TRUE(refused_with(upload_plan(), Destination::EarthBank, "embargo"));
}

TEST_P(CheckPlanTest, LiveAnalysisEmbargoRefusesUpload) {
  publish(create(), true);
  embargo(ps::EmbargoScopeType::Analysis, analysis2_.str());
  EXPECT_TRUE(refused_with(upload_plan(), Destination::EarthBank, "embargo"));
}

TEST_P(CheckPlanTest, LiftedEmbargoAllowsUpload) {
  publish(create(), true);
  embargo(ps::EmbargoScopeType::Sample, "FC-1");
  ASSERT_TRUE(lift_embargoes_on(*store_, reducer(), ps::EmbargoScopeType::Sample, "FC-1"));
  EXPECT_TRUE(allowed(upload_plan()));
}

TEST_P(CheckPlanTest, EndedEmbargoAllowsUpload) {
  publish(create(), true);
  embargo(ps::EmbargoScopeType::Sample, "FC-1", at("2026-10-01T00:00:00Z"));
  EXPECT_TRUE(allowed(upload_plan()));
}

TEST_P(CheckPlanTest, ReleaseEmbargoInFutureRefusesUpload) {
  publish(create(at("2027-01-01T00:00:00Z")), true);
  EXPECT_TRUE(refused_with(upload_plan(), Destination::EarthBank, "embargo until"));
}

TEST_P(CheckPlanTest, PreflightAndXlsxAllowedOnDraft) {
  create();
  embargo(ps::EmbargoScopeType::Sample, "FC-1");  // some, not all, members
  for (auto mode : {EarthBankMode::Preflight, EarthBankMode::ExportXlsx}) {
    auto p = plan_for();
    p.earthbank = EarthBankStep{mode};
    EXPECT_TRUE(allowed(p));
  }
}

TEST_P(CheckPlanTest, EveryMemberEmbargoedRefusesEarthBankAnyMode) {
  create();
  embargo(ps::EmbargoScopeType::Project, "Alpha");
  embargo(ps::EmbargoScopeType::Project, "Beta");
  for (auto mode : {EarthBankMode::Preflight, EarthBankMode::ExportXlsx, EarthBankMode::Upload}) {
    auto p = plan_for();
    p.earthbank = EarthBankStep{mode};
    EXPECT_TRUE(refused_with(p, Destination::EarthBank, "No member"));
  }
}

TEST_P(CheckPlanTest, StoreOnPublishedRefused) {
  publish(create(), false);
  auto p = plan_for();
  p.store = StoreStep{false, "/tmp/x"};
  EXPECT_TRUE(refused_with(p, Destination::Store, "already published"));
}

TEST_P(CheckPlanTest, StoreWithoutFolderOrDirRefused) {
  create();
  auto p = plan_for();
  p.store = StoreStep{};
  EXPECT_TRUE(refused_with(p, Destination::Store, "release folder"));
  p.store->dir = "/tmp/x";
  EXPECT_TRUE(allowed(p));
}

TEST_P(CheckPlanTest, DoiAndHostedAlwaysRefused) {
  create();
  auto p = plan_for();
  p.doi = p.hosted = true;
  EXPECT_TRUE(refused_with(p, Destination::Doi, "Not built yet"));
  EXPECT_TRUE(refused_with(p, Destination::Hosted, "Not built yet"));
}

TEST_P(CheckPlanTest, UnknownReleaseIsARefusalNotAnError) {
  EXPECT_TRUE(refused_with(plan_for("nope"), Destination::Folder, "no release"));
}

TEST_P(CheckPlanTest, NewReleaseWithNoAnalysesRefused) {
  PublicationPlan p;
  p.release = NewRelease{spec()};
  std::get<NewRelease>(p.release).spec.analyses.clear();
  p.earthbank = EarthBankStep{};
  EXPECT_TRUE(refused_with(p, Destination::EarthBank, "no analyses"));
  std::get<NewRelease>(p.release).spec.analyses = {"not-a-uuid"};
  EXPECT_TRUE(refused_with(p, Destination::EarthBank, "not-a-uuid"));
}

TEST_P(CheckPlanTest, NewReleaseUploadJudgedAsPrivateDraft) {
  PublicationPlan p;
  p.release = NewRelease{spec()};
  p.earthbank = EarthBankStep{EarthBankMode::Upload};
  EXPECT_TRUE(refused_with(p, Destination::EarthBank, "published"));
  p.folder = FolderStep{};
  p.store = StoreStep{true, std::nullopt};
  EXPECT_TRUE(allowed(p));
  embargo(ps::EmbargoScopeType::Analysis, analysis1_.str());
  EXPECT_TRUE(refused_with(p, Destination::EarthBank, "embargo"));
}

TEST_P(CheckPlanTest, WithdrawnReleaseUploadRefused) {
  const auto release = create();
  publish(release, true);
  ASSERT_TRUE(withdraw_release(*store_, reducer(), release, "superseded"));
  EXPECT_TRUE(refused_with(upload_plan(), Destination::EarthBank, "withdrawn"));
  auto refusal = earthbank_upload_refusal(*store_, release, false, kNow);
  ASSERT_TRUE(refusal) << to_string(refusal.error());
  ASSERT_TRUE(refusal->has_value());
  EXPECT_NE((*refusal)->find("withdrawn"), std::string::npos);
}

// A store error while judging the upload is an error, never "may go": the
// gate does not tell a failed lookup from an allowed one by its message.
TEST_P(CheckPlanTest, UploadGateStoreErrorIsAnError) {
  const auto release = create();
  publish(release, true);
  struct FailingEmbargo : ingest::testing::ForwardingStore {
    using ForwardingStore::ForwardingStore;
    Result<std::vector<ps::Uuid>> embargoed_analyses(std::span<const ps::Uuid>, ps::UtcTime) override {
      return fail(ErrorKind::Io, "");
    }
  } failing(*store_);
  auto refusal = earthbank_upload_refusal(failing, release, false, kNow);
  EXPECT_FALSE(refusal) << "a store error was taken for an allowed upload";
  EXPECT_FALSE(check_plan(failing, upload_plan(), kNow));
}

TEST(DependsOnFailed, FolderFailSkipsStoreAndUploadNotPreflight) {
  PublicationPlan p;
  p.folder = FolderStep{};
  p.store = StoreStep{};
  p.earthbank = EarthBankStep{EarthBankMode::Upload};
  const std::vector<StepResult> so_far{{Destination::Folder, StepState::Failed, "x"}};
  EXPECT_TRUE(depends_on_failed(p, Destination::Store, so_far));
  EXPECT_TRUE(depends_on_failed(p, Destination::EarthBank, {{Destination::Folder, StepState::Failed, ""},
                                                            {Destination::Store, StepState::Skipped, ""}}));
  p.earthbank->mode = EarthBankMode::Preflight;
  EXPECT_FALSE(depends_on_failed(p, Destination::EarthBank, so_far));
  p.earthbank->mode = EarthBankMode::ExportXlsx;
  EXPECT_FALSE(depends_on_failed(p, Destination::EarthBank, so_far));
}

TEST(DependsOnFailed, StoreFailSkipsUploadOnlyWhenStoreTicked) {
  PublicationPlan p;
  p.earthbank = EarthBankStep{EarthBankMode::Upload};
  const std::vector<StepResult> so_far{{Destination::Store, StepState::Failed, ""}};
  EXPECT_FALSE(depends_on_failed(p, Destination::EarthBank, so_far));
  p.store = StoreStep{};
  EXPECT_TRUE(depends_on_failed(p, Destination::EarthBank, so_far));
  EXPECT_TRUE(depends_on_failed(p, Destination::EarthBank, {{Destination::Store, StepState::Cancelled, ""}}));
}

TEST(DependsOnFailed, NothingFailedRunsAll) {
  PublicationPlan p;
  p.folder = FolderStep{};
  p.store = StoreStep{};
  p.earthbank = EarthBankStep{EarthBankMode::Upload};
  const std::vector<StepResult> so_far{{Destination::Folder, StepState::Ok, ""}, {Destination::Store, StepState::Ok, ""}};
  for (auto d : run_order(p)) EXPECT_FALSE(depends_on_failed(p, d, so_far));
}

INSTANTIATE_TEST_SUITE_P(Engines, CheckPlanTest, ::testing::ValuesIn(ps::testing::engines()),
                         [](const ::testing::TestParamInfo<std::string>& i) { return i.param; });
