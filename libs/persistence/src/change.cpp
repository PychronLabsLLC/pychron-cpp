#include <cstdio>

#include "sql/statements.hpp"
#include "store_impl.hpp"

namespace pychron::persistence::detail {

std::string json_string(std::string_view s) {
  std::string out = "\"";
  for (char c : s) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned>(c));
          out += buf;
        } else {
          out.push_back(c);
        }
    }
  }
  out.push_back('"');
  return out;
}

std::string json_created(const std::vector<std::pair<std::string, std::optional<std::string>>>& fields) {
  std::string out = "{";
  for (std::size_t i = 0; i < fields.size(); ++i) {
    if (i) out += ", ";
    out += json_string(fields[i].first) + ": [null, ";
    out += fields[i].second ? json_string(*fields[i].second) : std::string("null");
    out += "]";
  }
  return out + "}";
}

QString entity_type_of(SubjectType type) {
  switch (type) {
    case SubjectType::Analysis:
      return QStringLiteral("analysis");
    case SubjectType::Ref:
      return QStringLiteral("ref_object");
    case SubjectType::InterpretedAge:
      return QStringLiteral("interpreted_age");
  }
  return {};
}

Result<ChangeSeq> take_change(Db& db, const QString& kind, std::optional<Uuid> changeset, Uuid client,
                              const std::vector<ChangeEntityRow>& entities) {
  auto next = db.select_one(sql::kNextChangeSeq);
  if (!next) return fail(next.error());
  if (!*next) return fail(ErrorKind::Protocol, "change_counter row is missing");
  const ChangeSeq seq = (*next)->value("seq").toLongLong();

  Row log;
  log["change_seq"] = static_cast<qlonglong>(seq);
  log["changeset_uuid"] = qv(changeset);
  log["client_uuid"] = qv(client);
  log["kind"] = kind;
  if (auto r = db.insert("change_log", log); !r) return fail(r.error());

  QList<QVariantMap> rows;
  for (const auto& e : entities) {
    Row r;
    r["change_seq"] = static_cast<qlonglong>(seq);
    r["entity_type"] = e.entity_type;
    r["entity_uuid"] = qv(e.entity);
    r["op"] = e.op;
    r["detail"] = qv(e.detail_json);
    rows.push_back(std::move(r));
  }
  if (auto r = db.insert_many("change_entity", rows); !r) return fail(r.error());

  if (db.dialect() == Dialect::PostgreSql) {
    // A hint only (section 9.3); clients always poll the cursor for data.
    const std::string payload =
        "{\"seq\": " + std::to_string(seq) + ", \"kinds\": [" + json_string(kind.toStdString()) + "]}";
    if (auto r = db.select(sql::kNotify, {qv(payload)}); !r) return fail(r.error());
  }
  return seq;
}

namespace {
QString qstr(std::string_view s) { return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size())); }

Row changeset_row(const ChangesetInfo& c, std::optional<Uuid> import_source) {
  Row r;
  r["uuid"] = qv(c.uuid);
  r["kind"] = qstr(to_string(c.kind));
  r["author_user_uuid"] = qv(c.author_user);
  r["client_uuid"] = qv(c.client);
  r["message"] = qv(c.message);
  r["created_utc"] = qv(c.created);
  if (import_source) r["import_source_uuid"] = qv(*import_source);
  return r;
}
}  // namespace

Result<void> insert_changeset(Db& db, const ChangesetInfo& c, std::optional<Uuid> import_source) {
  return db.insert("changeset", changeset_row(c, import_source));
}

Result<bool> insert_changeset_if_absent(Db& db, const ChangesetInfo& c, std::optional<Uuid> import_source) {
  auto inserted = db.insert_or_ignore("changeset", changeset_row(c, import_source));
  if (!inserted) return fail(inserted.error());
  return *inserted > 0;
}

Result<void> insert_revision(Db& db, Uuid revision, Uuid changeset, Uuid subject, Kind kind,
                             std::optional<Uuid> parent, UtcTime created) {
  const SubjectType type = subject_type_of(kind);
  Row r;
  r["uuid"] = qv(revision);
  r["changeset_uuid"] = qv(changeset);
  r["subject_type"] = qstr(to_string(type));
  r["subject_uuid"] = qv(subject);
  r["analysis_uuid"] = type == SubjectType::Analysis ? qv(subject) : QVariant();
  r["ref_object_uuid"] = type == SubjectType::Ref ? qv(subject) : QVariant();
  r["ia_uuid"] = type == SubjectType::InterpretedAge ? qv(subject) : QVariant();
  r["kind"] = qstr(to_string(kind));
  r["parent_uuid"] = qv(parent);
  r["created_utc"] = qv(created);
  return db.insert("revision", r);
}

Result<void> insert_head_move(Db& db, Uuid changeset, Uuid subject, Kind kind, std::optional<Uuid> from, Uuid to,
                              MoveReason reason) {
  Row r;
  r["uuid"] = qv(Uuid::v7());
  r["changeset_uuid"] = qv(changeset);
  r["subject_uuid"] = qv(subject);
  r["kind"] = qstr(to_string(kind));
  r["from_revision_uuid"] = qv(from);
  r["to_revision_uuid"] = qv(to);
  r["reason"] = qstr(to_string(reason));
  return db.insert("head_move", r);
}

Result<std::optional<Uuid>> read_head(Db& db, Uuid subject, Kind kind) {
  auto row = db.select_one(sql::kSelectHead, {qv(subject), qstr(to_string(kind))});
  if (!row) return fail(row.error());
  if (!*row) return std::optional<Uuid>{};
  return std::optional<Uuid>{to_uuid((*row)->value("revision_uuid"))};
}

Result<bool> cas_head(Db& db, Uuid subject, Kind kind, std::optional<Uuid> expected, Uuid to) {
  Result<int> affected =
      expected ? db.affecting(sql::kCasHead, {qv(to), qv(subject), qstr(to_string(kind)), qv(*expected)})
               : db.affecting(sql::kInsertFirstHead, {qv(subject), qstr(to_string(kind)), qv(to)});
  if (!affected) return fail(affected.error());
  return *affected != 0;
}

Result<void> apply_identity(Db& db, Uuid analysis, const IdentityValue& value) {
  auto text = db.select_one(sql::kIdentifierText, {qv(value.identifier)});
  if (!text) return fail(text.error());
  if (!*text) return fail(ErrorKind::Protocol, "identity revision names an unknown identifier");
  const std::string runid = make_runid(to_std((*text)->value("identifier")), value.aliquot, value.increment);
  auto updated =
      db.affecting(sql::kApplyIdentity, {qv(value.identifier), value.aliquot, value.increment, qv(runid), qv(analysis)});
  if (!updated) return fail(updated.error());
  if (*updated != 1) return fail(ErrorKind::Protocol, "identity revision for a subject that is not an analysis");
  return {};
}

Result<std::optional<ChangesetInfo>> changeset_of_revision(Db& db, Uuid revision) {
  auto row = db.select_one(sql::kChangesetOfRevision.arg(sql::ts(db.dialect(), QStringLiteral("c.created_utc"))),
                           {qv(revision)});
  if (!row) return fail(row.error());
  if (!*row) return std::optional<ChangesetInfo>{};
  const Row& r = **row;
  ChangesetInfo c;
  c.uuid = to_uuid(r.value("uuid"));
  c.kind = parse_changeset_kind(to_std(r.value("kind"))).value_or(ChangesetKind::Reduction);
  c.author_user = to_uuid(r.value("author_user_uuid"));
  c.client = to_uuid(r.value("client_uuid"));
  c.created = to_time(r.value("created"));
  c.message = to_std(r.value("message"));
  return std::optional<ChangesetInfo>{c};
}

}  // namespace pychron::persistence::detail
