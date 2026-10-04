// Import source, provenance and conflict bookkeeping, and the import unit of
// work that writes a batch of a source's history (legacy ingestion spec).

#include <set>
#include <utility>

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

Result<ImportConflictRow> conflict_from(const Row& r) {
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
  if (!r.value("resolved").isNull()) c.resolved = to_time(r.value("resolved"));
  return c;
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

namespace {

// One import batch (legacy ingestion spec, section 3.1). Staged in memory,
// written by commit() in one transaction.
class ImportUnitOfWork final : public IImportUnitOfWork {
 public:
  ImportUnitOfWork(Db& db, Uuid source, Uuid client) : db_(db), source_(source), client_(client) {}

  Result<void> add_changeset(ImportedChangeset changeset) override {
    if (auto r = check_open(); !r) return r;
    if (changeset.kind != ChangesetKind::Import && changeset.kind != ChangesetKind::Reference)
      return fail(ErrorKind::Protocol, "an import writes import and reference changesets, not '" +
                                           std::string(to_string(changeset.kind)) + "'");
    for (const auto& rev : changeset.revisions)
      if (!payload_kind_matches(rev.kind, rev.payload))
        return fail(ErrorKind::Protocol,
                    "payload does not match revision kind '" + std::string(to_string(rev.kind)) + "'");
    changesets_.push_back(std::move(changeset));
    return {};
  }

  Result<void> add_provenance(ProvenanceRow row) override {
    if (auto r = check_open(); !r) return r;
    provenance_.push_back(std::move(row));
    return {};
  }

  Result<void> add_conflict(ImportConflictRow row) override {
    if (auto r = check_open(); !r) return r;
    conflicts_.push_back(std::move(row));
    return {};
  }

  Result<void> set_provenance_detail(std::string entity_type, Uuid entity, std::string detail_json) override {
    if (auto r = check_open(); !r) return r;
    details_.push_back({std::move(entity_type), entity, std::move(detail_json)});
    return {};
  }

  Result<void> resolve_conflict(Uuid conflict, std::string resolution) override {
    if (auto r = check_open(); !r) return r;
    resolutions_.emplace_back(conflict, std::move(resolution));
    return {};
  }

  Result<void> restate_conflict(ImportConflictRow row) override {
    if (auto r = check_open(); !r) return r;
    restated_.push_back(std::move(row));
    return {};
  }

  Result<void> set_progress(ImportProgress progress) override {
    if (auto r = check_open(); !r) return r;
    progress_ = std::move(progress);
    return {};
  }

  Result<ChangeSeq> commit() override {
    if (auto r = check_open(); !r) return fail(r.error());
    done_ = true;

    WriteTx tx(db_);
    if (auto r = tx.begin(); !r) return fail(r.error());
    std::vector<ChangeEntityRow> entities;
    for (const auto& changeset : changesets_)
      if (auto r = write_changeset(changeset, entities); !r) return fail(r.error());
    if (auto r = write_provenance(); !r) return fail(r.error());
    for (const auto& d : details_)
      if (auto r = db_.affecting(sql::kSetProvenanceDetail,
                                 {qv(d.detail_json), qv(d.entity_type), qv(d.entity), qv(source_)});
          !r)
        return fail(r.error());
    if (auto r = write_conflicts(); !r) return fail(r.error());
    const UtcTime now = UtcTime::now();
    for (const auto& c : restated_)
      if (auto r = db_.affecting(sql::kRestateConflict,
                                 {qv(c.path), qv(c.entity), qstr(to_string(c.kind)), qv(c.db_head_revision),
                                  qv(c.file_sha256), qv(c.detail_json), qv(c.resolution),
                                  resolved_at(c.resolution, now), qv(c.uuid), qv(source_)});
          !r)
        return fail(r.error());
    for (const auto& [conflict, resolution] : resolutions_)
      if (auto r = db_.affecting(sql::kResolveConflict, {qv(resolution), resolved_at(resolution, now), qv(conflict),
                                                         qv(source_), qv(resolution)});
          !r)
        return fail(r.error());
    if (progress_)
      if (auto r = write_progress(*progress_); !r) return fail(r.error());

    // A batch that stored no changeset and no revision is not a change.
    auto seq = entities.empty() ? latest_change_seq(db_)
                                : take_change(db_, QStringLiteral("changeset"), std::nullopt, client_, entities);
    if (!seq) return fail(seq.error());
    if (auto r = tx.commit(); !r) return fail(r.error());
    return *seq;
  }

 private:
  Result<void> check_open() const {
    if (done_) return fail(ErrorKind::Protocol, "import unit of work already committed");
    return {};
  }

  // import_conflict.resolved_utc: when a conflict left `pending`; null while it is pending.
  static QVariant resolved_at(const std::string& resolution, UtcTime now) {
    return resolution == "pending" ? QVariant{} : qv(now);
  }

  void note(std::vector<ChangeEntityRow>& entities, const QString& type, Uuid entity, const char* op) {
    if (noted_.emplace(type.toStdString(), entity).second)
      entities.push_back(ChangeEntityRow{type, entity, QString::fromUtf8(op), std::nullopt});
  }

  Result<void> write_changeset(const ImportedChangeset& c, std::vector<ChangeEntityRow>& entities) {
    const ChangesetInfo info{c.uuid, c.kind, c.author_user, client_, c.created, c.message};
    auto written = insert_changeset_if_absent(db_, info, source_);
    if (!written) return fail(written.error());
    // The batch's change_log entry cannot name several changesets; history
    // finds an imported changeset's change_seq through this row.
    if (*written) note(entities, QStringLiteral("changeset"), c.uuid, "insert");

    // Heads are locked in the order given, not sorted: one importer at a time is assumed.
    for (const auto& rev : c.revisions) {
      auto stored = db_.select_one(sql::kRevisionExists, {qv(rev.uuid)});
      if (!stored) return fail(stored.error());
      if (*stored) continue;
      auto parent = read_head(db_, rev.subject, rev.kind);
      if (!parent) return fail(parent.error());
      if (auto r = insert_revision(db_, rev.uuid, c.uuid, rev.subject, rev.kind, *parent, c.created); !r) return r;
      if (auto r = write_payload(db_, rev.uuid, rev.subject, rev.payload); !r) return r;
      auto moved = cas_head(db_, rev.subject, rev.kind, *parent, rev.uuid);
      if (!moved) return fail(moved.error());
      if (!*moved)
        return fail(ErrorKind::Protocol, "import: the head of " + rev.subject.str() + " '" +
                                             std::string(to_string(rev.kind)) + "' moved during the batch");
      if (auto r = insert_head_move(db_, c.uuid, rev.subject, rev.kind, *parent, rev.uuid, MoveReason::Commit); !r)
        return r;
      if (rev.kind == Kind::Identity)
        if (auto r = apply_identity(db_, rev.subject, std::get<IdentityValue>(rev.payload)); !r) return r;
      note(entities, entity_type_of(subject_type_of(rev.kind)), rev.subject, "upsert");
    }
    return {};
  }

  Result<void> write_provenance() {
    for (const auto& p : provenance_) {
      Row r;
      r["entity_type"] = qv(p.entity_type);
      r["entity_uuid"] = qv(p.entity);
      r["import_source_uuid"] = qv(source_);
      r["path"] = qv(p.path);
      r["commit_sha"] = qv(p.commit_sha);
      r["git_blob_sha"] = qv(p.git_blob_sha);
      r["git_author"] = qv(p.git_author);
      r["git_utc"] = qv(p.git_utc);
      r["detail"] = qv(p.detail_json);
      if (auto ins = db_.insert_or_ignore("import_provenance", r); !ins) return fail(ins.error());
    }
    return {};
  }

  Result<void> write_conflicts() {
    for (const auto& c : conflicts_) {
      Row r;
      r["uuid"] = qv(c.uuid);
      r["import_source_uuid"] = qv(source_);
      r["path"] = qv(c.path);
      r["entity_uuid"] = qv(c.entity);
      r["conflict_kind"] = qstr(to_string(c.kind));
      r["db_head_revision_uuid"] = qv(c.db_head_revision);
      r["file_sha256"] = qv(c.file_sha256);
      r["detail"] = qv(c.detail_json);
      r["resolution"] = qv(c.resolution);
      if (auto ins = db_.insert_or_ignore("import_conflict", r); !ins) return fail(ins.error());
    }
    return {};
  }

  Result<void> write_progress(const ImportProgress& p) {
    QString extra;
    Bindings b{qv(p.token), p.done, p.total, qv(p.status)};
    if (p.head_sha) {
      extra += QStringLiteral(", head_commit_sha = ?");
      b << qv(*p.head_sha);
    }
    if (p.status == "finished") {
      extra += QStringLiteral(", finished_utc = ?");
      b << qv(UtcTime::now());
    }
    b << qv(source_);
    auto updated = db_.affecting(sql::kSetImportProgress.arg(extra), b);
    if (!updated) return fail(updated.error());
    if (*updated != 1) return fail(ErrorKind::Protocol, "import progress for an unknown source " + source_.str());
    return {};
  }

  Db& db_;
  Uuid source_;
  Uuid client_;
  std::vector<ImportedChangeset> changesets_;
  std::vector<ProvenanceRow> provenance_;
  std::vector<ImportConflictRow> conflicts_;
  std::vector<ImportConflictRow> restated_;
  struct Detail {
    std::string entity_type;
    Uuid entity;
    std::string detail_json;
  };
  std::vector<Detail> details_;
  std::vector<std::pair<Uuid, std::string>> resolutions_;
  std::optional<ImportProgress> progress_;
  std::set<std::pair<std::string, Uuid>> noted_;  // (entity type, uuid) already in the change entry
  bool done_ = false;
};

}  // namespace

std::unique_ptr<IImportUnitOfWork> make_import_unit_of_work(Db& db, Uuid source, Uuid client) {
  return std::make_unique<ImportUnitOfWork>(db, source, client);
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
  QString sql = sql::kImportConflicts.arg(sql::ts(db.dialect(), QStringLiteral("resolved_utc")));
  if (!where.isEmpty()) sql += QStringLiteral(" WHERE ") + where.join(QStringLiteral(" AND "));
  sql += QStringLiteral(" ORDER BY path, uuid");
  auto rows = db.select(sql, b);
  if (!rows) return fail(rows.error());
  std::vector<ImportConflictRow> out;
  for (const auto& r : *rows) {
    auto c = conflict_from(r);
    if (!c) return fail(c.error());
    out.push_back(std::move(*c));
  }
  return out;
}

Result<std::optional<ImportConflictRow>> import_conflict(Db& db, Uuid conflict) {
  auto row = db.select_one(sql::kImportConflicts.arg(sql::ts(db.dialect(), QStringLiteral("resolved_utc"))) +
                               QStringLiteral(" WHERE uuid = ?"),
                           {qv(conflict)});
  if (!row) return fail(row.error());
  if (!*row) return std::optional<ImportConflictRow>{};
  auto c = conflict_from(**row);
  if (!c) return fail(c.error());
  return std::optional<ImportConflictRow>{std::move(*c)};
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
    p.source = to_uuid(r.value("import_source_uuid"));
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
