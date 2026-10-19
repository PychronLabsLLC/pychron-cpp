// The steps elctl publish and the Publish window share (data publishing
// design, sections 4.4, 5.8 and 6.2).

#include "pychron/publish/workflow.hpp"

#include <algorithm>
#include <fstream>
#include <random>
#include <sstream>

#include "pychron/core/sha256.hpp"
#include "pychron/publish/build.hpp"
#include "pychron/publish/release.hpp"

namespace pychron::publish {

namespace fs = std::filesystem;
namespace ps = persistence;

Result<ps::ReleaseRow> publish_built(ps::IStore& store, const ps::Actor& actor, const std::string& release,
                                     const fs::path& dir, bool make_public) {
  const fs::path path = dir / kManifestFile;
  std::ifstream in(path, std::ios::binary);
  if (!in) return fail(ErrorKind::Io, "cannot read " + path.string());
  std::stringstream ss;
  ss << in.rdbuf();
  const std::string bytes = ss.str();
  auto manifest = parse_manifest(bytes);
  if (!manifest) return fail(manifest.error());
  if (make_public && manifest->build.private_build)
    return fail(ErrorKind::Config, "this is a private build (it holds embargoed rows): it cannot be published as public");
  auto files = verify_files(dir);
  if (!files) return fail(files.error());
  if (!files->ok())
    return fail(ErrorKind::Config,
                "the directory does not match its manifest (elctl publish verify, or Verify in the Publish window, "
                "says what differs)");
  auto row = find_release(store, release);
  if (!row) return fail(row.error());
  if (manifest->release.uuid != row->uuid || manifest->pins.bookmark != row->bookmark)
    return fail(ErrorKind::Config, "the manifest's pins are not this release's (it was built from " +
                                       manifest->release.name + " v" + std::to_string(manifest->release.version) + ")");
  auto pins = store.bookmark_heads(row->bookmark);
  if (!pins) return fail(pins.error());
  for (const auto& a : manifest->pins.analyses)
    for (const auto& [kind, revision] : a.heads) {
      const bool pinned = std::any_of(pins->begin(), pins->end(), [&](const ps::HeadInfo& h) {
        return h.subject == a.uuid && std::string(ps::to_string(h.kind)) == kind && h.revision.str() == revision;
      });
      if (!pinned)
        return fail(ErrorKind::Config,
                    "the manifest's pins differ from the release's bookmark (" + a.runid + " " + kind + ")");
    }
  auto published = publish_release(store, actor, row->uuid, sha256(std::string_view(bytes)), make_public);
  if (!published) return fail(published.error());
  return find_release(store, row->uuid.str());
}

Result<StoreVerify> verify_against_store(ps::IStore& store, processing::StoreSource& source, const fs::path& dir,
                                         const std::string& store_url, std::optional<Uuid> expected) {
  StoreVerify out;
  auto manifest = read_manifest(dir);
  if (!manifest) return fail(manifest.error());
  if (expected && manifest->release.uuid != *expected)
    return fail(ErrorKind::Config, "this directory is a build of " + manifest->release.name + " v" +
                                       std::to_string(manifest->release.version) + ", not of the release chosen");
  auto files = verify_files(dir);
  if (!files) return fail(files.error());
  out.files = *files;
  const fs::path scratch = fs::temp_directory_path() / ("pychron_verify_" + std::to_string(std::random_device{}()) +
                                                        "_" + std::to_string(std::random_device{}()));
  std::error_code ec;
  fs::create_directories(scratch, ec);
  BuildOptions options;
  options.out = scratch;
  options.private_build = manifest->build.private_build;
  options.with_figures = has_figures(*manifest);  // rebuilt as it was built
  options.store_url = store_url;
  auto rebuilt = build_release(store, source, manifest->release.uuid, options, nullptr);
  if (!rebuilt) {
    fs::remove_all(scratch, ec);
    return fail(rebuilt.error());
  }
  auto against = verify_rebuild(dir, rebuilt->manifest);
  fs::remove_all(scratch, ec);
  if (!against) return fail(against.error());
  out.rebuild = *against;
  return out;
}

}  // namespace pychron::publish
