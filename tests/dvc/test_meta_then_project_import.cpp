// Two sources into one store: a MetaData repository, which makes every
// irradiation position bare, and a project repository imported with
// catalog_from_repos, whose records name the sample, project and material of
// the position their identifier sits in. Whatever the order, the analysis is
// found under its sample and project.

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "fixture_files.hpp"
#include "git_fixture.hpp"
#include "legacy_repo_builder.hpp"
#include "pychron/dvc/meta_adapter.hpp"
#include "pychron/dvc/project_adapter.hpp"
#include "pychron/ingest/writer.hpp"
#include "store_fixture.hpp"
#include "verify_support.hpp"

using namespace pychron;
using namespace pychron::dvc;
using namespace pychron::dvc::testing;
namespace P = pychron::persistence;
namespace pd = pychron::persistence::detail;
using ingest::RunStats;
using P::Uuid;

namespace {

const Uuid kE = *Uuid::parse(LegacyRepoBuilder::kFixtureUuid);
const char* const kLevelAdded = "2017-12-20T10:00:00-07:00";
const char* const kCollected = "2018-02-20T00:27:10-07:00";

std::string err(const Error& e) { return to_string(e); }

// An empty database: the store under test and a white-box connection to it.
struct World {
  explicit World(const std::string& engine) : database(engine, true) {
    store = P::testing::open_or_die(database.url());
    if (!store) return;
    client = *store->register_client({"import-1", "importer", std::nullopt, "test"});
    auto opened = pd::Db::open(P::StoreConfig{database.url(), false});
    if (opened) db = std::move(*opened);
  }

  // Declared first so it is destroyed last: the connections point into it.
  P::testing::TestDatabase database;
  std::unique_ptr<P::IStore> store;
  Uuid client;
  std::unique_ptr<pd::Db> db;
};

MetaAdapterConfig meta_config(GitFixture& repo) {
  MetaAdapterConfig c;
  c.git.repo = repo.path();
  c.git.branch = "main";
  c.git.scratch = repo.temp("scratch");
  c.url = "https://GitHub.com/NMGRLData/MetaData.git";
  c.lab_time_zone = "America/Denver";
  return c;
}

ProjectAdapterConfig project_config(GitFixture& repo) {
  ProjectAdapterConfig c;
  c.git.repo = repo.path();
  c.git.branch = "main";
  c.git.scratch = repo.temp("scratch");
  c.url = "https://GitHub.com/NMGRLData/IR1010.git";
  c.repository_name = "IR1010";
  c.lab_time_zone = "America/Denver";
  c.catalog_from_repos = true;
  return c;
}

class MetaThenProjectImportTest : public ::testing::TestWithParam<std::string> {
 protected:
  void SetUp() override {
    if (!GitFixture::available()) GTEST_SKIP() << "git not on PATH";
    meta_.init();
    project_.init();
    // The level file places identifier 66052 in NM-293/G/16; the record of
    // 66052-01E names that position, its sample, project and material.
    meta_.write("NM-293/G.json", fixture("meta/NM-293/G.json"));
    meta_.commit("Added level G to NM-293", kLevelAdded);
    legacy_.collect(LegacyRepoBuilder::kFixtureRunid, kE.str(), kCollected);
    world_ = std::make_unique<World>(GetParam());
    ASSERT_TRUE(world_->store);
    ASSERT_TRUE(world_->db);
  }

  Result<RunStats> import_meta(bool replay = false) {
    auto adapter = MetaRepoAdapter::open(meta_config(meta_));
    if (!adapter) return fail(adapter.error());
    auto config = verify_writer_config();
    config.replay = replay;
    ingest::BatchWriter batches(*world_->store, world_->client, std::move(config));
    return batches.run(**adapter, std::nullopt, {}, {});
  }

  Result<RunStats> import_project(bool replay = false) {
    auto adapter = ProjectRepoAdapter::open(project_config(project_));
    if (!adapter) return fail(adapter.error());
    auto config = verify_writer_config();
    config.replay = replay;
    ingest::BatchWriter batches(*world_->store, world_->client, std::move(config));
    return batches.run(**adapter, std::nullopt, {}, {});
  }

  // What the Data browser shows of the analysis: the browse row and the
  // detail's row both name its sample, project, material and position.
  void expect_placed() {
    auto listed = world_->store->browse({});
    ASSERT_TRUE(listed) << err(listed.error());
    ASSERT_EQ(listed->rows.size(), 1u);
    auto detail = world_->store->load_analysis_detail(kE);
    ASSERT_TRUE(detail) << err(detail.error());
    ASSERT_TRUE(detail->has_value());
    for (const P::BrowseRow* row : {&listed->rows.front(), &(*detail)->row}) {
      EXPECT_EQ(row->summary.uuid, kE);
      EXPECT_EQ(row->summary.identifier, "66052");
      EXPECT_EQ(row->sample, "SB15-03");
      EXPECT_EQ(row->project, "IR1010");
      EXPECT_EQ(row->material, "Feldspar");
      EXPECT_EQ(row->irradiation, "NM-293");
      EXPECT_EQ(row->level, "G");
      EXPECT_EQ(row->position, std::optional<int>{16});
    }
    P::BrowseRequest by_sample;
    by_sample.filter.samples = {"SB15-03"};
    by_sample.filter.projects = {"IR1010"};
    auto found = world_->store->browse(by_sample);
    ASSERT_TRUE(found) << err(found.error());
    EXPECT_EQ(found->rows.size(), 1u);
    auto samples = world_->store->facet(P::BrowseFacet::Sample, {});
    ASSERT_TRUE(samples);
    EXPECT_EQ(*samples, std::vector<std::string>{"SB15-03"});
    auto projects = world_->store->facet(P::BrowseFacet::Project, {});
    ASSERT_TRUE(projects);
    EXPECT_EQ(*projects, std::vector<std::string>{"IR1010"});
    // One position NM-293/G/16: the two sources agree on which row it is.
    auto holes = world_->db->select_one(
        QStringLiteral("SELECT count(*) AS n FROM irradiation_position WHERE position = 16"));
    ASSERT_TRUE(holes && *holes);
    EXPECT_EQ((*holes)->value("n").toInt(), 1);
  }

  // Both sources verify, and neither a run nor a replay of either writes.
  void expect_settled() {
    {
      auto adapter = MetaRepoAdapter::open(meta_config(meta_));
      ASSERT_TRUE(adapter) << err(adapter.error());
      const auto report = verify_source(*world_, **adapter);
      EXPECT_TRUE(report.ok());
      EXPECT_EQ(unaccounted(report), std::vector<std::string>{});
      EXPECT_EQ(report.would_write, 0);
      EXPECT_EQ(report.replay_would_write, 0);
    }
    {
      auto adapter = ProjectRepoAdapter::open(project_config(project_));
      ASSERT_TRUE(adapter) << err(adapter.error());
      const auto report = verify_source(*world_, **adapter);
      EXPECT_TRUE(report.ok());
      EXPECT_EQ(unaccounted(report), std::vector<std::string>{});
      EXPECT_EQ(report.would_write, 0);
      EXPECT_EQ(report.replay_would_write, 0);
    }
    const auto seq = *world_->store->latest_change_seq();
    for (const bool replay : {false, true}) {
      auto meta = import_meta(replay);
      ASSERT_TRUE(meta) << err(meta.error());
      auto project = import_project(replay);
      ASSERT_TRUE(project) << err(project.error());
      EXPECT_EQ(*world_->store->latest_change_seq(), seq) << (replay ? "a replay" : "a second run") << " wrote";
    }
    expect_placed();
  }

  // The repositories first: the adapters read them.
  GitFixture meta_, project_;
  LegacyRepoBuilder legacy_{project_};
  std::unique_ptr<World> world_;
};

}  // namespace

TEST_P(MetaThenProjectImportTest, AnalysisIsFoundUnderItsSampleAndProject) {
  auto meta = import_meta();
  ASSERT_TRUE(meta) << err(meta.error());
  EXPECT_TRUE(meta->finished);
  auto project = import_project();
  ASSERT_TRUE(project) << err(project.error());
  EXPECT_TRUE(project->finished);
  EXPECT_EQ(project->analyses, 1);
  expect_placed();
  expect_settled();
}

TEST_P(MetaThenProjectImportTest, ProjectBeforeMetaIsTheSame) {
  auto project = import_project();
  ASSERT_TRUE(project) << err(project.error());
  EXPECT_EQ(project->analyses, 1);
  auto meta = import_meta();
  ASSERT_TRUE(meta) << err(meta.error());
  EXPECT_TRUE(meta->finished);
  expect_placed();
  expect_settled();
}

INSTANTIATE_TEST_SUITE_P(Engines, MetaThenProjectImportTest, ::testing::ValuesIn(P::testing::engines()));
