// Building a release directory from the store (data publishing design,
// sections 5 and 6.1).

#include "pychron/publish/build.hpp"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#ifdef __APPLE__
#include <sys/xattr.h>
#endif

#include "pychron/processing/store_source.hpp"
#include "pychron/publish/records.hpp"
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

class BuildTest : public ReleaseFixture {
 protected:
  void SetUp() override {
    ReleaseFixture::SetUp();
    out_ = fs::temp_directory_path() / ("pychron_build_" + std::to_string(std::random_device{}()));
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

  Uuid create(const std::string& name = "r") {
    ReleaseSpec s;
    s.name = name;
    s.title = "Release " + name;
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

  BuildOptions options(bool private_build = false) {
    BuildOptions o;
    o.out = out_;
    o.private_build = private_build;
    o.store_url = db_->url();
    return o;
  }

  fs::path out_;
  std::unique_ptr<pp::StoreSource> source_;
};

}  // namespace

TEST_P(BuildTest, WritesEveryFileInTheManifestWithMatchingSha) {
  const Uuid release = create();
  auto built = build_release(*store_, *source_, release, options(), nullptr);
  ASSERT_TRUE(built) << to_string(built.error());
  EXPECT_EQ(built->dir, out_ / "r-v1");
  EXPECT_TRUE(fs::exists(built->dir / "manifest.json"));
  for (const char* file : {"catalog.json", "records/release.json", "tables/release.csv", "tables/release.json",
                           "constants.json", "LICENSE.txt", "CITATION.cff", "README.md", "tables/FC-1/report.csv"})
    EXPECT_TRUE(fs::exists(built->dir / file)) << file;
  auto verified = verify_files(built->dir);
  ASSERT_TRUE(verified) << to_string(verified.error());
  EXPECT_TRUE(verified->ok()) << verified->missing.size() << " missing, " << verified->changed.size() << " changed, "
                              << verified->extra.size() << " extra";
  const Manifest& m = built->manifest;
  EXPECT_EQ(m.release.name, "r");
  EXPECT_EQ(m.release.state, "draft");
  EXPECT_EQ(m.build.software, "pychron-cpp/0.4.0");
  EXPECT_EQ(m.build.store_kind, GetParam() == "pg" ? "postgresql" : "sqlite");
  EXPECT_EQ(m.build.schema_version, 7);
  EXPECT_EQ(m.pins.analyses.size(), 3u);
  EXPECT_EQ(m.pins.samples.size(), 3u);
  EXPECT_EQ(m.pins.analyses[0].heads.size(), 6u);
  EXPECT_EQ(m.tables.size(), 5u);
  // Ages were made: the positioned members have J.
  const std::string csv = slurp(built->dir / "tables/release.csv");
  EXPECT_NE(csv.find("70001-01"), std::string::npos);
  EXPECT_NE(csv.find("[analyses]"), std::string::npos);
  const auto constants = nlohmann::json::parse(slurp(built->dir / "constants.json"));
  EXPECT_TRUE(constants.is_object());
  const std::string licence = slurp(built->dir / "LICENSE.txt");
  EXPECT_NE(licence.find("Attribution 4.0 International"), std::string::npos);
  const std::string cff = slurp(built->dir / "CITATION.cff");
  EXPECT_NE(cff.find("cff-version: 1.2.0"), std::string::npos);
  EXPECT_NE(cff.find("orcid: https://orcid.org/0000-0002-1825-0097"), std::string::npos);
  EXPECT_NE(cff.find("license: CC-BY-4.0"), std::string::npos);
  if (!built->parquet_written) {
    EXPECT_FALSE(fs::exists(built->dir / "data"));
    ASSERT_FALSE(built->warnings.empty());
    EXPECT_NE(built->warnings[0].find("Parquet"), std::string::npos);
  }
}

TEST_P(BuildTest, TwoBuildsGiveEqualRowHashesAndEqualNonParquetBytes) {
  const Uuid release = create();
  auto first = build_release(*store_, *source_, release, options(), nullptr);
  ASSERT_TRUE(first) << to_string(first.error());
  BuildOptions second_out = options();
  second_out.out = out_ / "again";
  fs::create_directories(second_out.out);
  auto second = build_release(*store_, *source_, release, second_out, nullptr);
  ASSERT_TRUE(second) << to_string(second.error());
  ASSERT_EQ(first->manifest.tables.size(), second->manifest.tables.size());
  for (std::size_t i = 0; i < first->manifest.tables.size(); ++i)
    EXPECT_EQ(first->manifest.tables[i].rows_sha256, second->manifest.tables[i].rows_sha256)
        << first->manifest.tables[i].path;
  for (const auto& f : first->manifest.files) {
    if (f.path.ends_with(".parquet")) continue;
    EXPECT_EQ(slurp(first->dir / f.path), slurp(second->dir / f.path)) << f.path;
  }
  auto rebuilt = verify_rebuild(first->dir, second->manifest);
  ASSERT_TRUE(rebuilt) << to_string(rebuilt.error());
  EXPECT_TRUE(rebuilt->ok());
}

TEST_P(BuildTest, PublicBuildOmitsEmbargoedSample) {
  ASSERT_TRUE(store_->add_embargo(reducer(), {{ps::EmbargoScopeType::Sample, sample1_}, std::nullopt, "paper"}));
  const Uuid release = create();
  auto built = build_release(*store_, *source_, release, options(), nullptr);
  ASSERT_TRUE(built) << to_string(built.error());
  EXPECT_FALSE(fs::exists(built->dir / ("records/" + sample1_.str() + ".json")));
  EXPECT_TRUE(fs::exists(built->dir / ("records/" + sample2_.str() + ".json")));
  EXPECT_EQ(built->manifest.build.omitted_samples, 1u);
  EXPECT_EQ(built->manifest.build.omitted_analyses, 1u);
  EXPECT_FALSE(built->manifest.build.private_build);
  EXPECT_EQ(built->manifest.pins.analyses.size(), 2u);
  EXPECT_EQ(built->manifest.pins.samples.size(), 2u);
  EXPECT_EQ(slurp(built->dir / "tables/release.csv").find("70001-01"), std::string::npos);

  BuildOptions priv = options(true);
  priv.out = out_ / "private";
  fs::create_directories(priv.out);
  auto whole = build_release(*store_, *source_, release, priv, nullptr);
  ASSERT_TRUE(whole) << to_string(whole.error());
  EXPECT_TRUE(whole->manifest.build.private_build);
  EXPECT_EQ(whole->manifest.build.omitted_samples, 0u);
  EXPECT_EQ(whole->manifest.pins.analyses.size(), 3u);
  EXPECT_TRUE(fs::exists(whole->dir / ("records/" + sample1_.str() + ".json")));
}

TEST_P(BuildTest, SampleNamesAreMadeFileSafeAndUnique) {
  // A second "FC-1" in project Beta, with its own analysis, and a name that is not file-safe.
  const ps::Uuid twin = *store_->add_sample(lab_.reduction_client, {"FC-1", project2_, material_});
  ASSERT_TRUE(store_->add_identifier(lab_.acquisition_client, {.identifier = "70004", .sample = twin}));
  const ps::Uuid extra = ingest("70004", 1, "2026-10-02T13:00:00Z");
  ps::CatalogEditBatch rename;
  ps::CatalogUpdate u;
  u.table = ps::CatalogTable::Sample;
  u.uuid = sample3_;
  u.expected["name"] = std::string("BT-1");
  u.values["name"] = std::string("BT-1 / split (b)");
  rename.edits = {u};
  rename.message = "rename";
  auto edited = store_->apply_catalog_edits(lab_.reduction_client, rename);
  ASSERT_TRUE(edited) << to_string(edited.error());
  ReleaseSpec s;
  s.name = "names";
  s.title = "Names";
  s.analyses = analysis_texts();
  s.analyses.push_back(extra.str());
  s.options_json = release_options_json(ReleaseOptions{});
  s.reduction_version = "arar-1";
  s.software_version = "pychron-cpp/0.4.0";
  auto created = create_release(*store_, reducer(), s);
  ASSERT_TRUE(created) << to_string(created.error());
  auto built = build_release(*store_, *source_, created->release, options(), nullptr);
  ASSERT_TRUE(built) << to_string(built.error());
  EXPECT_EQ(created->samples, 4u);
  EXPECT_EQ(built->manifest.pins.samples.size(), 4u);
  std::vector<std::string> dirs;
  for (const auto& entry : fs::directory_iterator(built->dir / "tables"))
    if (entry.is_directory()) dirs.push_back(entry.path().filename().string());
  std::sort(dirs.begin(), dirs.end());
  std::string listing;
  for (const auto& d : dirs) listing += d + " ";
  ASSERT_EQ(dirs.size(), 4u) << "one per sample: " << listing;
  EXPECT_EQ(dirs[0], "BT-1___split__b_");
  EXPECT_EQ(dirs[1].rfind("FC-1-", 0), 0u);
  EXPECT_EQ(dirs[2].rfind("FC-1-", 0), 0u);
  EXPECT_NE(dirs[1], dirs[2]);
  EXPECT_EQ(dirs[3], "FC-2");
  for (const auto& f : built->manifest.files) EXPECT_TRUE(fs::exists(built->dir / f.path)) << f.path;
}

TEST_P(BuildTest, PerSampleReportsHoldOnlyTheirOwnAnalyses) {
  const ps::Uuid twin = *store_->add_sample(lab_.reduction_client, {"FC-1", project2_, material_});
  ASSERT_TRUE(store_->add_identifier(lab_.acquisition_client, {.identifier = "70004", .sample = twin}));
  const ps::Uuid extra = ingest("70004", 1, "2026-10-02T13:00:00Z");
  ReleaseSpec s;
  s.name = "twins";
  s.title = "Twins";
  s.analyses = analysis_texts();
  s.analyses.push_back(extra.str());
  s.options_json = release_options_json(ReleaseOptions{});
  s.reduction_version = "arar-1";
  s.software_version = "pychron-cpp/0.4.0";
  auto created = create_release(*store_, reducer(), s);
  ASSERT_TRUE(created);
  auto built = build_release(*store_, *source_, created->release, options(), nullptr);
  ASSERT_TRUE(built) << to_string(built.error());
  std::size_t fc1_dirs = 0;
  for (const auto& entry : fs::directory_iterator(built->dir / "tables")) {
    if (!entry.is_directory() || !entry.path().filename().string().starts_with("FC-1-")) continue;
    ++fc1_dirs;
    const std::string csv = slurp(entry.path() / "report.csv");
    const bool has1 = csv.find("70001-01") != std::string::npos, has4 = csv.find("70004-01") != std::string::npos;
    EXPECT_NE(has1, has4) << entry.path() << " holds both samples' analyses";
  }
  EXPECT_EQ(fc1_dirs, 2u);
}

TEST_P(BuildTest, ReadmeAndRecordsCarryTheReleaseDateNotTheBuildDate) {
  const Uuid release = create();
  auto built = build_release(*store_, *source_, release, options(), nullptr);
  ASSERT_TRUE(built) << to_string(built.error());
  const std::string readme = slurp(built->dir / "README.md");
  EXPECT_NE(readme.find(built->manifest.release.created.iso().substr(0, 10)), std::string::npos) << readme;
  EXPECT_EQ(readme.find("built on"), std::string::npos) << readme;
}

TEST_P(BuildTest, PublicBuildDropsAnInterpretedAgeWithAnEmbargoedMember) {
  auto ia = *store_->add_interpreted_age(lab_.reduction_client, {"FC-1 and BT-1"});
  {
    auto uow = *store_->begin(reducer());
    ps::InterpretedAgeValue v;
    v.age = 28.2;
    v.members = {{analysis1_, std::nullopt, std::nullopt, std::nullopt}, {analysis3_, std::nullopt, std::nullopt, std::nullopt}};
    ASSERT_TRUE(uow->add_revision(ia, ps::Kind::InterpretedAge, v, std::nullopt));
    ASSERT_TRUE(uow->commit(ps::ChangesetKind::Reduction, "ia"));
  }
  ASSERT_TRUE(store_->add_embargo(reducer(), {{ps::EmbargoScopeType::Analysis, analysis3_}, std::nullopt, "x"}));
  const Uuid release = create();
  auto built = build_release(*store_, *source_, release, options(), nullptr);
  ASSERT_TRUE(built) << to_string(built.error());
  EXPECT_TRUE(built->manifest.pins.interpreted_ages.empty()) << "a member is hidden";
  EXPECT_EQ(built->manifest.build.omitted_analyses, 1u);
  BuildOptions priv = options(true);
  priv.out = out_ / "private";
  fs::create_directories(priv.out);
  auto whole = build_release(*store_, *source_, release, priv, nullptr);
  ASSERT_TRUE(whole);
  EXPECT_EQ(whole->manifest.pins.interpreted_ages.size(), 1u);
}

TEST_P(BuildTest, RefusesANonEmptyTargetDirectory) {
  const Uuid release = create();
  fs::create_directories(out_ / "r-v1");
  std::ofstream(out_ / "r-v1" / "stale.txt") << "x";
  auto refused = build_release(*store_, *source_, release, options(), nullptr);
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().kind, ErrorKind::Config);
  EXPECT_FALSE(build_release(*store_, *source_, Uuid::v7(), options(), nullptr)) << "no such release";
}

TEST_P(BuildTest, EveryItemLinkInCatalogResolvesToAFile) {
  const Uuid release = create();
  auto built = build_release(*store_, *source_, release, options(), nullptr);
  ASSERT_TRUE(built) << to_string(built.error());
  const auto catalog = nlohmann::json::parse(slurp(built->dir / "catalog.json"));
  std::size_t items = 0;
  for (const auto& link : catalog.at("links")) {
    const std::string href = link.at("href");
    if (href.starts_with("https://")) continue;
    EXPECT_TRUE(fs::exists(built->dir / href)) << href;
    if (link.at("rel") == "item") ++items;
  }
  EXPECT_EQ(items, 4u) << "three samples and the release";
  for (const auto& entry : fs::directory_iterator(built->dir / "records")) {
    const auto record = nlohmann::json::parse(slurp(entry.path()));
    for (const auto& link : record.at("links")) {
      const std::string href = link.at("href");
      if (href.starts_with("https://")) continue;
      EXPECT_TRUE(fs::exists(entry.path().parent_path() / href)) << entry.path().filename() << " -> " << href;
    }
  }
}

TEST_P(BuildTest, OutputIsNotQuarantined) {
  const Uuid release = create();
  auto built = build_release(*store_, *source_, release, options(), nullptr);
  ASSERT_TRUE(built) << to_string(built.error());
#ifdef __APPLE__
  // The quarantine attribute, read as the system does (no command processor).
  const std::string path = (built->dir / "manifest.json").string();
  const ssize_t n = getxattr(path.c_str(), "com.apple.quarantine", nullptr, 0, 0, 0);
  EXPECT_LT(n, 0) << "manifest.json carries com.apple.quarantine";
#endif
}

TEST(ReleaseOptions, JsonRoundTrip) {
  ReleaseOptions o;
  o.sigma = 1;
  o.constants = "legacy";
  o.decay_error = true;
  o.plateau = "mahon";
  o.plateau_steps = 4;
  o.plateau_gas = 60.5;
  o.group_by = "sample";
  o.laboratory = "NMGRL";
  o.notes = {"a", "b"};
  const std::string json = release_options_json(o);
  EXPECT_EQ(json, R"({"sigma":1,"constants":"legacy","decay_error":true,"plateau":"mahon","plateau_steps":4,"plateau_gas":60.5,"group_by":"sample","laboratory":"NMGRL","notes":["a","b"]})");
  auto back = parse_release_options(json);
  ASSERT_TRUE(back);
  EXPECT_EQ(*back, o);
  auto defaults = parse_release_options("{}");
  ASSERT_TRUE(defaults);
  EXPECT_EQ(*defaults, ReleaseOptions{});
  EXPECT_FALSE(parse_release_options(R"({"sigma": 3})"));
  EXPECT_FALSE(parse_release_options("nope"));
}

INSTANTIATE_TEST_SUITE_P(Engines, BuildTest, ::testing::ValuesIn(ps::testing::engines()),
                         [](const ::testing::TestParamInfo<std::string>& i) { return i.param; });
