#pragma once

// Import bookkeeping types (legacy ingestion spec; DVC schema spec, section
// 13): the import source, per-entity provenance, and the conflicts an import
// leaves for an admin, and the unit of work an importer writes through.
// std-only, like the rest of the public headers.

#include <optional>
#include <string>
#include <vector>

#include "pychron/core/error.hpp"
#include "pychron/core/sha256.hpp"
#include "pychron/persistence/ids.hpp"
#include "pychron/persistence/model.hpp"

namespace pychron::persistence {

enum class ImportSourceKind { ProjectRepo, MetaRepo, LegacyDb };  // 'project_repo' | 'meta_repo' | 'legacy_db'
enum class ConflictKind { HandEdit, UnknownAnalysis, ValueMismatch, Unparseable, IdentityClash, ProvisionalRenumber };

// Stored spellings (the import_source.kind and import_conflict.conflict_kind columns).
std::string_view to_string(ImportSourceKind kind) noexcept;
std::optional<ImportSourceKind> parse_import_source_kind(std::string_view text) noexcept;
std::string_view to_string(ConflictKind kind) noexcept;
std::optional<ConflictKind> parse_conflict_kind(std::string_view text) noexcept;

struct ImportSourceSpec {
  Uuid uuid;  // caller-derived
  ImportSourceKind kind = ImportSourceKind::ProjectRepo;
  std::string url_or_path;  // already normalized
  std::optional<std::string> branch;
  std::string importer_version;
  std::string lab_time_zone;  // IANA
};

struct ImportSourceInfo {
  ImportSourceSpec spec;
  std::optional<std::string> head_sha, progress_token;
  int total = 0, done = 0;
  UtcTime started;
  std::optional<UtcTime> finished;
  std::string status;  // registered | running | paused | finished | failed
};

struct ProvenanceRow {
  std::string entity_type;  // analysis | revision | changeset | ref_object | bookmark
  Uuid entity;
  std::string path, commit_sha, git_blob_sha, git_author;
  UtcTime git_utc;
  std::optional<std::string> detail_json;
  // The import source the row belongs to. Set on read; a write takes it from
  // the batch.
  Uuid source = {};
};

struct ImportConflictRow {
  Uuid uuid;  // caller-derived, so a re-run does not duplicate it
  std::string path;
  std::optional<Uuid> entity;
  ConflictKind kind = ConflictKind::HandEdit;
  std::optional<Uuid> db_head_revision;
  std::optional<Sha256Digest> file_sha256;
  std::string detail_json = "{}";
  std::string resolution = "pending";
};

struct ImportProgress {
  std::string token;
  int done = 0, total = 0;
  std::optional<std::string> head_sha;
  std::string status;
};

struct ConflictFilter {
  std::optional<Uuid> source;
  std::optional<ConflictKind> kind;
  std::optional<std::string> resolution;
};

// ---------------------------------------------------------------- import writes

// A revision with a caller-derived id. Its parent is the head of
// (subject, kind) when it is written; the head then moves to it.
struct ImportedRevision {
  Uuid uuid;
  Uuid subject;
  Kind kind = Kind::Intercepts;
  RevisionPayload payload;
};

// A changeset with a caller-derived id and the source's own time (a git
// commit's author date), stamped on the changeset and its revisions.
struct ImportedChangeset {
  Uuid uuid;
  ChangesetKind kind = ChangesetKind::Import;  // Import or Reference
  Uuid author_user;
  UtcTime created;
  std::string message;
  std::vector<ImportedRevision> revisions;  // parent = head of (subject, kind) at write time
};

// One import batch. Everything staged is written by commit() in one
// transaction, in the order given; a failure leaves nothing behind, progress
// included. Ids come from the caller, so writing a batch again is a no-op.
class IImportUnitOfWork {
 public:
  virtual ~IImportUnitOfWork() = default;

  // A changeset whose uuid is already stored is not written again, but its
  // revisions still are when they are missing. A revision whose uuid is
  // already stored is skipped whole: no payload, no head move.
  virtual Result<void> add_changeset(ImportedChangeset changeset) = 0;
  // One row per (entity_type, entity, source); an existing row is kept.
  virtual Result<void> add_provenance(ProvenanceRow row) = 0;
  virtual Result<void> add_conflict(ImportConflictRow row) = 0;
  // Replaces the detail of this source's provenance row of (entity_type,
  // entity), after this batch's own rows are written (import_provenance is an
  // updatable table). A row that does not exist is left alone. Not a
  // change_log entry.
  virtual Result<void> set_provenance_detail(std::string entity_type, Uuid entity, std::string detail_json) = 0;
  // Sets the resolution of a stored conflict (import_conflict is an updatable
  // table), after this batch's own conflicts are written. A conflict that
  // does not exist is left alone. Not a change_log entry.
  virtual Result<void> resolve_conflict(Uuid conflict, std::string resolution) = 0;
  // Replaces a stored conflict of this source with `row`: its path, entity,
  // kind, head revision, file hash, detail and resolution (a conflict that
  // says something else is not resolved by what was decided about the old
  // one). Applied after this batch's own conflicts are written; a conflict
  // that does not exist is left alone. Not a change_log entry.
  virtual Result<void> restate_conflict(ImportConflictRow row) = 0;
  // The source's resume token and counters. A nullopt head_sha keeps the
  // stored one; status "finished" also stamps the finish time.
  virtual Result<void> set_progress(ImportProgress progress) = 0;
  // One transaction, one change_log entry. Rows whose uuid already exists are
  // skipped; a batch that adds no changeset and no revision adds no
  // change_log entry and returns the current change_seq.
  virtual Result<ChangeSeq> commit() = 0;
};

}  // namespace pychron::persistence
