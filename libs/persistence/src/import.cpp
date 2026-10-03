// Import source, provenance and conflict bookkeeping (legacy ingestion spec).
// Reads only here plus begin_import; the writes that record provenance,
// conflicts and progress belong to the import unit of work.

#include "sql/statements.hpp"
#include "store_impl.hpp"

namespace pychron::persistence {

namespace {
struct KindName {
  ImportSourceKind kind;
  std::string_view name;
};
constexpr KindName kSourceKinds[] = {{ImportSourceKind::ProjectRepo, "project_repo"},
                                     {ImportSourceKind::MetaRepo, "meta_repo"},
                                     {ImportSourceKind::LegacyDb, "legacy_db"}};
struct ConflictName {
  ConflictKind kind;
  std::string_view name;
};
constexpr ConflictName kConflictKinds[] = {{ConflictKind::HandEdit, "hand_edit"},
                                           {ConflictKind::UnknownAnalysis, "unknown_analysis"},
                                           {ConflictKind::ValueMismatch, "value_mismatch"},
                                           {ConflictKind::Unparseable, "unparseable"},
                                           {ConflictKind::IdentityClash, "identity_clash"},
                                           {ConflictKind::ProvisionalRenumber, "provisional_renumber"}};
}  // namespace

std::string_view to_string(ImportSourceKind kind) noexcept {
  for (const auto& k : kSourceKinds)
    if (k.kind == kind) return k.name;
  return "project_repo";
}
std::optional<ImportSourceKind> parse_import_source_kind(std::string_view text) noexcept {
  for (const auto& k : kSourceKinds)
    if (k.name == text) return k.kind;
  return std::nullopt;
}
std::string_view to_string(ConflictKind kind) noexcept {
  for (const auto& k : kConflictKinds)
    if (k.kind == kind) return k.name;
  return "hand_edit";
}
std::optional<ConflictKind> parse_conflict_kind(std::string_view text) noexcept {
  for (const auto& k : kConflictKinds)
    if (k.name == text) return k.kind;
  return std::nullopt;
}

namespace detail {
namespace {

QString qstr(std::string_view s) { return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size())); }

QString source_columns(Dialect d) {
  return sql::kImportSourceColumns.arg(sql::ts(d, QStringLiteral("started_utc")),
                                       sql::ts(d, QStringLiteral("finished_utc")));
}

Result<ImportSourceInfo> source_from(const Row& r) {
  ImportSourceInfo info;
  info.spec.uuid = to_uuid(r.value("uuid"));
  const auto kind = parse_import_source_kind(to_std(r.value("kind")));
  if (!kind) return fail(ErrorKind::Protocol, "unknown import source kind '" + to_std(r.value("kind")) + "'");
  info.spec.kind = *kind;
  info.spec.url_or_path = to_std(r.value("url_or_path"));
  info.spec.branch = opt_str(r.value("branch"));
  info.spec.importer_version = to_std(r.value("importer_version"));
  info.spec.lab_time_zone = to_std(r.value("lab_time_zone"));
  info.head_sha = opt_str(r.value("head_commit_sha"));
  info.progress_token = opt_str(r.value("progress_commit_sha"));
  info.total = opt_int(r.value("commits_total")).value_or(0);
  info.done = opt_int(r.value("commits_done")).value_or(0);
  info.started = to_time(r.value("started"));
  if (!r.value("finished").isNull()) info.finished = to_time(r.value("finished"));
  info.status = to_std(r.value("status"));
  return info;
}

}  // namespace

Result<ImportSourceInfo> begin_import(Db& db, Dialect dialect, const ImportSourceSpec& spec) {
  WriteTx tx(db);
  if (auto r = tx.begin(); !r) return fail(r.error());
  Row row;
  row["uuid"] = qv(spec.uuid);
  row["kind"] = qstr(to_string(spec.kind));
  row["url_or_path"] = qv(spec.url_or_path);
  row["branch"] = qv(spec.branch);
  row["commits_total"] = 0;
  row["commits_done"] = 0;
  row["importer_version"] = qv(spec.importer_version);
  row["lab_time_zone"] = qv(spec.lab_time_zone);
  row["started_utc"] = qv(UtcTime::now());
  row["status"] = QStringLiteral("registered");
  if (auto ins = db.insert_or_ignore("import_source", row); !ins) return fail(ins.error());
  auto stored = db.select_one(sql::kImportSourceByUuid.arg(source_columns(dialect)), {qv(spec.uuid)});
  if (!stored) return fail(stored.error());
  if (!*stored) return fail(ErrorKind::Protocol, "import source vanished after insert");
  auto info = source_from(**stored);
  if (!info) return fail(info.error());
  if (auto c = tx.commit(); !c) return fail(c.error());
  return info;
}

Result<std::vector<ImportSourceInfo>> import_sources(Db& db, Dialect dialect) {
  auto rows = db.select(sql::kImportSources.arg(source_columns(dialect)));
  if (!rows) return fail(rows.error());
  std::vector<ImportSourceInfo> out;
  for (const auto& r : *rows) {
    auto info = source_from(r);
    if (!info) return fail(info.error());
    out.push_back(std::move(*info));
  }
  return out;
}

Result<std::vector<ImportConflictRow>> import_conflicts(Db& db, const ConflictFilter& filter) {
  QStringList where;
  Bindings b;
  if (filter.source) {
    where << QStringLiteral("import_source_uuid = ?");
    b << qv(*filter.source);
  }
  if (filter.kind) {
    where << QStringLiteral("conflict_kind = ?");
    b << qstr(to_string(*filter.kind));
  }
  if (filter.resolution) {
    where << QStringLiteral("resolution = ?");
    b << qv(*filter.resolution);
  }
  QString sql = sql::kImportConflicts;
  if (!where.isEmpty()) sql += QStringLiteral(" WHERE ") + where.join(QStringLiteral(" AND "));
  sql += QStringLiteral(" ORDER BY path, uuid");
  auto rows = db.select(sql, b);
  if (!rows) return fail(rows.error());
  std::vector<ImportConflictRow> out;
  for (const auto& r : *rows) {
    ImportConflictRow c;
    c.uuid = to_uuid(r.value("uuid"));
    c.path = to_std(r.value("path"));
    c.entity = opt_uuid(r.value("entity_uuid"));
    const auto kind = parse_conflict_kind(to_std(r.value("conflict_kind")));
    if (!kind) return fail(ErrorKind::Protocol, "unknown conflict kind '" + to_std(r.value("conflict_kind")) + "'");
    c.kind = *kind;
    c.db_head_revision = opt_uuid(r.value("db_head_revision_uuid"));
    if (!r.value("file_sha256").isNull()) c.file_sha256 = to_digest(r.value("file_sha256"));
    c.detail_json = opt_str(r.value("detail")).value_or("{}");
    c.resolution = to_std(r.value("resolution"));
    out.push_back(std::move(c));
  }
  return out;
}

Result<std::vector<ProvenanceRow>> provenance_for(Db& db, Dialect dialect, Uuid entity) {
  auto rows = db.select(sql::kProvenanceFor.arg(sql::ts(dialect, QStringLiteral("git_utc"))), {qv(entity)});
  if (!rows) return fail(rows.error());
  std::vector<ProvenanceRow> out;
  for (const auto& r : *rows) {
    ProvenanceRow p;
    p.entity_type = to_std(r.value("entity_type"));
    p.entity = to_uuid(r.value("entity_uuid"));
    p.path = to_std(r.value("path"));
    p.commit_sha = to_std(r.value("commit_sha"));
    p.git_blob_sha = to_std(r.value("git_blob_sha"));
    p.git_author = to_std(r.value("git_author"));
    p.git_utc = to_time(r.value("git_ts"));
    p.detail_json = opt_str(r.value("detail"));
    out.push_back(std::move(p));
  }
  return out;
}

Result<bool> has_provenance(Db& db, Uuid source, std::string_view commit_sha, std::string_view path) {
  auto row = db.select_one(sql::kHasProvenance, {qv(source), qstr(commit_sha), qstr(path)});
  if (!row) return fail(row.error());
  return row->has_value();
}

Result<bool> has_provenance_blob(Db& db, Uuid source, std::string_view path, std::string_view git_blob_sha) {
  auto row = db.select_one(sql::kHasProvenanceBlob, {qv(source), qstr(path), qstr(git_blob_sha)});
  if (!row) return fail(row.error());
  return row->has_value();
}

Result<bool> has_conflict(Db& db, Uuid source, std::string_view path, const Sha256Digest& file_sha256) {
  auto row = db.select_one(sql::kHasConflict, {qv(source), qstr(path), qv(file_sha256)});
  if (!row) return fail(row.error());
  return row->has_value();
}

Result<std::optional<std::string>> imported_head_blob_sha(Db& db, Uuid source, Uuid subject, Kind kind) {
  auto row = db.select_one(sql::kImportedHeadBlobSha, {qv(source), qv(subject), qstr(to_string(kind))});
  if (!row) return fail(row.error());
  if (!*row) return std::optional<std::string>{};
  return opt_str((*row)->value("git_blob_sha"));
}

}  // namespace detail
}  // namespace pychron::persistence
