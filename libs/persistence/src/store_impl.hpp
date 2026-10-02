#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pychron/persistence/store.hpp"
#include "tiny/db.hpp"

namespace pychron::persistence::detail {

// ---------------------------------------------------------------- change cursor (9.1)

struct ChangeEntityRow {
  QString entity_type;  // analysis | ref_object | interpreted_age | app_user | client | ...
  Uuid entity;
  QString op = QStringLiteral("upsert");
  std::optional<std::string> detail_json;  // catalog edits: {"field": [old, new]} (D6)
};

// The last statements of every writing transaction: bump change_counter (the
// row lock orders commits), write change_log and change_entity, and NOTIFY on
// PostgreSQL. Returns the new change_seq.
Result<ChangeSeq> take_change(Db& db, const QString& kind, std::optional<Uuid> changeset, Uuid client,
                              const std::vector<ChangeEntityRow>& entities);

// ---------------------------------------------------------------- rows

Result<void> insert_changeset(Db& db, const ChangesetInfo& changeset);
Result<void> insert_revision(Db& db, Uuid revision, Uuid changeset, Uuid subject, Kind kind,
                             std::optional<Uuid> parent, UtcTime created);
Result<void> insert_head_move(Db& db, Uuid changeset, Uuid subject, Kind kind, std::optional<Uuid> from, Uuid to,
                              MoveReason reason);
Result<std::optional<ChangesetInfo>> changeset_of_revision(Db& db, Uuid revision);

// `subject` is needed to check a RefPayload against its object's ref_type.
Result<void> write_payload(Db& db, Uuid revision, Uuid subject, const RevisionPayload& payload);
// Content-addressed script text (insert-or-ignore). Returns its SHA-256.
Result<Sha256Digest> put_script_text(Db& db, const std::string& body);
Result<RevisionPayload> read_payload(Db& db, Uuid revision, Kind kind);

// ---------------------------------------------------------------- helpers

std::string json_string(std::string_view s);
// {"field": [null, "value"], ...} for a catalog insert.
std::string json_created(const std::vector<std::pair<std::string, std::optional<std::string>>>& fields);

QString entity_type_of(SubjectType type);

// Ingest (ingest.cpp).
Result<IngestAck> ingest_item(Db& db, const IngestItem& item);

// Find-or-create by unique name inside the caller's transaction; a created row
// is appended to `created` for the change log.
Result<Uuid> ensure_user_row(Db& db, const std::string& name, std::vector<ChangeEntityRow>& created);

std::unique_ptr<IUnitOfWork> make_unit_of_work(Db& db, const Actor& actor);

// Groups, repositories, bookmarks, collection rollback (collections.cpp).
Result<void> add_repository_members(Db& db, const Actor& actor, Uuid repository, const std::vector<Uuid>& analyses);
Result<Uuid> create_group(Db& db, const Actor& actor, const std::string& name, const std::vector<Uuid>& analyses);
Result<Uuid> create_bookmark(Db& db, const Actor& actor, const BookmarkSpec& spec);
Result<std::vector<HeadInfo>> bookmark_heads(Db& db, Uuid bookmark);
Result<CommitOutcome> restore_bookmark(Db& db, const Actor& actor, Uuid bookmark, std::string message);
Result<CommitOutcome> rollback_to_collection(Db& db, const Actor& actor, Uuid analysis, std::string message,
                                             std::vector<Kind> kinds);

// Reference resolution and the derived cache (refs.cpp).
Result<RefResolution> resolve_refs(Db& db, Uuid analysis, const RefPolicy& policy);
Result<Sha256Digest> input_fingerprint(Db& db, Uuid analysis, const std::string& reduction_version);
Result<void> put_derived(Db& db, Uuid analysis, const Sha256Digest& fingerprint, const std::string& reduction_version,
                         const std::vector<DerivedRow>& rows);
Result<std::optional<std::vector<DerivedRow>>> get_derived(Db& db, Uuid analysis, const std::string& reduction_version);
Result<int> prune_derived(Db& db, Uuid analysis);

Result<std::vector<HeadInfo>> read_heads(Db& db, Uuid subject);

// Browsing reads (browse.cpp).
Result<BrowseResult> browse(Db& db, Dialect dialect, const BrowseRequest& request);
Result<std::vector<std::string>> facet(Db& db, Dialect dialect, BrowseFacet facet, const BrowseFilter& filter);
Result<std::optional<AnalysisDetail>> load_analysis_detail(Db& db, Dialect dialect, Uuid analysis);
Result<std::optional<BlobData>> load_blob(Db& db, const Sha256Digest& sha);
Result<ChangeSeq> latest_change_seq(Db& db);

}  // namespace pychron::persistence::detail
