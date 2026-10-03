#pragma once

// The source adapter for a legacy Python-pychron project repository (legacy
// ingestion spec, sections 4.4 and 10). It walks the whole history of one
// branch and turns it into import batches:
//
//   - one collection per analysis, folded from the commits that first add its
//     record, intercepts, baselines, blanks and IC factors;
//   - one changeset per later commit, with a revision per changed file; a
//     merge also carries the files it moved away from what was last imported;
//   - a rewritten record or satellite file as an identity revision (run id
//     changed) and a note of what changed in its commit's provenance;
//   - interpreted ages and frozen productions as revisions of their own
//     subjects, spectrometer settings as the snapshot of the analyses that
//     name them, one bookmark per git tag;
//   - a conflict for every file that cannot be imported.
//
// It reads through GitReader and never touches the store; ingest::BatchWriter
// writes what it produces. The layout it understands is described in
// tests/dvc/fixtures/README.md.

#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "pychron/core/error.hpp"
#include "pychron/dvc/git_reader.hpp"
#include "pychron/ingest/adapter.hpp"
#include "pychron/persistence/ids.hpp"

namespace pychron::dvc {

inline constexpr int kDefaultCollectionWaitCommits = 20;

struct ProjectAdapterConfig {
  GitConfig git;                // the repository and branch to read
  std::string url;              // the source's url as registered; ids are derived from its normalized form
  std::string repository_name;  // every analysis seen becomes a member of this repository
  std::string lab_time_zone;    // IANA; legacy timestamps are naive local time
  // Commits per batch. A batch also ends after a tagged commit and after a
  // commit that rewrites the record of an analysis already imported. How the
  // walk is cut does not change what is stored.
  int batch_commits = 500;
  // An analysis whose collection is still incomplete this many commits after
  // the commit of its record is folded with the files it has; files that
  // arrive later are ordinary revisions. Less than 1: it waits for the end of
  // the walk.
  int collection_wait_commits = kDefaultCollectionWaitCommits;
  // No catalog dump: each record also yields the catalog rows its fields
  // imply, and one identity_clash conflict {"synthesized": true} per
  // identifier made up that way.
  bool catalog_from_repos = false;
  // The tag of an analysis that has no tags file (later legacy versions kept
  // tags only in the database). Empty, or nullopt from it: the default tag.
  std::function<std::optional<std::string>(const persistence::Uuid& analysis)> tag_lookup;
};

class ProjectRepoAdapter final : public ingest::ISourceAdapter {
 public:
  // Opens the repository (GitReader::open); the branch head is fixed here.
  static Result<std::unique_ptr<ProjectRepoAdapter>> open(ProjectAdapterConfig config);
  ~ProjectRepoAdapter() override;

  Result<ingest::SourceDescription> describe() override;
  // Without a token the walk starts at the first commit. A token names a
  // commit, its place in the walk order and a hash of the commits before it.
  // All three as they were: the walk resumes after it. Commit in the history
  // at another place or after other commits, or a token in an older format:
  // the walk starts again from the first commit. Commit not in the history:
  // an error whose message contains "history was rewritten". Returns the
  // commits to walk.
  Result<int> plan(std::optional<std::string> resume_token, ingest::IImportState& state) override;
  Result<std::optional<ingest::ImportBatch>> next_batch() override;

 private:
  class Impl;
  explicit ProjectRepoAdapter(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace pychron::dvc
