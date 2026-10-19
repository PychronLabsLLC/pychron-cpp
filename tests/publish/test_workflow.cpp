// The steps elctl publish and the Publish window share: publishing a built
// directory, and verifying one against a rebuild from the store.

#include "pychron/publish/workflow.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>

#include "pychron/core/sha256.hpp"
#include "pychron/processing/store_source.hpp"
#include "pychron/publish/build.hpp"
#include "pychron/publish/release.hpp"
#include "release_fixture.hpp"

namespace fs = std::filesystem;
namespace ps = pychron::persistence;
namespace pp = pychron::processing;
using namespace pychron;
using namespace pychron::publish;
using pychron::publish::testing::ReleaseFixture;

namespace {

std::string slurp(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

class Workflow : public ReleaseFixture {
 protected:
  void SetUp() override {
    ReleaseFixture::SetUp();
    out_ = fs::temp_directory_path() / ("pychron_workflow_" + std::to_string(std::random_device{}()));
    fs::create_directories(out_);
    auto source = pp::StoreSource::open(ps::StoreConfig{db_->url(), false}, pp::StoreSourceOptions{2, "tester", "test-host"});
    ASSERT_TRUE(source) << to_string(source.error());
    source_ = std::move(*source);
  }
  void TearDown() override {
    source_.reset();
    std::error_code ec;
    fs::remove_all(out_, ec);
    ReleaseFixture::TearDown();
  }

  Uuid create(const std::string& name) {
    ReleaseSpec s;
    s.name = name;
    s.title = "Release " + name;
    s.authors = {{"J. Ross", std::nullopt, std::nullopt}};
    s.analyses = analysis_texts();
    s.options_json = release_options_json(ReleaseOptions{});
    s.reduction_version = "arar-1";
    s.software_version = "pychron-cpp/0.4.0";
    auto created = create_release(*store_, reducer(), s);
    EXPECT_TRUE(created) << (created ? "" : to_string(created.error()));
    return created ? created->release : Uuid{};
  }

  fs::path build(Uuid release, bool private_build = false) {
    BuildOptions o;
    o.out = out_;
    o.private_build = private_build;
    o.store_url = db_->url();
    auto built = build_release(*store_, *source_, release, o, nullptr);
    EXPECT_TRUE(built) << (built ? "" : to_string(built.error()));
    return built ? built->dir : fs::path{};
  }

  fs::path out_;
  std::unique_ptr<pp::StoreSource> source_;
};

}  // namespace

TEST_P(Workflow, PublishBuiltPublishesAndRecordsTheManifestSha) {
  const Uuid release = create("r");
  const fs::path dir = build(release);
  auto published = publish_built(*store_, reducer(), "r", dir, false);
  ASSERT_TRUE(published) << to_string(published.error());
  EXPECT_EQ(published->state, ps::ReleaseState::Published);
  EXPECT_EQ(published->visibility, ps::Visibility::Private);
  ASSERT_TRUE(published->manifest_sha256);
  EXPECT_EQ(*published->manifest_sha256, sha256(std::string_view(slurp(dir / "manifest.json"))));
}

TEST_P(Workflow, APrivateBuildCannotBePublic) {
  const Uuid release = create("r");
  const fs::path dir = build(release, true);
  auto refused = publish_built(*store_, reducer(), "r", dir, true);
  ASSERT_FALSE(refused);
  EXPECT_NE(refused.error().what.find("private build"), std::string::npos) << refused.error().what;
}

TEST_P(Workflow, ADirectoryOfAnotherReleaseIsRefused) {
  const fs::path first = build(create("r1"));
  create("r2");
  auto refused = publish_built(*store_, reducer(), "r2", first, false);
  ASSERT_FALSE(refused);
  EXPECT_NE(refused.error().what.find("not this release"), std::string::npos) << refused.error().what;
}

TEST_P(Workflow, AChangedFileIsRefused) {
  const fs::path dir = build(create("r"));
  std::ofstream(dir / "README.md", std::ios::app) << "edited\n";
  auto refused = publish_built(*store_, reducer(), "r", dir, false);
  ASSERT_FALSE(refused);
  EXPECT_NE(refused.error().what.find("does not match its manifest"), std::string::npos) << refused.error().what;
  auto row = find_release(*store_, "r");
  ASSERT_TRUE(row);
  EXPECT_EQ(row->state, ps::ReleaseState::Draft);
}

TEST_P(Workflow, VerifyAgainstStoreAgreesAndNamesAChangedTable) {
  const fs::path dir = build(create("r"));
  auto agreed = verify_against_store(*store_, *source_, dir, db_->url());
  ASSERT_TRUE(agreed) << to_string(agreed.error());
  EXPECT_TRUE(agreed->ok());
  const fs::path csv = dir / "tables" / "release.csv";
  std::string text = slurp(csv);
  text[text.size() / 2] = text[text.size() / 2] == 'x' ? 'y' : 'x';
  std::ofstream(csv, std::ios::binary) << text;
  auto changed = verify_against_store(*store_, *source_, dir, db_->url());
  ASSERT_TRUE(changed) << to_string(changed.error());
  EXPECT_FALSE(changed->ok());
  ASSERT_EQ(changed->files.changed.size(), 1u);
  EXPECT_EQ(changed->files.changed[0], "tables/release.csv");
}

INSTANTIATE_TEST_SUITE_P(Engines, Workflow, ::testing::ValuesIn(ps::testing::engines()),
                         [](const ::testing::TestParamInfo<std::string>& i) { return i.param; });
