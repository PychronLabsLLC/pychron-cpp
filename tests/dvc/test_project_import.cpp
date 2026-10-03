// ProjectRepoAdapter end to end: a legacy-shaped git repository, walked by the
// adapter, written by the BatchWriter, read back from the store.

#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "fixture_files.hpp"
#include "git_fixture.hpp"
#include "legacy_repo_builder.hpp"
#include "pychron/core/sha256.hpp"
#include "pychron/dvc/project_adapter.hpp"
#include "pychron/ingest/ids.hpp"
#include "pychron/ingest/writer.hpp"
#include "store_fixture.hpp"

using namespace pychron;
using namespace pychron::dvc;
using namespace pychron::dvc::testing;
namespace P = pychron::persistence;
namespace pd = pychron::persistence::detail;
using ingest::RunStats;
using nlohmann::json;
using P::ConflictKind;
using P::Kind;
using P::Uuid;
using P::UtcTime;

namespace {

// The normalized form of the url the adapter is given.
const std::string kUrl = "https://github.com/NMGRLData/IR1010";
const std::string kRunE = "66052-01E";
const Uuid kE = *Uuid::parse(LegacyRepoBuilder::kFixtureUuid);
const Uuid kF = *Uuid::parse("22222222-2222-4222-8222-222222222222");
const Uuid kG = *Uuid::parse("33333333-3333-4333-8333-333333333333");

// The fixture's own collection time, and later ones.
const char* const kCollected = "2018-02-20T00:27:10-07:00";
const char* const kCollectedUtc = "2018-02-20T07:27:10Z";
const char* const kDay2 = "2018-02-21T10:00:00-07:00";
const char* const kRefit = "2018-06-05T14:57:22-06:00";
const char* const kRefitUtc = "2018-06-05T20:57:22Z";
const char* const kLater = "2018-06-07T14:20:47-06:00";

const Kind kSixKinds[] = {Kind::Signals, Kind::Intercepts, Kind::Baselines, Kind::Blanks, Kind::IcFactors, Kind::Tags};

std::string err(const Error& e) { return to_string(e); }

// A fresh database: the store under test and a white-box connection to it.
struct World {
  explicit World(const std::string& engine, bool with_catalog = true) : database(engine, true) {
    store = P::testing::open_or_die(database.url());
    if (!store) return;
    client = *store->register_client({"import-1", "importer", std::nullopt, "test"});
    auto opened = pd::Db::open(P::StoreConfig{database.url(), false});
    if (opened) db = std::move(*opened);
    if (!with_catalog) return;
    // What a catalog dump would have brought: the fixture's spectrometer
    // (lower case, as the legacy database names it), identifier and device.
    EXPECT_TRUE(store->add_mass_spectrometer(client, {"felix", std::nullopt, std::nullopt, std::nullopt}));
    P::IdentifierSpec identifier;
    identifier.identifier = "66052";
    EXPECT_TRUE(store->add_identifier(client, identifier));
    EXPECT_TRUE(store->add_extract_device(client, "Fusions Diode"));
  }

  long long count(const char* table) {
    auto row = db->select_one(QStringLiteral("SELECT count(*) AS n FROM %1").arg(QString::fromUtf8(table)));
    return row && *row ? (*row)->value("n").toLongLong() : -1;
  }

  std::map<std::string, long long> counts() {
    std::map<std::string, long long> out;
    for (const char* t : {"analysis", "signal_blob", "changeset", "revision", "head_move", "import_provenance",
                          "import_conflict", "repository_member", "bookmark", "interpreted_age"})
      out[t] = count(t);
    return out;
  }

  // Every revision of an analysis: id, parent, changeset, time, message.
  std::vector<std::string> revisions(Uuid analysis) {
    std::vector<std::string> out;
    for (const Kind kind : {Kind::Signals, Kind::Intercepts, Kind::Baselines, Kind::Blanks, Kind::IcFactors, Kind::Tags,
                            Kind::Cosmogenic}) {
      auto history = store->history(analysis, kind);
      if (!history) continue;
      for (const auto& r : *history)
        out.push_back(std::string(P::to_string(kind)) + " " + r.uuid.str() + " <- " +
                      (r.parent ? r.parent->str() : "root") + " in " + r.changeset.uuid.str() + " " +
                      std::string(P::to_string(r.changeset.kind)) + " " + r.changeset.created.iso() + " " +
                      r.changeset.message);
    }
    return out;
  }

  std::vector<P::ImportConflictRow> conflicts(std::optional<ConflictKind> kind = std::nullopt) {
    auto rows = store->import_conflicts({std::nullopt, kind, std::nullopt});
    EXPECT_TRUE(rows);
    return rows ? *rows : std::vector<P::ImportConflictRow>{};
  }

  P::ImportSourceInfo source(const std::string& url = kUrl) {
    auto all = store->import_sources();
    if (all)
      for (const auto& s : *all)
        if (s.spec.url_or_path == url) return s;
    ADD_FAILURE() << "no import source " << url;
    return {};
  }

  std::string analysis_detail(Uuid analysis) {
    auto rows = store->provenance_for(analysis);
    if (!rows || rows->empty()) return {};
    return rows->front().detail_json.value_or("");
  }

  // Declared first so it is destroyed last: the connections point into it.
  P::testing::TestDatabase database;
  std::unique_ptr<P::IStore> store;
  Uuid client;
  std::unique_ptr<pd::Db> db;
};

ProjectAdapterConfig adapter_config(GitFixture& repo, int batch_commits = 500) {
  ProjectAdapterConfig c;
  c.git.repo = repo.path();
  c.git.branch = "main";
  c.git.scratch = repo.temp("scratch");
  c.url = "https://GitHub.com/NMGRLData/IR1010.git";
  c.repository_name = "IR1010";
  c.lab_time_zone = "America/Denver";
  c.batch_commits = batch_commits;
  return c;
}

ingest::WriterConfig writer_config() {
  ingest::WriterConfig c;
  c.importer_version = "pychron-import/test";
  c.lab_time_zone = "America/Denver";
  return c;
}

// Opens the repository as it is now and imports it.
Result<RunStats> run_import(World& w, const ProjectAdapterConfig& config, std::optional<int> max_batches = std::nullopt,
                        ingest::WriterConfig writer = writer_config()) {
  auto adapter = ProjectRepoAdapter::open(config);
  if (!adapter) return fail(adapter.error());
  ingest::BatchWriter batches(*w.store, w.client, std::move(writer));
  return batches.run(**adapter, max_batches, {}, {});
}

double fixture_intercept(const std::string& isotope) {
  return json::parse(LegacyRepoBuilder::fixture_text(FileKind::Intercepts)).at(isotope).at("value").get<double>();
}

std::optional<double> head_intercept(World& w, Uuid analysis, const std::string& isotope) {
  auto view = w.store->load_analysis(analysis);
  if (!view || !*view) return std::nullopt;
  for (const auto& row : std::get<P::Intercepts>((*view)->payloads.at(Kind::Intercepts)))
    if (row.isotope == isotope) return row.value;
  return std::nullopt;
}

class ProjectImportTest : public ::testing::TestWithParam<std::string> {
 protected:
  void SetUp() override {
    if (!GitFixture::available()) GTEST_SKIP() << "git not on PATH";
    repo_.init();
    world_ = std::make_unique<World>(GetParam());
    ASSERT_TRUE(world_->store);
    ASSERT_TRUE(world_->db);
  }

  P::IStore& store() { return *world_->store; }
  std::unique_ptr<World> fresh_world(bool with_catalog = true) {
    return std::make_unique<World>(GetParam(), with_catalog);
  }

  // The repository first: the adapters read it.
  GitFixture repo_;
  LegacyRepoBuilder legacy_{repo_};
  std::unique_ptr<World> world_;
};

}  // namespace

TEST_P(ProjectImportTest, CollectionFoldsIntoOneChangeset) {
  const auto c = legacy_.collect(kRunE, kE.str(), kCollected);
  auto stats = run_import(*world_, adapter_config(repo_));
  ASSERT_TRUE(stats) << err(stats.error());
  EXPECT_TRUE(stats->finished);
  EXPECT_EQ(stats->analyses, 1);
  EXPECT_EQ(stats->changesets, 0);
  EXPECT_EQ(stats->conflicts, 0);

  // One analysis, with the legacy uuid.
  EXPECT_EQ(world_->count("analysis"), 1);
  auto view = store().load_analysis(kE);
  ASSERT_TRUE(view && view->has_value());
  EXPECT_EQ((*view)->summary.runid, kRunE);
  EXPECT_EQ((*view)->summary.mass_spectrometer, "felix");
  EXPECT_EQ((*view)->summary.signals_state, "complete");
  EXPECT_EQ((*view)->summary.timestamp.iso(), "2018-02-20T07:27:08.852603Z");

  // Six roots in one collection changeset, dated and authored by the <COLLECTION> commit.
  const Uuid changeset = ingest::collection_changeset_id(kUrl, c.collection, kE);
  for (const Kind kind : kSixKinds) {
    auto history = store().history(kE, kind);
    ASSERT_TRUE(history);
    ASSERT_EQ(history->size(), 1u) << P::to_string(kind);
    const auto& root = history->front();
    EXPECT_FALSE(root.parent.has_value());
    EXPECT_EQ(root.changeset.uuid, changeset);
    EXPECT_EQ(root.changeset.kind, P::ChangesetKind::Collection);
    EXPECT_EQ(root.changeset.created, *UtcTime::parse(kCollectedUtc));
    EXPECT_EQ(root.author_name, "git:ann@example.org");
  }
  // Each root is keyed by the commit that added its file.
  EXPECT_EQ(store().history(kE, Kind::Signals)->front().uuid,
            ingest::revision_id(kUrl, c.collection, LegacyRepoBuilder::path(kRunE, FileKind::Data)));
  EXPECT_EQ(store().history(kE, Kind::Intercepts)->front().uuid,
            ingest::revision_id(kUrl, c.isoevo, LegacyRepoBuilder::path(kRunE, FileKind::Intercepts)));
  EXPECT_EQ(store().history(kE, Kind::Blanks)->front().uuid,
            ingest::revision_id(kUrl, c.blanks, LegacyRepoBuilder::path(kRunE, FileKind::Blanks)));
  EXPECT_EQ(store().history(kE, Kind::IcFactors)->front().uuid,
            ingest::revision_id(kUrl, c.icfactors, LegacyRepoBuilder::path(kRunE, FileKind::IcFactors)));
  // No tags file: the default tag, rooted on the record.
  EXPECT_EQ(std::get<P::TagValue>((*view)->payloads.at(Kind::Tags)).name, "ok");

  // The payloads are the files'.
  EXPECT_EQ(head_intercept(*world_, kE, "Ar40"), std::optional<double>{fixture_intercept("Ar40")});
  EXPECT_EQ(std::get<P::SignalRefs>((*view)->payloads.at(Kind::Signals)).size(), 15u);
  EXPECT_EQ(std::get<P::Baselines>((*view)->payloads.at(Kind::Baselines)).size(), 5u);
  EXPECT_EQ(std::get<P::IcFactors>((*view)->payloads.at(Kind::IcFactors)).size(), 5u);

  // Provenance: the record, and every commit that contributed.
  auto rows = store().provenance_for(kE);
  ASSERT_TRUE(rows);
  ASSERT_EQ(rows->size(), 1u);
  EXPECT_EQ(rows->front().entity_type, "analysis");
  EXPECT_EQ(rows->front().commit_sha, c.collection);
  EXPECT_EQ(rows->front().path, LegacyRepoBuilder::path(kRunE, FileKind::Record));
  EXPECT_EQ(rows->front().git_author, "Ann <ann@example.org>");
  const std::string detail = rows->front().detail_json.value_or("");
  for (const std::string& sha : {c.collection, c.isoevo, c.blanks, c.icfactors})
    EXPECT_NE(detail.find(sha), std::string::npos) << detail;
  EXPECT_EQ(detail.find("synthetic_collection"), std::string::npos) << detail;
  const json parsed = json::parse(detail);
  EXPECT_EQ(parsed.at("collection_commits"), json::array({c.collection, c.isoevo, c.blanks, c.icfactors}));
  EXPECT_EQ(parsed.at("runid"), kRunE);
  // The raw data file's own keys have no column; they are kept here.
  EXPECT_TRUE(parsed.at("data_extra").contains("commit")) << detail;

  // Membership, the extraction file and the spectrometer file the record names.
  auto row = store().load_analysis_detail(kE);
  ASSERT_TRUE(row && row->has_value());
  EXPECT_EQ((*row)->row.repository, "IR1010");
  EXPECT_EQ((*row)->row.extract_device, "Fusions Diode");
  EXPECT_EQ((*row)->extraction.extract_value, std::optional<double>{4.0});
  EXPECT_EQ(world_->count("spectrometer_snapshot"), 1);
  auto snapshot = world_->db->select_one(QStringLiteral("SELECT legacy_sha1 FROM spectrometer_snapshot"));
  ASSERT_TRUE(snapshot && *snapshot);
  EXPECT_EQ(pd::to_std((*snapshot)->value("legacy_sha1")), LegacyRepoBuilder::kSpecSha);
  // The original spelling of the spectrometer is kept.
  auto meta = world_->db->select_one(QStringLiteral("SELECT legacy FROM analysis_meta"));
  ASSERT_TRUE(meta && *meta);
  EXPECT_NE(pd::to_std((*meta)->value("legacy")).find("Felix"), std::string::npos);

  const auto source = world_->source();
  EXPECT_EQ(source.status, "finished");
  EXPECT_EQ(source.progress_token, std::optional<std::string>{c.icfactors});
  EXPECT_EQ(source.head_sha, std::optional<std::string>{c.icfactors});
  EXPECT_EQ(source.done, 4);
  EXPECT_EQ(source.total, 4);
}

TEST_P(ProjectImportTest, ReferencesToOtherRepositoriesAreKeptButNotLinked) {
  // The fixture's blanks name blank analyses of a per-spectrometer repository.
  legacy_.collect(kRunE, kE.str(), kCollected);
  auto stats = run_import(*world_, adapter_config(repo_));
  ASSERT_TRUE(stats) << err(stats.error());
  EXPECT_EQ(stats->conflicts, 0);

  auto view = store().load_analysis(kE);
  ASSERT_TRUE(view && view->has_value());
  const auto& blanks = std::get<P::Blanks>((*view)->payloads.at(Kind::Blanks));
  const auto ar40 = std::find_if(blanks.begin(), blanks.end(), [](const auto& row) { return row.isotope == "Ar40"; });
  ASSERT_NE(ar40, blanks.end());
  ASSERT_EQ(ar40->references.size(), 7u);
  for (const auto& reference : ar40->references) EXPECT_FALSE(reference.ref_analysis.has_value());
  EXPECT_EQ(ar40->references[2].record_id, std::optional<std::string>{"bu-FD-F-789"});
  ASSERT_TRUE(ar40->extra_json.has_value());
  EXPECT_NE(ar40->extra_json->find("7834c4f8-f3b8-4aac-b586-53f7cb8d3d5c"), std::string::npos);  // verbatim
  const std::string detail = world_->analysis_detail(kE);
  EXPECT_NE(detail.find("unresolved_references"), std::string::npos) << detail;
  EXPECT_NE(detail.find("7834c4f8-f3b8-4aac-b586-53f7cb8d3d5c"), std::string::npos) << detail;
}

TEST_P(ProjectImportTest, RefitAddsRevision) {
  const auto c = legacy_.collect(kRunE, kE.str(), kCollected);
  const std::string refit = legacy_.refit(kRunE, "Ar40", 12.5, kRefit, "Bob <bob@example.org>");
  auto stats = run_import(*world_, adapter_config(repo_));
  ASSERT_TRUE(stats) << err(stats.error());
  EXPECT_EQ(stats->changesets, 1);
  EXPECT_EQ(stats->revisions, 1);
  EXPECT_EQ(stats->conflicts, 0);

  auto history = store().history(kE, Kind::Intercepts);
  ASSERT_TRUE(history);
  ASSERT_EQ(history->size(), 2u);
  const auto& root = (*history)[0];
  const auto& later = (*history)[1];
  EXPECT_EQ(later.uuid, ingest::revision_id(kUrl, refit, LegacyRepoBuilder::path(kRunE, FileKind::Intercepts)));
  EXPECT_EQ(later.parent, std::optional<Uuid>{root.uuid});
  EXPECT_EQ(later.changeset.uuid, ingest::changeset_id(kUrl, refit));
  EXPECT_EQ(later.changeset.kind, P::ChangesetKind::Import);
  EXPECT_EQ(later.changeset.message, "<ISOEVO> fits=Ar40(Parabolic)");
  EXPECT_EQ(later.changeset.created, *UtcTime::parse(kRefitUtc));
  EXPECT_EQ(later.author_name, "git:bob@example.org");
  EXPECT_EQ(head_intercept(*world_, kE, "Ar40"), std::optional<double>{12.5});
  EXPECT_EQ(head_intercept(*world_, kE, "Ar39"), std::optional<double>{fixture_intercept("Ar39")});
  for (const Kind kind : {Kind::Signals, Kind::Baselines, Kind::Blanks, Kind::IcFactors, Kind::Tags})
    EXPECT_EQ(store().history(kE, kind)->size(), 1u) << P::to_string(kind);
  EXPECT_EQ(world_->source().progress_token, std::optional<std::string>{refit});
}

TEST_P(ProjectImportTest, CollectionSplitAcrossBatches) {
  legacy_.collect(kRunE, kE.str(), kCollected);
  legacy_.refit(kRunE, "Ar40", 12.5, kRefit);
  auto whole = run_import(*world_, adapter_config(repo_, 500));
  ASSERT_TRUE(whole) << err(whole.error());
  EXPECT_EQ(whole->batches, 1);

  auto other = fresh_world();
  auto split = run_import(*other, adapter_config(repo_, 2));
  ASSERT_TRUE(split) << err(split.error());
  EXPECT_EQ(split->batches, 3);
  EXPECT_EQ(split->analyses, 1);
  EXPECT_EQ(other->revisions(kE), world_->revisions(kE));
  EXPECT_EQ(other->counts(), world_->counts());
  EXPECT_EQ(other->analysis_detail(kE), world_->analysis_detail(kE));
  EXPECT_EQ(other->source().progress_token, world_->source().progress_token);
}

TEST_P(ProjectImportTest, ResumeMidCollection) {
  legacy_.collect(kRunE, kE.str(), kCollected);
  legacy_.refit(kRunE, "Ar40", 12.5, kRefit);
  auto whole = run_import(*world_, adapter_config(repo_));
  ASSERT_TRUE(whole) << err(whole.error());

  // Stopped after the first two of the four collection commits: nothing is
  // written and the token has not moved past the pending analysis.
  auto other = fresh_world();
  auto first = run_import(*other, adapter_config(repo_, 2), 1);
  ASSERT_TRUE(first) << err(first.error());
  EXPECT_FALSE(first->finished);
  EXPECT_EQ(other->count("analysis"), 0);
  EXPECT_EQ(other->source().status, "paused");
  EXPECT_EQ(other->source().progress_token.value_or(""), "");

  auto second = run_import(*other, adapter_config(repo_, 2));
  ASSERT_TRUE(second) << err(second.error());
  EXPECT_TRUE(second->finished);
  EXPECT_EQ(other->revisions(kE), world_->revisions(kE));
  EXPECT_EQ(other->counts(), world_->counts());
}

TEST_P(ProjectImportTest, ResumeFromATokenBehindAPendingAnalysis) {
  const auto e = legacy_.collect(kRunE, kE.str(), kCollected);
  const auto f = legacy_.collect("66052-02A", kF.str(), kDay2);
  legacy_.refit(kRunE, "Ar40", 12.5, kRefit);
  legacy_.refit("66052-02A", "Ar39", 7.25, kLater);
  auto whole = run_import(*world_, adapter_config(repo_));
  ASSERT_TRUE(whole) << err(whole.error());

  // Five commits: E is complete, F has only its record. The token stays
  // before F's record.
  auto other = fresh_world();
  auto first = run_import(*other, adapter_config(repo_, 5), 1);
  ASSERT_TRUE(first) << err(first.error());
  EXPECT_EQ(other->count("analysis"), 1);
  EXPECT_EQ(other->source().progress_token, std::optional<std::string>{e.icfactors});

  // A different batch size on the way back makes no difference.
  auto second = run_import(*other, adapter_config(repo_, 3));
  ASSERT_TRUE(second) << err(second.error());
  EXPECT_TRUE(second->finished);
  EXPECT_EQ(other->revisions(kE), world_->revisions(kE));
  EXPECT_EQ(other->revisions(kF), world_->revisions(kF));
  EXPECT_EQ(other->counts(), world_->counts());
  EXPECT_EQ(store().history(kF, Kind::Signals)->front().changeset.uuid,
            ingest::collection_changeset_id(kUrl, f.collection, kF));
}

TEST_P(ProjectImportTest, SyntheticCollection) {
  // Every file in one commit that is not a <COLLECTION> commit.
  const std::string commit = legacy_.import_without_collection(kRunE, kE.str(), kCollected);
  auto stats = run_import(*world_, adapter_config(repo_));
  ASSERT_TRUE(stats) << err(stats.error());
  EXPECT_EQ(stats->analyses, 1);
  EXPECT_EQ(stats->conflicts, 0);

  const json detail = json::parse(world_->analysis_detail(kE));
  EXPECT_EQ(detail.at("synthetic_collection"), true);
  EXPECT_EQ(detail.at("collection_commits"), json::array({commit}));
  auto history = store().history(kE, Kind::Intercepts);
  ASSERT_EQ(history->size(), 1u);
  EXPECT_EQ(history->front().changeset.kind, P::ChangesetKind::Collection);
  EXPECT_EQ(history->front().changeset.created, *UtcTime::parse(kCollectedUtc));
  EXPECT_EQ(head_intercept(*world_, kE, "Ar40"), std::optional<double>{fixture_intercept("Ar40")});
}

TEST_P(ProjectImportTest, CollectionThatNeverCompletesIsFoldedAtTheEnd) {
  // No blanks and no IC factors, ever: folded with what there is.
  legacy_.import_without_collection(kRunE, kE.str(), kCollected, false);
  legacy_.collect("66052-02A", kF.str(), kDay2);
  auto stats = run_import(*world_, adapter_config(repo_));
  ASSERT_TRUE(stats) << err(stats.error());
  EXPECT_EQ(stats->analyses, 2);
  EXPECT_EQ(stats->conflicts, 0);

  EXPECT_EQ(json::parse(world_->analysis_detail(kE)).at("synthetic_collection"), true);
  EXPECT_FALSE(json::parse(world_->analysis_detail(kF)).contains("synthetic_collection"));
  auto view = store().load_analysis(kE);
  ASSERT_TRUE(view && view->has_value());
  EXPECT_EQ(std::get<P::Intercepts>((*view)->payloads.at(Kind::Intercepts)).size(), 5u);
  EXPECT_TRUE(std::get<P::Blanks>((*view)->payloads.at(Kind::Blanks)).empty());
  EXPECT_TRUE(std::get<P::IcFactors>((*view)->payloads.at(Kind::IcFactors)).empty());
  const std::string head = repo_.head();
  EXPECT_EQ(world_->source().progress_token, std::optional<std::string>{head});

  // Its blanks arrive in a later run: a revision on the empty root, not a second collection.
  legacy_.write(kRunE, FileKind::Blanks, LegacyRepoBuilder::fixture_text(FileKind::Blanks));
  const std::string added = legacy_.commit("<BLANKS> auto update blanks", kRefit);
  auto again = run_import(*world_, adapter_config(repo_));
  ASSERT_TRUE(again) << err(again.error());
  EXPECT_EQ(again->analyses, 0);
  EXPECT_EQ(again->revisions, 1);
  EXPECT_EQ(world_->count("analysis"), 2);
  auto blanks = store().history(kE, Kind::Blanks);
  ASSERT_EQ(blanks->size(), 2u);
  EXPECT_EQ((*blanks)[1].uuid, ingest::revision_id(kUrl, added, LegacyRepoBuilder::path(kRunE, FileKind::Blanks)));
  EXPECT_EQ((*blanks)[1].changeset.kind, P::ChangesetKind::Import);
  EXPECT_EQ(std::get<P::Blanks>(**store().load_payload((*blanks)[1].uuid)).size(), 5u);
}

TEST_P(ProjectImportTest, UnparseableFileIsConflictAndImportContinues) {
  legacy_.collect(kRunE, kE.str(), kCollected);
  const std::string garbage = "{\"Ar40\": {\"value\": 1.0,";
  legacy_.write(kRunE, FileKind::Intercepts, garbage);
  const std::string bad = legacy_.commit("<ISOEVO> fits=Ar40(Parabolic)", kDay2);
  legacy_.collect("66052-02A", kF.str(), kRefit);
  auto stats = run_import(*world_, adapter_config(repo_));
  ASSERT_TRUE(stats) << err(stats.error());
  EXPECT_TRUE(stats->finished);
  EXPECT_EQ(stats->analyses, 2);
  EXPECT_EQ(stats->conflicts, 1);

  const auto conflicts = world_->conflicts();
  ASSERT_EQ(conflicts.size(), 1u);
  const std::string path = LegacyRepoBuilder::path(kRunE, FileKind::Intercepts);
  EXPECT_EQ(conflicts[0].uuid, ingest::conflict_id(kUrl, bad, path));
  EXPECT_EQ(conflicts[0].kind, ConflictKind::Unparseable);
  EXPECT_EQ(conflicts[0].path, path);
  EXPECT_EQ(conflicts[0].entity, std::optional<Uuid>{kE});
  EXPECT_EQ(conflicts[0].file_sha256, std::optional<Sha256Digest>{sha256(std::string_view{garbage})});
  EXPECT_EQ(conflicts[0].resolution, "pending");
  EXPECT_TRUE(json::parse(conflicts[0].detail_json).contains("reason"));

  EXPECT_TRUE(store().load_analysis(kE)->has_value());
  EXPECT_TRUE(store().load_analysis(kF)->has_value());
  EXPECT_EQ(store().history(kE, Kind::Intercepts)->size(), 1u);  // the garbage is not a revision
}

TEST_P(ProjectImportTest, UnparseableCollectionFileLeavesAnEmptyRoot) {
  // The baselines are unreadable from the start: amending keeps one commit,
  // so the garbage is the first version of the file.
  legacy_.import_without_collection(kRunE, kE.str(), kCollected);
  legacy_.write(kRunE, FileKind::Baselines, "not json");
  repo_.git({"add", "-A"});
  repo_.git({"commit", "--quiet", "--amend", "-m", "<IMPORT> initial"}, kCollected);
  auto stats = run_import(*world_, adapter_config(repo_));
  ASSERT_TRUE(stats) << err(stats.error());
  EXPECT_EQ(stats->analyses, 1);
  EXPECT_EQ(stats->conflicts, 1);
  const auto conflicts = world_->conflicts(ConflictKind::Unparseable);
  ASSERT_EQ(conflicts.size(), 1u);
  EXPECT_EQ(conflicts[0].path, LegacyRepoBuilder::path(kRunE, FileKind::Baselines));
  auto view = store().load_analysis(kE);
  ASSERT_TRUE(view && view->has_value());
  EXPECT_TRUE(std::get<P::Baselines>((*view)->payloads.at(Kind::Baselines)).empty());
  EXPECT_EQ(std::get<P::Intercepts>((*view)->payloads.at(Kind::Intercepts)).size(), 5u);
  EXPECT_NE(world_->analysis_detail(kE).find("unparseable"), std::string::npos);
}

TEST_P(ProjectImportTest, NanInFileImports) {
  legacy_.collect(kRunE, kE.str(), kCollected);
  std::string text = LegacyRepoBuilder::intercepts_text("Ar40", 424242.5);
  const auto at = text.find("424242.5");
  ASSERT_NE(at, std::string::npos);
  text.replace(at, 8, "NaN");
  legacy_.write(kRunE, FileKind::Intercepts, text);
  legacy_.commit("<ISOEVO> fits=Ar40(Parabolic)", kRefit);
  auto stats = run_import(*world_, adapter_config(repo_));
  ASSERT_TRUE(stats) << err(stats.error());
  EXPECT_EQ(stats->conflicts, 0);
  EXPECT_EQ(stats->revisions, 1);
  EXPECT_TRUE(world_->conflicts().empty());

  auto history = store().history(kE, Kind::Intercepts);
  ASSERT_EQ(history->size(), 2u);
  // Unknown, not zero; the token is remembered in the row's extra.
  const auto rows = std::get<P::Intercepts>(**store().load_payload((*history)[1].uuid));
  const auto ar40 = std::find_if(rows.begin(), rows.end(), [](const auto& row) { return row.isotope == "Ar40"; });
  ASSERT_NE(ar40, rows.end());
  EXPECT_FALSE(ar40->value.has_value());
  ASSERT_TRUE(ar40->extra_json.has_value());
  EXPECT_NE(ar40->extra_json->find("NaN"), std::string::npos);
}

TEST_P(ProjectImportTest, RecordWithoutUuidGetsDerivedId) {
  legacy_.collect(kRunE, "", kCollected);
  auto stats = run_import(*world_, adapter_config(repo_));
  ASSERT_TRUE(stats) << err(stats.error());
  EXPECT_EQ(stats->analyses, 1);
  EXPECT_EQ(stats->conflicts, 0);

  const Uuid derived = ingest::derived_analysis_id(kUrl, kRunE);
  auto view = store().load_analysis(derived);
  ASSERT_TRUE(view && view->has_value());
  EXPECT_EQ((*view)->summary.runid, kRunE);
  EXPECT_EQ(world_->count("analysis"), 1);
  EXPECT_EQ(json::parse(world_->analysis_detail(derived)).at("derived_uuid"), true);

  // The same id on every run: a later refit lands on it.
  legacy_.refit(kRunE, "Ar40", 12.5, kRefit);
  ASSERT_TRUE(run_import(*world_, adapter_config(repo_)));
  EXPECT_EQ(world_->count("analysis"), 1);
  EXPECT_EQ(store().history(derived, Kind::Intercepts)->size(), 2u);
}

TEST_P(ProjectImportTest, SameRunidTwoUuidsIsIdentityClash) {
  legacy_.collect(kRunE, kE.str(), kCollected);
  // The record is overwritten by one with another uuid.
  const std::string overwritten = LegacyRepoBuilder::record_text(kRunE, kF.str());
  legacy_.write(kRunE, FileKind::Record, overwritten);
  const std::string clash = legacy_.commit("<EDIT> RunID", kDay2);
  legacy_.collect("66052-02A", kG.str(), kRefit);
  auto stats = run_import(*world_, adapter_config(repo_));
  ASSERT_TRUE(stats) << err(stats.error());
  EXPECT_TRUE(stats->finished);
  EXPECT_EQ(stats->conflicts, 1);

  const auto conflicts = world_->conflicts();
  ASSERT_EQ(conflicts.size(), 1u);
  EXPECT_EQ(conflicts[0].kind, ConflictKind::IdentityClash);
  EXPECT_EQ(conflicts[0].uuid, ingest::conflict_id(kUrl, clash, LegacyRepoBuilder::path(kRunE, FileKind::Record)));
  EXPECT_EQ(conflicts[0].entity, std::optional<Uuid>{kE});
  EXPECT_EQ(conflicts[0].file_sha256, std::optional<Sha256Digest>{sha256(std::string_view{overwritten})});
  const json detail = json::parse(conflicts[0].detail_json);
  EXPECT_EQ(detail.at("imported_uuid"), kE.str());
  EXPECT_EQ(detail.at("uuid"), kF.str());

  // The first analysis is intact, the second uuid was never created, later
  // analyses are imported.
  EXPECT_TRUE(store().load_analysis(kE)->has_value());
  for (const Kind kind : kSixKinds) EXPECT_EQ(store().history(kE, kind)->size(), 1u);
  EXPECT_FALSE(store().load_analysis(kF)->has_value());
  EXPECT_TRUE(store().load_analysis(kG)->has_value());
  EXPECT_EQ(world_->count("analysis"), 2);
}

TEST_P(ProjectImportTest, RecordRewrittenAfterCollectionIsHandEdit) {
  legacy_.collect(kRunE, kE.str(), kCollected);
  auto record = json::parse(LegacyRepoBuilder::record_text(kRunE, kE.str()));
  record["sample"] = "renamed by a sync";
  legacy_.write(kRunE, FileKind::Record, record.dump(4));
  const std::string sync = legacy_.commit("<SYNC> Synced repository with database", kDay2);
  auto stats = run_import(*world_, adapter_config(repo_));
  ASSERT_TRUE(stats) << err(stats.error());
  const auto conflicts = world_->conflicts();
  ASSERT_EQ(conflicts.size(), 1u);
  EXPECT_EQ(conflicts[0].kind, ConflictKind::HandEdit);
  EXPECT_EQ(conflicts[0].uuid, ingest::conflict_id(kUrl, sync, LegacyRepoBuilder::path(kRunE, FileKind::Record)));
  EXPECT_EQ(conflicts[0].entity, std::optional<Uuid>{kE});
  EXPECT_EQ(world_->count("analysis"), 1);
}

TEST_P(ProjectImportTest, SameUuidInSecondRepoAddsMembershipOnly) {
  // The same analysis, byte for byte, in two repositories.
  legacy_.collect(kRunE, kE.str(), kCollected);
  legacy_.refit(kRunE, "Ar40", 12.5, kRefit);
  GitFixture second;
  second.init();
  LegacyRepoBuilder copy(second);
  copy.import_without_collection(kRunE, kE.str(), kLater);

  ASSERT_TRUE(run_import(*world_, adapter_config(repo_)));
  const auto before = world_->revisions(kE);
  auto config = adapter_config(second);
  config.url = "https://github.com/NMGRLData/Shared";
  config.repository_name = "Shared";
  auto stats = run_import(*world_, config);
  ASSERT_TRUE(stats) << err(stats.error());
  EXPECT_TRUE(stats->finished);
  EXPECT_EQ(stats->conflicts, 0);
  EXPECT_TRUE(world_->conflicts().empty());

  EXPECT_EQ(world_->count("analysis"), 1);
  EXPECT_EQ(world_->count("repository_member"), 2);
  EXPECT_EQ(world_->count("repository"), 2);
  EXPECT_EQ(world_->revisions(kE), before);  // nothing of the copy is a revision
  auto rows = store().provenance_for(kE);
  ASSERT_TRUE(rows);
  EXPECT_EQ(rows->size(), 2u);  // one per source

  // Again: still one membership each.
  const auto seq = *store().latest_change_seq();
  ASSERT_TRUE(run_import(*world_, config, std::nullopt, [] {
    auto replay = writer_config();
    replay.replay = true;
    return replay;
  }()));
  EXPECT_EQ(*store().latest_change_seq(), seq);
  EXPECT_EQ(world_->count("repository_member"), 2);
}

TEST_P(ProjectImportTest, CopyWithADifferentRecordIsIdentityClash) {
  legacy_.collect(kRunE, kE.str(), kCollected);
  GitFixture second;
  second.init();
  LegacyRepoBuilder copy(second);
  copy.import_without_collection(kRunE, kE.str(), kLater);
  auto record = json::parse(LegacyRepoBuilder::record_text(kRunE, kE.str()));
  record["comment"] = "edited in the copy";
  copy.write(kRunE, FileKind::Record, record.dump(4));
  second.git({"add", "-A"});
  second.git({"commit", "--quiet", "--amend", "-m", "<IMPORT> initial"}, kLater);

  ASSERT_TRUE(run_import(*world_, adapter_config(repo_)));
  auto config = adapter_config(second);
  config.url = "https://github.com/NMGRLData/Shared";
  config.repository_name = "Shared";
  auto stats = run_import(*world_, config);
  ASSERT_TRUE(stats) << err(stats.error());
  EXPECT_EQ(world_->count("analysis"), 1);
  EXPECT_EQ(world_->count("repository_member"), 2);
  const auto conflicts = world_->conflicts();
  ASSERT_EQ(conflicts.size(), 1u);
  EXPECT_EQ(conflicts[0].kind, ConflictKind::IdentityClash);
  EXPECT_EQ(conflicts[0].entity, std::optional<Uuid>{kE});
}

TEST_P(ProjectImportTest, ChangeInACopyIsNotApplied) {
  // The copy is refit in its own repository: the analysis belongs to the
  // source it was imported from, so the refit is reported, not applied.
  legacy_.collect(kRunE, kE.str(), kCollected);
  GitFixture second;
  second.init();
  LegacyRepoBuilder copy(second);
  copy.import_without_collection(kRunE, kE.str(), kLater);
  const std::string refit = copy.refit(kRunE, "Ar40", 99.5, kLater);

  ASSERT_TRUE(run_import(*world_, adapter_config(repo_)));
  const auto before = world_->revisions(kE);
  auto config = adapter_config(second);
  config.url = "https://github.com/NMGRLData/Shared";
  config.repository_name = "Shared";
  auto stats = run_import(*world_, config);
  ASSERT_TRUE(stats) << err(stats.error());
  EXPECT_EQ(world_->revisions(kE), before);
  const auto conflicts = world_->conflicts();
  ASSERT_EQ(conflicts.size(), 1u);
  EXPECT_EQ(conflicts[0].kind, ConflictKind::IdentityClash);
  EXPECT_EQ(conflicts[0].uuid, ingest::conflict_id("https://github.com/NMGRLData/Shared", refit,
                                                   LegacyRepoBuilder::path(kRunE, FileKind::Intercepts)));
}

TEST_P(ProjectImportTest, ChangeBeforeTheCollectionIsCompleteIsALaterRevision) {
  // The intercepts are refit before the blanks and IC factors exist.
  const std::string first = legacy_.import_without_collection(kRunE, kE.str(), kCollected, false);
  const std::string refit = legacy_.refit(kRunE, "Ar40", 12.5, kDay2);
  legacy_.write(kRunE, FileKind::Blanks, LegacyRepoBuilder::fixture_text(FileKind::Blanks));
  legacy_.write(kRunE, FileKind::IcFactors, LegacyRepoBuilder::fixture_text(FileKind::IcFactors));
  legacy_.commit("<BLANKS> preceding bu-FD-F-789", kRefit);
  auto stats = run_import(*world_, adapter_config(repo_));
  ASSERT_TRUE(stats) << err(stats.error());
  EXPECT_EQ(stats->analyses, 1);
  EXPECT_EQ(stats->revisions, 1);
  EXPECT_EQ(stats->conflicts, 0);

  const std::string path = LegacyRepoBuilder::path(kRunE, FileKind::Intercepts);
  auto history = store().history(kE, Kind::Intercepts);
  ASSERT_EQ(history->size(), 2u);
  EXPECT_EQ((*history)[0].uuid, ingest::revision_id(kUrl, first, path));  // the first version is the root
  EXPECT_EQ((*history)[1].uuid, ingest::revision_id(kUrl, refit, path));
  EXPECT_EQ((*history)[1].changeset.created, *UtcTime::parse("2018-02-21T17:00:00Z"));
  EXPECT_EQ(head_intercept(*world_, kE, "Ar40"), std::optional<double>{12.5});
  EXPECT_EQ(std::get<P::Blanks>((*store().load_analysis(kE))->payloads.at(Kind::Blanks)).size(), 5u);
  EXPECT_EQ(store().history(kE, Kind::Blanks)->size(), 1u);
}

TEST_P(ProjectImportTest, UuidNamedFilesImport) {
  // Newer repositories name the files of an analysis by its uuid, under a
  // two-character directory; the run id is only in the record.
  const std::string name = kF.str();
  const auto at = [&](const std::string& directory, const std::string& suffix) {
    return name.substr(0, 2) + "/" + directory + name.substr(2) + suffix;
  };
  repo_.write(at("", ".json"), LegacyRepoBuilder::record_text("66052-03B", name));
  repo_.write(at(".data/", ".dat.json"), LegacyRepoBuilder::fixture_text(FileKind::Data));
  repo_.write(at("extraction/", ".extr.json"), LegacyRepoBuilder::fixture_text(FileKind::Extraction));
  const std::string collection = legacy_.commit("<COLLECTION>", kCollected);
  repo_.write(at("intercepts/", ".inte.json"), LegacyRepoBuilder::fixture_text(FileKind::Intercepts));
  repo_.write(at("baselines/", ".base.json"), LegacyRepoBuilder::fixture_text(FileKind::Baselines));
  repo_.write(at("blanks/", ".blan.json"), LegacyRepoBuilder::fixture_text(FileKind::Blanks));
  repo_.write(at("icfactors/", ".icfa.json"), LegacyRepoBuilder::fixture_text(FileKind::IcFactors));
  legacy_.commit("<ISOEVO> default collection fits", kCollected);
  repo_.write(at("intercepts/", ".inte.json"), LegacyRepoBuilder::intercepts_text("Ar40", 12.5));
  legacy_.commit("<ISOEVO>.intercepts fits=Ar40(Parabolic)", kRefit);
  auto stats = run_import(*world_, adapter_config(repo_));
  ASSERT_TRUE(stats) << err(stats.error());
  EXPECT_EQ(stats->analyses, 1);
  EXPECT_EQ(stats->conflicts, 0);

  auto view = store().load_analysis(kF);
  ASSERT_TRUE(view && view->has_value());
  EXPECT_EQ((*view)->summary.runid, "66052-03B");
  EXPECT_EQ(store().history(kF, Kind::Signals)->front().changeset.uuid,
            ingest::collection_changeset_id(kUrl, collection, kF));
  EXPECT_EQ(store().history(kF, Kind::Intercepts)->size(), 2u);
  EXPECT_FALSE(json::parse(world_->analysis_detail(kF)).contains("synthetic_collection"));
}

TEST_P(ProjectImportTest, MergeCommitLinearizes) {
  legacy_.collect(kRunE, kE.str(), kCollected);
  repo_.branch("side");
  repo_.checkout("side");
  const std::string refit = legacy_.refit(kRunE, "Ar40", 12.5, kDay2);
  repo_.checkout("main");
  legacy_.set_tag(kRunE, "omit", kRefit);
  repo_.merge("side", "Merge branch 'side'", kLater);
  auto stats = run_import(*world_, adapter_config(repo_));
  ASSERT_TRUE(stats) << err(stats.error());
  EXPECT_EQ(stats->conflicts, 0);

  // The merge repeats the refit against its first parent; it is one revision.
  auto history = store().history(kE, Kind::Intercepts);
  ASSERT_TRUE(history);
  ASSERT_EQ(history->size(), 2u);
  EXPECT_EQ((*history)[1].uuid, ingest::revision_id(kUrl, refit, LegacyRepoBuilder::path(kRunE, FileKind::Intercepts)));
  EXPECT_EQ((*history)[1].changeset.uuid, ingest::changeset_id(kUrl, refit));
  EXPECT_EQ(head_intercept(*world_, kE, "Ar40"), std::optional<double>{12.5});
  EXPECT_EQ(store().history(kE, Kind::Tags)->size(), 2u);
  EXPECT_EQ(world_->count("changeset"), 1 + 2);  // the collection, the refit, the tag; none for the merge
  EXPECT_EQ(world_->source().progress_token, std::optional<std::string>{repo_.head()});
}

TEST_P(ProjectImportTest, DeletedFileAddsNothing) {
  legacy_.collect(kRunE, kE.str(), kCollected);
  legacy_.set_tag(kRunE, "omit", kDay2);
  ASSERT_TRUE(run_import(*world_, adapter_config(repo_)));
  const auto revisions = world_->revisions(kE);
  const auto counts = world_->counts();
  const auto seq = *store().latest_change_seq();

  repo_.remove(LegacyRepoBuilder::path(kRunE, FileKind::Tags));
  repo_.remove(LegacyRepoBuilder::path(kRunE, FileKind::Intercepts));
  const std::string removed = legacy_.commit("removed by hand", kRefit);
  auto stats = run_import(*world_, adapter_config(repo_));
  ASSERT_TRUE(stats) << err(stats.error());
  EXPECT_EQ(stats->revisions, 0);
  EXPECT_EQ(stats->conflicts, 0);
  EXPECT_EQ(world_->revisions(kE), revisions);
  EXPECT_EQ(world_->counts(), counts);
  EXPECT_EQ(*store().latest_change_seq(), seq);
  EXPECT_EQ(std::get<P::TagValue>((*store().load_analysis(kE))->payloads.at(Kind::Tags)).name, "omit");
  EXPECT_EQ(world_->source().progress_token, std::optional<std::string>{removed});
}

TEST_P(ProjectImportTest, GitTagBecomesBookmark) {
  const auto c = legacy_.collect(kRunE, kE.str(), kCollected);
  repo_.tag("published-2018");
  const std::string refit = legacy_.refit(kRunE, "Ar40", 12.5, kRefit);
  auto stats = run_import(*world_, adapter_config(repo_));
  ASSERT_TRUE(stats) << err(stats.error());
  EXPECT_EQ(stats->batches, 2);  // a batch ends at the tagged commit

  ASSERT_EQ(world_->count("bookmark"), 1);
  const Uuid bookmark = ingest::bookmark_id(kUrl, "published-2018");
  auto heads = store().bookmark_heads(bookmark);
  ASSERT_TRUE(heads) << err(heads.error());
  ASSERT_EQ(heads->size(), 6u);
  const auto intercepts =
      std::find_if(heads->begin(), heads->end(), [](const auto& h) { return h.kind == Kind::Intercepts; });
  ASSERT_NE(intercepts, heads->end());
  // The heads as of the tagged commit, not the refit that followed.
  const std::string path = LegacyRepoBuilder::path(kRunE, FileKind::Intercepts);
  EXPECT_EQ(intercepts->revision, ingest::revision_id(kUrl, c.isoevo, path));
  EXPECT_EQ(*store().head(kE, Kind::Intercepts), std::optional<Uuid>{ingest::revision_id(kUrl, refit, path)});
  auto provenance = store().provenance_for(bookmark);
  ASSERT_TRUE(provenance);
  ASSERT_EQ(provenance->size(), 1u);
  EXPECT_EQ(provenance->front().commit_sha, c.icfactors);
}

TEST_P(ProjectImportTest, TagOnTheLastCommitCapturesAnalysesFoldedAtTheEnd) {
  legacy_.import_without_collection(kRunE, kE.str(), kCollected, false);  // never complete
  repo_.tag("v1", "annotated");
  auto stats = run_import(*world_, adapter_config(repo_));
  ASSERT_TRUE(stats) << err(stats.error());
  auto heads = store().bookmark_heads(ingest::bookmark_id(kUrl, "v1"));
  ASSERT_TRUE(heads);
  EXPECT_EQ(heads->size(), 6u);
}

TEST_P(ProjectImportTest, SecondRunIsNoOp) {
  legacy_.collect(kRunE, kE.str(), kCollected);
  legacy_.refit(kRunE, "Ar40", 12.5, kRefit);
  legacy_.set_tag(kRunE, "omit", kLater);
  legacy_.add_interpreted_age(kLater);
  repo_.write("notes.txt", "not a legacy file");
  legacy_.commit("stray file", kLater);
  repo_.tag("v1");
  ASSERT_TRUE(run_import(*world_, adapter_config(repo_)));
  const auto seq = *store().latest_change_seq();
  const auto counts = world_->counts();

  auto again = run_import(*world_, adapter_config(repo_));
  ASSERT_TRUE(again) << err(again.error());
  EXPECT_TRUE(again->finished);
  EXPECT_EQ(again->batches, 0);
  EXPECT_EQ(*store().latest_change_seq(), seq);
  EXPECT_EQ(world_->counts(), counts);

  // A replay walks everything again and still writes nothing.
  auto replay = writer_config();
  replay.replay = true;
  auto replayed = run_import(*world_, adapter_config(repo_, 3), std::nullopt, replay);
  ASSERT_TRUE(replayed) << err(replayed.error());
  EXPECT_GT(replayed->batches, 1);
  EXPECT_EQ(*store().latest_change_seq(), seq);
  EXPECT_EQ(world_->counts(), counts);

  // And a dry run finds nothing left to write.
  auto dry = writer_config();
  dry.dry_run = true;
  dry.replay = true;
  auto counted = run_import(*world_, adapter_config(repo_), std::nullopt, dry);
  ASSERT_TRUE(counted) << err(counted.error());
  EXPECT_EQ(counted->would_write, 0);
}

TEST_P(ProjectImportTest, NewCommitsAfterFinishAreImported) {
  legacy_.collect(kRunE, kE.str(), kCollected);
  ASSERT_TRUE(run_import(*world_, adapter_config(repo_)));
  EXPECT_EQ(store().history(kE, Kind::Intercepts)->size(), 1u);

  const std::string refit = legacy_.refit(kRunE, "Ar40", 12.5, kRefit);
  auto stats = run_import(*world_, adapter_config(repo_));
  ASSERT_TRUE(stats) << err(stats.error());
  EXPECT_EQ(stats->analyses, 0);
  EXPECT_EQ(stats->revisions, 1);
  auto history = store().history(kE, Kind::Intercepts);
  ASSERT_EQ(history->size(), 2u);
  EXPECT_EQ((*history)[1].parent, std::optional<Uuid>{(*history)[0].uuid});
  EXPECT_EQ(world_->count("analysis"), 1);
  EXPECT_EQ(world_->source().progress_token, std::optional<std::string>{refit});
  EXPECT_EQ(world_->source().head_sha, std::optional<std::string>{refit});

  // A second analysis, and a tag on the first that was collected before the token.
  legacy_.collect("66052-02A", kF.str(), kLater);
  repo_.tag("v2");
  ASSERT_TRUE(run_import(*world_, adapter_config(repo_)));
  EXPECT_EQ(world_->count("analysis"), 2);
  auto heads = store().bookmark_heads(ingest::bookmark_id(kUrl, "v2"));
  ASSERT_TRUE(heads);
  EXPECT_EQ(heads->size(), 12u);  // both analyses
}

TEST_P(ProjectImportTest, RewrittenHistoryStops) {
  legacy_.collect(kRunE, kE.str(), kCollected);
  legacy_.refit(kRunE, "Ar40", 12.5, kRefit);
  ASSERT_TRUE(run_import(*world_, adapter_config(repo_)));
  const auto seq = *store().latest_change_seq();
  const auto counts = world_->counts();
  const auto revisions = world_->revisions(kE);
  const auto token = world_->source().progress_token;

  repo_.git({"commit", "--quiet", "--amend", "-m", "<ISOEVO> rewritten"}, kLater);
  auto stats = run_import(*world_, adapter_config(repo_));
  ASSERT_FALSE(stats);
  EXPECT_NE(stats.error().what.find("history was rewritten"), std::string::npos) << stats.error().what;
  EXPECT_EQ(*store().latest_change_seq(), seq);
  EXPECT_EQ(world_->counts(), counts);
  EXPECT_EQ(world_->revisions(kE), revisions);
  EXPECT_EQ(world_->source().progress_token, token);
  EXPECT_EQ(world_->source().status, "failed");
}

TEST_P(ProjectImportTest, CatalogFromReposSynthesizes) {
  legacy_.collect(kRunE, kE.str(), kCollected);
  legacy_.collect("66052-02A", kF.str(), kDay2);
  legacy_.refit(kRunE, "Ar40", 12.5, kRefit);

  // An empty store and no catalog: nothing can be imported.
  auto bare = fresh_world(false);
  auto refused = run_import(*bare, adapter_config(repo_));
  ASSERT_TRUE(refused) << err(refused.error());
  EXPECT_TRUE(refused->finished);
  EXPECT_EQ(bare->count("analysis"), 0);
  const auto unknown = bare->conflicts();
  ASSERT_FALSE(unknown.empty());
  for (const auto& conflict : unknown) EXPECT_EQ(conflict.kind, ConflictKind::UnknownAnalysis);
  EXPECT_EQ(unknown.size(), 2u * 6u + 1u);  // one per file of each collection, and the refit

  // With the flag the catalog rows come from the records.
  auto other = fresh_world(false);
  auto config = adapter_config(repo_);
  config.catalog_from_repos = true;
  auto stats = run_import(*other, config);
  ASSERT_TRUE(stats) << err(stats.error());
  EXPECT_EQ(stats->analyses, 2);
  EXPECT_TRUE(other->store->load_analysis(kE)->has_value());
  const auto conflicts = other->conflicts();
  ASSERT_EQ(conflicts.size(), 1u);  // one identifier, two analyses
  EXPECT_EQ(conflicts[0].kind, ConflictKind::IdentityClash);
  const json detail = json::parse(conflicts[0].detail_json);
  EXPECT_EQ(detail.at("synthesized"), true);
  EXPECT_EQ(detail.at("identifier"), "66052");

  auto row = other->store->load_analysis_detail(kE);
  ASSERT_TRUE(row && row->has_value());
  EXPECT_EQ((*row)->row.sample, "SB15-03");
  EXPECT_EQ((*row)->row.material, "Feldspar");
  EXPECT_EQ((*row)->row.project, "IR1010");
  EXPECT_EQ((*row)->row.irradiation, "NM-293");
  EXPECT_EQ((*row)->row.level, "G");
  EXPECT_EQ((*row)->row.position, std::optional<int>{16});
  EXPECT_EQ((*row)->row.summary.mass_spectrometer, "felix");

  // The same conflict, not another, when the import is run again from the start.
  auto replay = writer_config();
  replay.replay = true;
  ASSERT_TRUE(run_import(*other, config, std::nullopt, replay));
  EXPECT_EQ(other->conflicts().size(), 1u);

  // The catalog now exists: a replay of the first store imports what was refused.
  EXPECT_TRUE(bare->store->add_mass_spectrometer(bare->client, {"felix", std::nullopt, std::nullopt, std::nullopt}));
  P::IdentifierSpec identifier;
  identifier.identifier = "66052";
  EXPECT_TRUE(bare->store->add_identifier(bare->client, identifier));
  EXPECT_TRUE(bare->store->add_extract_device(bare->client, "Fusions Diode"));
  auto retried = run_import(*bare, adapter_config(repo_), std::nullopt, replay);
  ASSERT_TRUE(retried) << err(retried.error());
  EXPECT_EQ(bare->count("analysis"), 2);
  EXPECT_EQ(bare->store->history(kE, Kind::Intercepts)->size(), 2u);  // the refit too, in order
  EXPECT_EQ(bare->conflicts().size(), 13u);
  auto pending = bare->store->import_conflicts({std::nullopt, std::nullopt, std::string("pending")});
  ASSERT_TRUE(pending);
  EXPECT_TRUE(pending->empty());
}

TEST_P(ProjectImportTest, InterpretedAgeBecomesARevision) {
  legacy_.collect(kRunE, kE.str(), kCollected);
  const std::string saved = legacy_.add_interpreted_age(kRefit);
  auto stats = run_import(*world_, adapter_config(repo_));
  ASSERT_TRUE(stats) << err(stats.error());
  EXPECT_EQ(stats->conflicts, 0);
  EXPECT_EQ(stats->revisions, 1);

  const std::string path(LegacyRepoBuilder::kInterpretedAgePath);
  const Uuid age = ingest::interpreted_age_id(kUrl, path);
  ASSERT_EQ(world_->count("interpreted_age"), 1);
  auto row = world_->db->select_one(QStringLiteral("SELECT uuid, name, identifier_uuid FROM interpreted_age"));
  ASSERT_TRUE(row && *row);
  EXPECT_EQ(pd::to_uuid((*row)->value("uuid")), age);
  EXPECT_EQ(pd::to_std((*row)->value("name")), "01");
  EXPECT_FALSE((*row)->value("identifier_uuid").isNull());

  auto history = store().history(age, Kind::InterpretedAge);
  ASSERT_TRUE(history);
  ASSERT_EQ(history->size(), 1u);
  EXPECT_EQ(history->front().uuid, ingest::revision_id(kUrl, saved, path));
  EXPECT_EQ(history->front().changeset.message, "<IA> added interpreted age 01");
  const auto value = std::get<P::InterpretedAgeValue>(**store().load_payload(history->front().uuid));
  // Of the members, only 66052-01E is in the store; the rest stay in the document.
  ASSERT_EQ(value.members.size(), 1u);
  EXPECT_EQ(value.members.front().analysis, kE);
  EXPECT_EQ(value.members.front().record_id, std::optional<std::string>{kRunE});
  EXPECT_NE(value.doc_json.find("66052-01A"), std::string::npos);
  const std::string detail = store().provenance_for(history->front().uuid)->front().detail_json.value_or("");
  EXPECT_NE(detail.find("unresolved_references"), std::string::npos) << detail;
  EXPECT_NE(detail.find("d9cb9f9a-250e-4bd3-8c25-a7186c51a94a"), std::string::npos) << detail;  // its legacy uuid
}

TEST_P(ProjectImportTest, TagLookupSuppliesTheTagOfAnAnalysisWithoutATagsFile) {
  legacy_.collect(kRunE, kE.str(), kCollected);
  legacy_.collect("66052-02A", kF.str(), kDay2);
  auto config = adapter_config(repo_);
  config.tag_lookup = [](const Uuid& analysis) -> std::optional<std::string> {
    if (analysis == kE) return "invalid";
    return std::nullopt;
  };
  ASSERT_TRUE(run_import(*world_, config));
  EXPECT_EQ(std::get<P::TagValue>((*store().load_analysis(kE))->payloads.at(Kind::Tags)).name, "invalid");
  EXPECT_EQ(json::parse(world_->analysis_detail(kE)).at("tag_from_db"), true);
  EXPECT_EQ(std::get<P::TagValue>((*store().load_analysis(kF))->payloads.at(Kind::Tags)).name, "ok");
  EXPECT_FALSE(json::parse(world_->analysis_detail(kF)).contains("tag_from_db"));
}

TEST_P(ProjectImportTest, FilesThatAreNotDataAreSkippedAndUnknownOnesAreConflicts) {
  repo_.write("README.md", "# IR1010\n");
  repo_.write(".gitignore", "*.pyc\n");
  legacy_.commit("Initial commit", "2018-02-16T10:00:00-07:00");
  legacy_.collect(kRunE, kE.str(), kCollected);
  repo_.write("660/logs/52-01E.logs.log", "a run log\n");
  repo_.write("docs/notes.txt", "what is this");
  const std::string stray = legacy_.commit("<COLLECTION> log", kDay2);
  // A reduction file of an analysis that has no record here.
  legacy_.write("66052-09A", FileKind::Intercepts, LegacyRepoBuilder::fixture_text(FileKind::Intercepts));
  const std::string orphan = legacy_.commit("<ISOEVO> fits=Ar40(Parabolic)", kRefit);
  auto stats = run_import(*world_, adapter_config(repo_));
  ASSERT_TRUE(stats) << err(stats.error());
  EXPECT_EQ(stats->analyses, 1);
  EXPECT_EQ(stats->conflicts, 2);

  auto unknown = store().import_conflict(ingest::conflict_id(kUrl, stray, "docs/notes.txt"));
  ASSERT_TRUE(unknown && unknown->has_value());
  EXPECT_EQ((*unknown)->kind, ConflictKind::Unparseable);
  EXPECT_EQ((*unknown)->file_sha256, std::optional<Sha256Digest>{sha256(std::string_view{"what is this"})});
  auto lost = store().import_conflict(
      ingest::conflict_id(kUrl, orphan, LegacyRepoBuilder::path("66052-09A", FileKind::Intercepts)));
  ASSERT_TRUE(lost && lost->has_value());
  EXPECT_EQ((*lost)->kind, ConflictKind::UnknownAnalysis);
  EXPECT_EQ(world_->count("import_conflict"), 2);
}

TEST_P(ProjectImportTest, FrozenProductionBecomesAReferenceObject) {
  legacy_.collect(kRunE, kE.str(), kCollected);
  repo_.write("NM-293.G.production.json", fixture("meta/NM-293/productions/Triga_PR.json"));
  const std::string frozen = legacy_.commit("<PR_FREEZE>", kRefit);
  auto stats = run_import(*world_, adapter_config(repo_));
  ASSERT_TRUE(stats) << err(stats.error());
  EXPECT_EQ(stats->conflicts, 0);

  auto row = world_->db->select_one(
      QStringLiteral("SELECT uuid, ref_type FROM ref_object WHERE key = 'frozen/IR1010/NM-293/G'"));
  ASSERT_TRUE(row && *row);
  EXPECT_EQ(pd::to_std((*row)->value("ref_type")), "production");
  const Uuid object = pd::to_uuid((*row)->value("uuid"));
  auto history = store().history(object, Kind::RefValue);
  ASSERT_TRUE(history);
  ASSERT_EQ(history->size(), 1u);
  EXPECT_EQ(history->front().uuid, ingest::revision_id(kUrl, frozen, "NM-293.G.production.json"));
  EXPECT_EQ(history->front().changeset.kind, P::ChangesetKind::Reference);
  const auto payload = std::get<P::RefPayload>(**store().load_payload(history->front().uuid));
  EXPECT_FALSE(std::get<P::ProductionValue>(payload).ratios.empty());
}

TEST_P(ProjectImportTest, OpenAndPlanErrors) {
  auto no_name = adapter_config(repo_);
  no_name.repository_name.clear();
  EXPECT_FALSE(ProjectRepoAdapter::open(no_name));
  // No commit yet: the branch names nothing.
  EXPECT_FALSE(ProjectRepoAdapter::open(adapter_config(repo_)));

  legacy_.collect(kRunE, kE.str(), kCollected);
  auto adapter = ProjectRepoAdapter::open(adapter_config(repo_));
  ASSERT_TRUE(adapter) << err(adapter.error());
  auto described = (*adapter)->describe();
  ASSERT_TRUE(described);
  EXPECT_EQ(described->kind, P::ImportSourceKind::ProjectRepo);
  EXPECT_EQ(described->url, "https://GitHub.com/NMGRLData/IR1010.git");
  EXPECT_EQ(described->branch, "main");
  EXPECT_EQ(described->head, repo_.head());
  EXPECT_FALSE((*adapter)->next_batch());  // not planned
}

INSTANTIATE_TEST_SUITE_P(Engines, ProjectImportTest, ::testing::ValuesIn(P::testing::engines()));
