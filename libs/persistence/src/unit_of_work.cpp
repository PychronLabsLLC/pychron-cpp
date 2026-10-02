// Unit of work and compare-and-swap commit (DVC schema spec, sections 5.4, 12.3).

#include <algorithm>
#include <set>
#include <tuple>

#include "sql/statements.hpp"
#include "store_impl.hpp"

namespace pychron::persistence::detail {
namespace {

QString qstr(std::string_view s) { return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size())); }

struct StagedRevision {
  Uuid uuid;
  Uuid subject;
  Kind kind;
  std::optional<Uuid> parent;
  RevisionPayload payload;
};

struct StagedMove {
  Uuid subject;
  Kind kind;
  std::optional<Uuid> expected;
  Uuid to;
  MoveReason reason;
};

class TinyUnitOfWork final : public IUnitOfWork {
 public:
  TinyUnitOfWork(Db& db, const Actor& actor) : db_(db), actor_(actor) {}

  Result<Uuid> add_revision(Uuid subject, Kind kind, RevisionPayload payload,
                            std::optional<Uuid> expected_head) override {
    if (auto r = check_open(subject, kind); !r) return fail(r.error());
    if (!payload_kind_matches(kind, payload))
      return fail(ErrorKind::Protocol, "payload does not match revision kind '" + std::string(to_string(kind)) + "'");
    const Uuid id = Uuid::v7();
    revisions_.push_back(StagedRevision{id, subject, kind, expected_head, std::move(payload)});
    moves_.push_back(StagedMove{subject, kind, expected_head, id, MoveReason::Commit});
    return id;
  }

  Result<void> move_head(Uuid subject, Kind kind, std::optional<Uuid> expected, Uuid to, MoveReason reason) override {
    if (auto r = check_open(subject, kind); !r) return r;
    if (reason == MoveReason::Ingest)
      return fail(ErrorKind::Protocol, "ingest head moves are written by ingest only");
    moves_.push_back(StagedMove{subject, kind, expected, to, reason});
    return {};
  }

  Result<CommitOutcome> commit(ChangesetKind kind, std::string message) override {
    if (done_) return fail(ErrorKind::Protocol, "unit of work already committed");
    done_ = true;
    if (moves_.empty()) return fail(ErrorKind::Protocol, "empty changeset");
    if (kind == ChangesetKind::Collection)
      return fail(ErrorKind::Protocol, "collection changesets are created by ingest only");

    const ChangesetInfo changeset{Uuid::v7(), kind, actor_.user, actor_.client, UtcTime::now(), std::move(message)};

    // Ascending (subject, kind) so concurrent multi-subject commits lock head
    // rows in one global order and cannot deadlock.
    std::sort(moves_.begin(), moves_.end(), [](const StagedMove& a, const StagedMove& b) {
      return std::tuple(a.subject.str(), to_string(a.kind)) < std::tuple(b.subject.str(), to_string(b.kind));
    });

    WriteTx tx(db_);
    if (auto r = tx.begin(); !r) return fail(r.error());
    if (auto r = insert_changeset(db_, changeset); !r) return fail(r.error());
    for (const auto& rev : revisions_) {
      if (auto r = insert_revision(db_, rev.uuid, changeset.uuid, rev.subject, rev.kind, rev.parent, changeset.created);
          !r)
        return fail(r.error());
      if (auto r = write_payload(db_, rev.uuid, rev.subject, rev.payload); !r) return fail(r.error());
    }

    std::vector<Conflict> conflicts;
    for (const auto& m : moves_) {
      Result<int> affected =
          m.expected ? db_.affecting(sql::kCasHead, {qv(m.to), qv(m.subject), qstr(to_string(m.kind)), qv(*m.expected)})
                     : db_.affecting(sql::kInsertFirstHead, {qv(m.subject), qstr(to_string(m.kind)), qv(m.to)});
      if (!affected) return fail(affected.error());
      if (*affected == 0) conflicts.push_back(Conflict{m.subject, m.kind, m.expected, std::nullopt, std::nullopt});
    }

    if (!conflicts.empty()) {
      tx.rollback();
      // Read the winners after the rollback, outside any transaction.
      for (auto& c : conflicts) {
        auto actual = db_.select_one(sql::kSelectHead, {qv(c.subject), qstr(to_string(c.kind))});
        if (!actual) return fail(actual.error());
        if (*actual) {
          c.actual = to_uuid((*actual)->value("revision_uuid"));
          auto by = changeset_of_revision(db_, *c.actual);
          if (!by) return fail(by.error());
          c.actual_by = *by;
        }
      }
      return CommitOutcome{std::move(conflicts)};
    }

    for (const auto& m : moves_)
      if (m.kind == Kind::Identity)
        if (auto r = apply_identity(m); !r) return fail(r.error());

    std::vector<ChangeEntityRow> entities;
    std::set<std::pair<std::string, Uuid>> seen;
    for (const auto& m : moves_) {
      if (auto r = insert_head_move(db_, changeset.uuid, m.subject, m.kind, m.expected, m.to, m.reason); !r)
        return fail(r.error());
      const QString type = entity_type_of(subject_type_of(m.kind));
      if (seen.emplace(type.toStdString(), m.subject).second) entities.push_back(ChangeEntityRow{type, m.subject, QStringLiteral("upsert"), std::nullopt});
    }
    auto seq = take_change(db_, QStringLiteral("changeset"), changeset.uuid, actor_.client, entities);
    if (!seq) return fail(seq.error());
    if (auto r = tx.commit(); !r) return fail(r.error());
    return CommitOutcome{Committed{changeset.uuid, *seq}};
  }

 private:
  Result<void> check_open(Uuid subject, Kind kind) const {
    if (done_) return fail(ErrorKind::Protocol, "unit of work already committed");
    for (const auto& m : moves_)
      if (m.subject == subject && m.kind == kind)
        return fail(ErrorKind::Protocol, "subject/kind staged twice in one unit of work");
    return {};
  }

  // The analysis row mirrors its identity head: rewrite the identity columns
  // and runid_text in the same transaction (UNIQUE keeps holding, I9).
  Result<void> apply_identity(const StagedMove& m) {
    std::optional<IdentityValue> value;
    for (const auto& rev : revisions_)
      if (rev.uuid == m.to) value = std::get<IdentityValue>(rev.payload);
    if (!value) {
      auto stored = read_payload(db_, m.to, Kind::Identity);
      if (!stored) return fail(stored.error());
      value = std::get<IdentityValue>(*stored);
    }
    auto text = db_.select_one(sql::kIdentifierText, {qv(value->identifier)});
    if (!text) return fail(text.error());
    if (!*text) return fail(ErrorKind::Protocol, "identity revision names an unknown identifier");
    const std::string runid = make_runid(to_std((*text)->value("identifier")), value->aliquot, value->increment);
    auto updated = db_.affecting(sql::kApplyIdentity, {qv(value->identifier), value->aliquot, value->increment,
                                                       qv(runid), qv(m.subject)});
    if (!updated) return fail(updated.error());
    if (*updated != 1) return fail(ErrorKind::Protocol, "identity revision for a subject that is not an analysis");
    return {};
  }

  Db& db_;
  Actor actor_;
  std::vector<StagedRevision> revisions_;
  std::vector<StagedMove> moves_;
  bool done_ = false;
};

}  // namespace

std::unique_ptr<IUnitOfWork> make_unit_of_work(Db& db, const Actor& actor) {
  return std::make_unique<TinyUnitOfWork>(db, actor);
}

}  // namespace pychron::persistence::detail
