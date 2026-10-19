// Releases, embargoes and external ids (data publishing spec, section 4).

#include "pychron/persistence/publish.hpp"

#include <QStringList>

#include <algorithm>
#include <set>

#include "sql/publish.hpp"
#include "store_impl.hpp"

namespace pychron::persistence {

std::string_view to_string(ReleaseState state) noexcept {
  switch (state) {
    case ReleaseState::Draft:
      return "draft";
    case ReleaseState::Published:
      return "published";
    case ReleaseState::Withdrawn:
      return "withdrawn";
  }
  return "draft";
}

std::string_view to_string(Visibility visibility) noexcept {
  return visibility == Visibility::Public ? "public" : "private";
}

std::string_view to_string(EmbargoScopeType type) noexcept {
  switch (type) {
    case EmbargoScopeType::Project:
      return "project";
    case EmbargoScopeType::Sample:
      return "sample";
    case EmbargoScopeType::Analysis:
      return "analysis";
  }
  return "sample";
}

std::optional<ReleaseState> parse_release_state(std::string_view text) noexcept {
  for (auto s : {ReleaseState::Draft, ReleaseState::Published, ReleaseState::Withdrawn})
    if (to_string(s) == text) return s;
  return std::nullopt;
}

std::optional<Visibility> parse_visibility(std::string_view text) noexcept {
  for (auto v : {Visibility::Private, Visibility::Public})
    if (to_string(v) == text) return v;
  return std::nullopt;
}

std::optional<EmbargoScopeType> parse_embargo_scope_type(std::string_view text) noexcept {
  for (auto t : {EmbargoScopeType::Project, EmbargoScopeType::Sample, EmbargoScopeType::Analysis})
    if (to_string(t) == text) return t;
  return std::nullopt;
}

namespace detail {
namespace {

QString qstr(std::string_view s) { return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size())); }

ChangeEntityRow entity(const char* type, Uuid uuid, const char* op = "insert") {
  return ChangeEntityRow{QString::fromUtf8(type), uuid, QString::fromUtf8(op), std::nullopt};
}

std::optional<UtcTime> opt_time(const QVariant& v) {
  if (v.isNull()) return std::nullopt;
  return to_time(v);
}

std::optional<Sha256Digest> opt_digest(const QVariant& v) {
  if (v.isNull() || v.toByteArray().isEmpty()) return std::nullopt;
  return to_digest(v);
}

Result<ReleaseRow> release_of(const Row& r) {
  ReleaseRow out;
  out.uuid = to_uuid(r.value("uuid"));
  out.name = to_std(r.value("name"));
  out.title = to_std(r.value("title"));
  out.description = opt_str(r.value("description"));
  out.version = r.value("version").toInt();
  out.supersedes = opt_uuid(r.value("supersedes_uuid"));
  out.group = to_uuid(r.value("group_uuid"));
  out.bookmark = to_uuid(r.value("bookmark_uuid"));
  const auto state = parse_release_state(to_std(r.value("state")));
  const auto visibility = parse_visibility(to_std(r.value("visibility")));
  if (!state || !visibility) return fail(ErrorKind::Protocol, "release row has an unknown state or visibility");
  out.state = *state;
  out.visibility = *visibility;
  out.licence = to_std(r.value("licence"));
  out.authors_json = to_std(r.value("authors"));
  out.publisher = opt_str(r.value("publisher"));
  out.embargo_until = opt_time(r.value("embargo_until"));
  out.doi = opt_str(r.value("doi"));
  out.doi_state = opt_str(r.value("doi_state"));
  out.options_json = to_std(r.value("options"));
  out.reduction_version = to_std(r.value("reduction_version"));
  out.software_version = to_std(r.value("software_version"));
  out.manifest_sha256 = opt_digest(r.value("manifest_sha256"));
  out.created_by = to_uuid(r.value("created_by_user_uuid"));
  out.client = to_uuid(r.value("client_uuid"));
  out.created = to_time(r.value("created"));
  out.published = opt_time(r.value("published"));
  out.withdrawn = opt_time(r.value("withdrawn"));
  out.withdrawn_reason = opt_str(r.value("withdrawn_reason"));
  return out;
}

Result<std::optional<ReleaseRow>> one_release(Db& db, const QString& where, const Bindings& bindings) {
  auto row = db.select_one(sql::release_select(db.dialect(), where), bindings);
  if (!row) return fail(row.error());
  if (!*row) return std::optional<ReleaseRow>{};
  auto out = release_of(**row);
  if (!out) return fail(out.error());
  return std::optional<ReleaseRow>{std::move(*out)};
}

Bindings uuid_bindings(std::span<const Uuid> uuids) {
  Bindings b;
  for (const auto& u : uuids) b.push_back(qv(u));
  return b;
}

Result<std::vector<Uuid>> uuid_column(Db& db, const QString& sql, const Bindings& bindings, const char* column) {
  auto rows = db.select(sql, bindings);
  if (!rows) return fail(rows.error());
  std::vector<Uuid> out;
  for (const auto& r : *rows) out.push_back(to_uuid(r.value(column)));
  return out;
}

}  // namespace

Result<Uuid> add_release(Db& db, const Actor& actor, const ReleaseInsert& release) {
  WriteTx tx(db);
  if (auto r = tx.begin(); !r) return fail(r.error());
  Row row;
  row["uuid"] = qv(release.uuid);
  row["name"] = qv(release.name);
  row["title"] = qv(release.title);
  row["description"] = qv(release.description);
  row["version"] = qv(release.version);
  row["supersedes_uuid"] = qv(release.supersedes);
  row["group_uuid"] = qv(release.group);
  row["bookmark_uuid"] = qv(release.bookmark);
  row["licence"] = qv(release.licence);
  row["authors"] = qv(release.authors_json);
  row["publisher"] = qv(release.publisher);
  row["embargo_until_utc"] = qv(release.embargo_until);
  row["options"] = qv(release.options_json);
  row["reduction_version"] = qv(release.reduction_version);
  row["software_version"] = qv(release.software_version);
  row["created_by_user_uuid"] = qv(actor.user);
  row["client_uuid"] = qv(actor.client);
  row["created_utc"] = qv(UtcTime::now());
  if (auto r = db.insert("release", row); !r) return fail(r.error());
  QList<QVariantMap> samples;
  for (const auto& s : std::set<Uuid>(release.samples.begin(), release.samples.end())) {
    Row m;
    m["release_uuid"] = qv(release.uuid);
    m["sample_uuid"] = qv(s);
    samples.push_back(std::move(m));
  }
  if (auto r = db.insert_many("release_sample", samples); !r) return fail(r.error());
  QList<QVariantMap> ias;
  for (const auto& [ia, revision] : release.interpreted_ages) {
    Row m;
    m["release_uuid"] = qv(release.uuid);
    m["ia_uuid"] = qv(ia);
    m["revision_uuid"] = qv(revision);
    ias.push_back(std::move(m));
  }
  if (auto r = db.insert_many("release_interpreted_age", ias); !r) return fail(r.error());
  std::vector<ChangeEntityRow> entities{entity("release", release.uuid)};
  entities.front().detail_json = json_created({{"name", release.name}, {"version", std::to_string(release.version)}});
  for (const auto& s : release.samples) entities.push_back(entity("release_sample", s));
  if (auto seq = take_change(db, QStringLiteral("changeset"), std::nullopt, actor.client, entities); !seq)
    return fail(seq.error());
  if (auto r = tx.commit(); !r) return fail(r.error());
  return release.uuid;
}

Result<std::optional<ReleaseRow>> release(Db& db, Uuid uuid) { return one_release(db, sql::kReleaseByUuid, {qv(uuid)}); }

Result<std::optional<ReleaseRow>> find_release(Db& db, const std::string& name, std::optional<int> version) {
  if (version) return one_release(db, sql::kReleaseByNameVersion, {qv(name), qv(*version)});
  return one_release(db, sql::kReleaseNewestOfName, {qv(name)});
}

Result<std::vector<ReleaseRow>> releases(Db& db, const ReleaseQuery& query) {
  QStringList where;
  Bindings bindings;
  if (query.state) {
    where << QStringLiteral("r.state = ?");
    bindings.push_back(qstr(to_string(*query.state)));
  }
  if (query.name) {
    where << QStringLiteral("r.name = ?");
    bindings.push_back(qv(*query.name));
  }
  const QString clause =
      (where.isEmpty() ? QString() : QStringLiteral("WHERE ") + where.join(QStringLiteral(" AND ")) + QChar(' ')) +
      sql::kReleaseOrder;
  auto rows = db.select(sql::release_select(db.dialect(), clause), bindings);
  if (!rows) return fail(rows.error());
  std::vector<ReleaseRow> out;
  for (const auto& r : *rows) {
    auto row = release_of(r);
    if (!row) return fail(row.error());
    out.push_back(std::move(*row));
  }
  return out;
}

Result<void> update_release(Db& db, const Actor& actor, const ReleaseUpdate& update) {
  WriteTx tx(db);
  if (auto r = tx.begin(); !r) return fail(r.error());
  auto current = one_release(db, sql::kReleaseByUuid, {qv(update.release)});
  if (!current) return fail(current.error());
  if (!*current) return fail(ErrorKind::Config, "no release " + update.release.str());
  const ReleaseRow& row = **current;
  const auto refuse = [&](const std::string& what) {
    return fail(ErrorKind::Config, "release " + row.name + " v" + std::to_string(row.version) + ": " + what);
  };
  if (row.state == ReleaseState::Withdrawn) return refuse("a withdrawn release never changes");

  const ReleaseState state = update.state.value_or(row.state);
  const Visibility visibility = update.visibility.value_or(row.visibility);
  const UtcTime now = UtcTime::now();
  QStringList sets;
  Bindings bindings;
  const auto set = [&](const char* column, const QVariant& value) {
    sets << QString::fromUtf8(column) + QStringLiteral(" = ?");
    bindings.push_back(value);
  };
  if (state != row.state) {
    if (row.state == ReleaseState::Draft && state == ReleaseState::Published) {
      set("state", qstr("published"));
      set("published_utc", qv(now));
    } else if (row.state == ReleaseState::Published && state == ReleaseState::Withdrawn) {
      if (!update.withdrawn_reason || update.withdrawn_reason->empty()) return refuse("withdrawing needs a reason");
      set("state", qstr("withdrawn"));
      set("withdrawn_utc", qv(now));
      set("withdrawn_reason", qv(*update.withdrawn_reason));
    } else {
      return refuse(std::string("cannot go from ") + std::string(to_string(row.state)) + " to " +
                    std::string(to_string(state)));
    }
  }
  if (visibility != row.visibility) {
    if (state == ReleaseState::Draft) return refuse("a draft's visibility does not change; publish it");
    if (visibility == Visibility::Private && state != ReleaseState::Withdrawn)
      return refuse("a public release goes private only by being withdrawn");
    set("visibility", qstr(to_string(visibility)));
  }
  if (update.doi) set("doi", qv(*update.doi));
  if (update.doi_state) set("doi_state", qv(*update.doi_state));
  if (update.manifest_sha256) {
    if (row.manifest_sha256 && *row.manifest_sha256 != *update.manifest_sha256)
      return refuse("its published build is recorded and never changes (spec R2)");
    if (!row.manifest_sha256) set("manifest_sha256", qv(*update.manifest_sha256));
  }
  if (sets.isEmpty()) return tx.commit();
  bindings.push_back(qv(update.release));
  auto n = db.affecting(QStringLiteral("UPDATE release SET ") + sets.join(QStringLiteral(", ")) +
                            QStringLiteral(" WHERE uuid = ?"),
                        bindings);
  if (!n) return fail(n.error());
  if (auto seq = take_change(db, QStringLiteral("changeset"), std::nullopt, actor.client,
                             {entity("release", update.release, "update")});
      !seq)
    return fail(seq.error());
  return tx.commit();
}

Result<void> delete_release(Db& db, const Actor& actor, Uuid release) {
  WriteTx tx(db);
  if (auto r = tx.begin(); !r) return fail(r.error());
  auto current = one_release(db, sql::kReleaseByUuid, {qv(release)});
  if (!current) return fail(current.error());
  if (!*current) return fail(ErrorKind::Config, "no release " + release.str());
  if ((*current)->state != ReleaseState::Draft)
    return fail(ErrorKind::Config, "release " + (*current)->name + " is " +
                                       std::string(to_string((*current)->state)) + ": only a draft is deleted");
  for (const auto& sql : {sql::kDeleteReleaseSamples, sql::kDeleteReleaseInterpretedAges, sql::kDeleteRelease})
    if (auto r = db.affecting(sql, {qv(release)}); !r) return fail(r.error());
  if (auto seq = take_change(db, QStringLiteral("changeset"), std::nullopt, actor.client,
                             {entity("release", release, "delete")});
      !seq)
    return fail(seq.error());
  return tx.commit();
}

Result<std::vector<Uuid>> release_samples(Db& db, Uuid release) {
  return uuid_column(db, sql::kReleaseSamples, {qv(release)}, "sample_uuid");
}

Result<std::vector<ReleaseInterpretedAge>> release_interpreted_ages(Db& db, Uuid release) {
  auto rows = db.select(sql::kReleaseInterpretedAges, {qv(release)});
  if (!rows) return fail(rows.error());
  std::vector<ReleaseInterpretedAge> out;
  for (const auto& r : *rows) out.push_back({to_uuid(r.value("ia_uuid")), to_uuid(r.value("revision_uuid"))});
  return out;
}

Result<std::vector<InterpretedAgeRow>> interpreted_ages(Db& db) {
  auto rows = db.select(sql::kInterpretedAges);
  if (!rows) return fail(rows.error());
  std::vector<InterpretedAgeRow> out;
  for (const auto& r : *rows) {
    InterpretedAgeRow ia;
    ia.uuid = to_uuid(r.value("uuid"));
    ia.name = to_std(r.value("name"));
    ia.identifier = opt_uuid(r.value("identifier_uuid"));
    ia.identifier_text = to_std(r.value("identifier_text"));
    ia.repository = opt_uuid(r.value("repository_uuid"));
    ia.head = opt_uuid(r.value("revision_uuid"));
    out.push_back(std::move(ia));
  }
  return out;
}

// ---------------------------------------------------------------- embargoes

Result<Uuid> add_embargo(Db& db, const Actor& actor, const EmbargoInsert& embargo) {
  WriteTx tx(db);
  if (auto r = tx.begin(); !r) return fail(r.error());
  const Uuid uuid = Uuid::v7();
  Row row;
  row["uuid"] = qv(uuid);
  row["scope_type"] = qstr(to_string(embargo.scope.type));
  row["scope_uuid"] = qv(embargo.scope.uuid);
  row["until_utc"] = qv(embargo.until);
  row["reason"] = qv(embargo.reason);
  row["set_by_user_uuid"] = qv(actor.user);
  row["created_utc"] = qv(UtcTime::now());
  if (auto r = db.insert("embargo", row); !r) return fail(r.error());
  ChangeEntityRow e = entity("embargo", uuid);
  e.detail_json = json_created({{"scope_type", std::string(to_string(embargo.scope.type))},
                                {"scope_uuid", embargo.scope.uuid.str()},
                                {"until_utc", embargo.until ? std::optional<std::string>(embargo.until->iso())
                                                            : std::nullopt}});
  if (auto seq = take_change(db, QStringLiteral("catalog"), std::nullopt, actor.client, {e}); !seq)
    return fail(seq.error());
  if (auto r = tx.commit(); !r) return fail(r.error());
  return uuid;
}

Result<void> lift_embargo(Db& db, const Actor& actor, Uuid embargo) {
  WriteTx tx(db);
  if (auto r = tx.begin(); !r) return fail(r.error());
  auto row = db.select_one(sql::kEmbargoLifted, {qv(embargo)});
  if (!row) return fail(row.error());
  if (!*row) return fail(ErrorKind::Config, "no embargo " + embargo.str());
  if (!(*row)->value("lifted_utc").isNull()) return fail(ErrorKind::Config, "embargo " + embargo.str() + " is already lifted");
  auto n = db.affecting(sql::kLiftEmbargo, {qv(UtcTime::now()), qv(actor.user), qv(embargo)});
  if (!n) return fail(n.error());
  if (auto seq = take_change(db, QStringLiteral("catalog"), std::nullopt, actor.client, {entity("embargo", embargo, "update")});
      !seq)
    return fail(seq.error());
  return tx.commit();
}

Result<std::vector<EmbargoRow>> embargoes(Db& db, bool live_only, UtcTime at) {
  Bindings b;
  if (live_only) b.push_back(qv(at));
  auto rows = db.select(sql::embargoes_select(db.dialect(), live_only), b);
  if (!rows) return fail(rows.error());
  std::vector<EmbargoRow> out;
  for (const auto& r : *rows) {
    const auto type = parse_embargo_scope_type(to_std(r.value("scope_type")));
    if (!type) return fail(ErrorKind::Protocol, "embargo row has an unknown scope type");
    EmbargoRow e;
    e.uuid = to_uuid(r.value("uuid"));
    e.scope = {*type, to_uuid(r.value("scope_uuid"))};
    e.until = opt_time(r.value("until_t"));
    e.reason = opt_str(r.value("reason"));
    e.set_by = to_uuid(r.value("set_by_user_uuid"));
    e.created = to_time(r.value("created"));
    e.lifted = opt_time(r.value("lifted"));
    e.lifted_by = opt_uuid(r.value("lifted_by_user_uuid"));
    out.push_back(std::move(e));
  }
  return out;
}

// IN (...) lists are bound in parts of at most kChunk uuids (SQLite's
// default variable limit is 32766; refs.cpp chunks the same way).
constexpr std::size_t kChunk = 500;

Result<std::vector<Uuid>> embargoed_samples(Db& db, std::span<const Uuid> samples, UtcTime at) {
  std::set<Uuid> out;
  for (std::size_t start = 0; start < samples.size(); start += kChunk) {
    const auto part = samples.subspan(start, std::min(kChunk, samples.size() - start));
    Bindings b = uuid_bindings(part);
    b.push_back(qv(at));
    b.push_back(qv(at));
    auto found = uuid_column(db, sql::embargoed_samples(static_cast<qsizetype>(part.size())), b, "uuid");
    if (!found) return fail(found.error());
    out.insert(found->begin(), found->end());
  }
  return std::vector<Uuid>(out.begin(), out.end());
}

Result<std::vector<Uuid>> embargoed_analyses(Db& db, std::span<const Uuid> analyses, UtcTime at) {
  if (analyses.empty()) return std::vector<Uuid>{};
  // The analyses' samples, then the samples under embargo, then the analyses named directly.
  std::map<Uuid, Uuid> sample_of;
  std::set<Uuid> samples;
  for (std::size_t start = 0; start < analyses.size(); start += kChunk) {
    const auto part = analyses.subspan(start, std::min(kChunk, analyses.size() - start));
    auto rows = db.select(sql::analysis_samples(static_cast<qsizetype>(part.size())), uuid_bindings(part));
    if (!rows) return fail(rows.error());
    for (const auto& r : *rows) {
      auto s = opt_uuid(r.value("own"));
      if (!s) s = opt_uuid(r.value("positioned"));
      if (!s) continue;
      sample_of[to_uuid(r.value("uuid"))] = *s;
      samples.insert(*s);
    }
  }
  const std::vector<Uuid> sample_list(samples.begin(), samples.end());
  auto embargoed = embargoed_samples(db, sample_list, at);
  if (!embargoed) return fail(embargoed.error());
  const std::set<Uuid> hidden(embargoed->begin(), embargoed->end());
  std::set<Uuid> out;
  for (const auto& [analysis, sample] : sample_of)
    if (hidden.contains(sample)) out.insert(analysis);
  for (std::size_t start = 0; start < analyses.size(); start += kChunk) {
    const auto part = analyses.subspan(start, std::min(kChunk, analyses.size() - start));
    Bindings b = uuid_bindings(part);
    b.push_back(qv(at));
    auto direct = uuid_column(db, sql::embargoed_analyses_direct(static_cast<qsizetype>(part.size())), b, "uuid");
    if (!direct) return fail(direct.error());
    out.insert(direct->begin(), direct->end());
  }
  return std::vector<Uuid>(out.begin(), out.end());
}

// ---------------------------------------------------------------- external ids

Result<Uuid> add_external_id(Db& db, const Actor& actor, const ExternalIdInsert& id) {
  WriteTx tx(db);
  if (auto r = tx.begin(); !r) return fail(r.error());
  const Uuid uuid = Uuid::v7();
  Row row;
  row["uuid"] = qv(uuid);
  row["entity_type"] = qv(id.entity_type);
  row["entity_uuid"] = qv(id.entity);
  row["scheme"] = qv(id.scheme);
  row["value"] = qv(id.value);
  row["registrar"] = qv(id.registrar);
  row["state"] = qv(id.state);
  row["request_sha256"] = qv(id.request_sha256);
  row["response"] = qv(id.response_json);
  row["registered_utc"] = qv(UtcTime::now());
  if (auto r = db.insert("external_id", row); !r) return fail(r.error());
  ChangeEntityRow e = entity("external_id", uuid);
  e.detail_json = json_created({{"scheme", id.scheme}, {"value", id.value}, {"entity_uuid", id.entity.str()}});
  if (auto seq = take_change(db, QStringLiteral("catalog"), std::nullopt, actor.client, {e}); !seq)
    return fail(seq.error());
  if (auto r = tx.commit(); !r) return fail(r.error());
  return uuid;
}

Result<std::vector<ExternalIdRow>> external_ids(Db& db, std::string_view entity_type, Uuid entity) {
  auto rows = db.select(sql::external_ids_select(db.dialect()), {qstr(entity_type), qv(entity)});
  if (!rows) return fail(rows.error());
  std::vector<ExternalIdRow> out;
  for (const auto& r : *rows) {
    ExternalIdRow x;
    x.uuid = to_uuid(r.value("uuid"));
    x.entity_type = to_std(r.value("entity_type"));
    x.entity = to_uuid(r.value("entity_uuid"));
    x.scheme = to_std(r.value("scheme"));
    x.value = to_std(r.value("value"));
    x.registrar = to_std(r.value("registrar"));
    x.state = to_std(r.value("state"));
    x.request_sha256 = to_digest(r.value("request_sha256"));
    x.response_json = opt_str(r.value("response"));
    x.registered = to_time(r.value("registered"));
    out.push_back(std::move(x));
  }
  return out;
}

}  // namespace detail
}  // namespace pychron::persistence
