// Repositories, groups, bookmarks and rollback to collection (DVC schema spec,
// sections 3.6, 5.5).

#include <map>
#include <set>

#include "sql/statements.hpp"
#include "store_impl.hpp"

namespace pychron::persistence::detail {
namespace {

QString qstr(std::string_view s) { return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size())); }

ChangeEntityRow entity(const char* type, Uuid uuid, const char* op = "insert") {
  return ChangeEntityRow{QString::fromUtf8(type), uuid, QString::fromUtf8(op), std::nullopt};
}

Result<std::vector<HeadInfo>> heads_of(Db& db, const QString& sql, Uuid scope) {
  auto rows = db.select(sql, {qv(scope)});
  if (!rows) return fail(rows.error());
  std::vector<HeadInfo> out;
  for (const auto& r : *rows) {
    const auto kind = parse_kind(to_std(r.value("kind")));
    if (!kind) return fail(ErrorKind::Protocol, "unknown head kind");
    out.push_back(HeadInfo{to_uuid(r.value("subject_uuid")), *kind, to_uuid(r.value("revision_uuid")),
                           r.value("head_version").toInt()});
  }
  return out;
}

// Commits the staged moves, or reports "nothing to do" as Committed{nil, 0}.
Result<CommitOutcome> commit_moves(Db& db, const Actor& actor, ChangesetKind kind, std::string message,
                                   const std::vector<std::tuple<Uuid, Kind, std::optional<Uuid>, Uuid>>& moves,
                                   MoveReason reason) {
  if (moves.empty()) return CommitOutcome{Committed{}};
  auto uow = make_unit_of_work(db, actor);
  for (const auto& [subject, k, expected, to] : moves)
    if (auto r = uow->move_head(subject, k, expected, to, reason); !r) return fail(r.error());
  return uow->commit(kind, std::move(message));
}

}  // namespace

Result<void> add_repository_members(Db& db, const Actor& actor, Uuid repository, const std::vector<Uuid>& analyses) {
  WriteTx tx(db);
  if (auto r = tx.begin(); !r) return fail(r.error());
  std::vector<ChangeEntityRow> entities{entity("repository", repository, "upsert")};
  for (const auto& a : analyses) {
    Row r;
    r["repository_uuid"] = qv(repository);
    r["analysis_uuid"] = qv(a);
    auto ins = db.insert_or_ignore("repository_member", r);
    if (!ins) return fail(ins.error());
  }
  auto seq = take_change(db, QStringLiteral("catalog"), std::nullopt, actor.client, entities);
  if (!seq) return fail(seq.error());
  return tx.commit();
}

Result<Uuid> create_group(Db& db, const Actor& actor, const std::string& name, const std::vector<Uuid>& analyses,
                          std::optional<Uuid> given) {
  WriteTx tx(db);
  if (auto r = tx.begin(); !r) return fail(r.error());
  if (given) {
    auto stored = db.select_one(QStringLiteral("SELECT uuid FROM analysis_group WHERE uuid = ?"), {qv(*given)});
    if (!stored) return fail(stored.error());
    if (*stored) return *given;
  }
  const Uuid uuid = given.value_or(Uuid::v7());
  Row g;
  g["uuid"] = qv(uuid);
  g["name"] = qv(name);
  g["created_by_user_uuid"] = qv(actor.user);
  g["created_utc"] = qv(UtcTime::now());
  if (auto r = db.insert("analysis_group", g); !r) return fail(r.error());
  QList<QVariantMap> members;
  for (const auto& a : std::set<Uuid>(analyses.begin(), analyses.end())) {
    Row m;
    m["group_uuid"] = qv(uuid);
    m["analysis_uuid"] = qv(a);
    members.push_back(std::move(m));
  }
  if (auto r = db.insert_many("analysis_group_member", members); !r) return fail(r.error());
  ChangeEntityRow e = entity("analysis_group", uuid);
  e.detail_json = json_created({{"name", name}});
  if (auto seq = take_change(db, QStringLiteral("catalog"), std::nullopt, actor.client, {e}); !seq)
    return fail(seq.error());
  if (auto r = tx.commit(); !r) return fail(r.error());
  return uuid;
}

Result<Uuid> create_bookmark(Db& db, const Actor& actor, const BookmarkSpec& spec) {
  if (spec.repository.has_value() == spec.group.has_value())
    return fail(ErrorKind::Protocol, "a bookmark needs exactly one of repository or group");
  WriteTx tx(db);
  if (auto r = tx.begin(); !r) return fail(r.error());
  if (spec.uuid) {
    auto stored = db.select_one(QStringLiteral("SELECT uuid FROM bookmark WHERE uuid = ?"), {qv(*spec.uuid)});
    if (!stored) return fail(stored.error());
    if (*stored) return *spec.uuid;
  }
  // Read the heads inside the write transaction so the capture is consistent.
  auto heads = spec.repository ? heads_of(db, sql::kHeadsInRepository, *spec.repository)
                               : heads_of(db, sql::kHeadsInGroup, *spec.group);
  if (!heads) return fail(heads.error());
  const Uuid uuid = spec.uuid.value_or(Uuid::v7());
  Row b;
  b["uuid"] = qv(uuid);
  b["name"] = qv(spec.name);
  b["repository_uuid"] = qv(spec.repository);
  b["group_uuid"] = qv(spec.group);
  b["message"] = qv(spec.message);
  b["author_user_uuid"] = qv(actor.user);
  b["created_utc"] = qv(UtcTime::now());
  if (auto r = db.insert("bookmark", b); !r) return fail(r.error());
  QList<QVariantMap> entries;
  for (const auto& h : *heads) {
    Row e;
    e["bookmark_uuid"] = qv(uuid);
    e["subject_uuid"] = qv(h.subject);
    e["kind"] = qstr(to_string(h.kind));
    e["revision_uuid"] = qv(h.revision);
    entries.push_back(std::move(e));
  }
  if (auto r = db.insert_many("bookmark_entry", entries); !r) return fail(r.error());
  ChangeEntityRow e = entity("bookmark", uuid);
  e.detail_json = json_created({{"name", spec.name}});
  if (auto seq = take_change(db, QStringLiteral("catalog"), std::nullopt, actor.client, {e}); !seq)
    return fail(seq.error());
  if (auto r = tx.commit(); !r) return fail(r.error());
  return uuid;
}

Result<std::vector<HeadInfo>> bookmark_heads(Db& db, Uuid bookmark) {
  auto rows = db.select(sql::kBookmarkEntries, {qv(bookmark)});
  if (!rows) return fail(rows.error());
  std::vector<HeadInfo> out;
  for (const auto& r : *rows) {
    const auto kind = parse_kind(to_std(r.value("kind")));
    if (!kind) return fail(ErrorKind::Protocol, "unknown bookmark kind");
    out.push_back(HeadInfo{to_uuid(r.value("subject_uuid")), *kind, to_uuid(r.value("revision_uuid")), 0});
  }
  return out;
}

Result<CommitOutcome> restore_bookmark(Db& db, const Actor& actor, Uuid bookmark, std::string message) {
  auto entries = bookmark_heads(db, bookmark);
  if (!entries) return fail(entries.error());
  if (entries->empty()) {
    auto exists = db.select_one(QStringLiteral("SELECT uuid FROM bookmark WHERE uuid = ?"), {qv(bookmark)});
    if (!exists) return fail(exists.error());
    if (!*exists) return fail(ErrorKind::Protocol, "unknown bookmark " + bookmark.str());
  }
  auto current = db.select(sql::kCurrentHeadsOfBookmark, {qv(bookmark)});
  if (!current) return fail(current.error());
  std::map<std::pair<Uuid, std::string>, std::optional<Uuid>> now;
  for (const auto& r : *current)
    now[{to_uuid(r.value("subject_uuid")), to_std(r.value("kind"))}] = opt_uuid(r.value("revision_uuid"));
  std::vector<std::tuple<Uuid, Kind, std::optional<Uuid>, Uuid>> moves;
  for (const auto& e : *entries) {
    const auto expected = now[{e.subject, std::string(to_string(e.kind))}];
    if (expected != e.revision) moves.emplace_back(e.subject, e.kind, expected, e.revision);
  }
  return commit_moves(db, actor, ChangesetKind::BookmarkRestore, std::move(message), moves,
                      MoveReason::BookmarkRestore);
}

Result<CommitOutcome> rollback_to_collection(Db& db, const Actor& actor, Uuid analysis, std::string message,
                                             std::vector<Kind> kinds) {
  if (kinds.empty()) kinds.assign(std::begin(kCollectionKinds), std::end(kCollectionKinds));
  auto roots = db.select(sql::kCollectionRevisions, {qv(analysis)});
  if (!roots) return fail(roots.error());
  if (roots->empty()) return fail(ErrorKind::Protocol, "unknown analysis " + analysis.str());
  std::map<std::string, Uuid> root_of;
  for (const auto& r : *roots) root_of[to_std(r.value("kind"))] = to_uuid(r.value("uuid"));
  auto heads = read_heads(db, analysis);
  if (!heads) return fail(heads.error());
  std::map<Kind, Uuid> head_of;
  for (const auto& h : *heads) head_of[h.kind] = h.revision;

  std::vector<std::tuple<Uuid, Kind, std::optional<Uuid>, Uuid>> moves;
  for (Kind k : kinds) {
    auto root = root_of.find(std::string(to_string(k)));
    if (root == root_of.end())
      return fail(ErrorKind::Protocol, "kind '" + std::string(to_string(k)) + "' has no collection revision");
    auto head = head_of.find(k);
    const std::optional<Uuid> expected = head == head_of.end() ? std::nullopt : std::optional<Uuid>(head->second);
    if (expected != root->second) moves.emplace_back(analysis, k, expected, root->second);
  }
  return commit_moves(db, actor, ChangesetKind::Rollback, std::move(message), moves, MoveReason::CollectionRestore);
}

}  // namespace pychron::persistence::detail
