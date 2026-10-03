#pragma once

// The two halves of the project repository adapter (project_adapter.hpp).
//
// Walk (project_walk.cpp) looks only at paths and blob shas. It decides what
// each changed file is: part of an analysis's collection, a later change, or
// nothing. It reads no file, so replaying it over history that an earlier run
// already imported costs one diff listing.
//
// Mapper (project_map.cpp) reads the files the walk selected and turns them
// into batch items. Everything that depends on content is decided there: the
// uuid of an analysis, whether it is this source's to import, what cannot be
// parsed.

#include <functional>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "pychron/core/error.hpp"
#include "pychron/dvc/git_reader.hpp"
#include "pychron/dvc/legacy_layout.hpp"
#include "pychron/dvc/project_adapter.hpp"
#include "pychron/ingest/adapter.hpp"
#include "pychron/ingest/batch.hpp"

namespace pychron::dvc::detail {

// A file as one commit left it. `index` is the commit's place in the walk.
struct FileRef {
  int index = -1;
  std::string commit, path, blob_sha;
};

struct SeenFile {
  FileKind kind = FileKind::Unknown;
  FileRef ref;
  // For a record or satellite file that replaces an earlier version: that version.
  std::optional<FileRef> previous = std::nullopt;
  bool restored = false;  // see Change
};

// The files of one analysis: everything that shares a path key.
struct Track {
  std::string key;
  bool key_is_uuid = false;

  // The collection: the first add of each file, held until the analysis is
  // complete (record, intercepts, baselines, blanks and IC factors all seen),
  // the bounded wait runs out, or the walk ends.
  std::optional<FileRef> record, data, intercepts, baselines, blanks, icfactors, tags;
  std::vector<SeenFile> satellites;  // extraction, peak center, monitor: the first of each
  // Seen while pending and not part of the collection: a second version of a
  // file, or a kind that has no root. In walk order.
  std::vector<SeenFile> later;
  bool flushed = false;  // the collection was handed to the mapper, now or by an earlier run
  // The record and each satellite file as last seen. They are not revisioned;
  // a rewrite is reported with what changed against the version before it.
  std::vector<SeenFile> latest;

  // Set by the mapper once it has read the record.
  enum class Role {
    Unresolved,
    Imported,  // this source's analysis: later changes are revisions of it
    Foreign,   // in the store from another source, or a second copy here: membership only
    Broken     // could not be imported: its files are conflicts
  };
  Role role = Role::Unresolved;
  persistence::Uuid uuid;

  bool complete() const { return record && intercepts && baselines && blanks && icfactors; }
  // The collection slot of a kind; null for a kind that has none.
  std::optional<FileRef>* root_slot(FileKind kind);
  // Records `ref` as the latest version of a record or satellite file and
  // returns the version it replaces.
  std::optional<FileRef> replace_latest(FileKind kind, const FileRef& ref);
};

// The collection of `track` is ready to be folded into one analysis.
struct Collect {
  Track* track = nullptr;
  // The record as it is when the collection is folded, when it was rewritten
  // while the analysis was pending: the analysis is imported under the run
  // identity it has by then.
  std::optional<FileRef> record_now = std::nullopt;
};

// A file to import on its own: a later change of an analysis file (`track`
// set), or an interpreted age, frozen production, spectrometer file or
// unknown path (`track` null). A change whose track was never flushed is a
// file whose analysis has no record in this repository.
struct Change {
  PathInfo info;
  FileRef ref;
  Track* track = nullptr;
  std::optional<FileRef> previous = std::nullopt;  // see SeenFile
  // Seen while its analysis was pending and handed over with the collection:
  // it took effect in the fold, not at its own commit.
  bool held = false;
  // The path was removed and is back with the content it had when it was
  // removed: nothing changed. This is known from the walk alone, so it is the
  // same in a replay, where the store's head already reflects later commits.
  bool restored = false;
};

using Work = std::variant<Collect, Change>;

// What became of the files a walk was shown, written down only while the
// adapter lists its units (ISourceAdapter::for_each_unit). The walk notes
// every change it is given; the mapper notes the files it takes in without a
// row of their own. Everything else a file becomes is in the batch.
struct Ledger {
  enum class Seen {
    Taken,     // handed on: part of a collection, or a change
    Deleted,
    Repeated,  // the path already has this blob
    Ignored    // not a file the import reads
  };
  struct Change {
    std::string commit, path, blob_sha;
    Seen seen = Seen::Taken;
  };
  struct Silent {
    std::string commit, path;
    ingest::UnitDisposition disposition = ingest::UnitDisposition::Folded;
    ingest::Evidence evidence;
  };
  std::vector<Change> changes;  // in walk order
  std::vector<Silent> silent;
};

class Walk {
 public:
  // `wait`: an analysis still pending this many commits after its record's
  // commit is folded with what it has (less than 1: never).
  explicit Walk(int wait = 0) : wait_(wait) {}

  // Notes every change apply() is given in `ledger` (null: stop). The ledger
  // outlives the walk or is taken away first.
  void observe(Ledger* ledger) { ledger_ = ledger; }

  // Applies the changes of commit `index`, all of them at once (git lists the
  // files of one commit in path order, so a record can follow its own
  // intercepts). For a merge the caller adds what the merge kept of its other
  // parents. With `out` null only the state moves. Returns whether the commit
  // rewrote the record of an analysis already folded: that can change a run
  // identity, which must be stored before anything later is checked against
  // it, so a batch ends there.
  bool apply(int index, std::span<const GitChange> changes, std::vector<Work>* out);

  // After replaying, up to the head it had, a walk that an earlier run
  // finished: the analyses still pending there were folded by that run.
  void assume_written();

  // The pending analyses, earliest record first.
  std::vector<Track*> pending() const;
  // End of walk: folds a pending analysis with what it has.
  void force(Track& track, std::vector<Work>& out);
  // End of walk: the files of analyses that never got a record.
  void orphans(std::vector<Work>& out);

  const std::vector<Track*>& flushed() const { return flushed_; }
  // The file that holds spectrometer settings `sha1`, as last seen.
  const FileRef* spectrometer(const std::string& sha1) const;

 private:
  void flush(Track& track, std::vector<Work>* out);

  int wait_ = 0;
  Ledger* ledger_ = nullptr;

  std::map<std::string, Track> tracks_;  // by path key; nodes do not move
  // The blob each path was last seen with, and whether the path is still
  // there. A change that brings a path to the blob it already has (a merge
  // repeating a side branch) is skipped; one that brings a removed path back
  // with the blob it had is handed on as `restored`.
  struct LastSeen {
    std::string blob_sha;
    bool present = true;
  };
  std::unordered_map<std::string, LastSeen> last_blob_;
  std::map<std::string, FileRef> spectrometers_;
  std::set<std::pair<int, Track*>> pending_;  // (record commit index, track)
  std::vector<Track*> flushed_;
};

class Mapper {
 public:
  // `config`, `reader` and `walk` outlive the mapper. `url` is normalized.
  Mapper(const ProjectAdapterConfig& config, std::string url, GitReader& reader, const Walk& walk,
         ingest::IImportState& state);

  // Notes in `ledger` the files taken in without a row of their own (null:
  // stop). The ledger outlives the mapper or is taken away first.
  void observe(Ledger* ledger) { ledger_ = ledger; }

  // Commit metadata the mapper will need; anything else is fetched on demand.
  void remember(std::vector<GitCommit> commits);

  // Turns the work of one batch into its items. Tracks are updated: role,
  // uuid, and the collection files of a folded track are released.
  Result<void> map(const std::vector<Work>& work, ingest::ImportBatch& batch);

  // A bookmark of every analysis imported so far, for a tag on a commit the
  // batch ends at.
  Result<void> bookmark(const GitTag& tag, ingest::ImportBatch& batch);

 private:
  struct Reading;  // the records of one batch, parsed
  struct Output;   // one batch being built

  // For the ledger: `ref` has no row of its own; `evidence` is where its
  // content is.
  void silent(const FileRef& ref, ingest::UnitDisposition disposition, ingest::Evidence evidence);

  Result<const GitCommit*> commit(const std::string& sha);
  Result<void> fetch(std::vector<std::string> blob_shas);
  Result<void> read_records(const std::vector<Collect>& folds, Reading& reading);
  Result<void> resolve(const std::vector<Track*>& tracks);

  Result<void> collect(const Collect& fold, Reading& reading, Output& out);
  Result<void> change(const Change& item, Output& out);
  Result<void> analysis_change(const Change& item, std::string_view text, Output& out);
  Result<void> rewritten(const Change& item, std::string_view text, Output& out);
  Result<ingest::ChangesetItem*> changeset_of(const FileRef& ref, Output& out);
  Result<void> add_revision(const FileRef& ref, ingest::SubjectRef subject, persistence::Kind kind,
                            persistence::RevisionPayload payload, std::string detail_json, Output& out,
                            std::string identifier = {});
  // Whether the store's head of (subject, kind) already has this blob.
  bool unchanged(const Change& item);
  Result<std::optional<persistence::SpectrometerSnapshot>> snapshot(const std::string& sha1);
  Result<void> synthesize_catalog(const ParsedRecord& record, const persistence::AnalysisIngest& analysis,
                                  const FileRef& from, Output& out);

  const ProjectAdapterConfig& config_;
  std::string url_;
  GitReader& reader_;
  const Walk& walk_;
  ingest::IImportState& state_;
  ParseContext context_;
  Ledger* ledger_ = nullptr;

  std::unordered_map<std::string, GitCommit> commits_;
  // Spectrometer settings by sha1: parsed, or nullopt when the file is bad.
  std::map<std::string, std::optional<persistence::SpectrometerSnapshot>> snapshots_;
  // catalog_from_repos: what was already sent, and which identifier this
  // walk put at each position (the store answers for earlier batches).
  std::set<std::string> sent_;
  std::map<std::string, std::string> positions_;
};

// Turns what a walk and its mapper did with each file into the units the
// verifier checks (ingest/adapter.hpp). A unit is settled in the batch that
// maps its file: from the batch come the rows the writer leaves for it, from
// the ledger where its content is when it has no row of its own. A file that
// repeats the blob its path already has is settled with the unit it repeats.
class UnitAccount {
 public:
  using Visit = std::function<Result<void>(const ingest::SourceUnit&)>;
  // `visit` outlives the account.
  explicit UnitAccount(const Visit& visit) : visit_(visit) {}

  // After a batch is mapped. The ledger is emptied.
  Result<void> settle(Ledger& ledger, const ingest::ImportBatch& batch);
  // After the last batch: what is still open. A spectrometer file no analysis
  // used holds nothing that is imported; anything else was never classified.
  Result<void> finish();

 private:
  using Key = std::pair<std::string, std::string>;  // commit, path
  struct Open {
    ingest::SourceUnit unit;
    std::vector<ingest::SourceUnit> repeats;
    bool folded = false;     // the mapper said where its content is
    bool touched = false;    // it got evidence in the batch being settled
  };
  // The last unit of a path that was handed on, and what it settled as.
  struct Last {
    std::string commit;
    bool settled = false;
    ingest::UnitDisposition disposition = ingest::UnitDisposition::Unclassified;
    std::vector<ingest::Evidence> evidence;
  };

  Result<void> close(Open& open);
  Result<void> repeat(ingest::SourceUnit unit, const Last& last);

  const Visit& visit_;
  std::map<Key, Open> open_;
  std::unordered_map<std::string, Last> last_;
};

}  // namespace pychron::dvc::detail
