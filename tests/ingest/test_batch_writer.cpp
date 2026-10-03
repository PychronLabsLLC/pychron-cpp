// BatchWriter against a real store: scripted batches in, rows out.

#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "fake_adapter.hpp"
#include "forwarding_store.hpp"
#include "pychron/ingest/ids.hpp"
#include "pychron/ingest/writer.hpp"
#include "store_fixture.hpp"

using namespace pychron;
using namespace pychron::ingest;
using namespace pychron::ingest::testing;
namespace P = pychron::persistence;
namespace pd = pychron::persistence::detail;
using P::Kind;
using P::Uuid;
using P::UtcTime;
using P::testing::open_or_die;
using P::testing::TestDatabase;

namespace {

// The normalized form of description().url.
const std::string kUrl = "https://github.com/NMGRLData/Henry_Hill";
const char* const kAlice = "alice@example.org";

SourceDescription description() {
  return {P::ImportSourceKind::ProjectRepo, "https://GitHub.com/NMGRLData/Henry_Hill.git/", "main", "head-sha"};
}

GitWho who(const std::string& email, const char* iso) { return {"A. Author", email, *UtcTime::parse(iso)}; }

std::string record_path(int aliquot) { return "665/73-0" + std::to_string(aliquot) + ".json"; }
std::string kind_path(const char* dir, int aliquot) {
  return std::string("665/") + dir + "/73-0" + std::to_string(aliquot) + ".json";
}

// PI -> project -> sample, irradiation -> level -> position holding
// identifier 66573, the spectrometer and the repository.
std::vector<CatalogItem> lab_catalog() {
  SampleItem sample;
  sample.fields.name = "HH-1";
  sample.fields.lat = 34.07;
  sample.project = "Henry Hill";
  sample.material = "sanidine";
  sample.pi_last_name = "Ross";
  sample.pi_first_initial = "J";
  PositionItem position;
  position.irradiation = "NM-300";
  position.level = "A";
  position.position = 1;
  position.identifier = "66573";
  position.sample = "HH-1";
  position.project = "Henry Hill";
  position.material = "sanidine";
  position.pi_last_name = "Ross";
  position.pi_first_initial = "J";
  return {PiItem{"Ross", "J", "NMT", std::nullopt},
          ProjectItem{"Henry Hill", "Ross", "J"},
          MaterialItem{"sanidine", ""},
          sample,
          IrradiationItem{"NM-300"},
          LevelItem{"NM-300", "A", "24-hole", 0.5, std::nullopt},
          position,
          MassSpecItem{{"jan", "argus", "j", std::nullopt}},
          RepositoryItem{"Henry_Hill"}};
}

// Adds one analysis of identifier 66573, with its two signal blobs, to `batch`.
void add_analysis(ImportBatch& batch, Uuid uuid, int aliquot, const std::string& commit, const GitWho& author) {
  const auto signal = P::testing::series(static_cast<float>(aliquot));
  const auto baseline = P::testing::series(0);
  AnalysisItem item;
  item.ingest = std::get<P::AnalysisIngest>(P::testing::analysis_item(P::testing::Lab{}, aliquot, signal, baseline).body);
  item.ingest.analysis = uuid;
  item.ingest.changeset = Uuid{};
  item.ingest.roots.signals = item.ingest.roots.intercepts = item.ingest.roots.baselines = Uuid{};
  item.ingest.roots.blanks = item.ingest.roots.icfactors = item.ingest.roots.tags = Uuid{};
  const std::string n = std::to_string(aliquot);
  item.keys.record = {commit, record_path(aliquot), "rec-" + n};
  item.keys.signals = {commit, kind_path(".data", aliquot), "dat-" + n};
  item.keys.intercepts = {commit, kind_path("intercepts", aliquot), "int-" + n};
  item.keys.baselines = {commit, kind_path("baselines", aliquot), "bas-" + n};
  item.keys.blanks = {commit, kind_path("blanks", aliquot), "bla-" + n};
  item.keys.icfactors = {commit, kind_path("icfactors", aliquot), "icf-" + n};
  // No tags file: keys.tags stays empty.
  item.who = author;
  item.repositories = {"Henry_Hill"};
  batch.blobs.push_back({item.keys.signals, {std::string(P::kCodecTv), signal, 4}});
  batch.blobs.push_back({item.keys.signals, {std::string(P::kCodecTv), baseline, 4}});
  batch.analyses.push_back(std::move(item));
}

P::Intercepts intercepts(double value) {
  P::InterceptRow row;
  row.isotope = "Ar40";
  row.detector = "H1";
  row.value = value;
  row.error = 0.5;
  row.fit = "parabolic";
  return {row};
}

// A later commit that refits the intercepts of `analysis`.
ChangesetItem refit(const std::string& commit, Uuid analysis, int aliquot, double value, const GitWho& author) {
  ChangesetItem c;
  c.commit = commit;
  c.who = author;
  c.message = "refit " + commit;
  c.revisions.push_back(
      {{commit, kind_path("intercepts", aliquot), "int-" + commit}, analysis, Kind::Intercepts, intercepts(value)});
  return c;
}

const Uuid kA = *Uuid::parse("11111111-1111-4111-8111-111111111111");
const Uuid kB = *Uuid::parse("22222222-2222-4222-8222-222222222222");

// One batch: the catalog, analysis A collected at c1, its intercepts refit at c2.
ImportBatch single_batch() {
  ImportBatch b;
  b.catalog = lab_catalog();
  add_analysis(b, kA, 1, "c1", who(kAlice, "2016-03-04T05:06:07Z"));
  b.changesets.push_back(refit("c2", kA, 1, 101.5, who(kAlice, "2016-03-05T00:00:00Z")));
  b.resume_token = "c2";
  b.done = 2;
  b.total = 2;
  return b;
}

// Four batches over five commits, two analyses and one unparseable file.
std::vector<ImportBatch> four_batches() {
  std::vector<ImportBatch> out(4);
  out[0].catalog = lab_catalog();
  add_analysis(out[0], kA, 1, "c1", who(kAlice, "2016-03-04T05:06:07Z"));
  add_analysis(out[1], kB, 2, "c2", who("bob@example.org", "2016-03-05T00:00:00Z"));
  out[1].changesets.push_back(refit("c3", kA, 1, 101.5, who(kAlice, "2016-03-06T00:00:00Z")));
  out[2].changesets.push_back(refit("c4", kB, 2, 55.5, who(kAlice, "2016-03-07T00:00:00Z")));
  out[2].conflicts.push_back({{"c4", "notes.txt", "blob-notes"},
                              std::nullopt,
                              P::ConflictKind::Unparseable,
                              sha256(std::string_view{"notes"}),
                              R"({"reason":"unknown file"})"});
  out[3].changesets.push_back(refit("c5", kA, 1, 102.5, who("bob@example.org", "2016-03-08T00:00:00Z")));
  const char* tokens[] = {"c1", "c3", "c4", "c5"};
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i].resume_token = tokens[i];
    out[i].done = static_cast<int>(i) + 1;
    out[i].total = 4;
  }
  return out;
}

WriterConfig config() {
  WriterConfig c;
  c.importer_version = "pychron-import/test";
  c.lab_time_zone = "America/Denver";
  return c;
}

// A fresh database: the store under test and a white-box connection to it.
struct World {
  explicit World(const std::string& engine) : database(engine, true) {
    store = open_or_die(database.url());
    if (!store) return;
    client = *store->register_client({"import-1", "importer", std::nullopt, "test"});
    auto opened = pd::Db::open(P::StoreConfig{database.url(), false});
    if (opened) db = std::move(*opened);
  }

  long long count(const char* table) {
    auto row = db->select_one(QStringLiteral("SELECT count(*) AS n FROM %1").arg(QString::fromUtf8(table)));
    return row && *row ? (*row)->value("n").toLongLong() : -1;
  }

  std::map<std::string, long long> counts() {
    std::map<std::string, long long> out;
    for (const char* t : {"analysis", "signal_blob", "changeset", "revision", "head_move", "import_provenance",
                          "import_conflict", "repository_member", "bookmark", "app_user", "sample", "identifier"})
      out[t] = count(t);
    return out;
  }

  P::ImportSourceInfo source() {
    auto all = store->import_sources();
    EXPECT_TRUE(all && all->size() == 1);
    return all && !all->empty() ? all->front() : P::ImportSourceInfo{};
  }

  // Declared first so it is destroyed last: the connections point into it.
  TestDatabase database;
  std::unique_ptr<P::IStore> store;
  Uuid client;
  std::unique_ptr<pd::Db> db;
};

Result<RunStats> run_all(World& w, ISourceAdapter& adapter, WriterConfig cfg = config()) {
  BatchWriter writer(*w.store, w.client, std::move(cfg));
  return writer.run(adapter, std::nullopt, {}, {});
}

class BatchWriterTest : public ::testing::TestWithParam<std::string> {
 protected:
  void SetUp() override {
    world_ = std::make_unique<World>(GetParam());
    ASSERT_TRUE(world_->store);
    ASSERT_TRUE(world_->db);
  }

  P::IStore& store() { return *world_->store; }

  std::unique_ptr<World> world_;
};

std::string err(const Error& e) { return to_string(e); }

}  // namespace

TEST_P(BatchWriterTest, WritesCatalogAnalysesAndRevisions) {
  FakeAdapter adapter(description(), {single_batch()});
  BatchWriter writer(store(), world_->client, config());

  auto opened = writer.open(adapter);
  ASSERT_TRUE(opened) << err(opened.error());
  EXPECT_EQ(opened->spec.uuid, source_id(P::ImportSourceKind::ProjectRepo, kUrl, "main"));
  EXPECT_EQ(opened->spec.url_or_path, kUrl);
  EXPECT_EQ(opened->spec.lab_time_zone, "America/Denver");
  EXPECT_EQ(opened->status, "registered");
  EXPECT_FALSE(opened->progress_token.has_value());

  auto stats = writer.run(adapter, std::nullopt, {}, {});
  ASSERT_TRUE(stats) << err(stats.error());
  EXPECT_TRUE(stats->finished);
  EXPECT_EQ(stats->batches, 1);
  EXPECT_EQ(stats->analyses, 1);
  EXPECT_EQ(stats->changesets, 1);
  EXPECT_EQ(stats->revisions, 1);
  EXPECT_EQ(stats->conflicts, 0);
  EXPECT_FALSE(adapter.planned_token().has_value());

  auto view = store().load_analysis(kA);
  ASSERT_TRUE(view && view->has_value());
  EXPECT_EQ((*view)->summary.runid, "66573-01");
  EXPECT_EQ((*view)->summary.signals_state, "complete");
  EXPECT_EQ(std::get<P::Intercepts>((*view)->payloads.at(Kind::Intercepts)), intercepts(101.5));

  // The collection root, then the refit, each at its commit's time.
  auto history = store().history(kA, Kind::Intercepts);
  ASSERT_TRUE(history);
  ASSERT_EQ(history->size(), 2u);
  const auto& root = (*history)[0];
  const auto& later = (*history)[1];
  EXPECT_EQ(root.uuid, revision_id(kUrl, "c1", kind_path("intercepts", 1)));
  EXPECT_EQ(root.changeset.uuid, collection_changeset_id(kUrl, "c1", kA));
  EXPECT_EQ(root.changeset.kind, P::ChangesetKind::Collection);
  EXPECT_EQ(root.changeset.created, *UtcTime::parse("2016-03-04T05:06:07Z"));
  EXPECT_EQ(later.uuid, revision_id(kUrl, "c2", kind_path("intercepts", 1)));
  EXPECT_EQ(later.parent, root.uuid);
  EXPECT_EQ(later.changeset.uuid, changeset_id(kUrl, "c2"));
  EXPECT_EQ(later.changeset.kind, P::ChangesetKind::Import);
  EXPECT_EQ(later.changeset.created, *UtcTime::parse("2016-03-05T00:00:00Z"));
  EXPECT_EQ(later.changeset.message, "refit c2");

  // A kind with no file is rooted on the record.
  auto tags = store().history(kA, Kind::Tags);
  ASSERT_TRUE(tags);
  ASSERT_EQ(tags->size(), 1u);
  EXPECT_EQ(tags->front().uuid, revision_id(kUrl, "c1", record_path(1) + "#tags"));

  // Provenance: the analysis, a root revision, the later revision, its changeset.
  auto of_analysis = store().provenance_for(kA);
  ASSERT_TRUE(of_analysis);
  ASSERT_EQ(of_analysis->size(), 1u);
  EXPECT_EQ(of_analysis->front().entity_type, "analysis");
  EXPECT_EQ(of_analysis->front().path, record_path(1));
  EXPECT_EQ(of_analysis->front().commit_sha, "c1");
  EXPECT_EQ(of_analysis->front().git_blob_sha, "rec-1");
  EXPECT_EQ(of_analysis->front().git_author, "A. Author <alice@example.org>");
  EXPECT_EQ(of_analysis->front().git_utc, *UtcTime::parse("2016-03-04T05:06:07Z"));
  auto of_root = store().provenance_for(root.uuid);
  ASSERT_TRUE(of_root);
  ASSERT_EQ(of_root->size(), 1u);
  EXPECT_EQ(of_root->front().entity_type, "revision");
  EXPECT_EQ(of_root->front().git_blob_sha, "int-1");
  auto of_later = store().provenance_for(later.uuid);
  ASSERT_TRUE(of_later);
  ASSERT_EQ(of_later->size(), 1u);
  EXPECT_EQ(of_later->front().git_blob_sha, "int-c2");
  auto of_changeset = store().provenance_for(later.changeset.uuid);
  ASSERT_TRUE(of_changeset);
  ASSERT_EQ(of_changeset->size(), 1u);
  EXPECT_EQ(of_changeset->front().entity_type, "changeset");
  EXPECT_EQ(of_changeset->front().commit_sha, "c2");
  const Uuid source = opened->spec.uuid;
  EXPECT_TRUE(*store().has_provenance(source, "c1", record_path(1)));
  EXPECT_TRUE(*store().has_provenance(source, "c1", kind_path(".data", 1)));
  EXPECT_TRUE(*store().has_provenance(source, "c2", kind_path("intercepts", 1)));

  // What an adapter asks before re-reading a file.
  auto head_blob = writer.state().head_blob_sha(SubjectRef{kA}, Kind::Intercepts);
  ASSERT_TRUE(head_blob);
  EXPECT_EQ(*head_blob, std::optional<std::string>{"int-c2"});
  auto blanks_blob = writer.state().head_blob_sha(SubjectRef{kA}, Kind::Blanks);
  ASSERT_TRUE(blanks_blob);
  EXPECT_EQ(*blanks_blob, std::optional<std::string>{"bla-1"});
  EXPECT_TRUE(*writer.state().analysis_exists(kA));
  EXPECT_FALSE(*writer.state().analysis_exists(kB));

  // The catalog chain and the repository membership.
  auto detail = store().load_analysis_detail(kA);
  ASSERT_TRUE(detail && detail->has_value());
  EXPECT_EQ((*detail)->row.sample, "HH-1");
  EXPECT_EQ((*detail)->row.project, "Henry Hill");
  EXPECT_EQ((*detail)->row.material, "sanidine");
  EXPECT_EQ((*detail)->row.irradiation, "NM-300");
  EXPECT_EQ((*detail)->row.level, "A");
  EXPECT_EQ((*detail)->row.position, std::optional<int>{1});
  EXPECT_EQ((*detail)->row.repository, "Henry_Hill");

  const auto stored = world_->source();
  EXPECT_EQ(stored.status, "finished");
  EXPECT_TRUE(stored.finished.has_value());
  EXPECT_EQ(stored.progress_token, std::optional<std::string>{"c2"});
  EXPECT_EQ(stored.head_sha, std::optional<std::string>{"head-sha"});
  EXPECT_EQ(stored.done, 2);
  EXPECT_EQ(stored.total, 2);
}

TEST_P(BatchWriterTest, CatalogRowsGetDerivedIds) {
  FakeAdapter adapter(description(), {single_batch()});
  ASSERT_TRUE(run_all(*world_, adapter));

  // Ensuring a row again returns the id the writer derived from its natural key.
  EXPECT_EQ(*store().add_material(world_->client, {"sanidine", "", std::nullopt}), catalog_id("material", "sanidine\n"));
  EXPECT_EQ(*store().add_identifier(world_->client, {"66573", "unknown", {}, {}, {}, {}, {}}),
            catalog_id("identifier", "66573"));
  EXPECT_EQ(*store().add_mass_spectrometer(world_->client, {"jan", {}, {}, {}}),
            catalog_id("mass_spectrometer", "jan"));
  EXPECT_EQ(*store().add_ref_object(world_->client, {P::RefType::IrradiationHolder, "24-hole", {}, {}, {}, {}, {}}),
            catalog_id("ref_object", "irradiation_holder\n24-hole"));
  EXPECT_EQ(world_->count("principal_investigator"), 1);
  EXPECT_EQ(world_->count("project"), 1);
  EXPECT_EQ(world_->count("sample"), 1);
  EXPECT_EQ(world_->count("irradiation_position"), 1);
}

TEST_P(BatchWriterTest, LoadAndReferenceObjectItems) {
  ImportBatch b;
  b.catalog = lab_catalog();
  P::LoadSpec load;
  load.name = "load-7";
  b.catalog.push_back(LoadItem{load, "221-hole"});
  RefObjectItem flux;
  flux.type = P::RefType::FluxPosition;
  flux.key = "NM-300/A/1";
  flux.irradiation = "NM-300";
  flux.level = "A";
  flux.position = 1;
  b.catalog.push_back(flux);
  b.catalog.push_back(SpecialIdentifierItem{"ba-01-J", "blank_air", "jan"});
  b.catalog.push_back(UserItem{"mheizler"});
  b.catalog.push_back(ExtractDeviceItem{"fusions_co2"});
  // A reference changeset on the flux object, named by key.
  ChangesetItem c;
  c.commit = "m1";
  c.kind = P::ChangesetKind::Reference;
  c.who = who(kAlice, "2016-03-04T05:06:07Z");
  c.message = "flux";
  P::FluxValue value;
  value.j = 0.001;
  value.j_err = 0.00001;
  c.revisions.push_back({{"m1", "NM-300/A.json", "flux-blob"},
                         RefObjectKey{"flux_position", "NM-300/A/1"},
                         Kind::RefValue,
                         P::RefPayload{value}});
  b.changesets.push_back(c);
  b.resume_token = "m1";
  FakeAdapter adapter(description(), {b});
  BatchWriter writer(store(), world_->client, config());
  auto stats = writer.run(adapter, std::nullopt, {}, {});
  ASSERT_TRUE(stats) << err(stats.error());

  EXPECT_EQ(world_->count("load"), 1);
  EXPECT_EQ(*store().add_ref_object(world_->client, {P::RefType::LoadHolder, "221-hole", {}, {}, {}, {}, {}}),
            catalog_id("ref_object", "load_holder\n221-hole"));
  const Uuid flux_object = catalog_id("ref_object", "flux_position\nNM-300/A/1");
  auto head = store().head(flux_object, Kind::RefValue);
  ASSERT_TRUE(head);
  EXPECT_EQ(*head, std::optional<Uuid>{revision_id(kUrl, "m1", "NM-300/A.json")});
  auto blob = writer.state().head_blob_sha(RefObjectKey{"flux_position", "NM-300/A/1"}, Kind::RefValue);
  ASSERT_TRUE(blob);
  EXPECT_EQ(*blob, std::optional<std::string>{"flux-blob"});
  EXPECT_FALSE(writer.state().head_blob_sha(RefObjectKey{"no_such_type", "x"}, Kind::RefValue));
}

TEST_P(BatchWriterTest, UnknownAuthorBecomesGitUser) {
  FakeAdapter adapter(description(), {single_batch()});
  ASSERT_TRUE(run_all(*world_, adapter));

  auto history = store().history(kA, Kind::Intercepts);
  ASSERT_TRUE(history);
  ASSERT_EQ(history->size(), 2u);
  EXPECT_EQ((*history)[0].author_name, "git:alice@example.org");
  EXPECT_EQ((*history)[1].author_name, "git:alice@example.org");
  // The analyst of the record is a user of its own, not the git author.
  auto detail = store().load_analysis_detail(kA);
  ASSERT_TRUE(detail && detail->has_value());
  EXPECT_EQ((*detail)->analyst, std::optional<std::string>{"jross"});
}

TEST_P(BatchWriterTest, AuthorMapPicksExistingUser) {
  const Uuid alice = *store().ensure_user(world_->client, "asmith");
  const auto users = world_->count("app_user");
  auto cfg = config();
  cfg.author_map[kAlice] = "asmith";
  FakeAdapter adapter(description(), {single_batch()});
  ASSERT_TRUE(run_all(*world_, adapter, cfg));

  auto history = store().history(kA, Kind::Intercepts);
  ASSERT_TRUE(history);
  ASSERT_EQ(history->size(), 2u);
  for (const auto& revision : *history) {
    EXPECT_EQ(revision.author_name, "asmith");
    EXPECT_EQ(revision.changeset.author_user, alice);
  }
  EXPECT_EQ(world_->count("app_user"), users + 1);  // only the analyst "jross"
}

TEST_P(BatchWriterTest, MissingIdentifierBecomesConflict) {
  // The spectrometer is there, identifier 66573 is not. A later commit
  // revises the analysis that could not be imported.
  ImportBatch b;
  b.catalog = {MassSpecItem{{"jan", "argus", "j", std::nullopt}}};
  add_analysis(b, kA, 1, "c1", who(kAlice, "2016-03-04T05:06:07Z"));
  b.changesets.push_back(refit("c2", kA, 1, 101.5, who(kAlice, "2016-03-05T00:00:00Z")));
  b.resume_token = "c2";
  FakeAdapter adapter(description(), {b});
  auto stats = run_all(*world_, adapter);
  ASSERT_TRUE(stats) << err(stats.error());
  EXPECT_TRUE(stats->finished);
  EXPECT_EQ(stats->analyses, 0);
  EXPECT_EQ(stats->revisions, 0);
  // One per file of the refused collection (record, signals, intercepts,
  // baselines, blanks, icfactors) and one for the dropped revision.
  EXPECT_EQ(stats->conflicts, 6 + 1);

  auto view = store().load_analysis(kA);
  ASSERT_TRUE(view);
  EXPECT_FALSE(view->has_value());
  EXPECT_EQ(world_->count("changeset"), 0);
  EXPECT_EQ(world_->count("repository_member"), 0);

  const Uuid source = source_id(P::ImportSourceKind::ProjectRepo, kUrl, "main");
  auto conflicts = store().import_conflicts({source, P::ConflictKind::UnknownAnalysis, std::nullopt});
  ASSERT_TRUE(conflicts);
  ASSERT_EQ(conflicts->size(), 7u);
  for (const auto& c : *conflicts) {
    EXPECT_EQ(c.entity, std::optional<Uuid>{kA}) << c.path;
    EXPECT_EQ(c.resolution, "pending") << c.path;
  }
  // Every file is accounted for by a conflict keyed by its id.
  for (const char* dir : {".data", "intercepts", "baselines", "blanks", "icfactors"}) {
    auto row = store().import_conflict(conflict_id(kUrl, "c1", kind_path(dir, 1)));
    ASSERT_TRUE(row && row->has_value()) << dir;
    EXPECT_EQ((*row)->kind, P::ConflictKind::UnknownAnalysis);
  }
  auto of_blanks = store().import_conflict(conflict_id(kUrl, "c1", kind_path("blanks", 1)));
  EXPECT_EQ((*of_blanks)->file_sha256, std::optional<Sha256Digest>{sha256(std::string_view{"bla-1"})});
  auto of_record = std::find_if(conflicts->begin(), conflicts->end(),
                                [](const auto& c) { return c.path == record_path(1); });
  ASSERT_NE(of_record, conflicts->end());
  EXPECT_EQ(of_record->uuid, conflict_id(kUrl, "c1", record_path(1)));
  EXPECT_EQ(of_record->entity, std::optional<Uuid>{kA});
  EXPECT_EQ(of_record->file_sha256, std::optional<Sha256Digest>{sha256(std::string_view{"rec-1"})});
  EXPECT_NE(of_record->detail_json.find("66573"), std::string::npos);
  EXPECT_TRUE(*store().has_conflict(source, record_path(1), sha256(std::string_view{"rec-1"})));
  EXPECT_TRUE(*store().has_conflict(source, kind_path("intercepts", 1), sha256(std::string_view{"int-c2"})));

  // Once the identifier exists, a run that sees the analysis again imports
  // it and its later revision, and their conflicts are superseded.
  FakeAdapter again(description(), {single_batch()});
  auto second = run_all(*world_, again);
  ASSERT_TRUE(second) << err(second.error());
  EXPECT_EQ(second->analyses, 1);
  EXPECT_EQ(second->revisions, 1);
  EXPECT_EQ(second->conflicts, 0);
  view = store().load_analysis(kA);
  ASSERT_TRUE(view && view->has_value());
  conflicts = store().import_conflicts({source, std::nullopt, std::nullopt});
  ASSERT_TRUE(conflicts);
  ASSERT_EQ(conflicts->size(), 7u);
  for (const auto& c : *conflicts) EXPECT_EQ(c.resolution, "superseded") << c.path;
}

TEST_P(BatchWriterTest, MissingExtractDeviceBecomesConflict) {
  ImportBatch b;
  b.catalog = lab_catalog();
  add_analysis(b, kA, 1, "c1", who(kAlice, "2016-03-04T05:06:07Z"));
  b.analyses[0].ingest.extract_device = "fusions_co2";
  FakeAdapter adapter(description(), {b});
  auto stats = run_all(*world_, adapter);
  ASSERT_TRUE(stats) << err(stats.error());
  EXPECT_EQ(stats->analyses, 0);
  EXPECT_EQ(stats->conflicts, 6);
  auto record = store().import_conflict(conflict_id(kUrl, "c1", record_path(1)));
  ASSERT_TRUE(record && record->has_value());
  EXPECT_EQ((*record)->kind, P::ConflictKind::UnknownAnalysis);
  EXPECT_NE((*record)->detail_json.find("extract device"), std::string::npos);

  b.catalog.push_back(ExtractDeviceItem{"fusions_co2"});
  FakeAdapter fixed(description(), {b});
  stats = run_all(*world_, fixed);
  ASSERT_TRUE(stats) << err(stats.error());
  EXPECT_EQ(stats->analyses, 1);
  EXPECT_TRUE(store().import_conflicts({{}, {}, "pending"})->empty());
}

TEST_P(BatchWriterTest, ReplayImportsWhatWasRefused) {
  // Batch 1: analysis A, whose identifier is missing. Batch 2: analysis B of
  // the same identifier and a later refit of A. Batch 3: another refit of A.
  auto script = [](bool with_catalog) {
    std::vector<ImportBatch> out(3);
    out[0].catalog = {MassSpecItem{{"jan", "argus", "j", std::nullopt}}, RepositoryItem{"Henry_Hill"}};
    if (with_catalog) out[0].catalog = lab_catalog();
    add_analysis(out[0], kA, 1, "c1", who(kAlice, "2016-03-04T05:06:07Z"));
    add_analysis(out[1], kB, 2, "c2", who(kAlice, "2016-03-05T00:00:00Z"));
    out[1].changesets.push_back(refit("c3", kA, 1, 101.5, who(kAlice, "2016-03-06T00:00:00Z")));
    out[2].changesets.push_back(refit("c4", kA, 1, 102.5, who(kAlice, "2016-03-07T00:00:00Z")));
    const char* tokens[] = {"c1", "c3", "c4"};
    for (std::size_t i = 0; i < out.size(); ++i) {
      out[i].resume_token = tokens[i];
      out[i].done = static_cast<int>(i) + 1;
      out[i].total = 3;
    }
    return out;
  };
  const Uuid source = source_id(P::ImportSourceKind::ProjectRepo, kUrl, "main");
  const P::ConflictFilter pending{source, std::nullopt, "pending"};

  FakeAdapter adapter(description(), script(false));
  adapter.honour_token(true);
  auto first = run_all(*world_, adapter);
  ASSERT_TRUE(first) << err(first.error());
  EXPECT_TRUE(first->finished);
  EXPECT_EQ(first->analyses, 0);
  EXPECT_EQ(first->conflicts, 2 * 6 + 2);
  EXPECT_EQ(store().import_conflicts(pending)->size(), 14u);
  EXPECT_EQ(world_->source().progress_token, std::optional<std::string>{"c4"});

  // The catalog is fixed by hand.
  ASSERT_TRUE(store().add_identifier(world_->client, {"66573", "unknown", {}, {}, {}, {}, {}}));

  // A plain run resumes after the token: nothing is retried.
  const auto seq = *store().latest_change_seq();
  auto plain = run_all(*world_, adapter);
  ASSERT_TRUE(plain) << err(plain.error());
  EXPECT_EQ(plain->batches, 0);
  EXPECT_EQ(adapter.planned_token(), std::optional<std::string>{"c4"});
  EXPECT_EQ(*store().latest_change_seq(), seq);
  EXPECT_EQ(store().import_conflicts(pending)->size(), 14u);
  EXPECT_EQ(world_->count("analysis"), 0);

  // A replay walks the source from the start. Interrupted after one batch,
  // it must not set the stored token back.
  auto replay = config();
  replay.replay = true;
  {
    BatchWriter writer(store(), world_->client, replay);
    auto partial = writer.run(adapter, 1, {}, {});
    ASSERT_TRUE(partial) << err(partial.error());
    EXPECT_FALSE(adapter.planned_token().has_value());
    EXPECT_EQ(partial->batches, 1);
    EXPECT_EQ(partial->analyses, 1);
  }
  EXPECT_EQ(world_->source().status, "paused");
  EXPECT_EQ(world_->source().progress_token, std::optional<std::string>{"c4"});
  EXPECT_EQ(world_->source().done, 3);

  auto full = run_all(*world_, adapter, replay);
  ASSERT_TRUE(full) << err(full.error());
  EXPECT_TRUE(full->finished);
  EXPECT_EQ(full->batches, 3);
  EXPECT_EQ(full->analyses, 2);
  EXPECT_EQ(full->revisions, 2);
  EXPECT_EQ(full->conflicts, 0);
  EXPECT_EQ(world_->source().status, "finished");
  EXPECT_EQ(world_->source().progress_token, std::optional<std::string>{"c4"});

  EXPECT_TRUE(store().import_conflicts(pending)->empty());
  auto all = store().import_conflicts({source, std::nullopt, std::nullopt});
  ASSERT_TRUE(all);
  EXPECT_EQ(all->size(), 14u);
  for (const auto& c : *all) EXPECT_EQ(c.resolution, "superseded") << c.path;

  // The history is what a clean import of a source with a complete catalog gives.
  World clean(GetParam());
  ASSERT_TRUE(clean.store && clean.db);
  FakeAdapter clean_adapter(description(), script(true));
  ASSERT_TRUE(run_all(clean, clean_adapter));
  for (Uuid analysis : {kA, kB}) {
    for (Kind kind : {Kind::Intercepts, Kind::Blanks, Kind::Tags}) {
      auto mine = store().history(analysis, kind);
      auto theirs = clean.store->history(analysis, kind);
      ASSERT_TRUE(mine && theirs);
      ASSERT_EQ(mine->size(), theirs->size());
      for (std::size_t i = 0; i < mine->size(); ++i) {
        EXPECT_EQ((*mine)[i].uuid, (*theirs)[i].uuid);
        EXPECT_EQ((*mine)[i].parent, (*theirs)[i].parent);
        EXPECT_EQ((*mine)[i].changeset.created, (*theirs)[i].changeset.created);
      }
    }
  }
  auto history = store().history(kA, Kind::Intercepts);
  ASSERT_EQ(history->size(), 3u);
  EXPECT_EQ((*history)[2].uuid, revision_id(kUrl, "c4", kind_path("intercepts", 1)));
  for (const char* t : {"analysis", "changeset", "revision", "head_move", "repository_member", "import_provenance"})
    EXPECT_EQ(world_->count(t), clean.count(t)) << t;

  // A replay of a source with nothing left to fix changes nothing.
  const auto settled = *store().latest_change_seq();
  ASSERT_TRUE(run_all(*world_, adapter, replay));
  EXPECT_EQ(*store().latest_change_seq(), settled);
}

TEST_P(BatchWriterTest, ReplayMovesTheTokenForwardPastTheStoredOne) {
  auto batches = four_batches();
  FakeAdapter adapter(description(), batches);
  adapter.honour_token(true);
  {
    BatchWriter writer(store(), world_->client, config());
    ASSERT_TRUE(writer.run(adapter, 2, {}, {}));
  }
  EXPECT_EQ(world_->source().progress_token, std::optional<std::string>{"c3"});

  auto replay = config();
  replay.replay = true;
  std::vector<std::string> stored;
  BatchWriter writer(store(), world_->client, replay);
  auto stats = writer.run(adapter, std::nullopt, {}, [&](const RunStats&, const ImportBatch&) {
    stored.push_back(world_->source().progress_token.value_or(""));
  });
  ASSERT_TRUE(stats) << err(stats.error());
  EXPECT_EQ(stats->batches, 4);
  // Unchanged until the walk passes the stored token, then it advances.
  EXPECT_EQ(stored, (std::vector<std::string>{"c3", "c3", "c4", "c5"}));
  EXPECT_EQ(world_->source().done, 4);
  EXPECT_EQ(world_->source().status, "finished");
}

TEST_P(BatchWriterTest, MissingMassSpectrometerBecomesConflict) {
  ImportBatch b;
  b.catalog = lab_catalog();
  b.catalog.erase(b.catalog.end() - 2);  // the spectrometer
  add_analysis(b, kA, 1, "c1", who(kAlice, "2016-03-04T05:06:07Z"));
  FakeAdapter adapter(description(), {b});
  auto stats = run_all(*world_, adapter);
  ASSERT_TRUE(stats) << err(stats.error());
  EXPECT_EQ(stats->conflicts, 6);
  auto conflicts = store().import_conflicts({});
  ASSERT_TRUE(conflicts);
  ASSERT_EQ(conflicts->size(), 6u);
  for (const auto& c : *conflicts) {
    EXPECT_EQ(c.kind, P::ConflictKind::UnknownAnalysis);
    EXPECT_NE(c.detail_json.find("mass spectrometer"), std::string::npos);
  }
}

TEST_P(BatchWriterTest, AdapterConflictIsStoredWithItsFileHash) {
  FakeAdapter adapter(description(), four_batches());
  auto stats = run_all(*world_, adapter);
  ASSERT_TRUE(stats) << err(stats.error());
  EXPECT_EQ(stats->conflicts, 1);
  auto conflicts = store().import_conflicts({});
  ASSERT_TRUE(conflicts);
  ASSERT_EQ(conflicts->size(), 1u);
  EXPECT_EQ(conflicts->front().uuid, conflict_id(kUrl, "c4", "notes.txt"));
  EXPECT_EQ(conflicts->front().kind, P::ConflictKind::Unparseable);
  EXPECT_EQ(conflicts->front().file_sha256, std::optional<Sha256Digest>{sha256(std::string_view{"notes"})});
  EXPECT_NE(conflicts->front().detail_json.find("unknown file"), std::string::npos);
}

TEST_P(BatchWriterTest, ResumeAfterFailureMatchesCleanRun) {
  FakeAdapter adapter(description(), four_batches());
  adapter.honour_token(true);
  adapter.fail_at(2);
  {
    BatchWriter writer(store(), world_->client, config());
    auto failed = writer.run(adapter, std::nullopt, {}, {});
    ASSERT_FALSE(failed);
    EXPECT_EQ(failed.error().kind, ErrorKind::Io);
  }
  auto stored = world_->source();
  EXPECT_EQ(stored.status, "failed");
  EXPECT_EQ(stored.progress_token, std::optional<std::string>{"c1"});
  EXPECT_EQ(stored.done, 1);
  EXPECT_FALSE(stored.finished.has_value());

  adapter.fail_at(std::nullopt);
  auto resumed = run_all(*world_, adapter);
  ASSERT_TRUE(resumed) << err(resumed.error());
  EXPECT_TRUE(resumed->finished);
  EXPECT_EQ(resumed->batches, 3);
  EXPECT_EQ(adapter.planned_token(), std::optional<std::string>{"c1"});
  EXPECT_EQ(world_->source().status, "finished");

  // The same batches replayed from the start, as after a crash before the
  // token was stored, add nothing either.
  adapter.honour_token(false);
  ASSERT_TRUE(run_all(*world_, adapter));

  World clean(GetParam());
  ASSERT_TRUE(clean.store && clean.db);
  FakeAdapter clean_adapter(description(), four_batches());
  auto clean_run = run_all(clean, clean_adapter);
  ASSERT_TRUE(clean_run) << err(clean_run.error());
  EXPECT_EQ(clean_run->batches, 4);

  EXPECT_EQ(world_->counts(), clean.counts());
  EXPECT_EQ(clean.count("changeset"), 2 + 3);  // two collections, three refits
  EXPECT_EQ(clean.count("import_conflict"), 1);
  for (Uuid analysis : {kA, kB}) {
    auto mine = store().history(analysis, Kind::Intercepts);
    auto theirs = clean.store->history(analysis, Kind::Intercepts);
    ASSERT_TRUE(mine && theirs);
    ASSERT_EQ(mine->size(), theirs->size());
    for (std::size_t i = 0; i < mine->size(); ++i) EXPECT_EQ((*mine)[i].uuid, (*theirs)[i].uuid);
  }
}

TEST_P(BatchWriterTest, SecondRunWritesNothing) {
  FakeAdapter adapter(description(), four_batches());
  ASSERT_TRUE(run_all(*world_, adapter));
  const auto seq = *store().latest_change_seq();
  const auto before = world_->counts();

  auto second = run_all(*world_, adapter);  // the fake replays every batch
  ASSERT_TRUE(second) << err(second.error());
  EXPECT_TRUE(second->finished);
  EXPECT_EQ(second->batches, 4);
  EXPECT_EQ(adapter.planned_token(), std::optional<std::string>{"c5"});
  EXPECT_EQ(*store().latest_change_seq(), seq);
  EXPECT_EQ(world_->counts(), before);
  EXPECT_EQ(world_->source().status, "finished");
}

TEST_P(BatchWriterTest, DryRunCountsWithoutWriting) {
  const auto seq = *store().latest_change_seq();
  const auto before = world_->counts();
  auto dry = config();
  dry.dry_run = true;

  FakeAdapter adapter(description(), four_batches());
  auto first = run_all(*world_, adapter, dry);
  ASSERT_TRUE(first) << err(first.error());
  EXPECT_TRUE(first->finished);
  EXPECT_EQ(first->batches, 4);
  // 2 analyses, 3 distinct blobs (the baseline series is shared), 3
  // changesets, 3 revisions, 1 conflict.
  EXPECT_EQ(first->would_write, 2 + 3 + 3 + 3 + 1);
  EXPECT_EQ(*store().latest_change_seq(), seq);
  EXPECT_EQ(world_->counts(), before);
  EXPECT_TRUE(store().import_sources()->empty());

  ASSERT_TRUE(run_all(*world_, adapter));
  const auto after = world_->counts();
  const auto stored = world_->source();
  auto second = run_all(*world_, adapter, dry);
  ASSERT_TRUE(second) << err(second.error());
  EXPECT_EQ(second->would_write, 0);
  EXPECT_EQ(world_->counts(), after);
  EXPECT_EQ(world_->source().status, stored.status);
  EXPECT_EQ(world_->source().finished, stored.finished);
}

TEST_P(BatchWriterTest, DryRunDoesNotCountRecordedConflicts) {
  // An analysis the store cannot take is a recorded conflict, not pending work.
  ImportBatch b;
  b.catalog = {MassSpecItem{{"jan", "argus", "j", std::nullopt}}};
  add_analysis(b, kA, 1, "c1", who(kAlice, "2016-03-04T05:06:07Z"));
  b.changesets.push_back(refit("c2", kA, 1, 101.5, who(kAlice, "2016-03-05T00:00:00Z")));
  FakeAdapter adapter(description(), {b});
  ASSERT_TRUE(run_all(*world_, adapter));

  auto dry = config();
  dry.dry_run = true;
  auto stats = run_all(*world_, adapter, dry);
  ASSERT_TRUE(stats) << err(stats.error());
  EXPECT_EQ(stats->would_write, 0);
}

TEST_P(BatchWriterTest, PauseStopsAfterCurrentBatch) {
  FakeAdapter adapter(description(), four_batches());
  adapter.honour_token(true);
  std::vector<std::string> seen;
  {
    BatchWriter writer(store(), world_->client, config());
    auto stats = writer.run(
        adapter, std::nullopt, [] { return false; },
        [&](const RunStats& s, const ImportBatch& b) {
          seen.push_back(b.resume_token + ":" + std::to_string(s.batches));
          // The batch is committed before the callback runs.
          EXPECT_EQ(world_->source().progress_token, std::optional<std::string>{b.resume_token});
          EXPECT_EQ(world_->source().status, "running");
        });
    ASSERT_TRUE(stats) << err(stats.error());
    EXPECT_FALSE(stats->finished);
    EXPECT_EQ(stats->batches, 1);
  }
  EXPECT_EQ(seen, std::vector<std::string>{"c1:1"});
  auto stored = world_->source();
  EXPECT_EQ(stored.status, "paused");
  EXPECT_EQ(stored.progress_token, std::optional<std::string>{"c1"});
  EXPECT_EQ(stored.done, 1);
  EXPECT_EQ(stored.total, 4);
  EXPECT_EQ(adapter.served(), 1u);

  // max_batches pauses the same way; the run after that finishes.
  {
    BatchWriter writer(store(), world_->client, config());
    auto stats = writer.run(adapter, 2, {}, {});
    ASSERT_TRUE(stats) << err(stats.error());
    EXPECT_FALSE(stats->finished);
    EXPECT_EQ(stats->batches, 2);
  }
  stored = world_->source();
  EXPECT_EQ(stored.status, "paused");
  EXPECT_EQ(stored.progress_token, std::optional<std::string>{"c4"});
  auto rest = run_all(*world_, adapter);
  ASSERT_TRUE(rest) << err(rest.error());
  EXPECT_TRUE(rest->finished);
  EXPECT_EQ(rest->batches, 1);
  EXPECT_EQ(world_->source().status, "finished");
  EXPECT_EQ(world_->source().progress_token, std::optional<std::string>{"c5"});
}

TEST_P(BatchWriterTest, BookmarkAndMembership) {
  // The tag sits on c1; a later batch refits the intercepts.
  std::vector<ImportBatch> batches(2);
  batches[0].catalog = lab_catalog();
  add_analysis(batches[0], kA, 1, "c1", who(kAlice, "2016-03-04T05:06:07Z"));
  batches[0].bookmarks.push_back({"v1.0", "c1", {kA}, who(kAlice, "2016-03-04T06:00:00Z")});
  batches[0].resume_token = "c1";
  batches[1].changesets.push_back(refit("c2", kA, 1, 101.5, who(kAlice, "2016-03-05T00:00:00Z")));
  batches[1].resume_token = "c2";
  FakeAdapter adapter(description(), batches);
  ASSERT_TRUE(run_all(*world_, adapter));

  EXPECT_EQ(world_->count("repository_member"), 1);
  auto detail = store().load_analysis_detail(kA);
  ASSERT_TRUE(detail && detail->has_value());
  EXPECT_EQ((*detail)->row.repository, "Henry_Hill");

  ASSERT_EQ(world_->count("bookmark"), 1);
  auto row = world_->db->select_one(QStringLiteral("SELECT uuid, name FROM bookmark"));
  ASSERT_TRUE(row && *row);
  const Uuid bookmark = pd::to_uuid((*row)->value("uuid"));
  EXPECT_EQ(pd::to_std((*row)->value("name")), "v1.0");
  auto heads = store().bookmark_heads(bookmark);
  ASSERT_TRUE(heads);
  EXPECT_EQ(heads->size(), 6u);
  auto intercepts_head =
      std::find_if(heads->begin(), heads->end(), [](const auto& h) { return h.kind == Kind::Intercepts; });
  ASSERT_NE(intercepts_head, heads->end());
  EXPECT_EQ(intercepts_head->revision, revision_id(kUrl, "c1", kind_path("intercepts", 1)));
  auto provenance = store().provenance_for(bookmark);
  ASSERT_TRUE(provenance);
  ASSERT_EQ(provenance->size(), 1u);
  EXPECT_EQ(provenance->front().entity_type, "bookmark");
  EXPECT_EQ(provenance->front().commit_sha, "c1");
  EXPECT_EQ(provenance->front().path, "refs/tags/v1.0");
  EXPECT_EQ(world_->source().progress_token, std::optional<std::string>{"c2"});

  // A replay makes no second bookmark and no second membership.
  const auto seq = *store().latest_change_seq();
  ASSERT_TRUE(run_all(*world_, adapter));
  EXPECT_EQ(world_->count("bookmark"), 1);
  EXPECT_EQ(world_->count("repository_member"), 1);
  EXPECT_EQ(*store().latest_change_seq(), seq);

  // A second source that holds the same analysis only adds a membership.
  ImportBatch other;
  other.catalog = {RepositoryItem{"Other_Repo"}};
  other.memberships.push_back(
      {kA, {"x1", record_path(1), "rec-1"}, who("bob@example.org", "2017-01-01T00:00:00Z"), {"Other_Repo"}});
  other.memberships.push_back(
      {kB, {"x1", record_path(2), "rec-2"}, who("bob@example.org", "2017-01-01T00:00:00Z"), {"Other_Repo"}});
  other.resume_token = "x1";
  FakeAdapter other_adapter(
      {P::ImportSourceKind::ProjectRepo, "https://github.com/NMGRLData/Other_Repo", "main", "x1"}, {other});
  auto stats = run_all(*world_, other_adapter);
  ASSERT_TRUE(stats) << err(stats.error());
  EXPECT_EQ(stats->conflicts, 1);  // kB is in no source
  EXPECT_EQ(world_->count("analysis"), 1);
  EXPECT_EQ(world_->count("repository_member"), 2);
  auto of_analysis = store().provenance_for(kA);
  ASSERT_TRUE(of_analysis);
  EXPECT_EQ(of_analysis->size(), 2u);  // one per source
  const auto seq2 = *store().latest_change_seq();
  ASSERT_TRUE(run_all(*world_, other_adapter));
  EXPECT_EQ(*store().latest_change_seq(), seq2);
}

TEST_P(BatchWriterTest, SyntheticCollectionIsNotedInProvenance) {
  // The intercepts file first appears one commit after the record.
  ImportBatch b;
  b.catalog = lab_catalog();
  add_analysis(b, kA, 1, "c1", who(kAlice, "2016-03-04T05:06:07Z"));
  b.analyses[0].synthetic_collection = true;
  b.analyses[0].keys.intercepts.commit = "c9";
  b.analyses[0].detail_json = R"({"derived_uuid": true})";
  add_analysis(b, kB, 2, "c1", who(kAlice, "2016-03-04T05:06:07Z"));  // a second analysis in the same commit
  FakeAdapter adapter(description(), {b});
  auto stats = run_all(*world_, adapter);
  ASSERT_TRUE(stats) << err(stats.error());
  EXPECT_EQ(stats->analyses, 2);

  auto rows = store().provenance_for(kA);
  ASSERT_TRUE(rows);
  ASSERT_EQ(rows->size(), 1u);
  ASSERT_TRUE(rows->front().detail_json.has_value());
  const std::string detail = *rows->front().detail_json;
  EXPECT_NE(detail.find("derived_uuid"), std::string::npos) << detail;
  EXPECT_NE(detail.find("synthetic_collection"), std::string::npos) << detail;
  EXPECT_NE(detail.find("root_commits"), std::string::npos) << detail;
  EXPECT_NE(detail.find("c9"), std::string::npos) << detail;
  auto root = store().history(kA, Kind::Intercepts);
  ASSERT_TRUE(root);
  ASSERT_EQ(root->size(), 1u);
  EXPECT_EQ(root->front().uuid, revision_id(kUrl, "c9", kind_path("intercepts", 1)));

  auto plain = store().provenance_for(kB);
  ASSERT_TRUE(plain);
  ASSERT_EQ(plain->size(), 1u);
  EXPECT_EQ(plain->front().detail_json.value_or("{}"), "{}");
}

TEST_P(BatchWriterTest, FailureBeforeTheFirstBatchLeavesNoToken) {
  FakeAdapter adapter(description(), four_batches());
  adapter.fail_at(1);
  ASSERT_FALSE(run_all(*world_, adapter));
  EXPECT_EQ(world_->source().status, "failed");
  EXPECT_EQ(world_->source().done, 0);

  adapter.fail_at(std::nullopt);
  auto stats = run_all(*world_, adapter);
  ASSERT_TRUE(stats) << err(stats.error());
  EXPECT_FALSE(adapter.planned_token().has_value());
  EXPECT_EQ(stats->batches, 4);
}

TEST_P(BatchWriterTest, StoreErrorFailsTheRunAndKeepsTheToken) {
  // The second batch carries a payload that does not match its kind.
  auto batches = four_batches();
  batches[1].changesets[0].revisions[0].kind = Kind::Baselines;
  FakeAdapter adapter(description(), batches);
  auto stats = run_all(*world_, adapter);
  ASSERT_FALSE(stats);
  EXPECT_EQ(world_->source().status, "failed");
  EXPECT_EQ(world_->source().progress_token, std::optional<std::string>{"c1"});
  EXPECT_EQ(world_->count("import_provenance"), 1 + 5);  // batch 1 only: analysis A and its five root files
}

TEST_P(BatchWriterTest, ReplayBatchWithoutATokenDoesNotMoveTheTokenBack) {
  FakeAdapter adapter(description(), four_batches());
  ASSERT_TRUE(run_all(*world_, adapter));
  ASSERT_EQ(world_->source().progress_token, std::optional<std::string>{"c5"});
  ASSERT_EQ(world_->source().done, 4);

  // The replay's first batch carries no resume token; it is paused after the second.
  auto batches = four_batches();
  batches[0].resume_token.clear();
  FakeAdapter replayed(description(), batches);
  auto replay = config();
  replay.replay = true;
  std::vector<std::string> stored;
  {
    BatchWriter writer(store(), world_->client, replay);
    auto stats = writer.run(replayed, 2, {}, [&](const RunStats&, const ImportBatch&) {
      stored.push_back(world_->source().progress_token.value_or(""));
    });
    ASSERT_TRUE(stats) << err(stats.error());
    EXPECT_EQ(stats->batches, 2);
  }
  EXPECT_EQ(stored, (std::vector<std::string>{"c5", "c5"}));
  EXPECT_EQ(world_->source().status, "paused");
  EXPECT_EQ(world_->source().progress_token, std::optional<std::string>{"c5"});
  EXPECT_EQ(world_->source().done, 4);
  EXPECT_EQ(world_->source().total, 4);
}

TEST_P(BatchWriterTest, ReplayThatNeverMeetsTheStoredTokenAdoptsTheEndOfTheWalk) {
  FakeAdapter adapter(description(), four_batches());
  ASSERT_TRUE(run_all(*world_, adapter));
  ASSERT_EQ(world_->source().progress_token, std::optional<std::string>{"c5"});

  // The same content, cut into batches whose tokens the stored one is not among.
  auto batches = four_batches();
  const char* tokens[] = {"x1", "x2", "x3", "x4"};
  for (std::size_t i = 0; i < batches.size(); ++i) {
    batches[i].resume_token = tokens[i];
    batches[i].total = 9;
  }
  FakeAdapter recut(description(), batches);
  auto replay = config();
  replay.replay = true;
  const auto seq = *store().latest_change_seq();
  const auto before = world_->counts();
  std::vector<std::string> stored;
  {
    BatchWriter writer(store(), world_->client, replay);
    auto stats = writer.run(recut, std::nullopt, {}, [&](const RunStats&, const ImportBatch&) {
      stored.push_back(world_->source().progress_token.value_or(""));
    });
    ASSERT_TRUE(stats) << err(stats.error());
    EXPECT_TRUE(stats->finished);
  }
  // Untouched during the walk, then the walk's end.
  EXPECT_EQ(stored, (std::vector<std::string>{"c5", "c5", "c5", "c5"}));
  EXPECT_EQ(world_->source().status, "finished");
  EXPECT_EQ(world_->source().progress_token, std::optional<std::string>{"x4"});
  EXPECT_EQ(world_->source().done, 4);
  EXPECT_EQ(world_->source().total, 9);

  // The same replay again writes nothing and keeps the token.
  auto again = run_all(*world_, recut, replay);
  ASSERT_TRUE(again) << err(again.error());
  EXPECT_TRUE(again->finished);
  EXPECT_EQ(again->batches, 4);
  EXPECT_EQ(*store().latest_change_seq(), seq);
  EXPECT_EQ(world_->counts(), before);
  EXPECT_EQ(world_->source().progress_token, std::optional<std::string>{"x4"});
  EXPECT_EQ(world_->source().done, 4);
}

TEST_P(BatchWriterTest, BookmarkSurvivesAFailureBeforeItIsRecorded) {
  ImportBatch b;
  b.catalog = lab_catalog();
  add_analysis(b, kA, 1, "c1", who(kAlice, "2016-03-04T05:06:07Z"));
  b.bookmarks.push_back({"v1.0", "c1", {kA}, who(kAlice, "2016-03-04T06:00:00Z")});
  b.resume_token = "c1";
  FakeAdapter adapter(description(), {b});

  // The batch commits (1st unit of work), the group and the bookmark are
  // created, and the unit of work that records them (2nd) cannot be opened.
  ForwardingStore flaky(store());
  flaky.before_import_batch = [](int n) -> Result<void> {
    if (n == 2) return fail(ErrorKind::Io, "connection lost");
    return {};
  };
  {
    BatchWriter writer(flaky, world_->client, config());
    auto failed = writer.run(adapter, std::nullopt, {}, {});
    ASSERT_FALSE(failed);
    EXPECT_EQ(failed.error().what, "connection lost");
  }
  EXPECT_EQ(world_->count("analysis_group"), 1);
  EXPECT_EQ(world_->count("bookmark"), 1);
  EXPECT_EQ(world_->source().status, "failed");
  EXPECT_EQ(world_->source().progress_token.value_or(""), "");
  const Uuid bookmark = bookmark_id(kUrl, "v1.0");
  EXPECT_TRUE(store().provenance_for(bookmark)->empty());

  auto stats = run_all(*world_, adapter);
  ASSERT_TRUE(stats) << err(stats.error());
  EXPECT_EQ(world_->count("analysis_group"), 1);
  EXPECT_EQ(world_->count("bookmark"), 1);
  EXPECT_EQ(world_->count("analysis_group_member"), 1);
  EXPECT_EQ(store().bookmark_heads(bookmark)->size(), 6u);
  EXPECT_EQ(store().provenance_for(bookmark)->size(), 1u);
  EXPECT_EQ(world_->source().progress_token, std::optional<std::string>{"c1"});

  // The same holds when the failure comes between the group and the bookmark.
  World other(GetParam());
  ASSERT_TRUE(other.store && other.db);
  const P::Actor actor{*other.store->ensure_user(other.client, "someone"), other.client};
  {
    ImportBatch without = b;
    without.bookmarks.clear();
    without.resume_token.clear();
    FakeAdapter first(description(), {without});
    ASSERT_TRUE(run_all(other, first));
  }
  ASSERT_TRUE(other.store->create_group(actor, "v1.0", {kA}, bookmark_group_id(kUrl, "v1.0")));
  FakeAdapter second(description(), {b});
  ASSERT_TRUE(run_all(other, second));
  EXPECT_EQ(other.count("analysis_group"), 1);
  EXPECT_EQ(other.count("bookmark"), 1);
  EXPECT_EQ(other.store->bookmark_heads(bookmark)->size(), 6u);
}

TEST_P(BatchWriterTest, TagWithNoStoredAnalysisIsSkippedAndNotPending) {
  // The tag lists an analysis that was refused and one that was never sent.
  ImportBatch b;
  b.catalog = {MassSpecItem{{"jan", "argus", "j", std::nullopt}}};
  add_analysis(b, kA, 1, "c1", who(kAlice, "2016-03-04T05:06:07Z"));
  b.bookmarks.push_back({"v0.1", "c1", {kA, kB}, who(kAlice, "2016-03-04T06:00:00Z")});
  b.resume_token = "c1";
  FakeAdapter adapter(description(), {b});

  auto dry = config();
  dry.dry_run = true;
  auto before = run_all(*world_, adapter, dry);
  ASSERT_TRUE(before) << err(before.error());
  // Blobs and the analysis, and the bookmark that would capture it.
  EXPECT_EQ(before->would_write, 2 + 1 + 1);

  ASSERT_TRUE(run_all(*world_, adapter));
  EXPECT_EQ(world_->count("bookmark"), 0);
  EXPECT_EQ(world_->count("analysis_group"), 0);
  EXPECT_EQ(world_->source().progress_token, std::optional<std::string>{"c1"});

  auto after = run_all(*world_, adapter, dry);
  ASSERT_TRUE(after) << err(after.error());
  EXPECT_EQ(after->would_write, 0);
}

TEST_P(BatchWriterTest, InterpretedAgeIsEnsuredAndRevised) {
  const std::string path = "665/ia/73.ia.json";
  ImportBatch b = single_batch();
  b.catalog.push_back(InterpretedAgeItem{path, "66573 plateau", "66573", "Henry_Hill"});
  P::InterpretedAgeValue value;
  value.age = 28.2;
  value.age_err = 0.03;
  value.age_kind = "Plateau";
  value.nanalyses = 2;
  value.doc_json = R"({"name":"66573 plateau"})";
  value.members = {{kA, "66573-01", true, "ok"}, {kB, "66573-02", false, "omit"}};  // kB is in no source
  ChangesetItem saved;
  saved.commit = "c3";
  saved.who = who(kAlice, "2016-03-06T00:00:00Z");
  saved.message = "<IA> added interpreted age 73";
  saved.revisions.push_back(
      {{"c3", path, "ia-1"}, InterpretedAgeKey{path}, Kind::InterpretedAge, value, R"({"legacy_format":"flat"})"});
  b.changesets.push_back(saved);
  b.resume_token = "c3";
  FakeAdapter adapter(description(), {b});
  BatchWriter writer(store(), world_->client, config());
  auto stats = writer.run(adapter, std::nullopt, {}, {});
  ASSERT_TRUE(stats) << err(stats.error());
  EXPECT_EQ(stats->conflicts, 0);

  // One interpreted age, with the id derived from the source and the file path.
  const Uuid age = interpreted_age_id(kUrl, path);
  ASSERT_EQ(world_->count("interpreted_age"), 1);
  auto row = world_->db->select_one(QStringLiteral("SELECT uuid, name, identifier_uuid, repository_uuid FROM interpreted_age"));
  ASSERT_TRUE(row && *row);
  EXPECT_EQ(pd::to_uuid((*row)->value("uuid")), age);
  EXPECT_EQ(pd::to_std((*row)->value("name")), "66573 plateau");
  EXPECT_FALSE((*row)->value("identifier_uuid").isNull());  // 66573 has an analysis
  EXPECT_FALSE((*row)->value("repository_uuid").isNull());

  auto history = store().history(age, Kind::InterpretedAge);
  ASSERT_TRUE(history);
  ASSERT_EQ(history->size(), 1u);
  EXPECT_EQ(history->front().uuid, revision_id(kUrl, "c3", path));
  EXPECT_EQ(history->front().changeset.message, "<IA> added interpreted age 73");
  // The member that is not in the store is dropped and named in provenance.
  auto payload = store().load_payload(history->front().uuid);
  ASSERT_TRUE(payload && payload->has_value());
  const auto& stored = std::get<P::InterpretedAgeValue>(**payload);
  ASSERT_EQ(stored.members.size(), 1u);
  EXPECT_EQ(stored.members.front().analysis, kA);
  EXPECT_EQ(stored.age, std::optional<double>{28.2});
  auto provenance = store().provenance_for(history->front().uuid);
  ASSERT_TRUE(provenance);
  ASSERT_EQ(provenance->size(), 1u);
  const std::string detail = provenance->front().detail_json.value_or("");
  EXPECT_NE(detail.find("unresolved_references"), std::string::npos) << detail;
  EXPECT_NE(detail.find(kB.str()), std::string::npos) << detail;
  EXPECT_EQ(detail.find(kA.str()), std::string::npos) << detail;
  EXPECT_NE(detail.find("legacy_format"), std::string::npos) << detail;

  auto head_blob = writer.state().head_blob_sha(SubjectRef{InterpretedAgeKey{path}}, Kind::InterpretedAge);
  ASSERT_TRUE(head_blob) << err(head_blob.error());
  EXPECT_EQ(*head_blob, std::optional<std::string>{"ia-1"});

  // Again: no second interpreted age, nothing written.
  const auto seq = *store().latest_change_seq();
  ASSERT_TRUE(run_all(*world_, adapter));
  EXPECT_EQ(world_->count("interpreted_age"), 1);
  EXPECT_EQ(*store().latest_change_seq(), seq);
}

TEST_P(BatchWriterTest, InterpretedAgeOfAnUnusedIdentifierHasNone) {
  ImportBatch b;
  b.catalog.push_back(InterpretedAgeItem{"999/ia/01.ia.json", "99901", "99901", std::nullopt});
  FakeAdapter adapter(description(), {b});
  ASSERT_TRUE(run_all(*world_, adapter));
  auto row = world_->db->select_one(QStringLiteral("SELECT identifier_uuid FROM interpreted_age"));
  ASSERT_TRUE(row && *row);
  EXPECT_TRUE((*row)->value("identifier_uuid").isNull());
  EXPECT_EQ(world_->count("identifier"), 0);  // not created as a side effect
}

TEST_P(BatchWriterTest, ReferencesToAnalysesNotInTheStoreAreClearedAndNoted) {
  // The blanks of A name A itself (stored) and B (in no source), first in the
  // collection root, then in a later revision.
  P::BlankRow blank;
  blank.isotope = "Ar40";
  blank.value = 0.5;
  blank.error = 0.05;
  blank.fit = "Bracketing Interpolate";
  blank.extra_json = R"({"references":[{"record_id":"bu-1","uuid":")" + kB.str() + R"("}]})";
  blank.references = {{0, kB, "bu-1", false}};
  ImportBatch b;
  b.catalog = lab_catalog();
  add_analysis(b, kA, 1, "c1", who(kAlice, "2016-03-04T05:06:07Z"));
  b.analyses[0].ingest.roots.blanks_rows = {blank};
  blank.references = {{0, kA, "66573-01", false}, {1, kB, "bu-1", true}};
  ChangesetItem later;
  later.commit = "c2";
  later.who = who(kAlice, "2016-03-05T00:00:00Z");
  later.message = "<BLANKS> auto update blanks";
  later.revisions.push_back({{"c2", kind_path("blanks", 1), "bla-c2"}, kA, Kind::Blanks, P::Blanks{blank}});
  b.changesets.push_back(later);
  b.resume_token = "c2";
  FakeAdapter adapter(description(), {b});
  auto stats = run_all(*world_, adapter);
  ASSERT_TRUE(stats) << err(stats.error());
  EXPECT_EQ(stats->conflicts, 0);
  EXPECT_EQ(stats->revisions, 1);

  auto history = store().history(kA, Kind::Blanks);
  ASSERT_TRUE(history);
  ASSERT_EQ(history->size(), 2u);
  const auto root = std::get<P::Blanks>(**store().load_payload((*history)[0].uuid));
  ASSERT_EQ(root.size(), 1u);
  ASSERT_EQ(root[0].references.size(), 1u);
  EXPECT_FALSE(root[0].references[0].ref_analysis.has_value());
  EXPECT_EQ(root[0].references[0].record_id, std::optional<std::string>{"bu-1"});
  ASSERT_TRUE(root[0].extra_json.has_value());
  EXPECT_NE(root[0].extra_json->find(kB.str()), std::string::npos);  // verbatim, from the adapter
  const auto head = std::get<P::Blanks>(**store().load_payload((*history)[1].uuid));
  ASSERT_EQ(head[0].references.size(), 2u);
  EXPECT_EQ(head[0].references[0].ref_analysis, std::optional<Uuid>{kA});
  EXPECT_FALSE(head[0].references[1].ref_analysis.has_value());
  EXPECT_TRUE(head[0].references[1].exclude);

  const std::string of_analysis = store().provenance_for(kA)->front().detail_json.value_or("");
  EXPECT_NE(of_analysis.find("unresolved_references"), std::string::npos) << of_analysis;
  EXPECT_NE(of_analysis.find(kB.str()), std::string::npos) << of_analysis;
  const std::string of_revision = store().provenance_for((*history)[1].uuid)->front().detail_json.value_or("");
  EXPECT_NE(of_revision.find("unresolved_references"), std::string::npos) << of_revision;
  EXPECT_NE(of_revision.find(kB.str()), std::string::npos) << of_revision;
  EXPECT_EQ(of_revision.find(kA.str()), std::string::npos) << of_revision;
}

TEST_P(BatchWriterTest, AnalysisOriginTellsWhoCollectedIt) {
  FakeAdapter adapter(description(), {single_batch()});
  BatchWriter writer(store(), world_->client, config());
  ASSERT_TRUE(writer.run(adapter, std::nullopt, {}, {}));

  auto here = writer.state().analysis_origin(kA, "c1");
  ASSERT_TRUE(here) << err(here.error());
  ASSERT_TRUE(here->has_value());
  EXPECT_TRUE((*here)->from_this_source);
  EXPECT_EQ((*here)->record_blob_sha, "rec-1");
  auto other_commit = writer.state().analysis_origin(kA, "c2");
  ASSERT_TRUE(other_commit && other_commit->has_value());
  EXPECT_FALSE((*other_commit)->from_this_source);
  auto missing = writer.state().analysis_origin(kB, "c1");
  ASSERT_TRUE(missing);
  EXPECT_FALSE(missing->has_value());

  // A second source that holds a copy: the analysis is not its own, and the
  // record it was created from is still the first source's.
  ImportBatch copy;
  copy.memberships.push_back(
      {kA, {"c1", record_path(1), "rec-copy"}, who("bob@example.org", "2017-01-01T00:00:00Z"), {"Other_Repo"}});
  copy.resume_token = "c1";
  FakeAdapter other_adapter(
      {P::ImportSourceKind::ProjectRepo, "https://github.com/NMGRLData/Other_Repo", "main", "c1"}, {copy});
  BatchWriter other(store(), world_->client, config());
  ASSERT_TRUE(other.run(other_adapter, std::nullopt, {}, {}));
  auto there = other.state().analysis_origin(kA, "c1");
  ASSERT_TRUE(there && there->has_value());
  EXPECT_FALSE((*there)->from_this_source);
  EXPECT_EQ((*there)->record_blob_sha, "rec-1");
}

TEST_P(BatchWriterTest, IdentityRevisionNamesItsIdentifier) {
  // A and B are 66573-01 and 66573-02. Later commits renumber A to 66573-07
  // (free), then to 66573-02 (B's), then to an identifier nobody knows.
  ImportBatch b;
  b.catalog = lab_catalog();
  add_analysis(b, kA, 1, "c1", who(kAlice, "2016-03-04T05:06:07Z"));
  add_analysis(b, kB, 2, "c1", who(kAlice, "2016-03-04T05:06:07Z"));
  const auto renumber = [&](const std::string& commit, const std::string& identifier, int aliquot) {
    ChangesetItem c;
    c.commit = commit;
    c.who = who(kAlice, "2016-03-05T00:00:00Z");
    c.message = "<EDIT> RunID";
    RevisionItem revision{{commit, record_path(1), "rec-" + commit}, kA, Kind::Identity,
                          P::IdentityValue{Uuid{}, aliquot, -1, "legacy record rewritten"}};
    revision.identifier = identifier;
    c.revisions.push_back(std::move(revision));
    return c;
  };
  b.changesets = {renumber("c2", "66573", 7), renumber("c3", "66573", 2), renumber("c4", "99999", 1)};
  b.resume_token = "c4";
  FakeAdapter adapter(description(), {b});
  BatchWriter writer(store(), world_->client, config());
  auto stats = writer.run(adapter, std::nullopt, {}, {});
  ASSERT_TRUE(stats) << err(stats.error());
  EXPECT_EQ(stats->revisions, 1);
  EXPECT_EQ(stats->conflicts, 2);

  EXPECT_EQ((*store().load_analysis(kA))->summary.runid, "66573-07");
  EXPECT_EQ((*store().load_analysis(kB))->summary.runid, "66573-02");
  auto history = store().history(kA, Kind::Identity);
  ASSERT_TRUE(history);
  ASSERT_EQ(history->size(), 1u);
  EXPECT_EQ(history->front().uuid, revision_id(kUrl, "c2", record_path(1)));
  auto clash = store().import_conflict(conflict_id(kUrl, "c3", record_path(1)));
  ASSERT_TRUE(clash && clash->has_value());
  EXPECT_EQ((*clash)->kind, P::ConflictKind::IdentityClash);
  EXPECT_EQ((*clash)->entity, std::optional<Uuid>{kA});
  EXPECT_NE((*clash)->detail_json.find(kB.str()), std::string::npos);
  auto unknown = store().import_conflict(conflict_id(kUrl, "c4", record_path(1)));
  ASSERT_TRUE(unknown && unknown->has_value());
  EXPECT_EQ((*unknown)->kind, P::ConflictKind::UnknownAnalysis);

  // What an adapter asks before it sends an analysis.
  EXPECT_EQ(*writer.state().analysis_with_runid("66573", 7, -1), std::optional<Uuid>{kA});
  EXPECT_FALSE(writer.state().analysis_with_runid("66573", 1, -1)->has_value());
  EXPECT_EQ(*writer.state().identifier_at("NM-300", "A", 1), std::optional<std::string>{"66573"});
  EXPECT_FALSE(writer.state().identifier_at("NM-300", "A", 2)->has_value());

  const auto seq = *store().latest_change_seq();
  ASSERT_TRUE(run_all(*world_, adapter));
  EXPECT_EQ(*store().latest_change_seq(), seq);
}

TEST_P(BatchWriterTest, LevelProductionRevisionNamesItsProduction) {
  // The production a level uses is named by key, like everything in a batch;
  // the object need not have been sent, and may exist under any uuid.
  const Uuid existing = *store().add_ref_object(world_->client, {P::RefType::Production, "NM-300/Triga", {}, {}, {}, {}, {}});
  ImportBatch b;
  ChangesetItem c;
  c.commit = "m1";
  c.kind = P::ChangesetKind::Reference;
  c.who = who(kAlice, "2016-03-04T05:06:07Z");
  c.message = "productions";
  const auto level = [&](const std::string& name, const std::string& production) {
    RevisionItem revision{{"m1", "NM-300/productions.json#" + name, "blob"},
                          RefObjectKey{"level_production", "NM-300/" + name},
                          Kind::RefValue,
                          P::RefPayload{P::LevelProductionValue{}}};
    revision.production_key = production;
    return revision;
  };
  c.revisions = {level("A", "NM-300/Triga"), level("B", "NM-300/Other")};
  // A name on a payload that has no production is a mistake of the adapter.
  ChangesetItem wrong = c;
  wrong.commit = "m2";
  wrong.revisions = {{{"m2", "NM-300/A.json#1", "blob"}, RefObjectKey{"flux_position", "NM-300/A/1"}, Kind::RefValue,
                      P::RefPayload{P::FluxValue{}}}};
  wrong.revisions[0].production_key = "NM-300/Triga";
  b.changesets.push_back(c);
  b.resume_token = "m1";
  FakeAdapter adapter(description(), {b});
  BatchWriter writer(store(), world_->client, config());
  auto stats = writer.run(adapter, std::nullopt, {}, {});
  ASSERT_TRUE(stats) << err(stats.error());

  const auto production_of = [&](const std::string& name) {
    auto head = store().head(catalog_id("ref_object", "level_production\nNM-300/" + name), Kind::RefValue);
    EXPECT_TRUE(head && head->has_value());
    auto payload = store().load_payload(**head);
    EXPECT_TRUE(payload && payload->has_value());
    return std::get<P::LevelProductionValue>(std::get<P::RefPayload>(**payload)).production;
  };
  EXPECT_EQ(production_of("A"), existing);
  EXPECT_EQ(production_of("B"), catalog_id("ref_object", "production\nNM-300/Other"));

  ImportBatch bad;
  bad.changesets.push_back(wrong);
  bad.resume_token = "m2";
  FakeAdapter mistaken(description(), {bad});
  BatchWriter second(store(), world_->client, config());
  EXPECT_FALSE(second.run(mistaken, std::nullopt, {}, {}));
}

TEST_P(BatchWriterTest, ChangesetWithoutRevisionsKeepsItsDetail) {
  ImportBatch b = single_batch();
  ChangesetItem sync;
  sync.commit = "c3";
  sync.who = who(kAlice, "2016-03-06T00:00:00Z");
  sync.message = "<SYNC> Synced repository with database";
  sync.detail_json = R"({"rewrites":[{"path":"665/73-01.json","blob":"rec-c3"}]})";
  b.changesets.push_back(sync);
  b.resume_token = "c3";
  FakeAdapter adapter(description(), {b});
  auto stats = run_all(*world_, adapter);
  ASSERT_TRUE(stats) << err(stats.error());
  EXPECT_EQ(stats->changesets, 2);
  EXPECT_EQ(stats->conflicts, 0);

  auto rows = store().provenance_for(changeset_id(kUrl, "c3"));
  ASSERT_TRUE(rows);
  ASSERT_EQ(rows->size(), 1u);
  EXPECT_EQ(rows->front().entity_type, "changeset");
  const std::string detail = rows->front().detail_json.value_or("");
  EXPECT_NE(detail.find("rewrites"), std::string::npos) << detail;
  EXPECT_NE(detail.find("rec-c3"), std::string::npos) << detail;
  auto row = world_->db->select_one(QStringLiteral("SELECT message FROM changeset WHERE uuid = ?"),
                                    {pd::qv(changeset_id(kUrl, "c3"))});
  ASSERT_TRUE(row && *row);
  // The refit's changeset has no detail of its own.
  EXPECT_FALSE(store().provenance_for(changeset_id(kUrl, "c2"))->front().detail_json.has_value());

  const auto seq = *store().latest_change_seq();
  ASSERT_TRUE(run_all(*world_, adapter));
  EXPECT_EQ(*store().latest_change_seq(), seq);
}

TEST_P(BatchWriterTest, SecondAnalysisWithATakenRunIdIsAConflictNotAnError) {
  // B claims the run id A has: in the same batch, and against the store.
  ImportBatch b;
  b.catalog = lab_catalog();
  add_analysis(b, kA, 1, "c1", who(kAlice, "2016-03-04T05:06:07Z"));
  add_analysis(b, kB, 1, "c2", who(kAlice, "2016-03-05T00:00:00Z"));
  b.analyses[1].keys.record.path = "665/other.json";
  b.resume_token = "c2";
  FakeAdapter adapter(description(), {b});
  auto stats = run_all(*world_, adapter);
  ASSERT_TRUE(stats) << err(stats.error());
  EXPECT_EQ(stats->analyses, 1);
  EXPECT_EQ(stats->conflicts, 6);  // the record and five root files
  EXPECT_TRUE(store().load_analysis(kA)->has_value());
  EXPECT_FALSE(store().load_analysis(kB)->has_value());
  auto clash = store().import_conflict(conflict_id(kUrl, "c2", "665/other.json"));
  ASSERT_TRUE(clash && clash->has_value());
  EXPECT_EQ((*clash)->kind, P::ConflictKind::IdentityClash);
  EXPECT_EQ((*clash)->entity, std::optional<Uuid>{kA});
  EXPECT_NE((*clash)->detail_json.find(kB.str()), std::string::npos);

  const auto seq = *store().latest_change_seq();
  ASSERT_TRUE(run_all(*world_, adapter));
  EXPECT_EQ(*store().latest_change_seq(), seq);
  EXPECT_EQ(world_->count("import_conflict"), 6);
}

TEST_P(BatchWriterTest, IdentityRevisionsOfOneBatchSeeEachOther) {
  const auto renumber = [&](const std::string& commit, Uuid analysis, int file, int aliquot) {
    ChangesetItem c;
    c.commit = commit;
    c.who = who(kAlice, "2016-03-05T00:00:00Z");
    c.message = "<EDIT> RunID";
    RevisionItem revision{{commit, record_path(file), "rec-" + commit}, analysis, Kind::Identity,
                          P::IdentityValue{Uuid{}, aliquot, -1, "legacy record rewritten"}};
    revision.identifier = "66573";
    c.revisions.push_back(std::move(revision));
    return c;
  };
  // A leaves 66573-01 for -07; B then takes -01. Then A moves on to -08 and B to -07.
  ImportBatch first;
  first.catalog = lab_catalog();
  add_analysis(first, kA, 1, "c1", who(kAlice, "2016-03-04T05:06:07Z"));
  add_analysis(first, kB, 2, "c1", who(kAlice, "2016-03-04T05:06:07Z"));
  first.changesets = {renumber("c2", kA, 1, 7), renumber("c3", kB, 2, 1), renumber("c4", kA, 1, 8),
                      renumber("c5", kB, 2, 7)};
  first.resume_token = "c5";
  FakeAdapter adapter(description(), {first});
  auto stats = run_all(*world_, adapter);
  ASSERT_TRUE(stats) << err(stats.error());
  EXPECT_EQ(stats->conflicts, 0);
  EXPECT_EQ(stats->revisions, 4);
  EXPECT_EQ((*store().load_analysis(kA))->summary.runid, "66573-08");
  EXPECT_EQ((*store().load_analysis(kB))->summary.runid, "66573-07");
  EXPECT_EQ(store().history(kA, Kind::Identity)->size(), 2u);

  // A replay meets revisions whose run ids have since been taken by the other
  // analysis. They are stored: nothing is checked, nothing is written.
  const auto seq = *store().latest_change_seq();
  auto replay = config();
  replay.replay = true;
  auto again = run_all(*world_, adapter, replay);
  ASSERT_TRUE(again) << err(again.error());
  EXPECT_EQ(again->conflicts, 0);
  EXPECT_EQ(world_->count("import_conflict"), 0);
  EXPECT_EQ(*store().latest_change_seq(), seq);
  EXPECT_EQ((*store().load_analysis(kA))->summary.runid, "66573-08");
}

TEST_P(BatchWriterTest, RewritesOfOneCommitAreMergedAcrossBatches) {
  const auto with_notes = [&](std::vector<FileNote> notes) {
    ChangesetItem c;
    c.commit = "c3";
    c.who = who(kAlice, "2016-03-06T00:00:00Z");
    c.message = "<SYNC> Synced repository with database";
    c.rewrites = std::move(notes);
    return c;
  };
  // The same commit in two batches: first a revision and one note, later two
  // more notes, one of them a repeat.
  std::vector<ImportBatch> batches(2);
  batches[0] = single_batch();
  batches[0].changesets.push_back(with_notes({{"665/73-09.json", R"({"path":"665/73-09.json","blob":"b9"})"}}));
  batches[0].resume_token = "c3";
  batches[1].changesets.push_back(with_notes(
      {{"665/73-09.json", R"({"path":"665/73-09.json","blob":"b9"})"},
       {"665/73-01.json",
        R"({"path":"665/73-01.json","blob":"b1","changed":{"hash_id":{"old":-7399522718437156748,"new":"a ] \" }"}}})"}}));
  batches[1].resume_token = "c4";
  FakeAdapter adapter(description(), batches);
  auto stats = run_all(*world_, adapter);
  ASSERT_TRUE(stats) << err(stats.error());

  const auto detail_of = [&] {
    auto rows = store().provenance_for(changeset_id(kUrl, "c3"));
    EXPECT_TRUE(rows && rows->size() == 1);
    return rows && !rows->empty() ? rows->front().detail_json.value_or("") : std::string();
  };
  const std::string detail = detail_of();
  // Both paths, once each, sorted by path; a 64-bit number keeps its digits
  // and a string with brackets in it is not taken apart.
  const auto first = detail.find("665/73-01.json");
  const auto second = detail.find("665/73-09.json");
  ASSERT_NE(first, std::string::npos) << detail;
  ASSERT_NE(second, std::string::npos) << detail;
  EXPECT_LT(first, second) << detail;
  EXPECT_EQ(detail.find("665/73-09.json", second + 1), std::string::npos) << detail;
  EXPECT_NE(detail.find("-7399522718437156748"), std::string::npos) << detail;
  EXPECT_NE(detail.find(R"(a ] \" })"), std::string::npos) << detail;
  EXPECT_EQ(world_->count("changeset"), 3);  // the collection, c2, c3

  // Again, and replayed: the row is not touched.
  const auto seq = *store().latest_change_seq();
  auto replay = config();
  replay.replay = true;
  ASSERT_TRUE(run_all(*world_, adapter, replay));
  EXPECT_EQ(*store().latest_change_seq(), seq);
  EXPECT_EQ(detail_of(), detail);
}

TEST_P(BatchWriterTest, ChangesetThatBringsNothingNewIsNotWritten) {
  // A root file sent again as a revision of its own commit (an adapter does
  // that for an analysis it sends twice): the revision is the stored root.
  ImportBatch b = single_batch();
  ChangesetItem repeat;
  repeat.commit = "c1";
  repeat.who = who(kAlice, "2016-03-04T05:06:07Z");
  repeat.message = "<ISOEVO> default collection fits";
  repeat.revisions.push_back({{"c1", kind_path("intercepts", 1), "int-1"}, kA, Kind::Intercepts, intercepts(100.5)});
  b.changesets.push_back(repeat);
  FakeAdapter adapter(description(), {b});
  auto stats = run_all(*world_, adapter);
  ASSERT_TRUE(stats) << err(stats.error());

  EXPECT_EQ(store().history(kA, Kind::Intercepts)->size(), 2u);  // root and the refit of c2
  EXPECT_EQ(world_->count("changeset"), 2);                      // the collection and c2: none for c1
  EXPECT_TRUE(store().provenance_for(changeset_id(kUrl, "c1"))->empty());
  auto row = world_->db->select_one(QStringLiteral("SELECT uuid FROM changeset WHERE uuid = ?"),
                                    {pd::qv(changeset_id(kUrl, "c1"))});
  ASSERT_TRUE(row);
  EXPECT_FALSE(row->has_value());
}

TEST_P(BatchWriterTest, StateNeedsAnOpenSource) {
  BatchWriter writer(store(), world_->client, config());
  EXPECT_FALSE(writer.state().head_blob_sha(SubjectRef{kA}, Kind::Intercepts));
  auto exists = writer.state().analysis_exists(kA);
  ASSERT_TRUE(exists);
  EXPECT_FALSE(*exists);
  EXPECT_FALSE(writer.state().analysis_origin(kA, "c1"));
}

// What a legacy catalog has beyond the natural keys reaches the store.
TEST_P(BatchWriterTest, CatalogItemsCarryTheirDescriptiveColumns) {
  ImportBatch b;
  b.catalog.push_back(PiItem{"Ross", "J", "NMT", std::nullopt});
  ProjectItem project{"Henry Hill", "Ross", "J"};
  project.checkin_date = "2016-02-29";
  project.comment = "two crates";
  project.lab_contact = "mheizler";
  project.institution = "NMT";
  b.catalog.push_back(project);
  const UtcTime made = *UtcTime::parse("2014-05-06T07:08:09Z");
  b.catalog.push_back(IrradiationItem{"NM-300", made});
  b.catalog.push_back(LevelItem{"NM-300", "A", std::nullopt, std::nullopt, std::nullopt});
  PositionItem position;
  position.irradiation = "NM-300";
  position.level = "A";
  position.position = 4;
  position.identifier = "66573";
  position.weight = 12.5;
  position.packet = "p4";
  position.note = "chipped";
  b.catalog.push_back(position);
  b.catalog.push_back(UserItem{"mheizler", "m@nmt.edu", "NMT", "staff"});
  LoadItem load;
  load.spec.name = "load-7";
  load.spec.archived = true;
  load.spec.created = made;
  load.created_by = "mheizler";
  b.catalog.push_back(load);
  b.catalog.push_back(LoadPositionItem{"load-7", 3, "66573", 1.5, 2, "big"});
  // A load position may come first: its load and identifier are made bare.
  b.catalog.push_back(LoadPositionItem{"load-8", 1, "66600", std::nullopt, std::nullopt, std::nullopt});
  b.resume_token = "c1";
  FakeAdapter adapter(description(), {b});
  ASSERT_TRUE(run_all(*world_, adapter));

  const auto one = [&](const char* sql) {
    auto row = world_->db->select_one(QString::fromUtf8(sql));
    EXPECT_TRUE(row && *row) << sql;
    return row && *row ? **row : pd::Row{};
  };
  auto r = one("SELECT checkin_date, comment, lab_contact, institution FROM project");
  EXPECT_EQ(pd::to_std(r.value("checkin_date")).substr(0, 10), "2016-02-29");
  EXPECT_EQ(pd::to_std(r.value("comment")), "two crates");
  EXPECT_EQ(pd::to_std(r.value("lab_contact")), "mheizler");
  EXPECT_EQ(pd::to_std(r.value("institution")), "NMT");
  EXPECT_EQ(pd::to_time(one("SELECT created_utc FROM irradiation").value("created_utc")), made);
  r = one("SELECT weight, packet, note FROM irradiation_position");
  EXPECT_DOUBLE_EQ(r.value("weight").toDouble(), 12.5);
  EXPECT_EQ(pd::to_std(r.value("packet")), "p4");
  EXPECT_EQ(pd::to_std(r.value("note")), "chipped");
  r = one("SELECT email, affiliation, category FROM app_user WHERE name = 'mheizler'");
  EXPECT_EQ(pd::to_std(r.value("email")), "m@nmt.edu");
  EXPECT_EQ(pd::to_std(r.value("affiliation")), "NMT");
  EXPECT_EQ(pd::to_std(r.value("category")), "staff");
  r = one("SELECT l.archived AS archived, l.created_utc AS created_utc, u.name AS creator FROM load l "
          "JOIN app_user u ON u.uuid = l.created_by_user_uuid WHERE l.name = 'load-7'");
  EXPECT_TRUE(r.value("archived").toBool());
  EXPECT_EQ(pd::to_time(r.value("created_utc")), made);
  EXPECT_EQ(pd::to_std(r.value("creator")), "mheizler");
  r = one("SELECT p.position AS position, p.weight AS weight, p.nxtals AS nxtals, p.note AS note, "
          "i.identifier AS identifier FROM load_position p JOIN load l ON l.uuid = p.load_uuid "
          "JOIN identifier i ON i.uuid = p.identifier_uuid WHERE l.name = 'load-7'");
  EXPECT_EQ(r.value("position").toInt(), 3);
  EXPECT_DOUBLE_EQ(r.value("weight").toDouble(), 1.5);
  EXPECT_EQ(r.value("nxtals").toInt(), 2);
  EXPECT_EQ(pd::to_std(r.value("note")), "big");
  EXPECT_EQ(pd::to_std(r.value("identifier")), "66573");
  EXPECT_EQ(world_->count("load"), 2);
  EXPECT_EQ(world_->count("load_position"), 2);
  EXPECT_EQ(world_->count("identifier"), 2);

  // The batch again writes nothing.
  const auto users = world_->count("app_user");
  FakeAdapter again(description(), {b});
  ASSERT_TRUE(run_all(*world_, again));
  EXPECT_EQ(world_->count("load_position"), 2);
  EXPECT_EQ(world_->count("load"), 2);
  EXPECT_EQ(world_->count("app_user"), users);
}

INSTANTIATE_TEST_SUITE_P(Engines, BatchWriterTest, ::testing::ValuesIn(P::testing::engines()));
