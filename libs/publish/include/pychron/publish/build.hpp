#pragma once

// Building a release directory from the store (data publishing design,
// sections 5 and 6.1): the members at their pins, reduced and grouped as
// `elctl export` does, the Schaen tables, the row tables, the records
// catalog, the licence, citation and readme, the figures when asked for,
// the Parquet tables when the
// build has Arrow, and the manifest last. Every file written goes through
// mark_as_user_file.

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pychron/core/error.hpp"
#include "pychron/persistence/ids.hpp"
#include "pychron/persistence/store.hpp"
#include "pychron/processing/store_source.hpp"
#include "pychron/publish/manifest.hpp"
#include "pychron/publish/tables.hpp"

namespace pychron::publish {

// The reduction and report options a release is built with (`release.options`),
// the ones of `elctl export`. Stored as JSON with these keys.
struct ReleaseOptions {
  int sigma = 2;
  std::string constants = "default";  // default | legacy | legacy_preferences
  bool decay_error = false;
  std::string plateau = "fleck";  // fleck | mahon
  int plateau_steps = 3;
  double plateau_gas = 50.0;
  std::string group_by = "auto";  // auto | aliquot | identifier | sample | none
  std::string laboratory;
  std::vector<std::string> notes;
  friend bool operator==(const ReleaseOptions&, const ReleaseOptions&) = default;
};
std::string release_options_json(const ReleaseOptions& options);
Result<ReleaseOptions> parse_release_options(std::string_view json);

struct BuildOptions {
  std::filesystem::path out;  // the directory the release directory is made in
  bool private_build = false; // keep embargoed rows; the manifest says so
  bool with_figures = false;  // figures/<sample>/*.svg|pdf (figures.hpp)
  bool with_viewer = false;   // accepted, not yet (phase 3)
  std::string store_url;      // for manifest.build.store (kind and id); may be empty
};

class IProgress {
 public:
  virtual ~IProgress() = default;
  virtual void stage(std::string_view name) = 0;  // select, load, reduce, tables, files, figures, parquet, records, manifest
};

struct BuildResult {
  Manifest manifest;
  std::filesystem::path dir;  // <out>/<name>-v<version>
  bool parquet_written = false;
  std::vector<std::string> warnings;
};

// Config when the release does not exist or the target directory exists and
// is not empty; Io when a file cannot be written.
Result<BuildResult> build_release(persistence::IStore& store, processing::StoreSource& source, Uuid release,
                                  const BuildOptions& options, IProgress* progress);

// The directory a build of `release` makes under `out`: <out>/<name>-v<version>.
std::filesystem::path release_dir(const std::filesystem::path& out, const persistence::ReleaseRow& release);

}  // namespace pychron::publish
