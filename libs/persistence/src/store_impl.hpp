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

Result<void> write_payload(Db& db, Uuid revision, const RevisionPayload& payload);
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

}  // namespace pychron::persistence::detail
