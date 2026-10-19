#pragma once

// The steps `elctl publish` and the Publish window share (data publishing
// design, sections 4.4, 5.8 and 6.2): publishing a built directory, and
// verifying one against a rebuild from the store.

#include <filesystem>
#include <optional>
#include <string>

#include "pychron/core/error.hpp"
#include "pychron/persistence/store.hpp"
#include "pychron/processing/store_source.hpp"
#include "pychron/publish/manifest.hpp"
#include "pychron/publish/release.hpp"

namespace pychron::publish {

// Draft -> published from a built directory: reads <dir>/manifest.json and
// refuses (Config, one sentence each) a private build made public, a
// directory that does not match its manifest, and a manifest whose release,
// bookmark or pinned heads are not `release`'s (a name, name@vN or uuid);
// then publish_release with the SHA-256 of the manifest's bytes. Returns the
// release as it now is.
Result<persistence::ReleaseRow> publish_built(persistence::IStore& store, const persistence::Actor& actor,
                                              const std::string& release, const std::filesystem::path& dir,
                                              bool make_public);

struct StoreVerify {
  VerifyReport files;    // the directory against its manifest
  VerifyReport rebuild;  // the manifest against a rebuild from the store
  bool ok() const { return files.ok() && rebuild.ok(); }
};

// verify_files, then a rebuild of the manifest's release (its private flag,
// figures when the directory has them) into a scratch directory under the
// system's temporary directory, removed after, compared with verify_rebuild.
// With `expected`, a directory built from another release is refused
// (Config, naming the release it was built from).
Result<StoreVerify> verify_against_store(persistence::IStore& store, processing::StoreSource& source,
                                         const std::filesystem::path& dir, const std::string& store_url,
                                         std::optional<Uuid> expected = std::nullopt);

}  // namespace pychron::publish
