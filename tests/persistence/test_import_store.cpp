// Import source, provenance and conflict reads (legacy ingestion spec; DVC
// schema spec, section 13).

#include <gtest/gtest.h>

#include "store_fixture.hpp"

using namespace pychron;
using namespace pychron::persistence;
using namespace pychron::persistence::testing;
namespace pd = pychron::persistence::detail;

namespace {

class ImportStoreTest : public StoreTest {};

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

}  // namespace

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
  EXPECT_EQ(status->size(), 2u);
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

// Rows the import unit of work will write (a later task) are inserted by hand
// to prove the reads, through a second connection to the same database.
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
