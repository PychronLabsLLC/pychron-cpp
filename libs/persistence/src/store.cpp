#include "pychron/persistence/store.hpp"

#include <algorithm>
#include <cstdio>

#include "migrate.hpp"
#include "sql/statements.hpp"
#include "store_impl.hpp"

namespace pychron::persistence {

std::string make_runid(const std::string& identifier, int aliquot, int increment) {
  char buf[16];
  std::snprintf(buf, sizeof buf, "%02d", aliquot);
  std::string out = identifier + "-" + buf;
  if (increment >= 0) {
    // 0 -> A, 25 -> Z, 26 -> AA (legacy alphas()).
    std::string letters;
    for (int n = increment + 1; n > 0; n = (n - 1) / 26) letters.insert(letters.begin(), static_cast<char>('A' + (n - 1) % 26));
    out += letters;
  }
  return out;
}

namespace detail {

Result<std::vector<HeadInfo>> read_heads(Db& db, Uuid subject) {
  auto rows = db.select(sql::kSelectHeads, {qv(subject)});
  if (!rows) return fail(rows.error());
  std::vector<HeadInfo> out;
  for (const auto& r : *rows) {
    const auto kind = parse_kind(to_std(r.value("kind")));
    if (!kind) return fail(ErrorKind::Protocol, "unknown head kind '" + to_std(r.value("kind")) + "'");
    out.push_back(HeadInfo{subject, *kind, to_uuid(r.value("revision_uuid")), r.value("head_version").toInt()});
  }
  return out;
}

namespace {

QString qstr(std::string_view s) { return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size())); }

// History rows arrive ordered by change_seq. An import batch stores several
// revisions of one (subject, kind) under one change_seq; within such a run a
// revision is listed after its parent, whatever their times and uuids.
void chain_order(std::vector<RevisionInfo>& history) {
  for (std::size_t begin = 0; begin < history.size();) {
    std::size_t end = begin + 1;
    while (end < history.size() && history[end].change_seq == history[begin].change_seq) ++end;
    // Selection by "parent not still waiting in this run"; a run is a handful of rows.
    for (std::size_t placed = begin; placed + 1 < end; ++placed) {
      const auto waiting = [&](const std::optional<Uuid>& parent) {
        return parent && std::any_of(history.begin() + static_cast<std::ptrdiff_t>(placed),
                                     history.begin() + static_cast<std::ptrdiff_t>(end),
                                     [&](const RevisionInfo& r) { return r.uuid == *parent; });
      };
      const auto first = history.begin() + static_cast<std::ptrdiff_t>(placed);
      const auto last = history.begin() + static_cast<std::ptrdiff_t>(end);
      const auto next = std::find_if(first, last, [&](const RevisionInfo& r) { return !waiting(r.parent); });
      if (next == last) break;  // a parent cycle cannot be stored; leave the rest as read
      std::rotate(first, next, next + 1);
    }
    begin = end;
  }
}

AnalysisSummary summary_from(const Row& r) {
  AnalysisSummary s;
  s.uuid = to_uuid(r.value("uuid"));
  s.runid = to_std(r.value("runid_text"));
  s.identifier = to_std(r.value("identifier"));
  s.aliquot = r.value("aliquot").toInt();
  s.increment = r.value("increment").toInt();
  s.provisional = r.value("provisional").toBool();
  s.analysis_type = to_std(r.value("analysis_type"));
  s.timestamp = to_time(r.value("ts"));
  s.mass_spectrometer = to_std(r.value("mass_spectrometer"));
  s.signals_state = to_std(r.value("signals_state"));
  return s;
}

class TinyStore final : public IStore {
 public:
  explicit TinyStore(std::unique_ptr<Db> db) : db_(std::move(db)) {}

  Dialect dialect() const noexcept override { return db_->dialect(); }

  Result<std::vector<AppliedMigration>> schema_status() override { return migrate(*db_, false); }

  Result<std::unique_ptr<IUnitOfWork>> begin(const Actor& actor) override {
    if (actor.user.is_nil() || actor.client.is_nil()) return fail(ErrorKind::Protocol, "actor needs a user and a client");
    return make_unit_of_work(*db_, actor);
  }

  // ------------------------------------------------------------ import bookkeeping

  Result<ImportSourceInfo> begin_import(const ImportSourceSpec& spec) override {
    return detail::begin_import(*db_, dialect(), spec);
  }
  Result<std::unique_ptr<IImportUnitOfWork>> begin_import_batch(Uuid source, Uuid client) override {
    return make_import_unit_of_work(*db_, source, client);
  }
  Result<std::vector<ImportSourceInfo>> import_sources() override { return detail::import_sources(*db_, dialect()); }
  Result<std::vector<ImportConflictRow>> import_conflicts(const ConflictFilter& filter) override {
    return detail::import_conflicts(*db_, filter);
  }
  Result<std::optional<ImportConflictRow>> import_conflict(Uuid conflict) override {
    return detail::import_conflict(*db_, conflict);
  }
  Result<std::vector<ProvenanceRow>> provenance_for(Uuid entity) override {
    return detail::provenance_for(*db_, dialect(), entity);
  }
  Result<bool> has_provenance(Uuid source, std::string_view commit_sha, std::string_view path) override {
    return detail::has_provenance(*db_, source, commit_sha, path);
  }
  Result<bool> has_provenance_blob(Uuid source, std::string_view path, std::string_view git_blob_sha) override {
    return detail::has_provenance_blob(*db_, source, path, git_blob_sha);
  }
  Result<bool> has_conflict(Uuid source, std::string_view path, const Sha256Digest& file_sha256) override {
    return detail::has_conflict(*db_, source, path, file_sha256);
  }
  Result<std::optional<std::string>> imported_head_blob_sha(Uuid source, Uuid subject, Kind kind) override {
    return detail::imported_head_blob_sha(*db_, source, subject, kind);
  }

  // ------------------------------------------------------------ catalog

  Result<Uuid> register_client(const ClientRegistration& reg) override {
    WriteTx tx(*db_);
    if (auto r = tx.begin(); !r) return fail(r.error());
    auto existing = db_->select_one(sql::kClientByHost, {qv(reg.hostname), qv(reg.role)});
    if (!existing) return fail(existing.error());
    if (*existing) return to_uuid((*existing)->value("uuid"));
    const Uuid uuid = Uuid::v7();
    Row row;
    row["uuid"] = qv(uuid);
    row["hostname"] = qv(reg.hostname);
    row["role"] = qv(reg.role);
    row["mass_spectrometer_uuid"] = qv(reg.mass_spectrometer);
    row["software_version"] = qv(reg.software_version);
    row["created_utc"] = qv(UtcTime::now());
    if (auto r = db_->insert("client", row); !r) return fail(r.error());
    const std::string detail = json_created({{"hostname", reg.hostname}, {"role", reg.role}});
    return finish_catalog(tx, uuid, {ChangeEntityRow{QStringLiteral("client"), uuid, QStringLiteral("insert"), detail}});
  }

  Result<Uuid> ensure_user(Uuid client, const std::string& name) override {
    WriteTx tx(*db_);
    if (auto r = tx.begin(); !r) return fail(r.error());
    std::vector<ChangeEntityRow> created;
    auto uuid = ensure_user_row(*db_, name, created);
    if (!uuid) return fail(uuid.error());
    if (created.empty()) return *uuid;
    return finish_catalog(tx, client, created, *uuid);
  }

  Result<Uuid> add_mass_spectrometer(Uuid client, const MassSpectrometerSpec& spec) override {
    Row row;
    row["name"] = qv(spec.name);
    row["kind"] = qv(spec.kind);
    row["code"] = qv(spec.code);
    return ensure_catalog_row(client, "mass_spectrometer", {{"name", qv(spec.name)}}, spec.uuid, row,
                              json_created({{"name", spec.name}, {"kind", spec.kind}, {"code", spec.code}}));
  }

  Result<Uuid> add_identifier(Uuid client, const IdentifierSpec& spec) override {
    Row row;
    row["identifier"] = qv(spec.identifier);
    row["kind"] = qv(spec.kind);
    row["analysis_type"] = qv(spec.analysis_type);
    row["mass_spectrometer_uuid"] = qv(spec.mass_spectrometer);
    row["position_uuid"] = qv(spec.position);
    row["sample_uuid"] = qv(spec.sample);
    return ensure_catalog_row(
        client, "identifier", {{"identifier", qv(spec.identifier)}}, spec.uuid, row,
        json_created({{"identifier", spec.identifier}, {"kind", spec.kind}, {"analysis_type", spec.analysis_type}}));
  }

  Result<Uuid> add_extract_device(Uuid client, const std::string& name) override {
    Row row;
    row["name"] = qv(name);
    return ensure_catalog_row(client, "extract_device", {{"name", qv(name)}}, std::nullopt, row,
                              json_created({{"name", name}}));
  }

  Result<Uuid> add_principal_investigator(Uuid client, const PrincipalInvestigatorSpec& spec) override {
    Row row;
    row["last_name"] = qv(spec.last_name);
    row["first_initial"] = qv(spec.first_initial);
    row["affiliation"] = qv(spec.affiliation);
    row["email"] = qv(spec.email);
    return ensure_catalog_row(client, "principal_investigator",
                              {{"last_name", qv(spec.last_name)}, {"first_initial", qv(spec.first_initial)}}, spec.uuid,
                              row, json_created({{"last_name", spec.last_name}, {"first_initial", spec.first_initial}}));
  }

  Result<Uuid> add_project(Uuid client, const ProjectSpec& spec) override {
    Row row;
    row["name"] = qv(spec.name);
    row["pi_uuid"] = qv(spec.principal_investigator);
    // UNIQUE (name, pi_uuid) does not constrain rows without a PI; the key still matches them.
    return ensure_catalog_row(client, "project", {{"name", qv(spec.name)}, {"pi_uuid", qv(spec.principal_investigator)}},
                              spec.uuid, row, json_created({{"name", spec.name}}));
  }

  Result<Uuid> add_material(Uuid client, const MaterialSpec& spec) override {
    Row row;
    row["name"] = qv(spec.name);
    row["grainsize"] = qv(spec.grainsize);
    return ensure_catalog_row(client, "material", {{"name", qv(spec.name)}, {"grainsize", qv(spec.grainsize)}},
                              spec.uuid, row, json_created({{"name", spec.name}, {"grainsize", spec.grainsize}}));
  }

  Result<Uuid> add_sample(Uuid client, const SampleSpec& spec) override {
    Row row;
    row["name"] = qv(spec.name);
    row["project_uuid"] = qv(spec.project);
    row["material_uuid"] = qv(spec.material);
    row["note"] = qv(spec.note);
    row["igsn"] = qv(spec.igsn);
    row["lat"] = qv(spec.lat);
    row["lon"] = qv(spec.lon);
    row["elevation"] = qv(spec.elevation);
    row["storage_location"] = qv(spec.storage_location);
    row["location"] = qv(spec.location);
    row["unit"] = qv(spec.unit);
    row["lithology"] = qv(spec.lithology);
    row["lithology_class"] = qv(spec.lithology_class);
    row["lithology_type"] = qv(spec.lithology_type);
    row["lithology_group"] = qv(spec.lithology_group);
    row["approximate_age"] = qv(spec.approximate_age);
    row["updated_utc"] = qv(UtcTime::now());
    return ensure_catalog_row(
        client, "sample",
        {{"name", qv(spec.name)}, {"project_uuid", qv(spec.project)}, {"material_uuid", qv(spec.material)}}, spec.uuid,
        row, json_created({{"name", spec.name}}));
  }

  Result<Uuid> add_irradiation(Uuid client, const std::string& name) override {
    Row row;
    row["name"] = qv(name);
    return ensure_catalog_row(client, "irradiation", {{"name", qv(name)}}, std::nullopt, row,
                              json_created({{"name", name}}));
  }

  Result<Uuid> add_level(Uuid client, const LevelSpec& spec) override {
    Row row;
    row["irradiation_uuid"] = qv(spec.irradiation);
    row["name"] = qv(spec.name);
    row["holder_ref_uuid"] = qv(spec.holder);
    row["z"] = qv(spec.z);
    row["note"] = qv(spec.note);
    return ensure_catalog_row(client, "level", {{"irradiation_uuid", qv(spec.irradiation)}, {"name", qv(spec.name)}},
                              spec.uuid, row, json_created({{"name", spec.name}}));
  }

  Result<Uuid> add_irradiation_position(Uuid client, const PositionSpec& spec) override {
    Row row;
    row["level_uuid"] = qv(spec.level);
    row["position"] = spec.position;
    row["sample_uuid"] = qv(spec.sample);
    row["weight"] = qv(spec.weight);
    row["packet"] = qv(spec.packet);
    row["note"] = qv(spec.note);
    return ensure_catalog_row(client, "irradiation_position",
                              {{"level_uuid", qv(spec.level)}, {"position", qv(spec.position)}}, spec.uuid, row,
                              json_created({{"position", std::to_string(spec.position)}}));
  }

  Result<Uuid> add_ref_object(Uuid client, const RefObjectSpec& spec) override {
    Row row;
    row["ref_type"] = qstr(to_string(spec.type));
    row["key"] = qv(spec.key);
    row["irradiation_uuid"] = qv(spec.irradiation);
    row["level_uuid"] = qv(spec.level);
    row["position_uuid"] = qv(spec.position);
    row["mass_spectrometer_uuid"] = qv(spec.mass_spectrometer);
    return ensure_catalog_row(client, "ref_object",
                              {{"ref_type", qstr(to_string(spec.type))}, {"key", qv(spec.key)}}, spec.uuid, row,
                              json_created({{"ref_type", std::string(to_string(spec.type))}, {"key", spec.key}}));
  }

  Result<Uuid> add_load(Uuid client, const LoadSpec& spec) override {
    Row row;
    row["name"] = qv(spec.name);
    row["holder_ref_uuid"] = qv(spec.holder);
    row["holder_ref_revision_uuid"] = qv(spec.holder_revision);
    row["created_by_user_uuid"] = qv(spec.created_by_user);
    row["archived"] = spec.archived;
    if (spec.created) row["created_utc"] = qv(*spec.created);
    return ensure_catalog_row(client, "load", {{"name", qv(spec.name)}}, spec.uuid, row,
                              json_created({{"name", spec.name}}));
  }

  Result<void> add_load_position(Uuid client, const LoadPositionSpec& spec) override {
    Row row;
    row["load_uuid"] = qv(spec.load);
    row["position"] = spec.position;
    row["identifier_uuid"] = qv(spec.identifier);
    row["weight"] = qv(spec.weight);
    row["nxtals"] = qv(spec.nxtals);
    row["note"] = qv(spec.note);
    auto uuid = ensure_catalog_row(
        client, "load_position",
        {{"load_uuid", qv(spec.load)}, {"position", qv(spec.position)}, {"identifier_uuid", qv(spec.identifier)}},
        std::nullopt, row, json_created({{"position", std::to_string(spec.position)}}));
    if (!uuid) return fail(uuid.error());
    return {};
  }

  Result<Uuid> add_interpreted_age(Uuid client, const InterpretedAgeSpec& spec) override {
    Row row;
    row["name"] = qv(spec.name);
    row["identifier_uuid"] = qv(spec.identifier);
    row["repository_uuid"] = qv(spec.repository);
    return add_catalog_row(client, "interpreted_age", row, json_created({{"name", spec.name}}));
  }

  Result<Uuid> add_repository(Uuid client, const std::string& name) override {
    Row row;
    row["name"] = qv(name);
    return ensure_catalog_row(client, "repository", {{"name", qv(name)}}, std::nullopt, row,
                              json_created({{"name", name}}));
  }

  // ------------------------------------------------------------ groups, bookmarks

  Result<void> add_repository_members(const Actor& actor, Uuid repository,
                                      const std::vector<Uuid>& analyses) override {
    return detail::add_repository_members(*db_, actor, repository, analyses);
  }
  Result<Uuid> create_group(const Actor& actor, const std::string& name, const std::vector<Uuid>& analyses,
                            std::optional<Uuid> uuid) override {
    return detail::create_group(*db_, actor, name, analyses, uuid);
  }
  Result<Uuid> create_bookmark(const Actor& actor, const BookmarkSpec& spec) override {
    return detail::create_bookmark(*db_, actor, spec);
  }
  Result<std::vector<HeadInfo>> bookmark_heads(Uuid bookmark) override { return detail::bookmark_heads(*db_, bookmark); }
  Result<CommitOutcome> restore_bookmark(const Actor& actor, Uuid bookmark, std::string message) override {
    return detail::restore_bookmark(*db_, actor, bookmark, std::move(message));
  }
  Result<CommitOutcome> rollback_to_collection(const Actor& actor, Uuid analysis, std::string message,
                                               std::vector<Kind> kinds) override {
    return detail::rollback_to_collection(*db_, actor, analysis, std::move(message), std::move(kinds));
  }

  // ------------------------------------------------------------ references, derived cache

  Result<RefResolution> resolve_refs(Uuid analysis, const RefPolicy& policy) override {
    return detail::resolve_refs(*db_, analysis, policy);
  }
  Result<Sha256Digest> input_fingerprint(Uuid analysis, const std::string& reduction_version) override {
    return detail::input_fingerprint(*db_, analysis, reduction_version);
  }
  Result<void> put_derived(Uuid analysis, const Sha256Digest& fingerprint, const std::string& reduction_version,
                           const std::vector<DerivedRow>& rows) override {
    return detail::put_derived(*db_, analysis, fingerprint, reduction_version, rows);
  }
  Result<std::optional<std::vector<DerivedRow>>> get_derived(Uuid analysis,
                                                            const std::string& reduction_version) override {
    return detail::get_derived(*db_, analysis, reduction_version);
  }
  Result<int> prune_derived(Uuid analysis) override { return detail::prune_derived(*db_, analysis); }

  // ------------------------------------------------------------ ingest

  Result<IngestAck> ingest(const IngestItem& item) override { return ingest_item(*db_, item); }

  // ------------------------------------------------------------ reads

  Result<std::optional<Uuid>> head(Uuid subject, Kind kind) override {
    return read_head(*db_, subject, kind);
  }

  Result<std::vector<HeadInfo>> heads(Uuid subject) override { return read_heads(*db_, subject); }

  Result<std::vector<RevisionInfo>> history(Uuid subject, Kind kind) override {
    auto rows = db_->select(sql::kHistory.arg(sql::ts(dialect(), QStringLiteral("c.created_utc"))),
                            {qv(subject), qstr(to_string(kind))});
    if (!rows) return fail(rows.error());
    std::vector<RevisionInfo> out;
    for (const auto& r : *rows) {
      RevisionInfo info;
      info.uuid = to_uuid(r.value("uuid"));
      info.subject = subject;
      info.kind = kind;
      info.parent = opt_uuid(r.value("parent_uuid"));
      info.changeset.uuid = to_uuid(r.value("cs_uuid"));
      info.changeset.kind = parse_changeset_kind(to_std(r.value("cs_kind"))).value_or(ChangesetKind::Reduction);
      info.changeset.author_user = to_uuid(r.value("author_user_uuid"));
      info.changeset.client = to_uuid(r.value("client_uuid"));
      info.changeset.created = to_time(r.value("cs_created"));
      info.changeset.message = to_std(r.value("message"));
      info.change_seq = r.value("visible_seq").toLongLong();
      info.author_name = to_std(r.value("author_name"));
      info.client_hostname = to_std(r.value("client_hostname"));
      out.push_back(std::move(info));
    }
    chain_order(out);
    return out;
  }

  Result<std::optional<RevisionPayload>> load_payload(Uuid revision) override {
    auto row = db_->select_one(sql::kRevisionKind, {qv(revision)});
    if (!row) return fail(row.error());
    if (!*row) return std::optional<RevisionPayload>{};
    const auto kind = parse_kind(to_std((*row)->value("kind")));
    if (!kind) return fail(ErrorKind::Protocol, "unknown revision kind");
    auto payload = read_payload(*db_, revision, *kind);
    if (!payload) return fail(payload.error());
    return std::optional<RevisionPayload>{std::move(*payload)};
  }

  Result<std::optional<AnalysisView>> load_analysis(Uuid analysis) override {
    auto row = db_->select_one(summary_select() + QStringLiteral(" WHERE a.uuid = ?"), {qv(analysis)});
    if (!row) return fail(row.error());
    if (!*row) return std::optional<AnalysisView>{};
    AnalysisView view;
    view.summary = summary_from(**row);
    auto hs = heads(analysis);
    if (!hs) return fail(hs.error());
    view.heads = std::move(*hs);
    for (const auto& h : view.heads) {
      auto payload = read_payload(*db_, h.revision, h.kind);
      if (!payload) return fail(payload.error());
      view.payloads.emplace(h.kind, std::move(*payload));
    }
    return std::optional<AnalysisView>{std::move(view)};
  }

  Result<std::vector<AnalysisSummary>> find_analyses(const AnalysisQuery& q) override {
    QStringList where;
    Bindings b;
    if (q.identifier) {
      where << QStringLiteral("i.identifier = ?");
      b << qv(*q.identifier);
    }
    if (q.mass_spectrometer) {
      where << QStringLiteral("m.name = ?");
      b << qv(*q.mass_spectrometer);
    }
    if (q.analysis_type) {
      where << QStringLiteral("a.analysis_type = ?");
      b << qv(*q.analysis_type);
    }
    // ISO-8601 UTC strings compare correctly as text on SQLite and as
    // timestamptz literals on PostgreSQL.
    if (q.from) {
      where << QStringLiteral("a.timestamp_utc >= ?");
      b << qv(*q.from);
    }
    if (q.to) {
      where << QStringLiteral("a.timestamp_utc <= ?");
      b << qv(*q.to);
    }
    QString sql = summary_select();
    if (!where.isEmpty()) sql += QStringLiteral(" WHERE ") + where.join(QStringLiteral(" AND "));
    sql += QStringLiteral(" ORDER BY a.timestamp_utc, a.uuid LIMIT ?");
    b << q.limit;
    auto rows = db_->select(sql, b);
    if (!rows) return fail(rows.error());
    std::vector<AnalysisSummary> out;
    for (const auto& r : *rows) out.push_back(summary_from(r));
    return out;
  }

  Result<BrowseResult> browse(const BrowseRequest& request) override {
    return detail::browse(*db_, dialect(), request);
  }

  Result<std::vector<std::string>> facet(BrowseFacet f, const BrowseFilter& filter) override {
    return detail::facet(*db_, dialect(), f, filter);
  }

  Result<std::optional<AnalysisDetail>> load_analysis_detail(Uuid analysis) override {
    return detail::load_analysis_detail(*db_, dialect(), analysis);
  }

  Result<std::optional<BlobData>> load_blob(const Sha256Digest& sha) override { return detail::load_blob(*db_, sha); }

  Result<ChangeSeq> latest_change_seq() override { return detail::latest_change_seq(*db_); }
  Result<ChangePage> changes_since(ChangeSeq cursor, int limit) override {
    if (limit <= 0) return fail(ErrorKind::Protocol, "changes_since: limit must be positive");
    auto rows = db_->select(sql::kChangesSince.arg(sql::ts(dialect(), QStringLiteral("committed_utc"))),
                            {static_cast<qlonglong>(cursor), limit + 1});
    if (!rows) return fail(rows.error());
    ChangePage page;
    page.cursor = cursor;
    page.more = static_cast<int>(rows->size()) > limit;
    if (page.more) rows->pop_back();
    for (const auto& r : *rows) {
      ChangeEntry e;
      e.seq = r.value("change_seq").toLongLong();
      e.committed = to_time(r.value("committed"));
      e.changeset = opt_uuid(r.value("changeset_uuid"));
      e.client = to_uuid(r.value("client_uuid"));
      e.kind = to_std(r.value("kind"));
      page.entries.push_back(std::move(e));
    }
    if (page.entries.empty()) return page;
    page.cursor = page.entries.back().seq;
    auto ents = db_->select(sql::kChangeEntitiesBetween,
                            {static_cast<qlonglong>(cursor), static_cast<qlonglong>(page.cursor)});
    if (!ents) return fail(ents.error());
    std::size_t i = 0;
    for (const auto& r : *ents) {
      const ChangeSeq seq = r.value("change_seq").toLongLong();
      while (i < page.entries.size() && page.entries[i].seq < seq) ++i;
      if (i == page.entries.size()) break;
      page.entries[i].entities.push_back(
          ChangeEntity{to_std(r.value("entity_type")), to_uuid(r.value("entity_uuid")), to_std(r.value("op"))});
    }
    return page;
  }

  Db& db() { return *db_; }

 private:
  QString summary_select() const {
    return sql::kAnalysisSummarySelect.arg(sql::ts(dialect(), QStringLiteral("a.timestamp_utc")));
  }

  using NaturalKey = std::vector<std::pair<const char*, QVariant>>;

  // The uuid of the row `key` names, matching a null part with IS NULL.
  Result<std::optional<Uuid>> find_by_key(const char* table, const NaturalKey& key) {
    QString sql = QStringLiteral("SELECT uuid FROM %1 WHERE ").arg(QString::fromUtf8(table));
    Bindings bindings;
    for (std::size_t i = 0; i < key.size(); ++i) {
      if (i) sql += QStringLiteral(" AND ");
      sql += QString::fromUtf8(key[i].first);
      if (key[i].second.isNull()) {
        sql += QStringLiteral(" IS NULL");
      } else {
        sql += QStringLiteral(" = ?");
        bindings.push_back(key[i].second);
      }
    }
    auto found = db_->select_one(sql, bindings);
    if (!found) return fail(found.error());
    if (!*found) return std::optional<Uuid>{};
    return std::optional<Uuid>{to_uuid((**found).value("uuid"))};
  }

  // Ensure by natural key: an existing row wins untouched (no update, no
  // change_log entry); otherwise `row` is inserted under `uuid` (or a new v7).
  Result<Uuid> ensure_catalog_row(Uuid client, const char* table, const NaturalKey& key, std::optional<Uuid> uuid,
                                  Row row, const std::string& detail) {
    WriteTx tx(*db_);
    if (auto r = tx.begin(); !r) return fail(r.error());
    auto existing = find_by_key(table, key);
    if (!existing) return fail(existing.error());
    if (*existing) return **existing;
    return insert_catalog_row(tx, client, table, uuid.value_or(Uuid::v7()), std::move(row), detail);
  }

  // Inserts one catalog row with a fresh uuid and created_utc, audited (D6).
  Result<Uuid> add_catalog_row(Uuid client, const char* table, Row row, const std::string& detail) {
    WriteTx tx(*db_);
    if (auto r = tx.begin(); !r) return fail(r.error());
    return insert_catalog_row(tx, client, table, Uuid::v7(), std::move(row), detail);
  }

  Result<Uuid> insert_catalog_row(WriteTx& tx, Uuid client, const char* table, Uuid uuid, Row row,
                                  const std::string& detail) {
    row["uuid"] = qv(uuid);
    if (!row.contains("created_utc")) row["created_utc"] = qv(UtcTime::now());
    if (auto r = db_->insert(table, row); !r) return fail(r.error());
    return finish_catalog(
        tx, client, {ChangeEntityRow{QString::fromUtf8(table), uuid, QStringLiteral("insert"), detail}}, uuid);
  }

  Result<Uuid> finish_catalog(WriteTx& tx, Uuid client, const std::vector<ChangeEntityRow>& entities,
                              std::optional<Uuid> result = std::nullopt) {
    auto seq = take_change(*db_, QStringLiteral("catalog"), std::nullopt, client, entities);
    if (!seq) return fail(seq.error());
    if (auto r = tx.commit(); !r) return fail(r.error());
    return result.value_or(entities.front().entity);
  }

  std::unique_ptr<Db> db_;
};

Result<void> check_sqlite(Db& db, bool file_backed) {
  auto version = db.select_one(sql::kSqliteVersion);
  if (!version) return fail(version.error());
  int major = 0, minor = 0;
  std::sscanf(to_std((*version)->value("v")).c_str(), "%d.%d", &major, &minor);
  if (major < 3 || (major == 3 && minor < 37))
    return fail(ErrorKind::Config, "SQLite >= 3.37 is required (STRICT tables); found " + to_std((*version)->value("v")));
  // Asserted, not assumed (section 11.3): FK enforcement is per connection.
  if (auto r = db.unprepared(sql::kSqliteForeignKeysOn); !r) return fail(r.error());
  auto fk = db.select_one(sql::kSqliteForeignKeys);
  if (!fk) return fail(fk.error());
  if (!*fk || (*fk)->value("foreign_keys").toInt() != 1)
    return fail(ErrorKind::Config, "SQLite foreign key enforcement is off");
  if (file_backed) {
    if (auto r = db.select(sql::kSqliteWal); !r) return fail(r.error());
    if (auto r = db.unprepared(sql::kSqliteSynchronousFull); !r) return fail(r.error());
  }
  return {};
}

}  // namespace
}  // namespace detail

Result<std::unique_ptr<IStore>> open_store(const StoreConfig& config) {
  auto db = detail::Db::open(config);
  if (!db) return fail(db.error());
  if ((*db)->dialect() == Dialect::Sqlite)
    if (auto r = detail::check_sqlite(**db, config.url != "sqlite::memory:"); !r) return fail(r.error());
  if (auto r = detail::migrate(**db, config.migrate); !r) return fail(r.error());
  return std::unique_ptr<IStore>(std::make_unique<detail::TinyStore>(std::move(*db)));
}

}  // namespace pychron::persistence
