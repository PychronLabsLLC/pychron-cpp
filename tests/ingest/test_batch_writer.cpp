// BatchWriter against a real store: scripted batches in, rows out.

#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "fake_adapter.hpp"
#include "forwarding_store.hpp"
#include <nlohmann/json.hpp>

#include "pychron/ingest/ids.hpp"
#include "pychron/ingest/verify.hpp"
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
  sample.fields.lon = -106.9;  // a location is one point: both halves or neither
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

// A later commit that renumbers `analysis`, whose record is the file of `file`.
ChangesetItem renumber(const std::string& commit, Uuid analysis, int file, const std::string& identifier,
                       int aliquot) {
  ChangesetItem c;
  c.commit = commit;
  c.who = who(kAlice, "2016-03-05T00:00:00Z");
  c.message = "<EDIT> RunID";
  RevisionItem revision{{commit, record_path(file), "rec-" + commit}, analysis, Kind::Identity,
                        P::IdentityValue{Uuid{}, aliquot, -1, "legacy record rewritten"}};
  revision.identifier = identifier;
  c.revisions.push_back(std::move(revision));
  return c;
}

// A is 66573-01 at c1. c5 renumbers it to 66599-02, an identifier the catalog
// lacks; c9, when `with_c9`, renumbers it to 66573-03.
std::vector<ImportBatch> renumbered_twice(bool with_c9 = true) {
  std::vector<ImportBatch> out(with_c9 ? 3 : 2);
  out[0].catalog = lab_catalog();
  add_analysis(out[0], kA, 1, "c1", who(kAlice, "2016-03-04T05:06:07Z"));
  out[0].resume_token = "c1";
  out[1].changesets.push_back(renumber("c5", kA, 1, "66599", 2));
  out[1].resume_token = "c5";
  if (with_c9) {
    out[2].changesets.push_back(renumber("c9", kA, 1, "66573", 3));
    out[2].resume_token = "c9";
  }
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i].done = static_cast<int>(i) + 1;
    out[i].total = static_cast<int>(out.size());
  }
  return out;
}

// The batches as one.
ImportBatch merged(const std::vector<ImportBatch>& batches) {
  ImportBatch all;
  for (const auto& b : batches) {
    all.catalog.insert(all.catalog.end(), b.catalog.begin(), b.catalog.end());
    all.blobs.insert(all.blobs.end(), b.blobs.begin(), b.blobs.end());
    all.analyses.insert(all.analyses.end(), b.analyses.begin(), b.analyses.end());
    all.changesets.insert(all.changesets.end(), b.changesets.begin(), b.changesets.end());
    all.resume_token = b.resume_token;
    all.done = b.done;
    all.total = b.total;
  }
  return all;
}

WriterConfig replay_config() {
  auto c = config();
  c.replay = true;
  return c;
}

// The rows a replay could change, in a fixed order.
std::vector<std::string> rows_of(World& w) {
  std::vector<std::string> out;
  const auto rows = [&](const char* label, const QString& sql, const std::vector<const char*>& columns) {
    auto found = w.db->select(sql);
    ASSERT_TRUE(found) << label;
    std::vector<std::string> lines;
    for (const auto& row : *found) {
      std::string line = label;
      for (const char* column : columns) line += " | " + pd::to_std(row.value(column));
      lines.push_back(std::move(line));
    }
    std::sort(lines.begin(), lines.end());
    out.insert(out.end(), lines.begin(), lines.end());
  };
  rows("analysis", QStringLiteral("SELECT uuid, runid_text FROM analysis"), {"uuid", "runid_text"});
  rows("revision", QStringLiteral("SELECT uuid, subject_uuid, kind, parent_uuid, changeset_uuid FROM revision"),
       {"uuid", "subject_uuid", "kind", "parent_uuid", "changeset_uuid"});
  rows("head", QStringLiteral("SELECT subject_uuid, kind, revision_uuid FROM head"),
       {"subject_uuid", "kind", "revision_uuid"});
  rows("changeset", QStringLiteral("SELECT uuid, message FROM changeset"), {"uuid", "message"});
  rows("provenance", QStringLiteral("SELECT entity_type, entity_uuid, path, commit_sha FROM import_provenance"),
       {"entity_type", "entity_uuid", "path", "commit_sha"});
  rows("conflict", QStringLiteral("SELECT uuid, path, conflict_kind, resolution, detail FROM import_conflict"),
       {"uuid", "path", "conflict_kind", "resolution", "detail"});
  return out;
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
  for (const auto& c : *all) {
    EXPECT_EQ(c.resolution, "superseded") << c.path;
    // Every revision of a refused analysis is written in order: none is late.
    EXPECT_EQ(c.kind, P::ConflictKind::UnknownAnalysis) << c.path;
    EXPECT_EQ(c.detail_json.find("late_revision_not_applied"), std::string::npos) << c.path;
  }

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

// Fix wave F10: "membership only" is the top-level flag the writer sets, read
// as JSON. The same words elsewhere in an analysis's detail (a key of the
// record the adapter kept, a flag that is false) do not make the analysis a
// mere member.
TEST_P(BatchWriterTest, MembershipOnlyIsAFlagNotAWord) {
  ImportBatch batch = single_batch();
  batch.analyses[0].detail_json = R"({"membership_only":false,"notes":{"membership_only":true}})";
  FakeAdapter adapter(description(), {batch});
  BatchWriter writer(store(), world_->client, config());
  ASSERT_TRUE(writer.run(adapter, std::nullopt, {}, {}));
  auto here = writer.state().analysis_origin(kA, "c1");
  ASSERT_TRUE(here) << err(here.error());
  ASSERT_TRUE(here->has_value());
  EXPECT_TRUE((*here)->from_this_source);
  EXPECT_TRUE((*here)->in_this_source);
  EXPECT_EQ((*here)->record_blob_sha, "rec-1");
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

namespace {

// What a meta repository brings for NM-300/A/1: the level and the position,
// both bare, under a flux object scoped to the position.
ImportBatch bare_position_batch() {
  ImportBatch b;
  b.catalog.push_back(IrradiationItem{"NM-300"});
  b.catalog.push_back(LevelItem{"NM-300", "A", std::nullopt, std::nullopt, std::nullopt});
  b.catalog.push_back(RefObjectItem{P::RefType::FluxPosition, "NM-300/A/1", "NM-300", "A", 1, std::nullopt});
  b.resume_token = "m1";
  return b;
}

// What a project repository's record implies for the same position.
ImportBatch sampled_position_batch() {
  PositionItem position;
  position.irradiation = "NM-300";
  position.level = "A";
  position.position = 1;
  position.identifier = "66573";
  position.sample = "HH-1";
  position.project = "Henry Hill";
  position.material = "sanidine";
  ImportBatch b;
  b.catalog.push_back(position);
  b.resume_token = "p1";
  return b;
}

// "<identifier> | <sample> | <project> | <material>" of every position, by position.
std::vector<std::string> placed(World& w) {
  auto rows = w.db->select(QStringLiteral(
      "SELECT i.identifier AS identifier, s.name AS sample, pr.name AS project, m.name AS material "
      "FROM irradiation_position p LEFT JOIN identifier i ON i.position_uuid = p.uuid "
      "LEFT JOIN sample s ON s.uuid = p.sample_uuid LEFT JOIN project pr ON pr.uuid = s.project_uuid "
      "LEFT JOIN material m ON m.uuid = s.material_uuid ORDER BY p.position"));
  EXPECT_TRUE(rows);
  std::vector<std::string> out;
  if (!rows) return out;
  for (const auto& r : *rows)
    out.push_back(pd::to_std(r.value("identifier")) + " | " + pd::to_std(r.value("sample")) + " | " +
                  pd::to_std(r.value("project")) + " | " + pd::to_std(r.value("material")));
  return out;
}

}  // namespace

// A position made bare by one source gets its sample from a later one.
TEST_P(BatchWriterTest, PositionMadeBareIsFilledByALaterSource) {
  FakeAdapter meta({P::ImportSourceKind::MetaRepo, "https://github.com/NMGRLData/MetaData", "main", "head-sha"},
                   {bare_position_batch()});
  ASSERT_TRUE(run_all(*world_, meta));
  EXPECT_EQ(placed(*world_), std::vector<std::string>{" |  |  | "});
  FakeAdapter project(description(), {sampled_position_batch()});
  ASSERT_TRUE(run_all(*world_, project));
  EXPECT_EQ(placed(*world_), std::vector<std::string>{"66573 | HH-1 | Henry Hill | sanidine"});
  EXPECT_EQ(world_->count("irradiation_position"), 1);

  // Again: nothing is left to fill.
  const auto seq = *store().latest_change_seq();
  FakeAdapter again(description(), {sampled_position_batch()});
  ASSERT_TRUE(run_all(*world_, again, replay_config()));
  auto entries = store().changes_since(seq, 100);
  ASSERT_TRUE(entries);
  for (const auto& entry : entries->entries) EXPECT_NE(entry.kind, "catalog");
}

// The same in one run, in two batches and in one: the writer's memory of a
// row it made bare does not keep a fuller item from the store.
TEST_P(BatchWriterTest, PositionMadeBareIsFilledWithinARun) {
  FakeAdapter two(description(), {bare_position_batch(), sampled_position_batch()});
  ASSERT_TRUE(run_all(*world_, two));
  EXPECT_EQ(placed(*world_), std::vector<std::string>{"66573 | HH-1 | Henry Hill | sanidine"});

  World one(GetParam());
  ASSERT_TRUE(one.store && one.db);
  FakeAdapter merged_run(description(), {merged({bare_position_batch(), sampled_position_batch()})});
  ASSERT_TRUE(run_all(one, merged_run));
  EXPECT_EQ(placed(one), std::vector<std::string>{"66573 | HH-1 | Henry Hill | sanidine"});
}

// The other order: the bare items change nothing.
TEST_P(BatchWriterTest, PositionWithItsSampleIsKeptByALaterBareItem) {
  FakeAdapter project(description(), {sampled_position_batch()});
  ASSERT_TRUE(run_all(*world_, project));
  const auto seq = *store().latest_change_seq();
  FakeAdapter meta({P::ImportSourceKind::MetaRepo, "https://github.com/NMGRLData/MetaData", "main", "head-sha"},
                   {bare_position_batch()});
  ASSERT_TRUE(run_all(*world_, meta));
  EXPECT_EQ(placed(*world_), std::vector<std::string>{"66573 | HH-1 | Henry Hill | sanidine"});
  EXPECT_EQ(world_->count("irradiation_position"), 1);
  auto row = world_->db->select_one(QStringLiteral(
      "SELECT count(*) AS n FROM change_entity WHERE op = 'update' AND change_seq > %1").arg(seq));
  ASSERT_TRUE(row && *row);
  EXPECT_EQ((*row)->value("n").toInt(), 0);
}

// A level a position made bare takes its holder, z and note from a later
// LevelItem; a second LevelItem with other values changes nothing.
TEST_P(BatchWriterTest, LevelMadeBareIsFilledByALaterLevelItem) {
  ImportBatch first = sampled_position_batch();
  ImportBatch second;
  second.catalog.push_back(LevelItem{"NM-300", "A", "24-hole", 0.5, "top"});
  second.catalog.push_back(LevelItem{"NM-300", "A", "48-hole", 9.0, "other"});
  second.resume_token = "p2";
  FakeAdapter adapter(description(), {first, second});
  ASSERT_TRUE(run_all(*world_, adapter));
  auto row = world_->db->select_one(QStringLiteral(
      "SELECT l.z AS z, l.note AS note, o.key AS holder FROM level l "
      "LEFT JOIN ref_object o ON o.uuid = l.holder_ref_uuid"));
  ASSERT_TRUE(row && *row);
  EXPECT_DOUBLE_EQ((*row)->value("z").toDouble(), 0.5);
  EXPECT_EQ(pd::to_std((*row)->value("note")), "top");
  EXPECT_EQ(pd::to_std((*row)->value("holder")), "24-hole");
  EXPECT_EQ(world_->count("level"), 1);
}

// Spec 10.42. The row exists and the store will not take the values an item
// brings for it: an identifier given a position another identifier holds, a
// spectrometer given a code another has. Bad data never stops an import: a
// warning conflict says what was not applied, and the batch goes on.
TEST_P(BatchWriterTest, FillThatCannotBeAppliedIsAConflictAndTheBatchGoesOn) {
  const auto at = [](int hole, const char* identifier) {
    PositionItem position;
    position.irradiation = "NM-300";
    position.level = "A";
    position.position = hole;
    position.identifier = identifier;
    return position;
  };
  ImportBatch b;
  b.catalog.push_back(at(1, "66573"));
  b.catalog.push_back(LoadPositionItem{"L-1", 1, "66574", std::nullopt, std::nullopt, std::nullopt});  // 66574, bare
  b.catalog.push_back(at(1, "66574"));  // the hole 66573 sits in
  b.catalog.push_back(MassSpecItem{{"jan", "argus", "j", std::nullopt}});
  b.catalog.push_back(MassSpecItem{{"obama", std::nullopt, std::nullopt, std::nullopt}});
  b.catalog.push_back(MassSpecItem{{"obama", "argus", "j", std::nullopt}});  // the code jan has
  b.catalog.push_back(ExtractDeviceItem{"Fusions CO2"});
  b.catalog.push_back(at(2, "66575"));
  b.resume_token = "t1";
  b.done = b.total = 1;

  FakeAdapter adapter(description(), {b});
  auto stats = run_all(*world_, adapter);
  ASSERT_TRUE(stats) << err(stats.error());
  EXPECT_TRUE(stats->finished);
  EXPECT_EQ(stats->conflicts, 2);
  EXPECT_EQ(world_->source().status, "finished");

  // The rows are as they were; what came after them was written.
  EXPECT_EQ(*store().identifier_at("NM-300", "A", 1), std::optional<std::string>{"66573"});
  EXPECT_EQ(*store().identifier_at("NM-300", "A", 2), std::optional<std::string>{"66575"});
  auto row = world_->db->select_one(QStringLiteral("SELECT position_uuid FROM identifier WHERE identifier = '66574'"));
  ASSERT_TRUE(row && *row);
  EXPECT_TRUE((*row)->value("position_uuid").isNull());
  row = world_->db->select_one(QStringLiteral("SELECT kind, code FROM mass_spectrometer WHERE name = 'obama'"));
  ASSERT_TRUE(row && *row);
  EXPECT_TRUE((*row)->value("kind").isNull());
  EXPECT_TRUE((*row)->value("code").isNull());
  EXPECT_EQ(world_->count("extract_device"), 1);

  auto conflicts = store().import_conflicts({std::nullopt, std::nullopt, std::nullopt});
  ASSERT_TRUE(conflicts);
  ASSERT_EQ(conflicts->size(), 2u);
  std::map<std::string, nlohmann::json> details;
  for (const auto& conflict : *conflicts) {
    EXPECT_EQ(conflict.kind, P::ConflictKind::IdentityClash) << conflict.path;
    EXPECT_EQ(conflict.resolution, "pending") << conflict.path;
    EXPECT_EQ(conflict.uuid, conflict_id(kUrl, "", conflict.path)) << conflict.path;
    EXPECT_TRUE(is_warning_conflict(conflict)) << conflict.path;
    details[conflict.path] = nlohmann::json::parse(conflict.detail_json);
  }
  ASSERT_TRUE(details.contains("catalog-fill/identifier/66574"));
  const auto& identifier = details.at("catalog-fill/identifier/66574");
  EXPECT_EQ(identifier.at("imported"), true);
  EXPECT_EQ(identifier.at("table"), "identifier");
  EXPECT_EQ(identifier.at("natural_key"), nlohmann::json::array({"66574"}));
  EXPECT_TRUE(identifier.at("reason").is_string());
  EXPECT_NE(identifier.at("reason").get<std::string>().find("identifier"), std::string::npos);
  ASSERT_TRUE(details.contains("catalog-fill/mass_spectrometer/obama"));
  const auto& spectrometer = details.at("catalog-fill/mass_spectrometer/obama");
  EXPECT_EQ(spectrometer.at("imported"), true);
  EXPECT_EQ(spectrometer.at("table"), "mass_spectrometer");
  EXPECT_EQ(spectrometer.at("natural_key"), nlohmann::json::array({"obama"}));

  // Again, from the start: the same two conflicts and nothing new.
  const auto before = world_->counts();
  const auto seq = *store().latest_change_seq();
  FakeAdapter again(description(), {b});
  auto replayed = run_all(*world_, again, replay_config());
  ASSERT_TRUE(replayed) << err(replayed.error());
  EXPECT_TRUE(replayed->finished);
  EXPECT_EQ(replayed->conflicts, 2);
  EXPECT_EQ(world_->counts(), before);
  EXPECT_EQ(*store().latest_change_seq(), seq);
}

// Only a constraint makes a refused fill. A fill that fails for another
// reason (here: a trigger that cannot run) stops the run, with no conflict.
TEST_P(BatchWriterTest, FillThatFailsForAnotherReasonStopsTheRun) {
  ImportBatch b;
  b.catalog.push_back(MassSpecItem{{"obama", std::nullopt, std::nullopt, std::nullopt}});
  b.resume_token = "t1";
  FakeAdapter bare(description(), {b});
  ASSERT_TRUE(run_all(*world_, bare));
  auto broken = P::testing::break_updates_of(*world_->db, "mass_spectrometer");
  ASSERT_TRUE(broken) << err(broken.error());

  b.catalog.push_back(MassSpecItem{{"obama", "argus", "o", std::nullopt}});
  FakeAdapter adapter(description(), {b});
  auto stats = run_all(*world_, adapter, replay_config());
  ASSERT_FALSE(stats);
  EXPECT_FALSE(P::is_refused_catalog_fill(stats.error()));
  EXPECT_EQ(world_->count("import_conflict"), 0);
  EXPECT_EQ(world_->source().status, "failed");
}

// A new row the store refuses is still an error: only a fill is passed over.
TEST_P(BatchWriterTest, InsertThatFailsStillStopsTheRun) {
  PositionItem first, second;
  first.irradiation = second.irradiation = "NM-300";
  first.level = second.level = "A";
  first.position = second.position = 1;
  first.identifier = "66573";
  second.identifier = "66574";  // a new identifier, in the hole 66573 sits in
  ImportBatch b;
  b.catalog.push_back(first);
  b.catalog.push_back(second);
  b.resume_token = "t1";
  FakeAdapter adapter(description(), {b});
  auto stats = run_all(*world_, adapter);
  ASSERT_FALSE(stats);
  EXPECT_FALSE(P::is_refused_catalog_fill(stats.error()));
  EXPECT_EQ(world_->count("import_conflict"), 0);
  EXPECT_EQ(world_->source().status, "failed");
}

// Spec 10.31. The first import refuses the renumber at c5 and stores the one
// at c9. Once the identifier exists, a replay could write c5: behind c9.
TEST_P(BatchWriterTest, ReplayDoesNotWriteAnIdentityBehindAStoredOne) {
  FakeAdapter adapter(description(), renumbered_twice());
  adapter.honour_token(true);
  auto first = run_all(*world_, adapter);
  ASSERT_TRUE(first) << err(first.error());
  EXPECT_EQ(first->revisions, 1);
  EXPECT_EQ(first->conflicts, 1);
  EXPECT_EQ((*store().load_analysis(kA))->summary.runid, "66573-03");
  const Uuid c5 = conflict_id(kUrl, "c5", record_path(1));
  const Uuid head = revision_id(kUrl, "c9", record_path(1));
  auto refused = store().import_conflict(c5);
  ASSERT_TRUE(refused && refused->has_value());
  EXPECT_EQ((*refused)->kind, P::ConflictKind::UnknownAnalysis);
  EXPECT_EQ(*store().head(kA, Kind::Identity), std::optional<Uuid>{head});

  ASSERT_TRUE(store().add_identifier(world_->client, {"66599", "unknown", {}, {}, {}, {}, {}}));
  const auto seq = *store().latest_change_seq();
  const auto revisions = world_->count("revision");

  auto replayed = run_all(*world_, adapter, replay_config());
  ASSERT_TRUE(replayed) << err(replayed.error());
  EXPECT_TRUE(replayed->finished);
  EXPECT_EQ(replayed->batches, 3);
  EXPECT_EQ(replayed->revisions, 1);  // c9, already stored
  EXPECT_EQ(replayed->conflicts, 1);

  EXPECT_EQ((*store().load_analysis(kA))->summary.runid, "66573-03");
  EXPECT_EQ(*store().head(kA, Kind::Identity), std::optional<Uuid>{head});
  auto history = store().history(kA, Kind::Identity);
  ASSERT_TRUE(history);
  ASSERT_EQ(history->size(), 1u);
  EXPECT_EQ(history->front().uuid, head);
  EXPECT_FALSE(*store().has_revision(revision_id(kUrl, "c5", record_path(1))));
  EXPECT_EQ(world_->count("revision"), revisions);
  // A conflict is not a change: nothing else was written.
  EXPECT_EQ(*store().latest_change_seq(), seq);

  // The conflict of c5 now says what became of it, and what it held.
  const Uuid source = source_id(P::ImportSourceKind::ProjectRepo, kUrl, "main");
  auto conflicts = store().import_conflicts({source, std::nullopt, std::nullopt});
  ASSERT_TRUE(conflicts);
  ASSERT_EQ(conflicts->size(), 1u);
  const auto& late = conflicts->front();
  EXPECT_EQ(late.uuid, c5);
  EXPECT_EQ(late.kind, P::ConflictKind::IdentityClash);
  EXPECT_EQ(late.resolution, "pending");
  EXPECT_EQ(late.entity, std::optional<Uuid>{kA});
  EXPECT_EQ(late.path, record_path(1));
  EXPECT_EQ(late.file_sha256, std::optional<Sha256Digest>{sha256(std::string_view{"rec-c5"})});
  for (const char* text : {"late_revision_not_applied", "\"late\"", "\"c5\"", "\"identity\"", "66599", "rec-c5",
                           "legacy record rewritten", "\"c9\""})
    EXPECT_NE(late.detail_json.find(text), std::string::npos) << text << " in " << late.detail_json;
  EXPECT_NE(late.detail_json.find(record_path(1)), std::string::npos) << late.detail_json;

  // Recorded, so neither a dry run nor a dry replay has anything left to write.
  for (const bool replay : {false, true}) {
    auto dry = config();
    dry.dry_run = true;
    dry.replay = replay;
    auto counted = run_all(*world_, adapter, dry);
    ASSERT_TRUE(counted) << err(counted.error());
    EXPECT_EQ(counted->would_write, 0) << replay;
  }

  // A second replay changes nothing and does not record the conflict twice.
  const auto settled = rows_of(*world_);
  auto again = run_all(*world_, adapter, replay_config());
  ASSERT_TRUE(again) << err(again.error());
  EXPECT_EQ(again->conflicts, 1);
  EXPECT_EQ(rows_of(*world_), settled);
  EXPECT_EQ(*store().latest_change_seq(), seq);
  EXPECT_EQ(world_->count("import_conflict"), 1);
}

// What a replay is for: with nothing stored after it, the refused renumber is written.
TEST_P(BatchWriterTest, ReplayWritesARefusedIdentityThatNothingFollows) {
  FakeAdapter adapter(description(), renumbered_twice(false));
  adapter.honour_token(true);
  ASSERT_TRUE(run_all(*world_, adapter));
  EXPECT_EQ((*store().load_analysis(kA))->summary.runid, "66573-01");
  ASSERT_TRUE(store().add_identifier(world_->client, {"66599", "unknown", {}, {}, {}, {}, {}}));

  auto replayed = run_all(*world_, adapter, replay_config());
  ASSERT_TRUE(replayed) << err(replayed.error());
  EXPECT_EQ(replayed->conflicts, 0);
  EXPECT_EQ((*store().load_analysis(kA))->summary.runid, "66599-02");
  EXPECT_EQ(*store().head(kA, Kind::Identity), std::optional<Uuid>{revision_id(kUrl, "c5", record_path(1))});
  auto conflict = store().import_conflict(conflict_id(kUrl, "c5", record_path(1)));
  ASSERT_TRUE(conflict && conflict->has_value());
  EXPECT_EQ((*conflict)->kind, P::ConflictKind::UnknownAnalysis);
  EXPECT_EQ((*conflict)->resolution, "superseded");

  const auto seq = *store().latest_change_seq();
  ASSERT_TRUE(run_all(*world_, adapter, replay_config()));
  EXPECT_EQ(*store().latest_change_seq(), seq);
}

// The rule is not about identities: a refit at c5 of an analysis that was not
// in the store then, with the analysis and a refit at c9 stored since.
TEST_P(BatchWriterTest, ReplayDoesNotWriteARevisionBehindAStoredOne) {
  std::vector<ImportBatch> script(3);
  script[0].catalog = lab_catalog();
  script[0].changesets.push_back(refit("c5", kA, 1, 55.5, who(kAlice, "2016-03-05T00:00:00Z")));
  add_analysis(script[1], kA, 1, "c6", who(kAlice, "2016-03-06T00:00:00Z"));
  script[2].changesets.push_back(refit("c9", kA, 1, 99.5, who(kAlice, "2016-03-09T00:00:00Z")));
  const char* tokens[] = {"c5", "c6", "c9"};
  for (std::size_t i = 0; i < script.size(); ++i) script[i].resume_token = tokens[i];
  FakeAdapter adapter(description(), script);
  adapter.honour_token(true);
  auto first = run_all(*world_, adapter);
  ASSERT_TRUE(first) << err(first.error());
  EXPECT_EQ(first->conflicts, 1);
  const Uuid head = revision_id(kUrl, "c9", kind_path("intercepts", 1));
  ASSERT_EQ(*store().head(kA, Kind::Intercepts), std::optional<Uuid>{head});
  // Someone set the first refusal aside. Restated, the conflict says something
  // else, and is pending again (spec 10.35).
  {
    auto uow = store().begin_import_batch(source_id(P::ImportSourceKind::ProjectRepo, kUrl, "main"), world_->client);
    ASSERT_TRUE(uow);
    ASSERT_TRUE((*uow)->resolve_conflict(conflict_id(kUrl, "c5", kind_path("intercepts", 1)), "ignored"));
    ASSERT_TRUE((*uow)->commit());
  }
  const auto seq = *store().latest_change_seq();
  const auto revisions = world_->count("revision");

  auto replayed = run_all(*world_, adapter, replay_config());
  ASSERT_TRUE(replayed) << err(replayed.error());
  EXPECT_EQ(replayed->conflicts, 1);
  EXPECT_EQ(*store().head(kA, Kind::Intercepts), std::optional<Uuid>{head});
  EXPECT_EQ(store().history(kA, Kind::Intercepts)->size(), 2u);  // the root and c9
  EXPECT_EQ(world_->count("revision"), revisions);
  EXPECT_EQ(*store().latest_change_seq(), seq);
  const auto value = std::get<P::Intercepts>(**store().load_payload(**store().head(kA, Kind::Intercepts)));
  EXPECT_EQ(value.front().value, std::optional<double>{99.5});

  auto late = store().import_conflict(conflict_id(kUrl, "c5", kind_path("intercepts", 1)));
  ASSERT_TRUE(late && late->has_value());
  EXPECT_EQ((*late)->kind, P::ConflictKind::IdentityClash);
  EXPECT_EQ((*late)->resolution, "pending");
  EXPECT_EQ((*late)->entity, std::optional<Uuid>{kA});
  for (const char* text : {"late_revision_not_applied", "\"c5\"", "\"intercepts\"", "55.5", "Ar40", "parabolic"})
    EXPECT_NE((*late)->detail_json.find(text), std::string::npos) << text << " in " << (*late)->detail_json;
  EXPECT_EQ(world_->count("import_conflict"), 1);

  const auto settled = rows_of(*world_);
  ASSERT_TRUE(run_all(*world_, adapter, replay_config()));
  EXPECT_EQ(rows_of(*world_), settled);
}

// A stored revision from a commit the walk no longer has means the history
// was rewritten under the import. A revision kept back for that reason is not
// a warning: verify must not say ok (fix wave A1).
TEST_P(BatchWriterTest, RevisionBehindACommitTheWalkLostIsBlocking) {
  ImportBatch first = single_batch();  // A at c1, refit at c2
  FakeAdapter imported(description(), {first});
  ASSERT_TRUE(run_all(*world_, imported));
  const Uuid head = revision_id(kUrl, "c2", kind_path("intercepts", 1));
  ASSERT_EQ(*store().head(kA, Kind::Intercepts), std::optional<Uuid>{head});

  // The source now holds c1 and c3: c2 is gone.
  ImportBatch second;
  second.catalog = lab_catalog();
  add_analysis(second, kA, 1, "c1", who(kAlice, "2016-03-04T05:06:07Z"));
  second.changesets.push_back(refit("c3", kA, 1, 77.5, who(kAlice, "2016-03-06T00:00:00Z")));
  second.resume_token = "c3";
  second.done = second.total = 2;
  FakeAdapter rewritten(description(), {second});
  rewritten.walk({"c1", "c3"});
  auto stats = run_all(*world_, rewritten);
  ASSERT_TRUE(stats) << err(stats.error());
  EXPECT_EQ(stats->conflicts, 1);
  EXPECT_EQ(*store().head(kA, Kind::Intercepts), std::optional<Uuid>{head});

  auto kept = store().import_conflict(conflict_id(kUrl, "c3", kind_path("intercepts", 1)));
  ASSERT_TRUE(kept && kept->has_value());
  EXPECT_EQ((*kept)->kind, P::ConflictKind::IdentityClash);
  EXPECT_EQ((*kept)->resolution, "pending");
  const auto detail = nlohmann::json::parse((*kept)->detail_json);
  EXPECT_EQ(detail.at("reason"), "late_revision_not_applied");
  EXPECT_EQ(detail.at("cause"), "stored_commit_unknown");
  EXPECT_EQ(detail.at("behind"), "c2");
  EXPECT_FALSE(detail.contains("late")) << (*kept)->detail_json;
  EXPECT_FALSE(is_warning_conflict(**kept));
}

// Spec 10.16: the replay decides the same however it is cut, and when it is
// stopped after the batch that holds c5 and started again.
TEST_P(BatchWriterTest, LateRevisionIsDecidedTheSameAtAnyCutOfTheReplay) {
  const auto imported = [&](World& w) {
    FakeAdapter adapter(description(), renumbered_twice());
    adapter.honour_token(true);
    EXPECT_TRUE(run_all(w, adapter));
    EXPECT_TRUE(w.store->add_identifier(w.client, {"66599", "unknown", {}, {}, {}, {}, {}}));
  };
  // Three batches, one commit each.
  imported(*world_);
  FakeAdapter by_commit(description(), renumbered_twice());
  ASSERT_TRUE(run_all(*world_, by_commit, replay_config()));
  const auto expected = rows_of(*world_);
  EXPECT_EQ((*store().load_analysis(kA))->summary.runid, "66573-03");

  // One batch.
  World whole(GetParam());
  ASSERT_TRUE(whole.store && whole.db);
  imported(whole);
  FakeAdapter at_once(description(), {merged(renumbered_twice())});
  ASSERT_TRUE(run_all(whole, at_once, replay_config()));
  EXPECT_EQ(rows_of(whole), expected);

  // Stopped after c5, then replayed again from the start.
  World stopped(GetParam());
  ASSERT_TRUE(stopped.store && stopped.db);
  imported(stopped);
  FakeAdapter interrupted(description(), renumbered_twice());
  {
    BatchWriter writer(*stopped.store, stopped.client, replay_config());
    auto partial = writer.run(interrupted, 2, {}, {});
    ASSERT_TRUE(partial) << err(partial.error());
    EXPECT_EQ(partial->batches, 2);
    EXPECT_FALSE(partial->finished);
  }
  EXPECT_EQ((*stopped.store->load_analysis(kA))->summary.runid, "66573-03");
  ASSERT_TRUE(run_all(stopped, interrupted, replay_config()));
  EXPECT_EQ(rows_of(stopped), expected);
}

// Spec 10.34, the pull-merge shape. Main is c1 c2 c3 c4 and is imported. A
// line b1 b2 forked at c2 is merged in, and the walk becomes
// c1 c2 b1 b2 c3 c4 m: the token no longer fits, the adapter walks from the
// first commit, and this is a plain run. b1 and b2 refit what c3 and c4 refit.
TEST_P(BatchWriterTest, CommitsMergedInEarlierInTheWalkAreNotWrittenBehind) {
  const auto main_line = [] {
    std::vector<ImportBatch> out(4);
    out[0].catalog = lab_catalog();
    add_analysis(out[0], kA, 1, "c1", who(kAlice, "2016-03-04T05:06:07Z"));
    add_analysis(out[1], kB, 2, "c2", who(kAlice, "2016-03-05T00:00:00Z"));
    out[2].changesets.push_back(refit("c3", kA, 1, 103.0, who(kAlice, "2016-03-08T00:00:00Z")));
    out[3].changesets.push_back(refit("c4", kA, 1, 104.0, who(kAlice, "2016-03-09T00:00:00Z")));
    const char* tokens[] = {"c1", "c2", "c3", "c4"};
    for (std::size_t i = 0; i < out.size(); ++i) out[i].resume_token = tokens[i];
    return out;
  };
  FakeAdapter before(description(), main_line());
  ASSERT_TRUE(run_all(*world_, before));
  const Uuid head = revision_id(kUrl, "c4", kind_path("intercepts", 1));
  ASSERT_EQ(*store().head(kA, Kind::Intercepts), std::optional<Uuid>{head});
  const auto seq = *store().latest_change_seq();
  const auto revisions = world_->count("revision");
  const auto changesets = world_->count("changeset");

  auto merged_in = main_line();
  ImportBatch b1, b2, m;
  b1.changesets.push_back(refit("b1", kA, 1, 201.0, who(kAlice, "2016-03-06T00:00:00Z")));
  b1.resume_token = "b1";
  b2.changesets.push_back(refit("b2", kA, 1, 202.0, who(kAlice, "2016-03-07T00:00:00Z")));
  b2.resume_token = "b2";
  m.resume_token = "m";  // the merge keeps c4's file: it changes nothing
  merged_in.insert(merged_in.begin() + 2, {b1, b2});
  merged_in.push_back(m);
  FakeAdapter after(description(), merged_in);  // rewinds: the token does not fit the new order
  auto stats = run_all(*world_, after);
  ASSERT_TRUE(stats) << err(stats.error());
  EXPECT_TRUE(stats->finished);
  EXPECT_EQ(stats->conflicts, 2);

  EXPECT_EQ(*store().head(kA, Kind::Intercepts), std::optional<Uuid>{head});
  const auto value = std::get<P::Intercepts>(**store().load_payload(head));
  EXPECT_EQ(value.front().value, std::optional<double>{104.0});
  EXPECT_EQ(store().history(kA, Kind::Intercepts)->size(), 3u);  // the root, c3, c4
  EXPECT_EQ(world_->count("revision"), revisions);
  EXPECT_EQ(world_->count("changeset"), changesets);
  EXPECT_EQ(*store().latest_change_seq(), seq);
  EXPECT_EQ(world_->source().progress_token, std::optional<std::string>{"m"});
  for (const char* commit : {"b1", "b2"}) {
    auto late = store().import_conflict(conflict_id(kUrl, commit, kind_path("intercepts", 1)));
    ASSERT_TRUE(late && late->has_value()) << commit;
    EXPECT_EQ((*late)->kind, P::ConflictKind::IdentityClash);
    EXPECT_EQ((*late)->resolution, "pending");
    EXPECT_NE((*late)->detail_json.find("late_revision_not_applied"), std::string::npos);
    EXPECT_NE((*late)->detail_json.find(commit == std::string("b1") ? "201" : "202"), std::string::npos);
  }
  EXPECT_EQ(world_->count("import_conflict"), 2);

  // Again, and replayed: the same rows.
  const auto settled = rows_of(*world_);
  ASSERT_TRUE(run_all(*world_, after));
  EXPECT_EQ(rows_of(*world_), settled);
  ASSERT_TRUE(run_all(*world_, after, replay_config()));
  EXPECT_EQ(rows_of(*world_), settled);
}

// A is folded at the end of the first walk with the intercepts file of c9.
// History grows: c11 rewrites that file. A walk from the start now folds A
// with the c11 file and sends it as a revision too (the root is stored under
// the id of c9); an incremental run sends the revision alone. Either way c11
// is later than c9 and becomes the head.
TEST_P(BatchWriterTest, CollectionFoldedAgainFromALaterFileMovesTheHead) {
  const std::string path = kind_path("intercepts", 1);
  const auto script = [&](bool grown) {
    std::vector<ImportBatch> out(grown ? 2 : 1);
    out[0].catalog = lab_catalog();
    add_analysis(out[0], kA, 1, "c1", who(kAlice, "2016-03-04T05:06:07Z"));
    out[0].analyses[0].keys.intercepts = grown ? SourceKey{"c11", path, "int-c11"} : SourceKey{"c9", path, "int-c9"};
    out[0].resume_token = "c9";
    if (grown) {
      out[1].changesets.push_back(refit("c11", kA, 1, 111.0, who(kAlice, "2016-03-11T00:00:00Z")));
      out[1].resume_token = "c11";
    }
    return out;
  };
  const Uuid c11 = revision_id(kUrl, "c11", path);
  for (const bool replay : {true, false}) {
    World w(GetParam());
    ASSERT_TRUE(w.store && w.db);
    FakeAdapter first(description(), script(false));
    first.walk({"c1", "c9"});
    ASSERT_TRUE(run_all(w, first));
    ASSERT_EQ(*w.store->head(kA, Kind::Intercepts), std::optional<Uuid>{revision_id(kUrl, "c9", path)});

    FakeAdapter grown(description(), script(true));
    grown.walk({"c1", "c9", "c11"});
    grown.honour_token(true);
    auto stats = run_all(w, grown, replay ? replay_config() : config());
    ASSERT_TRUE(stats) << err(stats.error());
    EXPECT_EQ(stats->batches, replay ? 2 : 1);
    EXPECT_EQ(stats->conflicts, 0) << replay;
    EXPECT_EQ(*w.store->head(kA, Kind::Intercepts), std::optional<Uuid>{c11}) << replay;
    const auto value = std::get<P::Intercepts>(**w.store->load_payload(c11));
    EXPECT_EQ(value.front().value, std::optional<double>{111.0});
    EXPECT_EQ(w.count("import_conflict"), 0) << replay;
  }
}

// A renumber refused at c5; then someone renumbers the analysis in the
// application. The identifier arrives and a replay could write c5: over a
// head this source did not make.
TEST_P(BatchWriterTest, RecoveredRevisionDoesNotReplaceAHeadMadeOutsideTheSource) {
  FakeAdapter adapter(description(), renumbered_twice(false));
  adapter.honour_token(true);
  ASSERT_TRUE(run_all(*world_, adapter));
  const Uuid user = *store().ensure_user(world_->client, "jross");
  const Uuid viewer = *store().register_client({"desk-1", "reduction", std::nullopt, "test"});
  Uuid by_hand;
  {
    auto uow = store().begin({user, viewer});
    ASSERT_TRUE(uow);
    auto added = (*uow)->add_revision(
        kA, Kind::Identity, P::IdentityValue{**store().find_identifier("66573"), 9, -1, "admin_repair"}, std::nullopt);
    ASSERT_TRUE(added) << err(added.error());
    by_hand = *added;
    ASSERT_TRUE((*uow)->commit(P::ChangesetKind::Admin, "renumber"));
  }
  ASSERT_EQ((*store().load_analysis(kA))->summary.runid, "66573-09");
  ASSERT_TRUE(store().add_identifier(world_->client, {"66599", "unknown", {}, {}, {}, {}, {}}));
  const auto seq = *store().latest_change_seq();

  auto replayed = run_all(*world_, adapter, replay_config());
  ASSERT_TRUE(replayed) << err(replayed.error());
  EXPECT_EQ(replayed->conflicts, 1);
  EXPECT_EQ((*store().load_analysis(kA))->summary.runid, "66573-09");
  EXPECT_EQ(*store().head(kA, Kind::Identity), std::optional<Uuid>{by_hand});
  EXPECT_EQ(*store().latest_change_seq(), seq);
  auto late = store().import_conflict(conflict_id(kUrl, "c5", record_path(1)));
  ASSERT_TRUE(late && late->has_value());
  EXPECT_EQ((*late)->kind, P::ConflictKind::IdentityClash);
  EXPECT_EQ((*late)->resolution, "pending");
  for (const char* text : {"late_revision_not_applied", "head_not_of_this_source", "66599"})
    EXPECT_NE((*late)->detail_json.find(text), std::string::npos) << text << " in " << (*late)->detail_json;
}

// The same for a commit that is simply new: after a refit made in the
// application, the source's next refit of that analysis is kept, not applied.
TEST_P(BatchWriterTest, NewRevisionDoesNotReplaceAHeadMadeOutsideTheSource) {
  // The source has finished once (the first three batches are all it had
  // then); the fourth batch, which refits A at c5, is a later commit.
  auto batches = four_batches();
  {
    FakeAdapter at_first(description(), {batches[0], batches[1], batches[2]});
    auto first = run_all(*world_, at_first);
    ASSERT_TRUE(first) << err(first.error());
    ASSERT_TRUE(first->finished);
  }
  FakeAdapter adapter(description(), batches);
  adapter.honour_token(true);
  const Uuid user = *store().ensure_user(world_->client, "jross");
  const Uuid viewer = *store().register_client({"desk-1", "reduction", std::nullopt, "test"});
  auto uow = store().begin({user, viewer});
  ASSERT_TRUE(uow);
  auto by_hand = (*uow)->add_revision(kA, Kind::Intercepts, intercepts(77.0), *store().head(kA, Kind::Intercepts));
  ASSERT_TRUE(by_hand) << err(by_hand.error());
  ASSERT_TRUE((*uow)->commit(P::ChangesetKind::Reduction, "refit by hand"));

  auto stats = run_all(*world_, adapter);
  ASSERT_TRUE(stats) << err(stats.error());
  EXPECT_EQ(stats->batches, 1);
  EXPECT_EQ(stats->conflicts, 1);
  EXPECT_EQ(*store().head(kA, Kind::Intercepts), std::optional<Uuid>{*by_hand});
  auto late = store().import_conflict(conflict_id(kUrl, "c5", kind_path("intercepts", 1)));
  ASSERT_TRUE(late && late->has_value());
  EXPECT_NE((*late)->detail_json.find("head_not_of_this_source"), std::string::npos) << (*late)->detail_json;
}

// ---------------------------------------------------------------- superseded conflicts (spec 10.37)

namespace {

ConflictItem unreadable(const std::string& commit, int aliquot) {
  return {{commit, kind_path("intercepts", aliquot), "bad-" + commit},
          kA,
          P::ConflictKind::Unparseable,
          sha256(std::string_view{"garbage"}),
          R"({"reason":"not JSON"})"};
}

// A at c1; its intercepts unreadable at c2, readable at c3 (which says c2's
// conflict no longer applies), unreadable again at c4.
std::vector<ImportBatch> broken_then_mended(bool broken_again = true) {
  std::vector<ImportBatch> out(broken_again ? 4 : 3);
  out[0].catalog = lab_catalog();
  add_analysis(out[0], kA, 1, "c1", who(kAlice, "2016-03-04T05:06:07Z"));
  out[1].conflicts.push_back(unreadable("c2", 1));
  out[2].changesets.push_back(refit("c3", kA, 1, 101.5, who(kAlice, "2016-03-06T00:00:00Z")));
  out[2].superseded.push_back({"c2", kind_path("intercepts", 1), ""});
  if (broken_again) out[3].conflicts.push_back(unreadable("c4", 1));
  const char* tokens[] = {"c1", "c2", "c3", "c4"};
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i].resume_token = tokens[i];
    out[i].done = static_cast<int>(i) + 1;
    out[i].total = static_cast<int>(out.size());
  }
  return out;
}

// The batches [first, last] as one.
ImportBatch joined(const std::vector<ImportBatch>& batches, std::size_t first, std::size_t last) {
  ImportBatch all;
  for (std::size_t i = first; i <= last; ++i) {
    const auto& b = batches[i];
    all.catalog.insert(all.catalog.end(), b.catalog.begin(), b.catalog.end());
    all.blobs.insert(all.blobs.end(), b.blobs.begin(), b.blobs.end());
    all.analyses.insert(all.analyses.end(), b.analyses.begin(), b.analyses.end());
    all.changesets.insert(all.changesets.end(), b.changesets.begin(), b.changesets.end());
    all.conflicts.insert(all.conflicts.end(), b.conflicts.begin(), b.conflicts.end());
    all.superseded.insert(all.superseded.end(), b.superseded.begin(), b.superseded.end());
    all.resume_token = b.resume_token;
    all.done = b.done;
    all.total = b.total;
  }
  return all;
}

}  // namespace

TEST_P(BatchWriterTest, SupersededConflictIsResolvedAndTheLaterOneStaysPending) {
  FakeAdapter adapter(description(), broken_then_mended());
  adapter.honour_token(true);
  auto stats = run_all(*world_, adapter);
  ASSERT_TRUE(stats) << err(stats.error());
  // Pending only: c2's conflict was written and superseded in this run.
  EXPECT_EQ(stats->conflicts, 1);

  auto mended = store().import_conflict(conflict_id(kUrl, "c2", kind_path("intercepts", 1)));
  ASSERT_TRUE(mended && mended->has_value());
  EXPECT_EQ((*mended)->kind, P::ConflictKind::Unparseable);
  EXPECT_EQ((*mended)->resolution, "superseded");
  EXPECT_TRUE((*mended)->resolved.has_value());
  // A bad version after a good one is not superseded by the earlier good one.
  auto broken = store().import_conflict(conflict_id(kUrl, "c4", kind_path("intercepts", 1)));
  ASSERT_TRUE(broken && broken->has_value());
  EXPECT_EQ((*broken)->resolution, "pending");
  EXPECT_FALSE((*broken)->resolved.has_value());
  EXPECT_EQ(world_->count("import_conflict"), 2);

  // A replay sends both conflicts and the supersession again: nothing moves,
  // the superseded one is not reopened, and its time stays.
  const auto rows = rows_of(*world_);
  const auto seq = *store().latest_change_seq();
  for (int again = 0; again < 2; ++again) {
    auto replayed = run_all(*world_, adapter, replay_config());
    ASSERT_TRUE(replayed) << err(replayed.error());
    EXPECT_EQ(replayed->conflicts, 1);
    EXPECT_EQ(rows_of(*world_), rows);
    EXPECT_EQ(*store().latest_change_seq(), seq);
    EXPECT_EQ((*store().import_conflict((*mended)->uuid))->resolved, (*mended)->resolved);
  }
  // A dry run finds nothing to write, resuming or replaying.
  for (const bool replay : {false, true}) {
    auto dry = config();
    dry.dry_run = true;
    dry.replay = replay;
    auto counted = run_all(*world_, adapter, dry);
    ASSERT_TRUE(counted) << err(counted.error());
    EXPECT_EQ(counted->would_write, 0) << replay;
    EXPECT_EQ(counted->conflicts, replay ? 1 : 0) << replay;
  }
}

// However the walk is cut, stopped or resumed, the rows are the same: the bad
// version and the good one in one batch or in two.
TEST_P(BatchWriterTest, SupersededConflictIsTheSameAtEveryCut) {
  const auto script = broken_then_mended();
  FakeAdapter by_commit(description(), script);
  auto reference = run_all(*world_, by_commit);
  ASSERT_TRUE(reference) << err(reference.error());
  const auto want = rows_of(*world_);

  const std::vector<std::vector<ImportBatch>> cuts = {
      {joined(script, 0, 3)},
      {joined(script, 0, 0), joined(script, 1, 2), joined(script, 3, 3)},  // bad and good in one batch
      {joined(script, 0, 1), joined(script, 2, 3)},
      {joined(script, 0, 2), joined(script, 3, 3)},
  };
  for (std::size_t n = 0; n < cuts.size(); ++n) {
    World cut(GetParam());
    ASSERT_TRUE(cut.store && cut.db);
    FakeAdapter adapter(description(), cuts[n]);
    auto stats = run_all(cut, adapter);
    ASSERT_TRUE(stats) << n << ": " << err(stats.error());
    EXPECT_EQ(stats->conflicts, 1) << n;
    EXPECT_EQ(rows_of(cut), want) << "cut " << n;

    // Stopped after every batch, each run resuming from the stored token.
    World stopped(GetParam());
    ASSERT_TRUE(stopped.store && stopped.db);
    FakeAdapter resumed(description(), cuts[n]);
    resumed.honour_token(true);
    int pending = 0;
    for (std::size_t run = 0; run <= cuts[n].size(); ++run) {
      BatchWriter writer(*stopped.store, stopped.client, config());
      auto one = writer.run(resumed, 1, {}, {});
      ASSERT_TRUE(one) << n << ": " << err(one.error());
      pending += one->conflicts;
      EXPECT_GE(one->conflicts, 0) << n;  // never negative: a run does not take back another run's count
    }
    EXPECT_EQ(rows_of(stopped), want) << "cut " << n << ", resumed";
    EXPECT_GE(pending, 1) << n;
    FakeAdapter replay(description(), script);
    ASSERT_TRUE(run_all(stopped, replay, replay_config()));
    EXPECT_EQ(rows_of(stopped), want) << "cut " << n << ", resumed, replayed";
  }
}

TEST_P(BatchWriterTest, SupersededNamesOnlyUnreadableOrRefusedFilesOfThisSource) {
  auto script = broken_then_mended(false);
  // A late revision's conflict, a conflict someone set aside, and a key with
  // no conflict at all are named too: none of them is touched.
  script[1].conflicts.push_back({{"c2", "665/x.json", "blob"},
                                 kA,
                                 P::ConflictKind::IdentityClash,
                                 std::nullopt,
                                 R"({"reason":"late_revision_not_applied","late":true})"});
  script[1].conflicts.push_back({{"c2", "665/y.json", "blob"},
                                 std::nullopt,
                                 P::ConflictKind::Unparseable,
                                 std::nullopt,
                                 R"({"reason":"not JSON"})"});
  script[2].superseded.push_back({"c2", "665/x.json", ""});
  script[2].superseded.push_back({"c2", "665/y.json", ""});
  script[2].superseded.push_back({"c2", "665/never.json", ""});
  FakeAdapter adapter(description(), script);
  adapter.honour_token(true);
  {
    BatchWriter writer(*world_->store, world_->client, config());
    ASSERT_TRUE(writer.run(adapter, 2, {}, {}));
  }
  {
    auto uow = store().begin_import_batch(source_id(P::ImportSourceKind::ProjectRepo, kUrl, "main"), world_->client);
    ASSERT_TRUE(uow);
    ASSERT_TRUE((*uow)->resolve_conflict(conflict_id(kUrl, "c2", "665/y.json"), "ignored"));
    ASSERT_TRUE((*uow)->commit());
  }
  // What a run would do now: the revision of c3, its changeset's provenance
  // and one supersession.
  auto dry = config();
  dry.dry_run = true;
  auto counted = run_all(*world_, adapter, dry);
  ASSERT_TRUE(counted) << err(counted.error());
  EXPECT_EQ(counted->would_write, 3);

  auto stats = run_all(*world_, adapter);
  ASSERT_TRUE(stats) << err(stats.error());
  EXPECT_EQ(stats->conflicts, 0);  // the conflict it superseded was counted by the run that wrote it
  EXPECT_EQ((*store().import_conflict(conflict_id(kUrl, "c2", kind_path("intercepts", 1))))->resolution, "superseded");
  EXPECT_EQ((*store().import_conflict(conflict_id(kUrl, "c2", "665/x.json")))->resolution, "pending");
  EXPECT_EQ((*store().import_conflict(conflict_id(kUrl, "c2", "665/y.json")))->resolution, "ignored");
  EXPECT_FALSE(store().import_conflict(conflict_id(kUrl, "c2", "665/never.json"))->has_value());
}

// Fix wave F2. An uninterrupted first import does not ask whose head it
// writes over (spec 10.34: a source with nothing stored needs no check). One
// that was stopped and resumed must end the same: until the source has
// finished once, a head made by a user or by another source does not keep a
// revision back.
TEST_P(BatchWriterTest, InterruptedFirstImportOverwritesAHeadMadeElsewhereAsAnUninterruptedOneDoes) {
  const std::string path = "NM-300/productions/Triga.json";
  const auto production = [&](const std::string& commit, const char* note) {
    ChangesetItem c;
    c.commit = commit;
    c.kind = P::ChangesetKind::Reference;
    c.who = who(kAlice, "2016-03-05T00:00:00Z");
    c.message = "production " + commit;
    P::ProductionValue value;
    value.note = note;
    c.revisions.push_back({{commit, path, "blob-" + commit},
                           RefObjectKey{"production", "NM-300/Triga"},
                           Kind::RefValue,
                           P::RefPayload{std::move(value)}});
    return c;
  };
  const auto script = [&](std::size_t batches) {
    std::vector<ImportBatch> out(batches);
    out[0].catalog = lab_catalog();
    add_analysis(out[0], kA, 1, "c1", who(kAlice, "2016-03-04T05:06:07Z"));
    out[1].changesets.push_back(production("c2", "from the source"));
    if (batches > 2) out[2].changesets.push_back(production("c3", "from the source, later"));
    const char* tokens[] = {"c1", "c2", "c3"};
    for (std::size_t i = 0; i < out.size(); ++i) {
      out[i].resume_token = tokens[i];
      out[i].done = static_cast<int>(i) + 1;
      out[i].total = static_cast<int>(batches);
    }
    return out;
  };
  // The production exists before the import, with a value someone gave it.
  const auto by_hand = [&](World& w, Uuid object, const char* note) {
    const Uuid user = *w.store->ensure_user(w.client, "jross");
    auto uow = w.store->begin({user, w.client});
    ASSERT_TRUE(uow);
    P::ProductionValue value;
    value.note = note;
    auto head = w.store->head(object, Kind::RefValue);
    ASSERT_TRUE(head);
    ASSERT_TRUE((*uow)->add_revision(object, Kind::RefValue, P::RefPayload{std::move(value)}, *head));
    ASSERT_TRUE((*uow)->commit(P::ChangesetKind::Reference, "by hand"));
  };
  for (const bool interrupted : {false, true}) {
    World w(GetParam());
    ASSERT_TRUE(w.store && w.db);
    const Uuid object =
        *w.store->add_ref_object(w.client, {P::RefType::Production, "NM-300/Triga", {}, {}, {}, {}, {}});
    by_hand(w, object, "by hand");
    FakeAdapter adapter(description(), script(2));
    adapter.honour_token(true);
    if (interrupted) {
      BatchWriter writer(*w.store, w.client, config());
      auto first = writer.run(adapter, 1, {}, {});
      ASSERT_TRUE(first) << err(first.error());
      EXPECT_FALSE(first->finished);
    }
    auto stats = run_all(w, adapter);
    ASSERT_TRUE(stats) << interrupted << ": " << err(stats.error());
    EXPECT_TRUE(stats->finished);
    EXPECT_EQ(stats->conflicts, 0) << interrupted;
    EXPECT_EQ(w.count("import_conflict"), 0) << interrupted;
    EXPECT_EQ(*w.store->head(object, Kind::RefValue), std::optional<Uuid>{revision_id(kUrl, "c2", path)})
        << interrupted;

    // The source has finished once: from now on a head it did not make stays.
    by_hand(w, object, "by hand again");
    const Uuid kept = **w.store->head(object, Kind::RefValue);
    FakeAdapter later(description(), script(3));
    later.honour_token(true);
    auto more = run_all(w, later);
    ASSERT_TRUE(more) << err(more.error());
    EXPECT_EQ(more->conflicts, 1) << interrupted;
    EXPECT_EQ(*w.store->head(object, Kind::RefValue), std::optional<Uuid>{kept}) << interrupted;
    auto late = w.store->import_conflict(conflict_id(kUrl, "c3", path));
    ASSERT_TRUE(late && late->has_value());
    EXPECT_NE((*late)->detail_json.find("head_not_of_this_source"), std::string::npos) << (*late)->detail_json;
  }
}

// The position check reads history. A source with nothing stored has nothing
// to be behind, and its first import asks for none.
TEST_P(BatchWriterTest, FirstImportAsksForNoHistory) {
  ForwardingStore counted(store());
  FakeAdapter adapter(description(), four_batches());
  BatchWriter writer(counted, world_->client, config());
  auto stats = writer.run(adapter, std::nullopt, {}, {});
  ASSERT_TRUE(stats) << err(stats.error());
  EXPECT_EQ(stats->revisions, 3);
  EXPECT_EQ(counted.history_calls(), 0);
  EXPECT_EQ(counted.provenance_calls(), 0);

  // A later run of the same source does check: here a new refit of A.
  auto more = four_batches();
  more.push_back({});
  more.back().changesets.push_back(refit("c6", kA, 1, 106.0, who(kAlice, "2016-03-09T00:00:00Z")));
  more.back().resume_token = "c6";
  FakeAdapter grown(description(), more);
  grown.honour_token(true);
  BatchWriter again(counted, world_->client, config());
  stats = again.run(grown, std::nullopt, {}, {});
  ASSERT_TRUE(stats) << err(stats.error());
  EXPECT_EQ(stats->revisions, 1);
  EXPECT_EQ(stats->conflicts, 0);
  EXPECT_EQ(counted.history_calls(), 1);
  EXPECT_EQ(*store().head(kA, Kind::Intercepts), std::optional<Uuid>{revision_id(kUrl, "c6", kind_path("intercepts", 1))});
}

INSTANTIATE_TEST_SUITE_P(Engines, BatchWriterTest, ::testing::ValuesIn(P::testing::engines()));
