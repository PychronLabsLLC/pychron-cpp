// Import source, provenance and conflict reads, and the import unit of work
// (legacy ingestion spec; DVC schema spec, section 13).

#include <gtest/gtest.h>

#include "store_fixture.hpp"

using namespace pychron;
using namespace pychron::persistence;
using namespace pychron::persistence::testing;
namespace pd = pychron::persistence::detail;

namespace {

ImportSourceSpec spec_for(Uuid uuid) {
  ImportSourceSpec spec;
  spec.uuid = uuid;
  spec.kind = ImportSourceKind::ProjectRepo;
  spec.url_or_path = "/data/repos/Henry_Hill";
  spec.branch = "main";
  spec.importer_version = "pychron-dvc-import/1";
  spec.lab_time_zone = "America/Denver";
  return spec;
}

const UtcTime kGitTime = *UtcTime::parse("2016-03-04T05:06:07Z");

Intercepts intercepts(double value) {
  InterceptRow row;
  row.isotope = "Ar40";
  row.detector = "H1";
  row.value = value;
  row.error = 0.5;
  row.fit = "parabolic";
  return {row};
}

// A file-backed database with one ingested analysis; `db_` is a second,
// white-box connection to it. `source_` is registered by the first batch().
class ImportStoreTest : public ::testing::TestWithParam<std::string> {
 protected:
  void SetUp() override {
    database_ = std::make_unique<TestDatabase>(GetParam(), true);
    store_ = open_or_die(database_->url());
    ASSERT_TRUE(store_);
    lab_ = seed_lab(*store_);
    source_ = spec_for(Uuid::v7());
    analysis_ = ingest_analysis(1);
    auto db = pd::Db::open(StoreConfig{database_->url(), false});
    ASSERT_TRUE(db) << to_string(db.error());
    db_ = std::move(*db);
  }

  Uuid ingest_analysis(int aliquot) {
    auto item = analysis_item(lab_, aliquot, series(1), series(0));
    const Uuid analysis = std::get<AnalysisIngest>(item.body).analysis;
    auto ack = store_->ingest(item);
    EXPECT_TRUE(ack) << (ack ? "" : to_string(ack.error()));
    return analysis;
  }

  ImportedChangeset changeset(Uuid uuid, std::vector<ImportedRevision> revisions) const {
    ImportedChangeset cs;
    cs.uuid = uuid;
    cs.kind = ChangesetKind::Import;
    cs.author_user = lab_.reducer;
    cs.created = kGitTime;
    cs.message = "refit intercepts";
    cs.revisions = std::move(revisions);
    return cs;
  }

  ImportedRevision intercepts_revision(Uuid uuid, double value = 101.0) const {
    return ImportedRevision{uuid, analysis_, Kind::Intercepts, intercepts(value)};
  }

  std::unique_ptr<IImportUnitOfWork> batch() {
    EXPECT_TRUE(store_->begin_import(source_));
    auto uow = store_->begin_import_batch(source_.uuid, lab_.reduction_client);
    EXPECT_TRUE(uow) << (uow ? "" : to_string(uow.error()));
    return uow ? std::move(*uow) : nullptr;
  }

  long long count(const char* table) {
    auto row = db_->select_one(QStringLiteral("SELECT count(*) AS n FROM %1").arg(QString::fromUtf8(table)));
    return row && *row ? (*row)->value("n").toLongLong() : -1;
  }

  // The head of (subject, kind). A failed read or a missing head fails the
  // test and gives a nil uuid, instead of dereferencing an empty Result.
  Uuid head_of(Uuid subject, Kind kind) {
    auto head = store_->head(subject, kind);
    if (!head || !*head) {
      ADD_FAILURE() << "no head of " << to_string(kind) << (head ? "" : ": " + to_string(head.error()));
      return Uuid{};
    }
    return **head;
  }

  // The stored conflict; likewise checked.
  ImportConflictRow conflict_row(Uuid conflict) {
    auto row = store_->import_conflict(conflict);
    if (!row || !*row) {
      ADD_FAILURE() << "no conflict " << conflict.str() << (row ? "" : ": " + to_string(row.error()));
      return ImportConflictRow{};
    }
    return **row;
  }

  // Declared first so it is destroyed last: the connections point at it.
  std::unique_ptr<TestDatabase> database_;
  std::unique_ptr<IStore> store_;
  std::unique_ptr<pd::Db> db_;
  Lab lab_;
  ImportSourceSpec source_;
  Uuid analysis_;
};

}  // namespace

TEST_P(ImportStoreTest, ImportedChangesetKeepsCallerIdsAndTime) {
  const Uuid previous = head_of(analysis_, Kind::Intercepts);
  const Uuid u = Uuid::v7(), r = Uuid::v7();
  auto uow = batch();
  ASSERT_TRUE(uow);
  ASSERT_TRUE(uow->add_changeset(changeset(u, {intercepts_revision(r)})));
  auto seq = uow->commit();
  ASSERT_TRUE(seq) << to_string(seq.error());

  EXPECT_EQ(*store_->head(analysis_, Kind::Intercepts), std::optional<Uuid>{r});
  auto history = store_->history(analysis_, Kind::Intercepts);
  ASSERT_TRUE(history) << to_string(history.error());
  ASSERT_EQ(history->size(), 2u);
  const RevisionInfo& last = history->back();
  EXPECT_EQ(last.uuid, r);
  EXPECT_EQ(last.parent, std::optional<Uuid>{previous});
  EXPECT_EQ(last.changeset.uuid, u);
  EXPECT_EQ(last.changeset.kind, ChangesetKind::Import);
  EXPECT_EQ(last.changeset.created, kGitTime);
  EXPECT_EQ(last.changeset.author_user, lab_.reducer);
  EXPECT_EQ(last.changeset.client, lab_.reduction_client);
  EXPECT_EQ(last.changeset.message, "refit intercepts");
  EXPECT_EQ(last.change_seq, *seq);
  EXPECT_EQ(*seq, *store_->latest_change_seq());

  auto payload = store_->load_payload(r);
  ASSERT_TRUE(payload && *payload);
  EXPECT_EQ(std::get<Intercepts>(**payload).at(0).value, std::optional<double>{101.0});

  auto stamped = db_->select_one("SELECT import_source_uuid FROM changeset WHERE uuid = ?", {pd::qv(u)});
  ASSERT_TRUE(stamped && *stamped);
  EXPECT_EQ(pd::to_uuid((*stamped)->value("import_source_uuid")), source_.uuid);
  auto move = db_->select_one("SELECT reason, from_revision_uuid FROM head_move WHERE to_revision_uuid = ?",
                              {pd::qv(r)});
  ASSERT_TRUE(move && *move);
  EXPECT_EQ(pd::to_std((*move)->value("reason")), "commit");
  EXPECT_EQ(pd::to_uuid((*move)->value("from_revision_uuid")), previous);
}

TEST_P(ImportStoreTest, RerunIsNoOp) {
  const Uuid u = Uuid::v7(), r = Uuid::v7();
  auto write = [&] {
    auto uow = batch();
    EXPECT_TRUE(uow->add_changeset(changeset(u, {intercepts_revision(r)})));
    return uow->commit();
  };
  auto first = write();
  ASSERT_TRUE(first) << to_string(first.error());
  const auto history = store_->history(analysis_, Kind::Intercepts)->size();
  const auto moves = count("head_move"), rows = count("intercept_value");

  auto second = write();
  ASSERT_TRUE(second) << to_string(second.error());
  EXPECT_EQ(*second, *first) << "nothing new: no change_log entry";
  EXPECT_EQ(*store_->latest_change_seq(), *first);
  EXPECT_EQ(store_->history(analysis_, Kind::Intercepts)->size(), history);
  EXPECT_EQ(count("head_move"), moves);
  EXPECT_EQ(count("intercept_value"), rows);
  EXPECT_EQ(*store_->head(analysis_, Kind::Intercepts), std::optional<Uuid>{r});
}

TEST_P(ImportStoreTest, RevisionsOfOneSubjectChainInOrder) {
  const Uuid previous = head_of(analysis_, Kind::Intercepts);
  const Uuid r1 = Uuid::v7(), r2 = Uuid::v7(), r3 = Uuid::v7();
  auto uow = batch();
  // The second changeset is older than the first: order given wins.
  auto later = changeset(Uuid::v7(), {intercepts_revision(r2, 2.0), intercepts_revision(r3, 3.0)});
  later.created = *UtcTime::parse("2015-01-01T00:00:00Z");
  ASSERT_TRUE(uow->add_changeset(changeset(Uuid::v7(), {intercepts_revision(r1, 1.0)})));
  ASSERT_TRUE(uow->add_changeset(std::move(later)));
  ASSERT_TRUE(uow->commit());

  EXPECT_EQ(*store_->head(analysis_, Kind::Intercepts), std::optional<Uuid>{r3});
  auto parent_of = [&](Uuid revision) {
    auto row = db_->select_one("SELECT parent_uuid FROM revision WHERE uuid = ?", {pd::qv(revision)});
    if (!row || !*row) {
      ADD_FAILURE() << "no revision " << revision.str();
      return std::optional<Uuid>{};
    }
    return pd::opt_uuid((**row).value("parent_uuid"));
  };
  EXPECT_EQ(parent_of(r1), std::optional<Uuid>{previous});
  EXPECT_EQ(parent_of(r2), std::optional<Uuid>{r1});
  EXPECT_EQ(parent_of(r3), std::optional<Uuid>{r2});
  auto history = store_->history(analysis_, Kind::Intercepts);
  ASSERT_EQ(history->size(), 4u);
  std::vector<Uuid> listed;
  for (const auto& revision : *history) listed.push_back(revision.uuid);
  EXPECT_EQ(listed, (std::vector<Uuid>{previous, r1, r2, r3})) << "chain order, whatever the git times";
}

TEST_P(ImportStoreTest, ExistingChangesetStillTakesItsMissingRevisions) {
  const Uuid u = Uuid::v7(), r1 = Uuid::v7(), r2 = Uuid::v7();
  auto first = batch();
  ASSERT_TRUE(first->add_changeset(changeset(u, {intercepts_revision(r1)})));
  ASSERT_TRUE(first->commit());
  const auto before = *store_->latest_change_seq();

  BaselineRow h1;
  h1.detector = "H1";
  h1.value = 0.02;
  auto second = batch();
  ASSERT_TRUE(second->add_changeset(
      changeset(u, {intercepts_revision(r1), ImportedRevision{r2, analysis_, Kind::Baselines, Baselines{h1}}})));
  auto seq = second->commit();
  ASSERT_TRUE(seq) << to_string(seq.error());
  EXPECT_EQ(*seq, before + 1);
  EXPECT_EQ(*store_->head(analysis_, Kind::Baselines), std::optional<Uuid>{r2});
  EXPECT_EQ(*store_->head(analysis_, Kind::Intercepts), std::optional<Uuid>{r1});
  EXPECT_EQ(store_->history(analysis_, Kind::Intercepts)->size(), 2u);
  auto n = db_->select_one("SELECT count(*) AS n FROM changeset WHERE uuid = ?", {pd::qv(u)});
  ASSERT_TRUE(n && *n);
  EXPECT_EQ((**n).value("n").toLongLong(), 1);
}

TEST_P(ImportStoreTest, ProvenanceAndConflictRoundTrip) {
  const Uuid previous = head_of(analysis_, Kind::Intercepts);
  const Uuid u = Uuid::v7(), r = Uuid::v7();
  const auto digest = sha256(std::string_view{"file text"});
  auto write = [&] {
    auto uow = batch();
    EXPECT_TRUE(uow->add_changeset(changeset(u, {intercepts_revision(r)})));
    ProvenanceRow rev{"revision", r, "ia/66573-01.intercepts.json", "c1", "b2", "jross", kGitTime,
                      R"({"synthetic_collection":true})"};
    ProvenanceRow cs{"changeset", u, "", "c1", "", "jross", kGitTime, std::nullopt};
    EXPECT_TRUE(uow->add_provenance(rev));
    EXPECT_TRUE(uow->add_provenance(cs));
    ImportConflictRow conflict;
    conflict.uuid = Uuid::v5(source_.uuid, "conflict a.json");
    conflict.path = "a.json";
    conflict.entity = analysis_;
    conflict.kind = ConflictKind::HandEdit;
    conflict.db_head_revision = previous;
    conflict.file_sha256 = digest;
    conflict.detail_json = R"({"field":"value"})";
    EXPECT_TRUE(uow->add_conflict(conflict));
    return uow->commit();
  };
  auto first = write();
  ASSERT_TRUE(first) << to_string(first.error());

  auto rows = store_->provenance_for(r);
  ASSERT_TRUE(rows) << to_string(rows.error());
  ASSERT_EQ(rows->size(), 1u);
  EXPECT_EQ((*rows)[0].entity_type, "revision");
  EXPECT_EQ((*rows)[0].path, "ia/66573-01.intercepts.json");
  EXPECT_EQ((*rows)[0].commit_sha, "c1");
  EXPECT_EQ((*rows)[0].git_blob_sha, "b2");
  EXPECT_EQ((*rows)[0].git_author, "jross");
  EXPECT_EQ((*rows)[0].git_utc, kGitTime);
  ASSERT_TRUE((*rows)[0].detail_json);
  EXPECT_NE((*rows)[0].detail_json->find("synthetic_collection"), std::string::npos);
  auto of_changeset = store_->provenance_for(u);
  ASSERT_EQ(of_changeset->size(), 1u);
  EXPECT_FALSE((*of_changeset)[0].detail_json);

  EXPECT_TRUE(*store_->has_provenance(source_.uuid, "c1", "ia/66573-01.intercepts.json"));
  EXPECT_TRUE(*store_->has_provenance_blob(source_.uuid, "ia/66573-01.intercepts.json", "b2"));
  EXPECT_TRUE(*store_->has_conflict(source_.uuid, "a.json", digest));
  EXPECT_EQ(*store_->imported_head_blob_sha(source_.uuid, analysis_, Kind::Intercepts),
            std::optional<std::string>{"b2"});

  ConflictFilter filter;
  filter.source = source_.uuid;
  auto conflicts = store_->import_conflicts(filter);
  ASSERT_TRUE(conflicts) << to_string(conflicts.error());
  ASSERT_EQ(conflicts->size(), 1u);
  const auto& c = (*conflicts)[0];
  EXPECT_EQ(c.uuid, Uuid::v5(source_.uuid, "conflict a.json"));
  EXPECT_EQ(c.path, "a.json");
  EXPECT_EQ(c.entity, std::optional<Uuid>{analysis_});
  EXPECT_EQ(c.kind, ConflictKind::HandEdit);
  EXPECT_EQ(c.db_head_revision, std::optional<Uuid>{previous});
  EXPECT_EQ(c.file_sha256, std::optional<Sha256Digest>{digest});
  EXPECT_NE(c.detail_json.find("field"), std::string::npos);
  EXPECT_EQ(c.resolution, "pending");

  // The same rows again: kept, not duplicated.
  ASSERT_TRUE(write());
  EXPECT_EQ(count("import_provenance"), 2);
  EXPECT_EQ(count("import_conflict"), 1);
}

TEST_P(ImportStoreTest, ConflictIsReadByUuidAndResolved) {
  const Uuid id = Uuid::v5(source_.uuid, "conflict b.json");
  const Uuid other = Uuid::v5(source_.uuid, "conflict c.json");
  auto missing = store_->import_conflict(id);
  ASSERT_TRUE(missing) << to_string(missing.error());
  EXPECT_FALSE(missing->has_value());

  auto uow = batch();
  for (const Uuid uuid : {id, other}) {
    ImportConflictRow row;
    row.uuid = uuid;
    row.path = uuid == id ? "b.json" : "c.json";
    row.entity = analysis_;
    row.kind = ConflictKind::UnknownAnalysis;
    row.detail_json = R"({"reason":"x"})";
    ASSERT_TRUE(uow->add_conflict(row));
  }
  ASSERT_TRUE(uow->commit());
  auto stored = store_->import_conflict(id);
  ASSERT_TRUE(stored && stored->has_value());
  EXPECT_EQ((*stored)->path, "b.json");
  EXPECT_EQ((*stored)->kind, ConflictKind::UnknownAnalysis);
  EXPECT_EQ((*stored)->entity, std::optional<Uuid>{analysis_});
  EXPECT_EQ((*stored)->resolution, "pending");

  // Resolving is staged with the batch; an absent conflict is not an error.
  const auto seq = *store_->latest_change_seq();
  auto resolve = batch();
  ASSERT_TRUE(resolve->resolve_conflict(id, "superseded"));
  ASSERT_TRUE(resolve->resolve_conflict(Uuid::v7(), "superseded"));
  EXPECT_EQ(conflict_row(id).resolution, "pending") << "staged, not written";
  ASSERT_TRUE(resolve->commit());
  EXPECT_EQ(*store_->latest_change_seq(), seq) << "a resolution alone is not a change";
  EXPECT_EQ(conflict_row(id).resolution, "superseded");
  EXPECT_EQ(conflict_row(other).resolution, "pending");

  ConflictFilter pending;
  pending.source = source_.uuid;
  pending.resolution = "pending";
  auto rows = store_->import_conflicts(pending);
  ASSERT_TRUE(rows);
  ASSERT_EQ(rows->size(), 1u);
  EXPECT_EQ(rows->front().uuid, other);

  // A conflict added and resolved in one batch ends resolved.
  const Uuid fresh = Uuid::v5(source_.uuid, "conflict d.json");
  auto both = batch();
  ImportConflictRow row;
  row.uuid = fresh;
  row.path = "d.json";
  row.kind = ConflictKind::UnknownAnalysis;
  ASSERT_TRUE(both->add_conflict(row));
  ASSERT_TRUE(both->resolve_conflict(fresh, "superseded"));
  ASSERT_TRUE(both->commit());
  EXPECT_EQ(conflict_row(fresh).resolution, "superseded");
}

// A conflict that leaves `pending` carries the time it did; resolving it again
// to what it already is changes nothing; pending again, the time is gone.
TEST_P(ImportStoreTest, ResolvedConflictCarriesTheTimeItWasResolved) {
  const Uuid id = Uuid::v5(source_.uuid, "conflict r.json");
  const auto stored = [&]() -> ImportConflictRow {
    auto row = store_->import_conflict(id);
    EXPECT_TRUE(row && row->has_value());
    return row && *row ? **row : ImportConflictRow{};
  };
  const auto resolve = [&](const char* resolution) {
    auto uow = batch();
    ASSERT_TRUE(uow->resolve_conflict(id, resolution));
    ASSERT_TRUE(uow->commit());
  };
  {
    auto uow = batch();
    ImportConflictRow row;
    row.uuid = id;
    row.path = "r.json";
    row.kind = ConflictKind::Unparseable;
    row.resolved = UtcTime::now();  // ignored on write
    ASSERT_TRUE(uow->add_conflict(row));
    ASSERT_TRUE(uow->commit());
  }
  EXPECT_EQ(stored().resolution, "pending");
  EXPECT_FALSE(stored().resolved.has_value());

  const UtcTime before = UtcTime::now();
  resolve("superseded");
  const auto first = stored();
  EXPECT_EQ(first.resolution, "superseded");
  ASSERT_TRUE(first.resolved.has_value());
  EXPECT_GE(first.resolved->micros, before.micros - 1000000);
  EXPECT_LE(first.resolved->micros, UtcTime::now().micros + 1000000);

  // Again, to the same resolution: the row is as it was.
  resolve("superseded");
  EXPECT_EQ(stored().resolved, first.resolved);

  resolve("pending");
  EXPECT_EQ(stored().resolution, "pending");
  EXPECT_FALSE(stored().resolved.has_value());

  // Restated, a resolved conflict is pending and has no time either.
  resolve("ignored");
  ASSERT_TRUE(stored().resolved.has_value());
  auto uow = batch();
  ImportConflictRow restated;
  restated.uuid = id;
  restated.path = "r.json";
  restated.kind = ConflictKind::IdentityClash;
  ASSERT_TRUE(uow->restate_conflict(restated));
  ASSERT_TRUE(uow->commit());
  EXPECT_EQ(stored().resolution, "pending");
  EXPECT_FALSE(stored().resolved.has_value());
}

TEST_P(ImportStoreTest, ConflictCanBeRestated) {
  const Uuid id = Uuid::v5(source_.uuid, "conflict e.json");
  const Uuid resolved = Uuid::v5(source_.uuid, "conflict f.json");
  {
    auto uow = batch();
    for (const Uuid uuid : {id, resolved}) {
      ImportConflictRow row;
      row.uuid = uuid;
      row.path = "e.json";
      row.kind = ConflictKind::UnknownAnalysis;
      row.detail_json = R"({"reason":"x"})";
      ASSERT_TRUE(uow->add_conflict(row));
    }
    ASSERT_TRUE(uow->resolve_conflict(resolved, "ignored"));
    ASSERT_TRUE(uow->commit());
  }

  const auto seq = *store_->latest_change_seq();
  const auto digest = sha256(std::string_view{"e"});
  auto uow = batch();
  for (const Uuid uuid : {id, resolved, Uuid::v7()}) {  // the last is not stored: left alone
    ImportConflictRow row;
    row.uuid = uuid;
    row.path = "e.json";
    row.entity = analysis_;
    row.kind = ConflictKind::IdentityClash;
    row.file_sha256 = digest;
    row.detail_json = R"({"reason":"y"})";
    row.resolution = "pending";
    ASSERT_TRUE(uow->restate_conflict(row));
  }
  EXPECT_EQ(conflict_row(id).kind, ConflictKind::UnknownAnalysis) << "staged, not written";
  ASSERT_TRUE(uow->commit());
  EXPECT_EQ(*store_->latest_change_seq(), seq) << "a restated conflict is not a change";
  EXPECT_EQ(count("import_conflict"), 2);

  auto stored = store_->import_conflict(id);
  ASSERT_TRUE(stored && stored->has_value());
  EXPECT_EQ((*stored)->kind, ConflictKind::IdentityClash);
  EXPECT_EQ((*stored)->entity, std::optional<Uuid>{analysis_});
  EXPECT_EQ((*stored)->file_sha256, std::optional<Sha256Digest>{digest});
  EXPECT_NE((*stored)->detail_json.find("\"y\""), std::string::npos);
  EXPECT_EQ((*stored)->resolution, "pending");
  // It says something else now: what it was resolved as does not carry over.
  auto reopened = store_->import_conflict(resolved);
  ASSERT_TRUE(reopened && reopened->has_value());
  EXPECT_EQ((*reopened)->kind, ConflictKind::IdentityClash);
  EXPECT_EQ((*reopened)->resolution, "pending");

  // A conflict added and restated in one batch ends restated.
  const Uuid fresh = Uuid::v5(source_.uuid, "conflict g.json");
  auto both = batch();
  ImportConflictRow row;
  row.uuid = fresh;
  row.path = "g.json";
  row.kind = ConflictKind::UnknownAnalysis;
  ASSERT_TRUE(both->add_conflict(row));
  row.kind = ConflictKind::IdentityClash;
  ASSERT_TRUE(both->restate_conflict(row));
  ASSERT_TRUE(both->commit());
  EXPECT_EQ(conflict_row(fresh).kind, ConflictKind::IdentityClash);
}

TEST_P(ImportStoreTest, ProgressIsStoredWithTheBatch) {
  auto uow = batch();
  ASSERT_TRUE(uow->set_progress({"abc", 3, 10, "head", "running"}));
  auto before = store_->begin_import(source_);
  EXPECT_FALSE(before->progress_token) << "staged, not written";
  const auto seq = *store_->latest_change_seq();
  ASSERT_TRUE(uow->commit());
  EXPECT_EQ(*store_->latest_change_seq(), seq) << "progress alone is not a change";

  auto info = store_->begin_import(source_);
  ASSERT_TRUE(info) << to_string(info.error());
  EXPECT_EQ(info->progress_token, std::optional<std::string>{"abc"});
  EXPECT_EQ(info->done, 3);
  EXPECT_EQ(info->total, 10);
  EXPECT_EQ(info->head_sha, std::optional<std::string>{"head"});
  EXPECT_EQ(info->status, "running");
  EXPECT_FALSE(info->finished);

  auto last = batch();
  ASSERT_TRUE(last->set_progress({"def", 10, 10, std::nullopt, "finished"}));
  ASSERT_TRUE(last->commit());
  info = store_->begin_import(source_);
  EXPECT_EQ(info->progress_token, std::optional<std::string>{"def"});
  EXPECT_EQ(info->head_sha, std::optional<std::string>{"head"}) << "nullopt keeps the stored head";
  EXPECT_EQ(info->status, "finished");
  EXPECT_TRUE(info->finished);
}

TEST_P(ImportStoreTest, ProgressOfAnUnknownSourceFails) {
  auto uow = store_->begin_import_batch(Uuid::v7(), lab_.reduction_client);
  ASSERT_TRUE(uow);
  ASSERT_TRUE((*uow)->set_progress({"abc", 1, 1, std::nullopt, "running"}));
  EXPECT_FALSE((*uow)->commit());
}

TEST_P(ImportStoreTest, FailedCommitLeavesNothing) {
  const Uuid good = Uuid::v7(), r = Uuid::v7();
  const auto seq = *store_->latest_change_seq();
  const auto changesets = count("changeset");
  auto uow = batch();
  ASSERT_TRUE(uow->add_changeset(changeset(good, {intercepts_revision(r)})));
  ASSERT_TRUE(uow->add_changeset(
      changeset(Uuid::v7(), {ImportedRevision{Uuid::v7(), Uuid::v7(), Kind::Intercepts, intercepts(1.0)}})));
  ASSERT_TRUE(uow->add_provenance({"revision", r, "a.json", "c1", "b1", "jross", kGitTime, std::nullopt}));
  ASSERT_TRUE(uow->set_progress({"abc", 3, 10, "head", "running"}));
  auto result = uow->commit();
  ASSERT_FALSE(result);

  auto info = store_->begin_import(source_);
  EXPECT_FALSE(info->progress_token);
  EXPECT_EQ(info->done, 0);
  EXPECT_EQ(count("import_provenance"), 0);
  EXPECT_EQ(count("changeset"), changesets);
  EXPECT_EQ(*store_->latest_change_seq(), seq);
  EXPECT_NE(*store_->head(analysis_, Kind::Intercepts), std::optional<Uuid>{r});
  EXPECT_FALSE(uow->commit()) << "a unit of work commits once";
}

TEST_P(ImportStoreTest, OneChangeLogEntryPerBatch) {
  const Uuid other = ingest_analysis(2);
  const auto before = *store_->latest_change_seq();
  auto uow = batch();
  ASSERT_TRUE(uow->add_changeset(changeset(Uuid::v7(), {intercepts_revision(Uuid::v7(), 1.0)})));
  ASSERT_TRUE(uow->add_changeset(changeset(Uuid::v7(), {intercepts_revision(Uuid::v7(), 2.0)})));
  ASSERT_TRUE(uow->add_changeset(
      changeset(Uuid::v7(), {ImportedRevision{Uuid::v7(), other, Kind::Intercepts, intercepts(3.0)}})));
  auto seq = uow->commit();
  ASSERT_TRUE(seq) << to_string(seq.error());
  EXPECT_EQ(*seq, before + 1);
  EXPECT_EQ(*store_->latest_change_seq(), before + 1);

  auto page = store_->changes_since(before, 10);
  ASSERT_TRUE(page) << to_string(page.error());
  ASSERT_EQ(page->entries.size(), 1u);
  EXPECT_EQ(page->entries[0].kind, "changeset");
  int analyses = 0;
  for (const auto& e : page->entries[0].entities) analyses += e.entity_type == "analysis";
  EXPECT_EQ(analyses, 2);
  const auto history = store_->history(analysis_, Kind::Intercepts);  // named: a range-for does not keep it alive
  ASSERT_TRUE(history);
  for (const auto& revision : *history) {
    if (revision.changeset.kind == ChangesetKind::Import) {
      EXPECT_EQ(revision.change_seq, *seq);
    }
  }
}

TEST_P(ImportStoreTest, ImportedIdentityRevisionRenumbersTheAnalysis) {
  IdentityValue identity{lab_.identifier2, 4, -1, "provisional_renumber"};
  auto uow = batch();
  ASSERT_TRUE(uow->add_changeset(
      changeset(Uuid::v7(), {ImportedRevision{Uuid::v7(), analysis_, Kind::Identity, identity}})));
  auto seq = uow->commit();
  ASSERT_TRUE(seq) << to_string(seq.error());
  auto row = db_->select_one("SELECT runid_text, aliquot FROM analysis WHERE uuid = ?", {pd::qv(analysis_)});
  ASSERT_TRUE(row && *row);
  EXPECT_EQ(pd::to_std((*row)->value("runid_text")), make_runid("66574", 4, -1));
  EXPECT_EQ((*row)->value("aliquot").toInt(), 4);
}

TEST_P(ImportStoreTest, LookupsByNaturalKeyCreateNothing) {
  const auto seq = *store_->latest_change_seq();
  // seed_lab: 66573 sits at NM-300/A/1, 66574 nowhere; SetUp ingested 66573-01.
  auto identifier = store_->find_identifier("66573");
  ASSERT_TRUE(identifier) << to_string(identifier.error());
  EXPECT_EQ(*identifier, std::optional<Uuid>{lab_.identifier});
  EXPECT_FALSE(store_->find_identifier("99999")->has_value());

  auto analysis = store_->find_analysis("66573", 1, -1);
  ASSERT_TRUE(analysis) << to_string(analysis.error());
  EXPECT_EQ(*analysis, std::optional<Uuid>{analysis_});
  EXPECT_FALSE(store_->find_analysis("66573", 1, 0)->has_value());   // another step
  EXPECT_FALSE(store_->find_analysis("66573", 2, -1)->has_value());  // another aliquot
  EXPECT_FALSE(store_->find_analysis("66574", 1, -1)->has_value());  // another identifier
  EXPECT_FALSE(store_->find_analysis("99999", 1, -1)->has_value());

  auto at = store_->identifier_at("NM-300", "A", 1);
  ASSERT_TRUE(at) << to_string(at.error());
  EXPECT_EQ(*at, std::optional<std::string>{"66573"});
  EXPECT_FALSE(store_->identifier_at("NM-300", "A", 2)->has_value());
  EXPECT_FALSE(store_->identifier_at("NM-300", "B", 1)->has_value());
  EXPECT_FALSE(store_->identifier_at("NM-999", "A", 1)->has_value());
  EXPECT_EQ(*store_->latest_change_seq(), seq);
  EXPECT_FALSE(store_->find_identifier("99999")->has_value());  // asking did not create it

  // The run identity is the current one: after a renumber the old one is free.
  IdentityValue identity{lab_.identifier2, 4, -1, "provisional_renumber"};
  auto uow = batch();
  ASSERT_TRUE(uow->add_changeset(
      changeset(Uuid::v7(), {ImportedRevision{Uuid::v7(), analysis_, Kind::Identity, identity}})));
  ASSERT_TRUE(uow->commit());
  EXPECT_FALSE(store_->find_analysis("66573", 1, -1)->has_value());
  EXPECT_EQ(*store_->find_analysis("66574", 4, -1), std::optional<Uuid>{analysis_});
}

TEST_P(ImportStoreTest, ProvenanceDetailCanBeReplaced) {
  const Uuid u = Uuid::v7(), r = Uuid::v7();
  {
    auto uow = batch();
    ASSERT_TRUE(uow->add_changeset(changeset(u, {intercepts_revision(r)})));
    ASSERT_TRUE(uow->add_provenance({"changeset", u, "", "c1", "", "jross", kGitTime, std::nullopt}));
    ASSERT_TRUE(uow->add_provenance({"revision", r, "a.json", "c1", "b1", "jross", kGitTime, R"({"extra":1})"}));
    // In the batch that writes the row, and for a row that is not there.
    ASSERT_TRUE(uow->set_provenance_detail("changeset", u, R"({"rewrites":[{"path":"x.json"}]})"));
    ASSERT_TRUE(uow->set_provenance_detail("changeset", Uuid::v7(), R"({"lost":true})"));
    ASSERT_TRUE(uow->commit());
  }
  EXPECT_TRUE(*store_->has_revision(r));
  EXPECT_FALSE(*store_->has_revision(u));
  auto rows = store_->provenance_for(u);
  ASSERT_TRUE(rows) << to_string(rows.error());
  ASSERT_EQ(rows->size(), 1u);
  EXPECT_EQ(rows->front().source, source_.uuid);
  EXPECT_NE(rows->front().detail_json.value_or("").find("x.json"), std::string::npos);

  // In a later batch: replaced, and only that row.
  const auto seq = *store_->latest_change_seq();
  {
    auto uow = batch();
    ASSERT_TRUE(uow->set_provenance_detail("changeset", u, R"({"rewrites":[{"path":"x.json"},{"path":"y.json"}]})"));
    ASSERT_TRUE(uow->commit());
  }
  EXPECT_EQ(*store_->latest_change_seq(), seq);  // not a change
  const std::string detail = store_->provenance_for(u)->front().detail_json.value_or("");
  EXPECT_NE(detail.find("x.json"), std::string::npos);
  EXPECT_NE(detail.find("y.json"), std::string::npos);
  EXPECT_NE(store_->provenance_for(r)->front().detail_json.value_or("").find("extra"), std::string::npos);
}

TEST_P(ImportStoreTest, RejectsWhatAnImportCannotWrite) {
  auto uow = batch();
  auto collection = changeset(Uuid::v7(), {});
  collection.kind = ChangesetKind::Collection;
  auto rejected = uow->add_changeset(collection);
  ASSERT_FALSE(rejected);
  EXPECT_EQ(rejected.error().kind, ErrorKind::Protocol);
  auto mismatch = uow->add_changeset(
      changeset(Uuid::v7(), {ImportedRevision{Uuid::v7(), analysis_, Kind::Baselines, intercepts(1.0)}}));
  ASSERT_FALSE(mismatch);
  EXPECT_EQ(mismatch.error().kind, ErrorKind::Protocol);
  auto reference = changeset(Uuid::v7(), {});
  reference.kind = ChangesetKind::Reference;
  EXPECT_TRUE(uow->add_changeset(reference));
}

TEST_P(ImportStoreTest, IngestStampsImportSource) {
  auto item = analysis_item(lab_, 7, series(1), series(0));
  auto& a = std::get<AnalysisIngest>(item.body);
  a.import_source = source_.uuid;
  a.author_user = lab_.reducer;
  ASSERT_TRUE(store_->begin_import(source_));
  ASSERT_TRUE(store_->ingest(item));
  auto row = db_->select_one("SELECT import_source_uuid, author_user_uuid FROM changeset WHERE uuid = ?",
                             {pd::qv(a.changeset)});
  ASSERT_TRUE(row && *row);
  EXPECT_EQ(pd::to_uuid((*row)->value("import_source_uuid")), source_.uuid);
  EXPECT_EQ(pd::to_uuid((*row)->value("author_user_uuid")), lab_.reducer);
  auto analyst = db_->select_one("SELECT analyst_user_uuid FROM analysis WHERE uuid = ?", {pd::qv(a.analysis)});
  ASSERT_TRUE(analyst && *analyst);
  EXPECT_EQ(pd::to_uuid((**analyst).value("analyst_user_uuid")), lab_.analyst);

  // An ordinary ingest stamps neither.
  auto plain = db_->select_one(
      "SELECT c.import_source_uuid, c.author_user_uuid FROM changeset c JOIN analysis a "
      "ON a.ingest_changeset_uuid = c.uuid WHERE a.uuid = ?",
      {pd::qv(analysis_)});
  ASSERT_TRUE(plain && *plain);
  EXPECT_TRUE((*plain)->value("import_source_uuid").isNull());
  EXPECT_EQ(pd::to_uuid((*plain)->value("author_user_uuid")), lab_.analyst);
}

TEST_P(ImportStoreTest, AppendOnlyTriggersStillHold) {
  const Uuid u = Uuid::v7(), r = Uuid::v7();
  auto uow = batch();
  ASSERT_TRUE(uow->add_changeset(changeset(u, {intercepts_revision(r)})));
  ASSERT_TRUE(uow->commit());
  EXPECT_FALSE(db_->affecting("UPDATE revision SET created_utc = created_utc WHERE uuid = ?", {pd::qv(r)}));
  EXPECT_FALSE(db_->affecting("UPDATE changeset SET message = 'x' WHERE uuid = ?", {pd::qv(u)}));
  EXPECT_FALSE(db_->affecting("DELETE FROM intercept_value WHERE revision_uuid = ?", {pd::qv(r)}));
}

TEST_P(ImportStoreTest, BeginImportInsertsThenReturnsStored) {
  const auto spec = spec_for(Uuid::v7());
  auto first = store_->begin_import(spec);
  ASSERT_TRUE(first) << to_string(first.error());
  EXPECT_EQ(first->status, "registered");
  EXPECT_EQ(first->done, 0);
  EXPECT_EQ(first->total, 0);
  EXPECT_EQ(first->spec.uuid, spec.uuid);
  EXPECT_EQ(first->spec.kind, ImportSourceKind::ProjectRepo);
  EXPECT_EQ(first->spec.url_or_path, spec.url_or_path);
  EXPECT_EQ(first->spec.branch, spec.branch);
  EXPECT_EQ(first->spec.importer_version, spec.importer_version);
  EXPECT_EQ(first->spec.lab_time_zone, spec.lab_time_zone);
  EXPECT_FALSE(first->head_sha);
  EXPECT_FALSE(first->progress_token);
  EXPECT_FALSE(first->finished);

  auto second = store_->begin_import(spec);
  ASSERT_TRUE(second) << to_string(second.error());
  EXPECT_EQ(second->spec.uuid, first->spec.uuid);
  EXPECT_EQ(second->started, first->started);
  EXPECT_EQ(second->status, "registered");

  auto all = store_->import_sources();
  ASSERT_TRUE(all) << to_string(all.error());
  EXPECT_EQ(all->size(), 1u);
}

TEST_P(ImportStoreTest, NullBranchRoundTripsAndKindsAreStored) {
  auto spec = spec_for(Uuid::v7());
  spec.branch = std::nullopt;
  spec.kind = ImportSourceKind::LegacyDb;
  auto info = store_->begin_import(spec);
  ASSERT_TRUE(info) << to_string(info.error());
  EXPECT_FALSE(info->spec.branch);
  EXPECT_EQ(info->spec.kind, ImportSourceKind::LegacyDb);
  EXPECT_EQ(parse_import_source_kind(to_string(ImportSourceKind::MetaRepo)), ImportSourceKind::MetaRepo);
  EXPECT_FALSE(parse_conflict_kind("nope"));
  for (auto k : {ConflictKind::HandEdit, ConflictKind::UnknownAnalysis, ConflictKind::ValueMismatch,
                 ConflictKind::Unparseable, ConflictKind::IdentityClash, ConflictKind::ProvisionalRenumber})
    EXPECT_EQ(parse_conflict_kind(to_string(k)), k);
}

TEST_P(ImportStoreTest, SchemaStatusListsSecondMigration) {
  auto status = store_->schema_status();
  ASSERT_TRUE(status) << to_string(status.error());
  ASSERT_GE(status->size(), 2u);
  EXPECT_EQ((*status)[1].description, "import_detail");
}

TEST_P(ImportStoreTest, EmptyReads) {
  auto conflicts = store_->import_conflicts({});
  ASSERT_TRUE(conflicts) << to_string(conflicts.error());
  EXPECT_TRUE(conflicts->empty());
  auto prov = store_->provenance_for(Uuid::v7());
  ASSERT_TRUE(prov) << to_string(prov.error());
  EXPECT_TRUE(prov->empty());
  const Uuid source = Uuid::v7();
  auto has = store_->has_provenance(source, "abc", "a/b.json");
  ASSERT_TRUE(has);
  EXPECT_FALSE(*has);
  auto blob = store_->has_provenance_blob(source, "a/b.json", "def");
  ASSERT_TRUE(blob);
  EXPECT_FALSE(*blob);
  auto conflict = store_->has_conflict(source, "a/b.json", sha256(std::string_view{"x"}));
  ASSERT_TRUE(conflict);
  EXPECT_FALSE(*conflict);
  auto head = store_->imported_head_blob_sha(source, Uuid::v7(), Kind::Intercepts);
  ASSERT_TRUE(head) << to_string(head.error());
  EXPECT_FALSE(*head);
}

// Rows inserted by hand, through a second connection to the same database, to
// prove the reads apart from the import unit of work.
TEST_P(ImportStoreTest, ReadsSeeProvenanceAndConflictRows) {
  TestDatabase shared(GetParam(), true);
  auto store = open_or_die(shared.url());
  ASSERT_TRUE(store);
  const Lab lab = seed_lab(*store);
  const auto spec = spec_for(Uuid::v7());
  ASSERT_TRUE(store->begin_import(spec));
  const auto other = spec_for(Uuid::v7());
  ASSERT_TRUE(store->begin_import(other));

  auto item = analysis_item(lab, 1, series(1), series(0));
  const Uuid analysis = std::get<AnalysisIngest>(item.body).analysis;
  ASSERT_TRUE(store->ingest(item));
  auto head = store->head(analysis, Kind::Intercepts);
  ASSERT_TRUE(head && *head);

  auto db = std::move(*pd::Db::open(StoreConfig{shared.url(), false}));
  const bool pg = db->dialect() == Dialect::PostgreSql;
  const QString json_param = pg ? QStringLiteral("CAST(? AS jsonb)") : QStringLiteral("?");

  auto provenance = [&](const char* type, Uuid entity, const char* path, const char* commit, const char* blob) {
    pd::Row r;
    r["entity_type"] = pd::qv(type);
    r["entity_uuid"] = pd::qv(entity);
    r["import_source_uuid"] = pd::qv(spec.uuid);
    r["path"] = pd::qv(path);
    r["commit_sha"] = pd::qv(commit);
    r["git_blob_sha"] = pd::qv(blob);
    r["git_author"] = pd::qv("jross");
    r["git_utc"] = pd::qv(*UtcTime::parse("2019-03-04T05:06:07.000000Z"));
    ASSERT_TRUE(db->insert("import_provenance", r));
  };
  provenance("analysis", analysis, "ia/66573-01.json", "c1", "b1");
  provenance("revision", **head, "ia/66573-01.intercepts.json", "c1", "b2");
  ASSERT_TRUE(db->affecting("UPDATE import_provenance SET detail = " + json_param + " WHERE entity_type = 'analysis'",
                            {pd::qv(R"({"derived_uuid":true})")}));

  auto rows = store->provenance_for(analysis);
  ASSERT_TRUE(rows) << to_string(rows.error());
  ASSERT_EQ(rows->size(), 1u);
  EXPECT_EQ((*rows)[0].entity_type, "analysis");
  EXPECT_EQ((*rows)[0].path, "ia/66573-01.json");
  EXPECT_EQ((*rows)[0].commit_sha, "c1");
  EXPECT_EQ((*rows)[0].git_blob_sha, "b1");
  EXPECT_EQ((*rows)[0].git_author, "jross");
  EXPECT_EQ((*rows)[0].git_utc, *UtcTime::parse("2019-03-04T05:06:07.000000Z"));
  ASSERT_TRUE((*rows)[0].detail_json);
  EXPECT_NE((*rows)[0].detail_json->find("derived_uuid"), std::string::npos);

  EXPECT_TRUE(*store->has_provenance(spec.uuid, "c1", "ia/66573-01.json"));
  EXPECT_FALSE(*store->has_provenance(spec.uuid, "c2", "ia/66573-01.json"));
  EXPECT_FALSE(*store->has_provenance(other.uuid, "c1", "ia/66573-01.json"));
  EXPECT_TRUE(*store->has_provenance_blob(spec.uuid, "ia/66573-01.json", "b1"));
  EXPECT_FALSE(*store->has_provenance_blob(spec.uuid, "ia/66573-01.json", "bX"));

  auto blob = store->imported_head_blob_sha(spec.uuid, analysis, Kind::Intercepts);
  ASSERT_TRUE(blob) << to_string(blob.error());
  EXPECT_EQ(*blob, std::optional<std::string>{"b2"});
  EXPECT_FALSE(*store->imported_head_blob_sha(other.uuid, analysis, Kind::Intercepts));
  EXPECT_FALSE(*store->imported_head_blob_sha(spec.uuid, analysis, Kind::Baselines));

  const auto digest = sha256(std::string_view{"file text"});
  auto conflict = [&](Uuid source, const char* kind, const char* path, const char* resolution) {
    pd::Row r;
    r["uuid"] = pd::qv(Uuid::v7());
    r["import_source_uuid"] = pd::qv(source);
    r["path"] = pd::qv(path);
    r["entity_uuid"] = pd::qv(analysis);
    r["conflict_kind"] = pd::qv(kind);
    r["db_head_revision_uuid"] = pd::qv(**head);
    r["file_sha256"] = pd::qv(digest);
    r["resolution"] = pd::qv(resolution);
    ASSERT_TRUE(db->insert("import_conflict", r));
  };
  conflict(spec.uuid, "hand_edit", "a.json", "pending");
  conflict(spec.uuid, "unparseable", "b.json", "rejected");
  conflict(other.uuid, "hand_edit", "c.json", "pending");

  auto all = store->import_conflicts({});
  ASSERT_TRUE(all) << to_string(all.error());
  ASSERT_EQ(all->size(), 3u);
  EXPECT_EQ((*all)[0].path, "a.json");
  EXPECT_EQ((*all)[0].kind, ConflictKind::HandEdit);
  EXPECT_EQ((*all)[0].entity, std::optional<Uuid>{analysis});
  EXPECT_EQ((*all)[0].db_head_revision, std::optional<Uuid>{**head});
  EXPECT_EQ((*all)[0].file_sha256, std::optional<Sha256Digest>{digest});
  EXPECT_EQ((*all)[0].detail_json, "{}");
  EXPECT_EQ((*all)[0].resolution, "pending");

  ConflictFilter f;
  f.source = spec.uuid;
  EXPECT_EQ(store->import_conflicts(f)->size(), 2u);
  f.kind = ConflictKind::Unparseable;
  EXPECT_EQ(store->import_conflicts(f)->size(), 1u);
  f = {};
  f.resolution = "pending";
  EXPECT_EQ(store->import_conflicts(f)->size(), 2u);

  EXPECT_TRUE(*store->has_conflict(spec.uuid, "a.json", digest));
  EXPECT_FALSE(*store->has_conflict(spec.uuid, "c.json", digest));
  EXPECT_FALSE(*store->has_conflict(spec.uuid, "a.json", sha256(std::string_view{"other"})));
}

INSTANTIATE_TEST_SUITE_P(Engines, ImportStoreTest, ::testing::ValuesIn(engines()),
                         [](const auto& p) { return p.param; });
