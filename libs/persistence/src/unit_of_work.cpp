// Unit of work and compare-and-swap commit (DVC schema spec, sections 5.4, 12.3).

#include <algorithm>
#include <set>
#include <tuple>

#include "catalog_impl.hpp"
#include "sql/statements.hpp"
#include "store_impl.hpp"

namespace pychron::persistence::detail {
namespace {

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

class TinyUnitOfWork final : public IUnitOfWork, public StagedRefs {
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
    if (auto r = prepare(kind, std::move(message)); !r) return fail(r.error());
    WriteTx tx(db_);
    if (auto r = tx.begin(); !r) return fail(r.error());
    std::vector<ChangeEntityRow> entities;
    auto lost = write_staged(entities);
    if (!lost) return fail(lost.error());
    if (!lost->empty()) {
      tx.rollback();
      // Read the winners after the rollback, outside any transaction.
      for (auto& c : *lost) {
        auto actual = read_head(db_, c.subject, c.kind);
        if (!actual) return fail(actual.error());
        if (*actual) {
          c.actual = *actual;
          auto by = changeset_of_revision(db_, *c.actual);
          if (!by) return fail(by.error());
          c.actual_by = *by;
        }
      }
      return CommitOutcome{std::move(*lost)};
    }
    auto seq = take_change(db_, QStringLiteral("changeset"), changeset_->uuid, actor_.client, entities);
    if (!seq) return fail(seq.error());
    if (auto r = tx.commit(); !r) return fail(r.error());
    return CommitOutcome{Committed{changeset_->uuid, *seq}};
  }

  // Consumes the staging and fixes the changeset this unit of work writes.
  Result<void> prepare(ChangesetKind kind, std::string message) {
    if (done_) return fail(ErrorKind::Protocol, "unit of work already committed");
    done_ = true;
    if (moves_.empty()) return fail(ErrorKind::Protocol, "empty changeset");
    if (kind == ChangesetKind::Collection)
      return fail(ErrorKind::Protocol, "collection changesets are created by ingest only");
    changeset_ = ChangesetInfo{Uuid::v7(), kind, actor_.user, actor_.client, UtcTime::now(), std::move(message)};
    // Ascending (subject, kind) so concurrent multi-subject commits lock head
    // rows in one global order and cannot deadlock.
    std::sort(moves_.begin(), moves_.end(), [](const StagedMove& a, const StagedMove& b) {
      return std::tuple(a.subject.str(), to_string(a.kind)) < std::tuple(b.subject.str(), to_string(b.kind));
    });
    return {};
  }

  // StagedRefs: inside a transaction someone else opened.
  Result<std::vector<RefConflict>> write(std::vector<ChangeEntityRow>& entities) override {
    auto lost = write_staged(entities);
    if (!lost) return fail(lost.error());
    std::vector<RefConflict> out;
    for (const auto& c : *lost) out.push_back(RefConflict{c.subject, c.expected, std::nullopt});
    if (!out.empty()) {
      // The transaction is rolled back by the caller; the winners are read now
      // (the head rows are not locked by a statement that changed nothing).
      for (auto& c : out) {
        for (const auto& m : moves_)
          if (m.subject == c.subject) {
            auto actual = read_head(db_, m.subject, m.kind);
            if (!actual) return fail(actual.error());
            c.actual = *actual;
          }
      }
    }
    return out;
  }
  bool uses(const Db& db) const noexcept { return &db == &db_; }
  std::optional<Uuid> changeset() const override {
    return changeset_ ? std::optional<Uuid>{changeset_->uuid} : std::nullopt;
  }

 private:
  // Changeset, revisions, payloads, CAS head moves; on success also the
  // identity rewrites and head_move rows. Returns the moves that lost.
  Result<std::vector<Conflict>> write_staged(std::vector<ChangeEntityRow>& entities) {
    const ChangesetInfo& changeset = *changeset_;
    if (auto r = insert_changeset(db_, changeset); !r) return fail(r.error());
    for (const auto& rev : revisions_) {
      if (auto r = insert_revision(db_, rev.uuid, changeset.uuid, rev.subject, rev.kind, rev.parent, changeset.created);
          !r)
        return fail(r.error());
      if (auto r = write_payload(db_, rev.uuid, rev.subject, rev.payload); !r) return fail(r.error());
    }
    std::vector<Conflict> conflicts;
    for (const auto& m : moves_) {
      auto moved = cas_head(db_, m.subject, m.kind, m.expected, m.to);
      if (!moved) return fail(moved.error());
      if (!*moved) conflicts.push_back(Conflict{m.subject, m.kind, m.expected, std::nullopt, std::nullopt});
    }
    if (!conflicts.empty()) return conflicts;
    for (const auto& m : moves_)
      if (m.kind == Kind::Identity)
        if (auto r = apply_identity(m); !r) return fail(r.error());
    std::set<std::pair<std::string, Uuid>> seen;
    for (const auto& m : moves_) {
      if (auto r = insert_head_move(db_, changeset.uuid, m.subject, m.kind, m.expected, m.to, m.reason); !r)
        return fail(r.error());
      const QString type = entity_type_of(subject_type_of(m.kind));
      if (seen.emplace(type.toStdString(), m.subject).second)
        entities.push_back(ChangeEntityRow{type, m.subject, QStringLiteral("upsert"), std::nullopt});
    }
    return conflicts;
  }

  Result<void> check_open(Uuid subject, Kind kind) const {
    if (done_) return fail(ErrorKind::Protocol, "unit of work already committed");
    for (const auto& m : moves_)
      if (m.subject == subject && m.kind == kind)
        return fail(ErrorKind::Protocol, "subject/kind staged twice in one unit of work");
    return {};
  }

  // The identity value a move points at: staged here, or already stored.
  Result<void> apply_identity(const StagedMove& m) {
    std::optional<IdentityValue> value;
    for (const auto& rev : revisions_)
      if (rev.uuid == m.to) value = std::get<IdentityValue>(rev.payload);
    if (!value) {
      auto stored = read_payload(db_, m.to, Kind::Identity);
      if (!stored) return fail(stored.error());
      value = std::get<IdentityValue>(*stored);
    }
    return detail::apply_identity(db_, m.subject, *value);
  }

  Db& db_;
  Actor actor_;
  std::vector<StagedRevision> revisions_;
  std::vector<StagedMove> moves_;
  bool done_ = false;
  std::optional<ChangesetInfo> changeset_;
};

}  // namespace

std::unique_ptr<IUnitOfWork> make_unit_of_work(Db& db, const Actor& actor) {
  return std::make_unique<TinyUnitOfWork>(db, actor);
}

Result<StagedRefs*> prepare_staged(IUnitOfWork& uow, Db& db, ChangesetKind kind, std::string message) {
  auto* tiny = dynamic_cast<TinyUnitOfWork*>(&uow);
  if (!tiny || !tiny->uses(db)) return fail(ErrorKind::Protocol, "the unit of work is not from this store");
  if (auto r = tiny->prepare(kind, std::move(message)); !r) return fail(r.error());
  return static_cast<StagedRefs*>(tiny);
}

}  // namespace pychron::persistence::detail
