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
};

// The files of one analysis: everything that shares a path key.
struct Track {
  std::string key;
  bool key_is_uuid = false;

  // The collection: the first add of each file, held until the analysis is
  // complete (record, intercepts, baselines, blanks and IC factors all seen)
  // or the walk ends.
  std::optional<FileRef> record, data, intercepts, baselines, blanks, icfactors, tags;
  std::vector<SeenFile> satellites;  // extraction, peak center, monitor: the first of each
  // Seen while pending and not part of the collection: a second version of a
  // file, or a kind that has no root. In walk order.
  std::vector<SeenFile> later;
  bool flushed = false;  // the collection was handed to the mapper, now or by an earlier run

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
};

// The collection of `track` is ready to be folded into one analysis.
struct Collect {
  Track* track = nullptr;
};

// A file to import on its own: a later change of an analysis file (`track`
// set), or an interpreted age, frozen production, spectrometer file or
// unknown path (`track` null). A change whose track was never flushed is a
// file whose analysis has no record in this repository.
struct Change {
  PathInfo info;
  FileRef ref;
  Track* track = nullptr;
};

using Work = std::variant<Collect, Change>;

class Walk {
 public:
  // Applies the changes of commit `index`, all of them at once (git lists the
  // files of one commit in path order, so a record can follow its own
  // intercepts). With `out` null only the state moves.
  void apply(int index, std::span<const GitChange> changes, std::vector<Work>* out);

  // After replaying the commits an earlier run walked: every analysis whose
  // record is among them was written by that run, complete or not (the resume
  // token never passes a pending analysis, except at the end of a walk, where
  // all of them are written).
  void assume_written();

  // The record's commit index of the earliest analysis still pending.
  std::optional<int> earliest_pending() const;
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

  std::map<std::string, Track> tracks_;  // by path key; nodes do not move
  // The blob each path was last seen with. A change that brings a path to the
  // blob it already has here (a merge repeating a side branch) is skipped.
  std::unordered_map<std::string, std::string> last_blob_;
  std::map<std::string, FileRef> spectrometers_;
  std::set<std::pair<int, Track*>> pending_;  // (record commit index, track)
  std::vector<Track*> flushed_;
};

class Mapper {
 public:
  // `config`, `reader` and `walk` outlive the mapper. `url` is normalized.
  Mapper(const ProjectAdapterConfig& config, std::string url, GitReader& reader, const Walk& walk,
         ingest::IImportState& state);

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

  Result<const GitCommit*> commit(const std::string& sha);
  Result<void> fetch(std::vector<std::string> blob_shas);
  Result<void> read_records(const std::vector<Track*>& tracks, Reading& reading);
  Result<void> resolve(const std::vector<Track*>& tracks);

  Result<void> collect(Track& track, Reading& reading, Output& out);
  Result<void> change(const Change& item, Output& out);
  Result<void> analysis_change(const Change& item, std::string_view text, Output& out);
  Result<void> add_revision(const FileRef& ref, ingest::SubjectRef subject, persistence::Kind kind,
                            persistence::RevisionPayload payload, std::string detail_json, Output& out);
  // Whether the store's head of (subject, kind) already has this blob.
  Result<bool> is_head(const FileRef& ref, const ingest::SubjectRef& subject, persistence::Kind kind, Output& out);
  Result<std::optional<persistence::SpectrometerSnapshot>> snapshot(const std::string& sha1);
  void synthesize_catalog(const ParsedRecord& record, const persistence::AnalysisIngest& analysis,
                          const FileRef& from, Output& out);

  const ProjectAdapterConfig& config_;
  std::string url_;
  GitReader& reader_;
  const Walk& walk_;
  ingest::IImportState& state_;
  ParseContext context_;

  std::unordered_map<std::string, GitCommit> commits_;
  // Spectrometer settings by sha1: parsed, or nullopt when the file is bad.
  std::map<std::string, std::optional<persistence::SpectrometerSnapshot>> snapshots_;
  std::map<persistence::Uuid, const Track*> owners_;  // the track that imported each analysis in this walk
  std::map<std::string, persistence::Uuid> runids_;   // run ids imported in this walk
  // catalog_from_repos: what was already sent, and which identifier sits at each position.
  std::set<std::string> sent_;
  std::map<std::string, std::string> positions_;
};

}  // namespace pychron::dvc::detail
