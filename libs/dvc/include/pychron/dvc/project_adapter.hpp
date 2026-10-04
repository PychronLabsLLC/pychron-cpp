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
//   - a conflict for every file that cannot be imported; a file that could
//     not be read is superseded by the next commit that brings a readable
//     version of it, or deletes it (ImportBatch::superseded, spec 10.37). A
//     bad version after a good one stays;
//   - an analysis whose first record cannot be read starts its collection,
//     as a synthetic one, at the first commit that brings a readable record,
//     with the files the repository has had for it by then.
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
  // arrive later are ordinary revisions. At least 1: open() refuses less (an
  // unbounded wait would hold an incomplete analysis for the whole walk).
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
  // The error plan() gives for a token whose commit is not in the history
  // ("history was rewritten"), without planning. Any other token is accepted:
  // one whose place merely changed makes plan() start from the first commit.
  Result<void> check_token(const std::string& resume_token) override;
  Result<std::optional<ingest::ImportBatch>> next_batch() override;
  // One unit per file a commit adds, changes or deletes (for a merge: as the
  // walk sees it), walked from the first commit as an import walks it:
  //   a file the import does not read (a run log)           Ignored
  //   a deletion                                            Removed, no evidence: the analysis stays
  //   a record; a root file; a later revision; an           Imported: the row at (commit, path) and the analysis
  //   interpreted age; a frozen production                  or revision it is of, or the conflict the writer
  //                                                         left there
  //   a rewritten record or satellite file                  Imported: listed under "rewrites" of its commit
  //                                                         (and the identity revision, when the run id changed)
  //   extraction, peak-center and monitor files folded      Folded: the row of the analysis's record
  //   into an analysis; the spectrometer file an analysis
  //   names; the other files of a membership-only analysis
  //   a second copy of an analysis of this source           Folded: the analysis's provenance row
  //   the blob its path already has (a merge repeating a    Unchanged: the rows of the one earlier unit of the
  //   side), or had when it was removed (a restored file)   path it repeats
  //   a file that cannot be read or belongs to nothing      Conflict
  //   a spectrometer file that comes after an analysis      Conflict (spectrometer_file_after_collection)
  //   naming it was folded
  //   a spectrometer file no imported analysis names        Ignored
  Result<void> for_each_unit(ingest::IImportState& state,
                             const std::function<Result<void>(const ingest::SourceUnit&)>& visit) override;
  // The commit's index in the walk order of the last plan().
  Result<std::optional<std::int64_t>> order_of(std::string_view commit) override;

 private:
  class Impl;
  explicit ProjectRepoAdapter(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace pychron::dvc
