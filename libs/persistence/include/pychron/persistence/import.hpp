#pragma once

// Import bookkeeping types (legacy ingestion spec; DVC schema spec, section
// 13): the import source, per-entity provenance, and the conflicts an import
// leaves for an admin. std-only, like the rest of the public headers.

#include <optional>
#include <string>
#include <vector>

#include "pychron/core/sha256.hpp"
#include "pychron/persistence/ids.hpp"

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

}  // namespace pychron::persistence
