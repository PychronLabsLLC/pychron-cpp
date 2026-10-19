// The manifest of a release directory and its verification (data publishing
// design, sections 5.7 and 5.8).

#include "pychron/publish/manifest.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <random>

namespace fs = std::filesystem;
namespace ps = pychron::persistence;
using namespace pychron;
using namespace pychron::publish;

namespace {

Manifest sample_manifest() {
  Manifest m;
  m.release.uuid = *ps::Uuid::parse("0192f3a0-0000-7000-8000-000000000010");
  m.release.name = "nmgrl-2026-10-fc2";
  m.release.version = 2;
  m.release.title = "Fish Canyon sanidine";
  m.release.description = "Two aliquots";
  m.release.state = "published";
  m.release.visibility = "public";
  m.release.licence = "CC-BY-4.0";
  m.release.authors = {{"J. Ross", "0000-0002-1825-0097", "NMT"}};
  m.release.publisher = "NMGRL";
  m.release.doi = "10.1234/abcd";
  m.release.supersedes = *ps::Uuid::parse("0192f3a0-0000-7000-8000-000000000009");
  m.release.created = *ps::UtcTime::parse("2026-10-18T00:00:00Z");
  m.release.published = *ps::UtcTime::parse("2026-10-19T00:00:00Z");
  m.build.built = *ps::UtcTime::parse("2026-10-18T01:02:03.000004Z");
  m.build.software = "pychron-cpp/0.4.0";
  m.build.reduction_version = "arar-1";
  m.build.constants_preset = "default";
  m.build.options_json = R"({"sigma":2})";
  m.build.store_kind = "sqlite";
  m.build.schema_version = 7;
  m.build.store_id = "0123456789abcdef";
  m.build.omitted_samples = 1;
  m.pins.bookmark = *ps::Uuid::parse("0192f3a0-0000-7000-8000-000000000020");
  ManifestAnalysisPins a;
  a.uuid = *ps::Uuid::parse("0192f3a0-0000-7000-8000-000000000030");
  a.runid = "70001-01";
  a.record_sha256 = "ab";
  a.heads = {{"intercepts", "0192f3a0-0000-7000-8000-000000000031"}, {"tags", "0192f3a0-0000-7000-8000-000000000032"}};
  a.refs = {{"0192f3a0-0000-7000-8000-000000000040", "0192f3a0-0000-7000-8000-000000000041"}};
  m.pins.analyses = {a};
  m.pins.interpreted_ages = {{*ps::Uuid::parse("0192f3a0-0000-7000-8000-000000000050"),
                              *ps::Uuid::parse("0192f3a0-0000-7000-8000-000000000051")}};
  m.pins.samples = {*ps::Uuid::parse("0192f3a0-0000-7000-8000-000000000060")};
  m.tables = {{"data/analyses.parquet", 2, sha256(std::string_view{"rows"}), {"analysis_uuid", "runid"}}};
  m.files = {{"LICENSE.txt", 3, sha256(std::string_view{"abc"}), "text/plain"},
             {"tables/release.csv", 4, sha256(std::string_view{"a,b\n"}), "text/csv"}};
  return m;
}

fs::path scratch() {
  const auto dir = fs::temp_directory_path() / ("pychron_manifest_" + std::to_string(std::random_device{}()));
  fs::create_directories(dir);
  return dir;
}

void write(const fs::path& path, std::string_view bytes) {
  fs::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary);
  out << bytes;
}

// A directory holding the files the manifest lists, with their bytes.
fs::path directory_of(const Manifest& m) {
  const fs::path dir = scratch();
  write(dir / "LICENSE.txt", "abc");
  write(dir / "tables/release.csv", "a,b\n");
  write(dir / kManifestFile, manifest_json(m));
  return dir;
}

}  // namespace

TEST(Manifest, RoundTripsThroughJson) {
  const Manifest m = sample_manifest();
  auto back = parse_manifest(manifest_json(m));
  ASSERT_TRUE(back) << to_string(back.error());
  EXPECT_EQ(*back, m);
  EXPECT_FALSE(parse_manifest("{"));
  EXPECT_FALSE(parse_manifest(R"({"format": "other/1"})"));
}

TEST(Manifest, JsonKeysAreInSpecOrderAndDeterministic) {
  const Manifest m = sample_manifest();
  const std::string json = manifest_json(m);
  EXPECT_EQ(json, manifest_json(m));
  EXPECT_EQ(json.rfind("{\n  \"format\": \"pychron-release/1\",\n  \"release\": {", 0), 0u) << json.substr(0, 80);
  const auto release = json.find("\"release\""), build = json.find("\"build\""), pins = json.find("\"pins\""),
             tables = json.find("\"tables\""), files = json.find("\"files\"");
  EXPECT_LT(release, build);
  EXPECT_LT(build, pins);
  EXPECT_LT(pins, tables);
  EXPECT_LT(tables, files);
  EXPECT_EQ(json.back(), '\n');
  EXPECT_EQ(json.find(" \n"), std::string::npos) << "no trailing spaces";
  EXPECT_NE(json.find("\"rows_sha256\": \"" + to_hex(m.tables[0].rows_sha256) + "\""), std::string::npos);
}

TEST(Manifest, VerifyFilesNamesMissingAndChanged) {
  const Manifest m = sample_manifest();
  const fs::path dir = directory_of(m);
  auto clean = verify_files(dir);
  ASSERT_TRUE(clean) << to_string(clean.error());
  EXPECT_TRUE(clean->ok()) << clean->missing.size() << " missing, " << clean->changed.size() << " changed, "
                           << clean->extra.size() << " extra";
  fs::remove(dir / "LICENSE.txt");
  write(dir / "tables/release.csv", "a,c\n");
  write(dir / "extra.txt", "x");
  auto report = verify_files(dir);
  ASSERT_TRUE(report);
  EXPECT_EQ(report->missing, std::vector<std::string>{"LICENSE.txt"});
  EXPECT_EQ(report->changed, std::vector<std::string>{"tables/release.csv"});
  EXPECT_EQ(report->extra, std::vector<std::string>{"extra.txt"});
  EXPECT_FALSE(report->ok());
  fs::remove_all(dir);
  EXPECT_FALSE(verify_files(dir)) << "no directory, no manifest";
}

TEST(Manifest, VerifyRebuildReportsReductionVersionOnly) {
  const Manifest m = sample_manifest();
  const fs::path dir = directory_of(m);
  Manifest other = m;
  other.build.reduction_version = "arar-2";
  other.tables[0].rows_sha256 = sha256(std::string_view{"different"});
  auto report = verify_rebuild(dir, other);
  ASSERT_TRUE(report) << to_string(report.error());
  ASSERT_TRUE(report->reduction_version_differs);
  EXPECT_NE(report->reduction_version_differs->find("arar-2"), std::string::npos);
  EXPECT_TRUE(report->changed.empty()) << "nothing else is compared";
  EXPECT_FALSE(report->ok());

  Manifest same = m;
  auto equal = verify_rebuild(dir, same);
  ASSERT_TRUE(equal);
  EXPECT_TRUE(equal->ok());
  same.tables[0].rows_sha256 = sha256(std::string_view{"different"});
  same.files[0].sha256 = sha256(std::string_view{"other licence"});
  auto differs = verify_rebuild(dir, same);
  ASSERT_TRUE(differs);
  EXPECT_EQ(differs->changed, (std::vector<std::string>{"LICENSE.txt", "data/analyses.parquet"}));
  fs::remove_all(dir);
}

TEST(Manifest, MediaTypesByExtension) {
  EXPECT_EQ(media_type_of("data/analyses.parquet"), "application/vnd.apache.parquet");
  EXPECT_EQ(media_type_of("records/x.json"), "application/geo+json");
  EXPECT_EQ(media_type_of("catalog.json"), "application/json");
  EXPECT_EQ(media_type_of("tables/release.csv"), "text/csv");
  EXPECT_EQ(media_type_of("figures/a/ideogram.pdf"), "application/pdf");
  EXPECT_EQ(media_type_of("figures/a/ideogram.svg"), "image/svg+xml");
  EXPECT_EQ(media_type_of("README.md"), "text/markdown");
  EXPECT_EQ(media_type_of("CITATION.cff"), "application/yaml");
  EXPECT_EQ(media_type_of("LICENSE.txt"), "text/plain");
}
