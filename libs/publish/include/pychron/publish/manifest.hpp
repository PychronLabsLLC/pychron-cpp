#pragma once

// The manifest of a release directory (data publishing design, section
// 5.7): what the release is, how it was built, what it pins, and every table
// and file with its hash. verify() is section 5.8.

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "pychron/core/error.hpp"
#include "pychron/core/sha256.hpp"
#include "pychron/persistence/ids.hpp"
#include "pychron/publish/release.hpp"

namespace pychron::publish {

inline constexpr std::string_view kManifestFormat = "pychron-release/1";
inline constexpr std::string_view kManifestFile = "manifest.json";

struct ManifestRelease {
  Uuid uuid;
  std::string name;
  int version = 1;
  std::string title;
  std::optional<std::string> description;
  std::string state, visibility, licence;
  std::vector<Author> authors;
  std::optional<std::string> publisher, doi;
  std::optional<Uuid> supersedes;
  std::optional<std::string> supersedes_doi;
  UtcTime created;
  std::optional<UtcTime> published, embargo_until;
  friend bool operator==(const ManifestRelease&, const ManifestRelease&) = default;
};

struct ManifestBuild {
  UtcTime built;
  std::string software;             // "pychron-cpp/<version>"
  std::string reduction_version;    // reduction::kReductionVersion
  std::string constants_preset;
  std::string options_json;         // the release options, canonical JSON
  std::string store_kind;           // sqlite | postgresql | ""
  int schema_version = 0;
  std::string store_id;             // first 16 hex of SHA-256 of the store url
  bool private_build = false;
  std::size_t omitted_samples = 0, omitted_analyses = 0;
  friend bool operator==(const ManifestBuild&, const ManifestBuild&) = default;
};

struct ManifestAnalysisPins {
  Uuid uuid;
  std::string runid;
  std::optional<std::string> record_sha256;  // hex
  std::map<std::string, std::string> heads;  // kind name -> revision uuid
  std::map<std::string, std::string> refs;   // ref object uuid -> revision uuid
  friend bool operator==(const ManifestAnalysisPins&, const ManifestAnalysisPins&) = default;
};

struct ManifestPins {
  Uuid bookmark;
  std::vector<ManifestAnalysisPins> analyses;
  std::vector<std::pair<Uuid, Uuid>> interpreted_ages;  // (ia, revision)
  std::vector<Uuid> samples;
  friend bool operator==(const ManifestPins&, const ManifestPins&) = default;
};

struct ManifestTable {
  std::string path;
  std::uint64_t rows = 0;
  Sha256Digest rows_sha256{};
  std::vector<std::string> columns;
  friend bool operator==(const ManifestTable&, const ManifestTable&) = default;
};

struct ManifestFile {
  std::string path;
  std::uint64_t bytes = 0;
  Sha256Digest sha256{};
  std::string media_type;
  friend bool operator==(const ManifestFile&, const ManifestFile&) = default;
};

struct Manifest {
  std::string format{kManifestFormat};
  ManifestRelease release;
  ManifestBuild build;
  ManifestPins pins;
  std::vector<ManifestTable> tables;
  std::vector<ManifestFile> files;  // every file but manifest.json, by path
  friend bool operator==(const Manifest&, const Manifest&) = default;
};

// Keys in the order of spec 5.7, two-space indent, "\n" line ends, no
// trailing spaces: the same manifest gives the same bytes.
std::string manifest_json(const Manifest& manifest);
Result<Manifest> parse_manifest(std::string_view json);
Result<Manifest> read_manifest(const std::filesystem::path& dir);

// The media type of a release file by its path ("application/vnd.apache.parquet", ...).
std::string_view media_type_of(std::string_view path);

struct VerifyReport {
  std::vector<std::string> missing, changed, extra;  // paths
  std::optional<std::string> reduction_version_differs;
  bool ok() const { return missing.empty() && changed.empty() && extra.empty() && !reduction_version_differs; }
};

// files[] against the directory: a listed file that is gone, one whose size
// or hash differs, one on disk that is not listed (manifest.json excepted).
Result<VerifyReport> verify_files(const std::filesystem::path& dir);
// The directory's manifest against a rebuild of the same release: when the
// reduction versions differ only that is reported; else every table's
// rows_sha256 and every non-Parquet file's hash, by path.
Result<VerifyReport> verify_rebuild(const std::filesystem::path& dir, const Manifest& rebuilt);

}  // namespace pychron::publish
