// ProjectRepoAdapter end to end: a legacy-shaped git repository, walked by the
// adapter, written by the BatchWriter, read back from the store.

#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <sstream>
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
#include "verify_support.hpp"

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

// The commits of the fixture's branch in the order the adapter walks them.
std::vector<std::string> walk_order(GitFixture& repo) {
  std::istringstream lines(repo.git({"rev-list", "--topo-order", "--reverse", "HEAD"}));
  std::vector<std::string> order;
  for (std::string line; std::getline(lines, line);)
    if (!line.empty()) order.push_back(line);
  return order;
}

// The resume token of a walk that stopped after commit `index` (0-based, in
// that order), which must be `sha`: the commit, its place, and the first 16
// hex digits of the SHA-256 of the commits up to it, one per line. `end`: the
// walk reached the head.
std::string token_at(GitFixture& repo, const std::string& sha, std::size_t index, bool end = false) {
  const auto order = walk_order(repo);
  if (index >= order.size() || order[index] != sha) {
    ADD_FAILURE() << "commit " << sha << " is not at place " << index << " of the walk";
    return {};
  }
  std::string before;
  for (std::size_t i = 0; i <= index; ++i) before += order[i] + "\n";
  const Sha256Digest digest = sha256(std::string_view{before});
  return sha + "@" + std::to_string(index) + "~" + to_hex(digest).substr(0, 16) + (end ? "+end" : "");
}

// Newer repositories name the files of an analysis by its uuid, under a
// two-character directory; the run id is only in the record.
std::string uuid_file(const Uuid& uuid, const std::string& directory, const std::string& suffix) {
  const std::string name = uuid.str();
  return name.substr(0, 2) + "/" + directory + name.substr(2) + suffix;
}

// Every file of a uuid-named analysis, uncommitted.
void write_uuid_named(GitFixture& repo, const Uuid& uuid, const std::string& runid) {
  repo.write(uuid_file(uuid, "", ".json"), LegacyRepoBuilder::record_text(runid, uuid.str()));
  repo.write(uuid_file(uuid, ".data/", ".dat.json"), LegacyRepoBuilder::fixture_text(FileKind::Data));
  repo.write(uuid_file(uuid, "extraction/", ".extr.json"), LegacyRepoBuilder::fixture_text(FileKind::Extraction));
  repo.write(uuid_file(uuid, "intercepts/", ".inte.json"), LegacyRepoBuilder::fixture_text(FileKind::Intercepts));
  repo.write(uuid_file(uuid, "baselines/", ".base.json"), LegacyRepoBuilder::fixture_text(FileKind::Baselines));
  repo.write(uuid_file(uuid, "blanks/", ".blan.json"), LegacyRepoBuilder::fixture_text(FileKind::Blanks));
  repo.write(uuid_file(uuid, "icfactors/", ".icfa.json"), LegacyRepoBuilder::fixture_text(FileKind::IcFactors));
}

// plan() takes a state it does not ask anything of.
struct NoState final : ingest::IImportState {
  Result<std::optional<std::string>> head_blob_sha(const ingest::SubjectRef&, Kind) override {
    return std::optional<std::string>{};
  }
  Result<bool> analysis_exists(Uuid) override { return false; }
  Result<std::optional<ingest::AnalysisOrigin>> analysis_origin(Uuid, std::string_view) override {
    return std::optional<ingest::AnalysisOrigin>{};
  }
  Result<std::optional<Uuid>> analysis_with_runid(const std::string&, int, int) override {
    return std::optional<Uuid>{};
  }
  Result<std::optional<std::string>> identifier_at(const std::string&, const std::string&, int) override {
    return std::optional<std::string>{};
  }
  Result<bool> imported(std::string_view, std::string_view) override { return false; }
};

// The store's head of one intercept equals the value in the repository's work tree.
double tree_intercept(GitFixture& repo, const std::string& runid, const std::string& isotope) {
  std::ifstream in(repo.path() / LegacyRepoBuilder::path(runid, FileKind::Intercepts), std::ios::binary);
  std::ostringstream text;
  text << in.rdbuf();
  return json::parse(text.str()).at(isotope).at("value").get<double>();
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

// Every stored row an import decides, without what differs by design from
// run to run (change sequence numbers, write times, random row ids).
std::vector<std::string> snapshot_of(World& w) {
  std::vector<std::string> out;
  const auto rows = [&](const char* label, const QString& sql, const std::vector<const char*>& columns,
                        const char* json_column = nullptr) {
    auto found = w.db->select(sql);
    ASSERT_TRUE(found) << label;
    std::vector<std::string> lines;
    for (const auto& row : *found) {
      std::string line = label;
      for (const char* column : columns) line += " | " + pd::to_std(row.value(column));
      if (json_column) {
        const std::string text = pd::to_std(row.value(json_column));
        line += " | " + (text.empty() ? std::string("-") : json::parse(text).dump());
      }
      lines.push_back(std::move(line));
    }
    std::sort(lines.begin(), lines.end());
    out.insert(out.end(), lines.begin(), lines.end());
  };
  rows("analysis", QStringLiteral("SELECT uuid, runid_text, aliquot, increment FROM analysis"),
       {"uuid", "runid_text", "aliquot", "increment"});
  rows("revision", QStringLiteral("SELECT uuid, subject_uuid, kind, parent_uuid, changeset_uuid FROM revision"),
       {"uuid", "subject_uuid", "kind", "parent_uuid", "changeset_uuid"});
  rows("head", QStringLiteral("SELECT subject_uuid, kind, revision_uuid FROM head"),
       {"subject_uuid", "kind", "revision_uuid"});
  rows("changeset", QStringLiteral("SELECT uuid, kind, message FROM changeset"), {"uuid", "kind", "message"});
  rows("provenance",
       QStringLiteral("SELECT entity_type, entity_uuid, path, commit_sha, git_blob_sha, detail FROM import_provenance"),
       {"entity_type", "entity_uuid", "path", "commit_sha", "git_blob_sha"}, "detail");
  rows("conflict", QStringLiteral("SELECT uuid, path, conflict_kind, resolution, detail FROM import_conflict"),
       {"uuid", "path", "conflict_kind", "resolution"}, "detail");
  rows("bookmark", QStringLiteral("SELECT uuid, name FROM bookmark"), {"uuid", "name"});
  rows("bookmark_entry", QStringLiteral("SELECT bookmark_uuid, subject_uuid, kind, revision_uuid FROM bookmark_entry"),
       {"bookmark_uuid", "subject_uuid", "kind", "revision_uuid"});
  rows("member", QStringLiteral("SELECT repository_uuid, analysis_uuid FROM repository_member"), {"analysis_uuid"});
  return out;
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
  EXPECT_EQ(source.progress_token, std::optional<std::string>{token_at(repo_, c.icfactors, 3, true)});
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
  EXPECT_EQ(world_->source().progress_token, std::optional<std::string>{token_at(repo_, refit, 4, true)});
}

TEST_P(ProjectImportTest, CollectionSplitAcrossBatches) {
  legacy_.collect(kRunE, kE.str(), kCollected);
  legacy_.refit(kRunE, "Ar40", 12.5, kRefit);
  auto whole = run_import(*world_, adapter_config(repo_, 500));
  ASSERT_TRUE(whole) << err(whole.error());
  EXPECT_EQ(whole->batches, 1);

  // Two commits per batch: the first batch holds half of the collection.
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
  const auto c = legacy_.collect(kRunE, kE.str(), kCollected);
  legacy_.refit(kRunE, "Ar40", 12.5, kRefit);
  auto whole = run_import(*world_, adapter_config(repo_));
  ASSERT_TRUE(whole) << err(whole.error());

  // Stopped after the first two of the four collection commits: nothing is
  // written yet, and the token is the second commit, with the analysis
  // incomplete at it.
  auto other = fresh_world();
  auto first = run_import(*other, adapter_config(repo_, 2), 1);
  ASSERT_TRUE(first) << err(first.error());
  EXPECT_FALSE(first->finished);
  EXPECT_EQ(other->count("analysis"), 0);
  EXPECT_EQ(other->source().status, "paused");
  EXPECT_EQ(other->source().progress_token, std::optional<std::string>{token_at(repo_, c.isoevo, 1)});

  // Resumed from that token: the collection is folded from the two commits
  // before it and the two after it.
  auto second = run_import(*other, adapter_config(repo_, 2));
  ASSERT_TRUE(second) << err(second.error());
  EXPECT_TRUE(second->finished);
  EXPECT_EQ(second->batches, 2);
  EXPECT_EQ(other->revisions(kE), world_->revisions(kE));
  EXPECT_EQ(other->counts(), world_->counts());
  EXPECT_EQ(other->analysis_detail(kE), world_->analysis_detail(kE));
  EXPECT_EQ(other->source().progress_token, world_->source().progress_token);
}

TEST_P(ProjectImportTest, ResumeWithASecondCollectionPending) {
  const auto e = legacy_.collect(kRunE, kE.str(), kCollected);
  const auto f = legacy_.collect("66052-02A", kF.str(), kDay2);
  legacy_.refit(kRunE, "Ar40", 12.5, kRefit);
  legacy_.refit("66052-02A", "Ar39", 7.25, kLater);
  auto whole = run_import(*world_, adapter_config(repo_));
  ASSERT_TRUE(whole) << err(whole.error());

  // Six commits: E is complete and written, F has its record and intercepts
  // only. The run stops with F incomplete at the token.
  auto other = fresh_world();
  auto first = run_import(*other, adapter_config(repo_, 6), 1);
  ASSERT_TRUE(first) << err(first.error());
  EXPECT_FALSE(first->finished);
  EXPECT_EQ(other->count("analysis"), 1);
  EXPECT_FALSE(other->store->load_analysis(kF)->has_value());
  EXPECT_EQ(other->source().progress_token, std::optional<std::string>{token_at(repo_, f.isoevo, 5)});
  (void)e;

  // Resumed from that token, with another batch size: F is folded from the
  // same four commits as in a run that was never interrupted.
  auto second = run_import(*other, adapter_config(repo_, 3));
  ASSERT_TRUE(second) << err(second.error());
  EXPECT_TRUE(second->finished);
  EXPECT_EQ(second->analyses, 1);  // only F: E is not sent again
  EXPECT_EQ(other->revisions(kE), world_->revisions(kE));
  EXPECT_EQ(other->revisions(kF), world_->revisions(kF));
  EXPECT_EQ(other->counts(), world_->counts());
  EXPECT_EQ(other->analysis_detail(kF), world_->analysis_detail(kF));
  EXPECT_EQ(other->source().progress_token, world_->source().progress_token);
  EXPECT_EQ(store().history(kF, Kind::Signals)->front().changeset.uuid,
            ingest::collection_changeset_id(kUrl, f.collection, kF));
}

TEST_P(ProjectImportTest, ResumeWithACollectionStraddlingTheToken) {
  // E's record and intercepts, then F's record, then the rest of E, then the
  // rest of F: the two collections interleave.
  legacy_.write_record_files(kRunE, kE.str());
  legacy_.commit("<COLLECTION>", kCollected);
  legacy_.write(kRunE, FileKind::Intercepts, LegacyRepoBuilder::fixture_text(FileKind::Intercepts));
  legacy_.write(kRunE, FileKind::Baselines, LegacyRepoBuilder::fixture_text(FileKind::Baselines));
  const std::string e_isoevo = legacy_.commit("<ISOEVO> default collection fits", kCollected);
  legacy_.write_record_files("66052-02A", kF.str());
  legacy_.commit("<COLLECTION>", kDay2);
  legacy_.write(kRunE, FileKind::Blanks, LegacyRepoBuilder::fixture_text(FileKind::Blanks));
  legacy_.commit("<BLANKS> preceding bu-FD-F-789", kDay2);
  legacy_.write(kRunE, FileKind::IcFactors, LegacyRepoBuilder::fixture_text(FileKind::IcFactors));
  auto extraction = json::parse(LegacyRepoBuilder::fixture_text(FileKind::Extraction));
  extraction["extract_value"] = 5.0;
  legacy_.write(kRunE, FileKind::Extraction, extraction.dump(4));  // a satellite rewritten before E is complete
  legacy_.commit("<ICFactor> default", kDay2);
  for (const FileKind kind : {FileKind::Intercepts, FileKind::Baselines, FileKind::Blanks, FileKind::IcFactors})
    legacy_.write("66052-02A", kind, LegacyRepoBuilder::fixture_text(kind));
  legacy_.commit("<ISOEVO> default collection fits", kRefit);
  auto whole = run_import(*world_, adapter_config(repo_));
  ASSERT_TRUE(whole) << err(whole.error());

  // Stopped after two commits: the token is E's second commit, with E's
  // record before it and its last files, and all of F, after it.
  auto other = fresh_world();
  auto first = run_import(*other, adapter_config(repo_, 2), 1);
  ASSERT_TRUE(first) << err(first.error());
  EXPECT_EQ(other->count("analysis"), 0);
  EXPECT_EQ(other->source().progress_token, std::optional<std::string>{token_at(repo_, e_isoevo, 1)});

  // The resumed run meets E's blanks, IC factors and rewritten extraction;
  // they are still its collection, not later changes.
  auto second = run_import(*other, adapter_config(repo_, 5));
  ASSERT_TRUE(second) << err(second.error());
  EXPECT_TRUE(second->finished);
  EXPECT_EQ(other->revisions(kE), world_->revisions(kE));
  EXPECT_EQ(other->revisions(kF), world_->revisions(kF));
  EXPECT_EQ(other->counts(), world_->counts());
  EXPECT_EQ(other->analysis_detail(kE), world_->analysis_detail(kE));
  EXPECT_TRUE(other->conflicts().empty());
}

TEST_P(ProjectImportTest, ResumeAcrossAMergeMatchesAnUninterruptedRun) {
  legacy_.collect(kRunE, kE.str(), kCollected);
  repo_.branch("side");
  legacy_.refit(kRunE, "Ar40", 11.0, kDay2);
  legacy_.collect("66052-02A", kF.str(), kDay2);
  repo_.checkout("side");
  legacy_.refit(kRunE, "Ar40", 22.0, kRefit);
  legacy_.set_tag(kRunE, "omit", kRefit);
  repo_.checkout("main");
  repo_.git({"merge", "--quiet", "--no-ff", "-s", "ours", "-m", "Merge branch 'side'", "side"}, kLater);
  legacy_.refit("66052-02A", "Ar39", 7.25, kLater);
  auto whole = run_import(*world_, adapter_config(repo_));
  ASSERT_TRUE(whole) << err(whole.error());
  EXPECT_EQ(head_intercept(*world_, kE, "Ar40"), std::optional<double>{11.0});

  // One batch of two commits per run, each run a new adapter resuming from
  // the stored token.
  auto other = fresh_world();
  int runs = 0;
  for (bool finished = false; !finished && runs < 20; ++runs) {
    auto stats = run_import(*other, adapter_config(repo_, 2), 1);
    ASSERT_TRUE(stats) << err(stats.error());
    finished = stats->finished;
  }
  EXPECT_GT(runs, 4);
  EXPECT_LT(runs, 20);
  EXPECT_EQ(other->revisions(kE), world_->revisions(kE));
  EXPECT_EQ(other->revisions(kF), world_->revisions(kF));
  EXPECT_EQ(other->counts(), world_->counts());
  EXPECT_EQ(other->source().progress_token, world_->source().progress_token);
}

TEST_P(ProjectImportTest, ResumeTokenNamesACommitItsPlaceAndWhatCameBefore) {
  const auto c = legacy_.collect(kRunE, kE.str(), kCollected);
  legacy_.refit(kRunE, "Ar40", 12.5, kRefit);
  auto adapter = ProjectRepoAdapter::open(adapter_config(repo_));
  ASSERT_TRUE(adapter) << err(adapter.error());
  NoState state;
  const std::string good = token_at(repo_, c.icfactors, 3);
  // No token: all five commits.
  EXPECT_EQ((*adapter)->plan(std::nullopt, state).value_or(-1), 5);
  // The commit is where the token says, after the commits it says: go on after it.
  EXPECT_EQ((*adapter)->plan(good, state).value_or(-1), 1);
  EXPECT_EQ((*adapter)->plan(token_at(repo_, repo_.head(), 4, true), state).value_or(-1), 0);
  // The commit is in the history but not there: from the start.
  const auto tilde = good.find('~');
  EXPECT_EQ((*adapter)->plan(c.icfactors + "@2" + good.substr(tilde), state).value_or(-1), 5);
  EXPECT_EQ((*adapter)->plan(c.icfactors + "@40" + good.substr(tilde), state).value_or(-1), 5);
  // It is there, but after other commits than the token saw: from the start.
  EXPECT_EQ((*adapter)->plan(good.substr(0, tilde) + "~0123456789abcdef", state).value_or(-1), 5);
  // Tokens of older formats: from the start.
  EXPECT_EQ((*adapter)->plan(c.icfactors, state).value_or(-1), 5);
  EXPECT_EQ((*adapter)->plan(c.icfactors + "@3", state).value_or(-1), 5);
  EXPECT_EQ((*adapter)->plan(c.icfactors + "@3+end", state).value_or(-1), 5);
  // The commit is not in the history at all.
  auto gone = (*adapter)->plan(std::string(40, 'a') + "@3" + good.substr(tilde), state);
  ASSERT_FALSE(gone);
  EXPECT_NE(gone.error().what.find("history was rewritten"), std::string::npos) << gone.error().what;
  // Something that is not a token is not a rewritten history.
  for (const std::string& junk : {std::string("not a token"), c.icfactors + "@x~12", c.icfactors + "@3~"}) {
    auto refused = (*adapter)->plan(junk, state);
    ASSERT_FALSE(refused) << junk;
    EXPECT_EQ(refused.error().what.find("history was rewritten"), std::string::npos) << refused.error().what;
  }
}

TEST_P(ProjectImportTest, IncompleteCollectionIsFoldedAfterABoundedWait) {
  // E never gets blanks or IC factors. 25 commits of other analyses follow.
  legacy_.import_without_collection(kRunE, kE.str(), kCollected, false);
  const char* const others[] = {"66052-02A", "66052-02B", "66052-02C", "66052-02D", "66052-02E", "66052-02F"};
  for (const char* runid : others)
    legacy_.collect(runid, Uuid::v5(kG, runid).str(), kDay2);
  const std::string last = legacy_.refit("66052-02A", "Ar40", 12.5, kRefit);
  auto whole = run_import(*world_, adapter_config(repo_));
  ASSERT_TRUE(whole) << err(whole.error());
  ASSERT_TRUE(store().load_analysis(kE)->has_value());
  EXPECT_EQ(json::parse(world_->analysis_detail(kE)).at("synthetic_collection"), true);
  EXPECT_TRUE(std::get<P::Blanks>((*store().load_analysis(kE))->payloads.at(Kind::Blanks)).empty());
  EXPECT_EQ(world_->source().progress_token, std::optional<std::string>{token_at(repo_, last, 25, true)});

  // 13 commits, then stopped: E is still waiting, and the token has moved
  // past it all the same. The next run goes on from there.
  auto other = fresh_world();
  auto config = adapter_config(repo_, 13);
  auto paused = run_import(*other, config, 1);
  ASSERT_TRUE(paused) << err(paused.error());
  EXPECT_FALSE(other->store->load_analysis(kE)->has_value());
  EXPECT_EQ(other->count("analysis"), 3);
  const std::string stored = other->source().progress_token.value_or("");
  EXPECT_EQ(stored, token_at(repo_, walk_order(repo_)[12], 12));
  auto adapter = ProjectRepoAdapter::open(config);
  ASSERT_TRUE(adapter);
  NoState state;
  EXPECT_EQ((*adapter)->plan(stored, state).value_or(-1), 13);
  // 20 commits after its record E is folded with what it has: in the batch
  // that holds commit 20, not at the end of the walk.
  std::vector<int> folded_in;
  {
    ingest::BatchWriter writer(*other->store, other->client, writer_config());
    auto resumed = ProjectRepoAdapter::open(adapter_config(repo_, 4));
    ASSERT_TRUE(resumed);
    auto stats = writer.run(**resumed, std::nullopt, {}, [&](const RunStats&, const ingest::ImportBatch& batch) {
      for (const auto& item : batch.analyses)
        if (item.ingest.analysis == kE) folded_in.push_back(batch.done);
    });
    ASSERT_TRUE(stats) << err(stats.error());
  }
  EXPECT_EQ(folded_in, (std::vector<int>{21}));  // batch 13..16, 17..20: done == 21
  EXPECT_EQ(other->revisions(kE), world_->revisions(kE));
  EXPECT_EQ(other->counts(), world_->counts());
  EXPECT_EQ(other->analysis_detail(kE), world_->analysis_detail(kE));

  // A shorter wait is a setting.
  auto impatient = fresh_world();
  config = adapter_config(repo_, 4);
  config.collection_wait_commits = 2;
  ASSERT_TRUE(run_import(*impatient, config, 1));
  EXPECT_TRUE(impatient->store->load_analysis(kE)->has_value());

  // Its blanks arrive after all: a revision on the empty root.
  legacy_.write(kRunE, FileKind::Blanks, LegacyRepoBuilder::fixture_text(FileKind::Blanks));
  const std::string late = legacy_.commit("<BLANKS> auto update blanks", kLater);
  auto third = run_import(*world_, adapter_config(repo_, 13));
  ASSERT_TRUE(third) << err(third.error());
  auto blanks = store().history(kE, Kind::Blanks);
  ASSERT_EQ(blanks->size(), 2u);
  EXPECT_EQ((*blanks)[1].uuid, ingest::revision_id(kUrl, late, LegacyRepoBuilder::path(kRunE, FileKind::Blanks)));
  EXPECT_EQ((*blanks)[1].changeset.kind, P::ChangesetKind::Import);
  EXPECT_TRUE(world_->conflicts().empty());
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
  EXPECT_EQ(world_->source().progress_token, std::optional<std::string>{token_at(repo_, head, 4, true)});

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

TEST_P(ProjectImportTest, LateFileOfAnAnalysisFoldedAtTheEndSurvivesAReplay) {
  // Folded at the end of the first run without blanks or IC factors.
  legacy_.import_without_collection(kRunE, kE.str(), kCollected, false);
  ASSERT_TRUE(run_import(*world_, adapter_config(repo_)));
  ASSERT_TRUE(std::get<P::Blanks>((*store().load_analysis(kE))->payloads.at(Kind::Blanks)).empty());

  // The blanks arrive, and the next run is a replay: walked from the first
  // commit, the blanks are part of the collection of an analysis the store
  // already has. They must still be written.
  legacy_.write(kRunE, FileKind::Blanks, LegacyRepoBuilder::fixture_text(FileKind::Blanks));
  const std::string late = legacy_.commit("<BLANKS> auto update blanks", kRefit);
  auto replay = writer_config();
  replay.replay = true;
  auto stats = run_import(*world_, adapter_config(repo_), std::nullopt, replay);
  ASSERT_TRUE(stats) << err(stats.error());
  EXPECT_EQ(stats->conflicts, 0);
  EXPECT_EQ(world_->count("analysis"), 1);
  auto blanks = store().history(kE, Kind::Blanks);
  ASSERT_EQ(blanks->size(), 2u);
  EXPECT_EQ((*blanks)[1].uuid, ingest::revision_id(kUrl, late, LegacyRepoBuilder::path(kRunE, FileKind::Blanks)));
  EXPECT_EQ(std::get<P::Blanks>((*store().load_analysis(kE))->payloads.at(Kind::Blanks)).size(), 5u);
  for (const Kind kind : {Kind::Signals, Kind::Intercepts, Kind::Baselines, Kind::IcFactors, Kind::Tags})
    EXPECT_EQ(store().history(kE, kind)->size(), 1u) << P::to_string(kind);

  const auto seq = *store().latest_change_seq();
  ASSERT_TRUE(run_import(*world_, adapter_config(repo_), std::nullopt, replay));
  EXPECT_EQ(*store().latest_change_seq(), seq);
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
  // And the run id of that last analysis turns up again under another path,
  // in a record with a third uuid.
  const Uuid kH = *Uuid::parse("44444444-4444-4444-8444-444444444444");
  write_uuid_named(repo_, kH, "66052-02A");
  const std::string twice = legacy_.commit("<IMPORT> initial", kLater);
  auto stats = run_import(*world_, adapter_config(repo_));
  ASSERT_TRUE(stats) << err(stats.error());
  EXPECT_TRUE(stats->finished);

  const auto clashes = world_->conflicts(ConflictKind::IdentityClash);
  ASSERT_EQ(clashes.size(), 2u);
  auto same_file = store().import_conflict(
      ingest::conflict_id(kUrl, clash, LegacyRepoBuilder::path(kRunE, FileKind::Record)));
  ASSERT_TRUE(same_file && same_file->has_value());
  EXPECT_EQ((*same_file)->kind, ConflictKind::IdentityClash);
  EXPECT_EQ((*same_file)->entity, std::optional<Uuid>{kE});
  EXPECT_EQ((*same_file)->file_sha256, std::optional<Sha256Digest>{sha256(std::string_view{overwritten})});
  const json detail = json::parse((*same_file)->detail_json);
  EXPECT_EQ(detail.at("imported_uuid"), kE.str());
  EXPECT_EQ(detail.at("uuid"), kF.str());
  auto other_file = store().import_conflict(ingest::conflict_id(kUrl, twice, uuid_file(kH, "", ".json")));
  ASSERT_TRUE(other_file && other_file->has_value());
  EXPECT_EQ((*other_file)->kind, ConflictKind::IdentityClash);
  EXPECT_EQ((*other_file)->entity, std::optional<Uuid>{kG});
  EXPECT_EQ(json::parse((*other_file)->detail_json).at("uuid"), kH.str());
  // The other files of the analysis that was not imported are accounted for.
  EXPECT_EQ(world_->conflicts(ConflictKind::UnknownAnalysis).size(), 6u);
  EXPECT_EQ(stats->conflicts, 2 + 6);

  // The first analysis is intact, the other uuids were never created, later
  // analyses are imported.
  EXPECT_TRUE(store().load_analysis(kE)->has_value());
  for (const Kind kind : kSixKinds) EXPECT_EQ(store().history(kE, kind)->size(), 1u);
  EXPECT_FALSE(store().load_analysis(kF)->has_value());
  EXPECT_FALSE(store().load_analysis(kH)->has_value());
  EXPECT_TRUE(store().load_analysis(kG)->has_value());
  EXPECT_EQ(world_->count("analysis"), 2);
}

TEST_P(ProjectImportTest, TwoAnalysesWithOneRunIdIsIdentityClash) {
  // Two uuid-named file sets whose records carry the same run id.
  write_uuid_named(repo_, kF, "66052-03B");
  legacy_.commit("<IMPORT> initial", kCollected);
  write_uuid_named(repo_, kG, "66052-03B");
  const std::string second = legacy_.commit("<IMPORT> initial", kDay2);
  auto stats = run_import(*world_, adapter_config(repo_));
  ASSERT_TRUE(stats) << err(stats.error());
  EXPECT_TRUE(stats->finished);
  EXPECT_EQ(world_->count("analysis"), 1);
  EXPECT_TRUE(store().load_analysis(kF)->has_value());
  EXPECT_FALSE(store().load_analysis(kG)->has_value());

  auto taken = store().import_conflict(ingest::conflict_id(kUrl, second, uuid_file(kG, "", ".json")));
  ASSERT_TRUE(taken && taken->has_value());
  EXPECT_EQ((*taken)->kind, ConflictKind::IdentityClash);
  EXPECT_EQ((*taken)->entity, std::optional<Uuid>{kF});
  EXPECT_EQ(json::parse((*taken)->detail_json).at("uuid"), kG.str());
  EXPECT_EQ(world_->conflicts(ConflictKind::IdentityClash).size(), 1u);
  // Every other file of the refused analysis is accounted for.
  EXPECT_EQ(world_->conflicts(ConflictKind::UnknownAnalysis).size(), 6u);
}

TEST_P(ProjectImportTest, SecondCopyInTheSameSourceIsMembershipOrIdentityClash) {
  // The analysis F under its uuid, then twice more under run-id paths: once
  // with the same record, byte for byte, once with an edited one.
  write_uuid_named(repo_, kF, "66052-03B");
  legacy_.commit("<IMPORT> initial", kCollected);
  const auto copies = [&] {
    legacy_.import_without_collection("66052-03B", kF.str(), kDay2);  // the same record text
    legacy_.import_without_collection("66052-08A", kF.str(), kRefit);
    auto record = json::parse(LegacyRepoBuilder::record_text("66052-03B", kF.str()));
    record["comment"] = "edited in the copy";
    legacy_.write("66052-08A", FileKind::Record, record.dump(4));
    repo_.git({"add", "-A"});
    repo_.git({"commit", "--quiet", "--amend", "-m", "<IMPORT> initial"}, kRefit);
    return repo_.head();
  };
  const auto expect = [&](World& w, const std::string& edited, const std::string& what) {
    EXPECT_EQ(w.count("analysis"), 1) << what;
    EXPECT_EQ(w.count("repository_member"), 1) << what;
    const auto conflicts = w.conflicts(ConflictKind::IdentityClash);
    ASSERT_EQ(conflicts.size(), 1u) << what;
    // The copies bring the spectrometer file F's record names, after F was
    // folded without it: that is said, once, at any cut (spec 10.29).
    const auto late = w.conflicts(ConflictKind::Unparseable);
    ASSERT_EQ(late.size(), 1u) << what;
    EXPECT_EQ(late[0].path, std::string(LegacyRepoBuilder::kSpecSha) + ".json") << what;
    EXPECT_EQ(json::parse(late[0].detail_json).at("reason"), "spectrometer_file_after_collection") << what;
    EXPECT_EQ(conflicts[0].uuid,
              ingest::conflict_id(kUrl, edited, LegacyRepoBuilder::path("66052-08A", FileKind::Record)))
        << what;
    EXPECT_EQ(conflicts[0].entity, std::optional<Uuid>{kF}) << what;
    EXPECT_EQ(w.revisions(kF).size(), 6u) << what;
  };

  // The copies arrive in a later run.
  ASSERT_TRUE(run_import(*world_, adapter_config(repo_)));
  const std::string edited = copies();
  auto later = run_import(*world_, adapter_config(repo_));
  ASSERT_TRUE(later) << err(later.error());
  expect(*world_, edited, "copies in a later run");
  const auto detail = world_->conflicts(ConflictKind::IdentityClash)[0].detail_json;

  // All in one walk, in one batch and in several: the same.
  for (const int batch_commits : {500, 1}) {
    auto other = fresh_world();
    auto stats = run_import(*other, adapter_config(repo_, batch_commits));
    ASSERT_TRUE(stats) << err(stats.error());
    expect(*other, edited, "one walk, batches of " + std::to_string(batch_commits));
    EXPECT_EQ(json::parse(other->conflicts(ConflictKind::IdentityClash)[0].detail_json), json::parse(detail));
  }
  // Again: nothing.
  const auto seq = *store().latest_change_seq();
  auto replay = writer_config();
  replay.replay = true;
  ASSERT_TRUE(run_import(*world_, adapter_config(repo_), std::nullopt, replay));
  EXPECT_EQ(*store().latest_change_seq(), seq);
}

TEST_P(ProjectImportTest, RunIdTakenInAnEarlierRunIsIdentityClash) {
  write_uuid_named(repo_, kF, "66052-03B");
  legacy_.commit("<IMPORT> initial", kCollected);
  ASSERT_TRUE(run_import(*world_, adapter_config(repo_)));
  ASSERT_TRUE(store().load_analysis(kF)->has_value());

  // The second analysis arrives after the token: nothing in this walk has seen the first.
  write_uuid_named(repo_, kG, "66052-03B");
  const std::string second = legacy_.commit("<IMPORT> initial", kDay2);
  legacy_.collect(kRunE, kE.str(), kRefit);
  auto stats = run_import(*world_, adapter_config(repo_));
  ASSERT_TRUE(stats) << err(stats.error());
  EXPECT_TRUE(stats->finished);
  EXPECT_FALSE(store().load_analysis(kG)->has_value());
  EXPECT_TRUE(store().load_analysis(kE)->has_value());  // the import went on
  auto taken = store().import_conflict(ingest::conflict_id(kUrl, second, uuid_file(kG, "", ".json")));
  ASSERT_TRUE(taken && taken->has_value());
  EXPECT_EQ((*taken)->kind, ConflictKind::IdentityClash);
  EXPECT_EQ((*taken)->entity, std::optional<Uuid>{kF});
  EXPECT_EQ(world_->source().status, "finished");
}

TEST_P(ProjectImportTest, RunIdTakenByAnotherSourceIsIdentityClash) {
  legacy_.collect(kRunE, kE.str(), kCollected);
  ASSERT_TRUE(run_import(*world_, adapter_config(repo_)));

  GitFixture second;
  second.init();
  LegacyRepoBuilder other(second);
  write_uuid_named(second, kG, kRunE);
  const std::string same = other.commit("<IMPORT> initial", kDay2);
  other.collect("66052-02A", kF.str(), kRefit);
  auto config = adapter_config(second);
  config.url = "https://github.com/NMGRLData/Shared";
  config.repository_name = "Shared";
  auto stats = run_import(*world_, config);
  ASSERT_TRUE(stats) << err(stats.error());
  EXPECT_TRUE(stats->finished);
  EXPECT_FALSE(store().load_analysis(kG)->has_value());
  EXPECT_TRUE(store().load_analysis(kF)->has_value());
  auto taken = store().import_conflict(
      ingest::conflict_id("https://github.com/NMGRLData/Shared", same, uuid_file(kG, "", ".json")));
  ASSERT_TRUE(taken && taken->has_value());
  EXPECT_EQ((*taken)->kind, ConflictKind::IdentityClash);
  EXPECT_EQ((*taken)->entity, std::optional<Uuid>{kE});
}

TEST_P(ProjectImportTest, PositionTakenInAnEarlierRunGetsAnIdentifierWithoutPosition) {
  // No catalog. 66052 takes NM-293/G/16 in the first run; the records of
  // 66053 name the same position in the second.
  auto bare = fresh_world(false);
  auto config = adapter_config(repo_);
  config.catalog_from_repos = true;
  legacy_.collect(kRunE, kE.str(), kCollected);
  ASSERT_TRUE(run_import(*bare, config));
  legacy_.collect("66053-01A", kF.str(), kDay2);
  auto stats = run_import(*bare, config);
  ASSERT_TRUE(stats) << err(stats.error());
  EXPECT_TRUE(stats->finished);
  ASSERT_TRUE(bare->store->load_analysis(kF)->has_value());
  EXPECT_EQ(*bare->store->identifier_at("NM-293", "G", 16), std::optional<std::string>{"66052"});
  EXPECT_TRUE(bare->store->find_identifier("66053")->has_value());
  auto made = bare->store->import_conflict(ingest::conflict_id(kUrl, "", "catalog/identifier/66053"));
  ASSERT_TRUE(made && made->has_value());
  EXPECT_EQ((*made)->kind, ConflictKind::IdentityClash);
  const json detail = json::parse((*made)->detail_json);
  EXPECT_EQ(detail.at("synthesized"), true);
  EXPECT_EQ(detail.at("position_taken_by"), "66052");

  // The same in one walk.
  auto fresh = fresh_world(false);
  auto both = run_import(*fresh, config);
  ASSERT_TRUE(both) << err(both.error());
  EXPECT_EQ(fresh->count("analysis"), 2);
  EXPECT_EQ(*fresh->store->identifier_at("NM-293", "G", 16), std::optional<std::string>{"66052"});
}

TEST_P(ProjectImportTest, RenumberedRecordIsAnIdentityRevision) {
  legacy_.collect(kRunE, kE.str(), kCollected);
  // <EDIT> RunID: the same file, the same uuid, another aliquot and step.
  const std::string path = LegacyRepoBuilder::path(kRunE, FileKind::Record);
  repo_.write(path, LegacyRepoBuilder::record_text("66052-07B", kE.str()));
  const std::string edit = legacy_.commit("<EDIT> RunID", kDay2, "Bob <bob@example.org>");
  auto stats = run_import(*world_, adapter_config(repo_));
  ASSERT_TRUE(stats) << err(stats.error());
  EXPECT_EQ(stats->conflicts, 0);
  EXPECT_TRUE(world_->conflicts().empty());

  EXPECT_EQ((*store().load_analysis(kE))->summary.runid, "66052-07B");
  auto history = store().history(kE, Kind::Identity);
  ASSERT_TRUE(history);
  ASSERT_EQ(history->size(), 1u);
  EXPECT_EQ(history->front().uuid, ingest::revision_id(kUrl, edit, path));
  EXPECT_EQ(history->front().changeset.uuid, ingest::changeset_id(kUrl, edit));
  EXPECT_EQ(history->front().changeset.message, "<EDIT> RunID");
  EXPECT_EQ(history->front().author_name, "git:bob@example.org");
  const auto value = std::get<P::IdentityValue>(**store().load_payload(history->front().uuid));
  EXPECT_EQ(value.aliquot, 7);
  EXPECT_EQ(value.increment, 1);
  // The old and new values are kept with the commit.
  const json detail = json::parse(store().provenance_for(ingest::changeset_id(kUrl, edit))->front().detail_json.value());
  ASSERT_EQ(detail.at("rewrites").size(), 1u);
  EXPECT_EQ(detail.at("rewrites")[0].at("path"), path);
  EXPECT_EQ(detail.at("rewrites")[0].at("changed").at("aliquot"), json({{"old", 1}, {"new", 7}}));
  EXPECT_EQ(detail.at("rewrites")[0].at("changed").at("increment"), json({{"old", 4}, {"new", 1}}));

  // The old run id is free again: another analysis takes it in a later run.
  write_uuid_named(repo_, kF, kRunE);
  legacy_.commit("<IMPORT> initial", kRefit);
  auto again = run_import(*world_, adapter_config(repo_));
  ASSERT_TRUE(again) << err(again.error());
  EXPECT_TRUE(again->finished);
  EXPECT_TRUE(world_->conflicts().empty());
  EXPECT_EQ((*store().load_analysis(kF))->summary.runid, kRunE);
}

TEST_P(ProjectImportTest, RenumberToATakenRunIdIsIdentityClash) {
  legacy_.collect(kRunE, kE.str(), kCollected);
  legacy_.collect("66052-02A", kF.str(), kDay2);
  repo_.write(LegacyRepoBuilder::path("66052-02A", FileKind::Record), LegacyRepoBuilder::record_text(kRunE, kF.str()));
  const std::string edit = legacy_.commit("<EDIT> RunID", kRefit);
  auto stats = run_import(*world_, adapter_config(repo_));
  ASSERT_TRUE(stats) << err(stats.error());
  EXPECT_TRUE(stats->finished);
  EXPECT_EQ((*store().load_analysis(kF))->summary.runid, "66052-02A");
  const auto conflicts = world_->conflicts();
  ASSERT_EQ(conflicts.size(), 1u);
  EXPECT_EQ(conflicts[0].kind, ConflictKind::IdentityClash);
  EXPECT_EQ(conflicts[0].uuid,
            ingest::conflict_id(kUrl, edit, LegacyRepoBuilder::path("66052-02A", FileKind::Record)));
}

TEST_P(ProjectImportTest, RewrittenRawDataIsASignalsRevision) {
  legacy_.collect(kRunE, kE.str(), kCollected);
  auto data = json::parse(LegacyRepoBuilder::fixture_text(FileKind::Data));
  data.at("sniffs").erase(0);  // one series fewer
  legacy_.write(kRunE, FileKind::Data, data.dump());
  const std::string rewrite = legacy_.commit("<DEFINE EQUIL> 66052-01E", kDay2);
  auto stats = run_import(*world_, adapter_config(repo_));
  ASSERT_TRUE(stats) << err(stats.error());
  EXPECT_EQ(stats->conflicts, 0);
  EXPECT_TRUE(world_->conflicts().empty());

  auto history = store().history(kE, Kind::Signals);
  ASSERT_EQ(history->size(), 2u);
  EXPECT_EQ((*history)[1].uuid, ingest::revision_id(kUrl, rewrite, LegacyRepoBuilder::path(kRunE, FileKind::Data)));
  EXPECT_EQ((*history)[1].parent, std::optional<Uuid>{(*history)[0].uuid});
  EXPECT_EQ(std::get<P::SignalRefs>(**store().load_payload((*history)[0].uuid)).size(), 15u);
  EXPECT_EQ(std::get<P::SignalRefs>(**store().load_payload((*history)[1].uuid)).size(), 14u);
  EXPECT_EQ((*store().load_analysis(kE))->summary.signals_state, "complete");
}

TEST_P(ProjectImportTest, RewrittenRecordAndSatelliteAreKeptWithTheCommit) {
  legacy_.collect(kRunE, kE.str(), kCollected);
  // <SYNC>: the record's sample is renamed, a key is added, one is removed;
  // the extraction file is edited in the same commit.
  auto record = json::parse(LegacyRepoBuilder::record_text(kRunE, kE.str()));
  record["sample"] = "renamed by a sync";
  record["principal_investigator"] = "Heizler, M";
  record.erase("comment");
  const std::string record_text = record.dump(4);
  legacy_.write(kRunE, FileKind::Record, record_text);
  auto extraction = json::parse(LegacyRepoBuilder::fixture_text(FileKind::Extraction));
  extraction["extract_value"] = 5.0;
  legacy_.write(kRunE, FileKind::Extraction, extraction.dump(4));
  const std::string sync = legacy_.commit("<SYNC> Synced repository with database", kDay2);
  // A peak center that arrives after collection has no earlier version.
  legacy_.write(kRunE, FileKind::PeakCenter, fixture(kUnknown + "660/peakcenter/52-01A.peak.json"));
  const std::string peak = legacy_.commit("<MANUAL> peak center", kRefit);
  auto stats = run_import(*world_, adapter_config(repo_));
  ASSERT_TRUE(stats) << err(stats.error());
  EXPECT_EQ(stats->conflicts, 0);
  EXPECT_TRUE(world_->conflicts().empty());
  EXPECT_EQ(stats->changesets, 2);
  EXPECT_EQ(stats->revisions, 0);

  // The analysis is as collected; nothing became a revision.
  EXPECT_EQ(world_->count("analysis"), 1);
  for (const Kind kind : kSixKinds) EXPECT_EQ(store().history(kE, kind)->size(), 1u);
  EXPECT_TRUE(store().history(kE, Kind::Identity)->empty());

  // The commit is an import changeset with no revision; what changed is in its provenance.
  auto row = world_->db->select_one(QStringLiteral("SELECT kind, message FROM changeset WHERE uuid = ?"),
                                    {pd::qv(ingest::changeset_id(kUrl, sync))});
  ASSERT_TRUE(row && *row);
  EXPECT_EQ(pd::to_std((*row)->value("kind")), "import");
  EXPECT_EQ(pd::to_std((*row)->value("message")), "<SYNC> Synced repository with database");
  auto provenance = store().provenance_for(ingest::changeset_id(kUrl, sync));
  ASSERT_TRUE(provenance);
  ASSERT_EQ(provenance->size(), 1u);
  const json rewrites = json::parse(provenance->front().detail_json.value()).at("rewrites");
  ASSERT_EQ(rewrites.size(), 2u);
  const auto of = [&](const std::string& path) {
    for (const auto& entry : rewrites)
      if (entry.at("path") == path) return entry;
    ADD_FAILURE() << "no rewrite of " << path;
    return json::object();
  };
  const json of_record = of(LegacyRepoBuilder::path(kRunE, FileKind::Record));
  EXPECT_EQ(of_record.at("analysis"), kE.str());
  EXPECT_EQ(of_record.at("blob").get<std::string>().size(), 40u);
  ASSERT_EQ(of_record.at("changed").size(), 3u);
  EXPECT_EQ(of_record.at("changed").at("sample"), json({{"old", "SB15-03"}, {"new", "renamed by a sync"}}));
  EXPECT_EQ(of_record.at("changed").at("principal_investigator"), json({{"new", "Heizler, M"}}));
  EXPECT_EQ(of_record.at("changed").at("comment"), json({{"old", "G:16 Plag, 4 mg"}}));
  const json of_extraction = of(LegacyRepoBuilder::path(kRunE, FileKind::Extraction));
  ASSERT_EQ(of_extraction.at("changed").size(), 1u);
  EXPECT_EQ(of_extraction.at("changed").at("extract_value"), json({{"old", 4.0}, {"new", 5.0}}));

  const json added =
      json::parse(store().provenance_for(ingest::changeset_id(kUrl, peak))->front().detail_json.value())
          .at("rewrites");
  ASSERT_EQ(added.size(), 1u);
  EXPECT_EQ(added[0].at("path"), LegacyRepoBuilder::path(kRunE, FileKind::PeakCenter));
  EXPECT_TRUE(added[0].at("changed").at("reference_detector").contains("new"));
  EXPECT_FALSE(added[0].at("changed").at("reference_detector").contains("old"));

  // Run again, and replayed from the start: the same rows.
  const auto seq = *store().latest_change_seq();
  auto replay = writer_config();
  replay.replay = true;
  ASSERT_TRUE(run_import(*world_, adapter_config(repo_), std::nullopt, replay));
  EXPECT_EQ(*store().latest_change_seq(), seq);
}

TEST_P(ProjectImportTest, RewritesAfterCollectionLeaveNoPendingConflict) {
  legacy_.collect(kRunE, kE.str(), kCollected);
  legacy_.collect("66052-02A", kF.str(), kCollected);
  auto record = json::parse(LegacyRepoBuilder::record_text(kRunE, kE.str()));
  record["project"] = "IR1010-renamed";
  legacy_.write(kRunE, FileKind::Record, record.dump(4));
  legacy_.commit("<SYNC> Synced repository with database", kDay2);
  repo_.write(LegacyRepoBuilder::path("66052-02A", FileKind::Record),
              LegacyRepoBuilder::record_text("66052-09C", kF.str()));
  legacy_.commit("<EDIT> RunID", kDay2);
  auto data = json::parse(LegacyRepoBuilder::fixture_text(FileKind::Data));
  data.at("signals").erase(0);
  legacy_.write(kRunE, FileKind::Data, data.dump());
  auto extraction = json::parse(LegacyRepoBuilder::fixture_text(FileKind::Extraction));
  extraction["cleanup_duration"] = 90.0;
  legacy_.write(kRunE, FileKind::Extraction, extraction.dump(4));
  legacy_.commit("<DEFINE EQUIL> 66052-01E", kRefit);
  legacy_.refit(kRunE, "Ar40", 12.5, kLater);

  // In one walk, and one commit per batch with a new adapter each time.
  for (const int batch_commits : {500, 1}) {
    auto world = fresh_world();
    bool finished = false;
    for (int runs = 0; !finished && runs < 40; ++runs) {
      auto stats = run_import(*world, adapter_config(repo_, batch_commits), 1);
      ASSERT_TRUE(stats) << err(stats.error());
      EXPECT_EQ(stats->conflicts, 0);
      finished = stats->finished;
    }
    EXPECT_TRUE(finished);
    EXPECT_TRUE(world->conflicts().empty()) << batch_commits;
    auto pending = world->store->import_conflicts({std::nullopt, std::nullopt, std::string("pending")});
    ASSERT_TRUE(pending);
    EXPECT_TRUE(pending->empty());
    EXPECT_EQ((*world->store->load_analysis(kF))->summary.runid, "66052-09C");
    EXPECT_EQ(world->store->history(kE, Kind::Signals)->size(), 2u);
    EXPECT_EQ(world->store->history(kE, Kind::Intercepts)->size(), 2u);
  }
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
  repo_.write(uuid_file(kF, "", ".json"), LegacyRepoBuilder::record_text("66052-03B", kF.str()));
  repo_.write(uuid_file(kF, ".data/", ".dat.json"), LegacyRepoBuilder::fixture_text(FileKind::Data));
  repo_.write(uuid_file(kF, "extraction/", ".extr.json"), LegacyRepoBuilder::fixture_text(FileKind::Extraction));
  const std::string collection = legacy_.commit("<COLLECTION>", kCollected);
  write_uuid_named(repo_, kF, "66052-03B");  // adds the four reduction files
  legacy_.commit("<ISOEVO> default collection fits", kCollected);
  repo_.write(uuid_file(kF, "intercepts/", ".inte.json"), LegacyRepoBuilder::intercepts_text("Ar40", 12.5));
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

TEST_P(ProjectImportTest, MergeThatKeepsTheFirstParentsVersionMovesTheHeadBack) {
  legacy_.collect(kRunE, kE.str(), kCollected);
  repo_.branch("side");
  const std::string on_main = legacy_.refit(kRunE, "Ar40", 11.0, kDay2);
  repo_.checkout("side");
  const std::string on_side = legacy_.refit(kRunE, "Ar40", 22.0, kRefit);
  repo_.checkout("main");
  // Both sides changed the file; the merge keeps main's.
  repo_.git({"merge", "--quiet", "--no-ff", "-s", "ours", "-m", "Merge branch 'side'", "side"}, kLater);
  const std::string merge = repo_.head();
  ASSERT_EQ(tree_intercept(repo_, kRunE, "Ar40"), 11.0);
  auto stats = run_import(*world_, adapter_config(repo_));
  ASSERT_TRUE(stats) << err(stats.error());
  EXPECT_EQ(stats->conflicts, 0);

  // The imported head is what the merge tree holds.
  EXPECT_EQ(head_intercept(*world_, kE, "Ar40"), std::optional<double>{tree_intercept(repo_, kRunE, "Ar40")});
  const std::string path = LegacyRepoBuilder::path(kRunE, FileKind::Intercepts);
  auto history = store().history(kE, Kind::Intercepts);
  ASSERT_EQ(history->size(), 4u);  // root, main's, the side's, and main's again at the merge
  EXPECT_EQ((*history)[1].uuid, ingest::revision_id(kUrl, on_main, path));
  EXPECT_EQ((*history)[2].uuid, ingest::revision_id(kUrl, on_side, path));
  EXPECT_EQ((*history)[3].uuid, ingest::revision_id(kUrl, merge, path));
  EXPECT_EQ((*history)[3].changeset.message, "Merge branch 'side'");

  // The same after a resume in the middle, and on a second run nothing moves.
  auto other = fresh_world();
  ASSERT_TRUE(run_import(*other, adapter_config(repo_, 5), 1));
  ASSERT_TRUE(run_import(*other, adapter_config(repo_, 5)));
  EXPECT_EQ(other->revisions(kE), world_->revisions(kE));
  const auto seq = *store().latest_change_seq();
  ASSERT_TRUE(run_import(*world_, adapter_config(repo_)));
  EXPECT_EQ(*store().latest_change_seq(), seq);
}

TEST_P(ProjectImportTest, MergeThatKeepsTheOtherParentsVersionAddsNoRevision) {
  legacy_.collect(kRunE, kE.str(), kCollected);
  repo_.branch("side");
  legacy_.refit(kRunE, "Ar40", 11.0, kDay2);
  repo_.checkout("side");
  legacy_.refit(kRunE, "Ar40", 22.0, kRefit);
  repo_.checkout("main");
  repo_.git({"merge", "--quiet", "--no-ff", "-X", "theirs", "-m", "Merge branch 'side'", "side"}, kLater);
  ASSERT_EQ(tree_intercept(repo_, kRunE, "Ar40"), 22.0);
  auto stats = run_import(*world_, adapter_config(repo_));
  ASSERT_TRUE(stats) << err(stats.error());
  EXPECT_EQ(head_intercept(*world_, kE, "Ar40"), std::optional<double>{22.0});
  // Root, main's, the side's: the side's is already the head when the merge comes.
  EXPECT_EQ(store().history(kE, Kind::Intercepts)->size(), 3u);
  EXPECT_EQ(world_->count("changeset"), 1 + 2);
}

TEST_P(ProjectImportTest, MergeResolvedByHandCarriesTheMergedContent) {
  legacy_.collect(kRunE, kE.str(), kCollected);
  repo_.branch("side");
  legacy_.refit(kRunE, "Ar40", 11.0, kDay2);
  repo_.checkout("side");
  legacy_.refit(kRunE, "Ar40", 22.0, kRefit);
  legacy_.set_tag(kRunE, "omit", kRefit);
  repo_.checkout("main");
  repo_.git({"merge", "--quiet", "--no-ff", "--no-commit", "-s", "ours", "side"}, kLater);
  legacy_.write(kRunE, FileKind::Intercepts, LegacyRepoBuilder::intercepts_text("Ar40", 33.0));
  const std::string merge = legacy_.commit("Merge branch 'side', by hand", kLater);
  auto stats = run_import(*world_, adapter_config(repo_));
  ASSERT_TRUE(stats) << err(stats.error());
  EXPECT_EQ(head_intercept(*world_, kE, "Ar40"), std::optional<double>{33.0});
  auto history = store().history(kE, Kind::Intercepts);
  EXPECT_EQ(history->back().uuid,
            ingest::revision_id(kUrl, merge, LegacyRepoBuilder::path(kRunE, FileKind::Intercepts)));
  // The side's tag file is not in the merge tree: deleted by the merge, which adds nothing.
  EXPECT_FALSE(std::filesystem::exists(repo_.path() / LegacyRepoBuilder::path(kRunE, FileKind::Tags)));
  EXPECT_EQ(store().history(kE, Kind::Tags)->size(), 2u);
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
  EXPECT_EQ(world_->source().progress_token, std::optional<std::string>{token_at(repo_, repo_.head(), 6, true)});
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
  EXPECT_EQ(world_->source().progress_token, std::optional<std::string>{token_at(repo_, removed, 5, true)});
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
  EXPECT_EQ(world_->source().progress_token, std::optional<std::string>{token_at(repo_, refit, 4, true)});
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

  // The same url now points at a repository that never had the token's commit.
  GitFixture replaced;
  replaced.init();
  LegacyRepoBuilder elsewhere(replaced);
  elsewhere.collect("66052-02A", kF.str(), kDay2);
  elsewhere.refit("66052-02A", "Ar40", 12.5, kRefit);
  auto unknown = run_import(*world_, adapter_config(replaced));
  ASSERT_FALSE(unknown);
  EXPECT_NE(unknown.error().what.find("history was rewritten"), std::string::npos) << unknown.error().what;
  EXPECT_EQ(*store().latest_change_seq(), seq);
  EXPECT_FALSE(store().load_analysis(kF)->has_value());
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

TEST_P(ProjectImportTest, RenumberedThenReusedRunIdAtEveryCut) {
  // U is collected at 66052-02A and renumbered; V then takes 66052-02A.
  legacy_.collect("66052-02A", kF.str(), kCollected);
  repo_.write(LegacyRepoBuilder::path("66052-02A", FileKind::Record),
              LegacyRepoBuilder::record_text("66052-07B", kF.str()));
  legacy_.commit("<EDIT> RunID", kDay2);
  const auto with_v = [&] {
    write_uuid_named(repo_, kG, "66052-02A");
    legacy_.commit("<IMPORT> initial", kRefit);
  };
  const auto expect = [&](World& w, const std::string& what) {
    ASSERT_TRUE(w.store->load_analysis(kF)->has_value()) << what;
    ASSERT_TRUE(w.store->load_analysis(kG)->has_value()) << what;
    EXPECT_EQ((*w.store->load_analysis(kF))->summary.runid, "66052-07B") << what;
    EXPECT_EQ((*w.store->load_analysis(kG))->summary.runid, "66052-02A") << what;
    for (const auto& conflict : w.conflicts()) ADD_FAILURE() << what << ": " << conflict.path << " " << conflict.detail_json;
  };
  const auto replay = [&](World& w, const std::string& what) {
    auto again = writer_config();
    again.replay = true;
    const auto seq = *w.store->latest_change_seq();
    auto stats = run_import(w, adapter_config(repo_), std::nullopt, again);
    ASSERT_TRUE(stats) << what << ": " << err(stats.error());
    EXPECT_EQ(*w.store->latest_change_seq(), seq) << what;
    expect(w, what + ", replayed");
  };

  // V arrives in a later run.
  ASSERT_TRUE(run_import(*world_, adapter_config(repo_)));
  with_v();
  auto later = run_import(*world_, adapter_config(repo_));
  ASSERT_TRUE(later) << err(later.error());
  expect(*world_, "V in a later run");
  replay(*world_, "V in a later run");

  for (const int batch_commits : {500, 3, 2, 1}) {
    const std::string what = "batches of " + std::to_string(batch_commits);
    auto cut = fresh_world();
    auto stats = run_import(*cut, adapter_config(repo_, batch_commits));
    ASSERT_TRUE(stats) << what << ": " << err(stats.error());
    expect(*cut, what);
    EXPECT_EQ(cut->revisions(kF), world_->revisions(kF)) << what;
    EXPECT_EQ(cut->counts(), world_->counts()) << what;
    replay(*cut, what);

    // Stopped after every batch: a resume at each token there is.
    auto resumed = fresh_world();
    bool finished = false;
    for (int runs = 0; !finished && runs < 30; ++runs) {
      auto one = run_import(*resumed, adapter_config(repo_, batch_commits), 1);
      ASSERT_TRUE(one) << what << ": " << err(one.error());
      finished = one->finished;
    }
    EXPECT_TRUE(finished) << what;
    expect(*resumed, what + ", resumed");
    EXPECT_EQ(resumed->counts(), world_->counts()) << what;
  }
}

TEST_P(ProjectImportTest, RenumberWhilePendingThenReuseOfTheRunId) {
  // U's record is renumbered before its collection is complete, and V takes
  // the old run id before U is folded.
  legacy_.write_record_files("66052-02A", kF.str());
  legacy_.commit("<COLLECTION>", kCollected);
  repo_.write(LegacyRepoBuilder::path("66052-02A", FileKind::Record),
              LegacyRepoBuilder::record_text("66052-07B", kF.str()));
  const std::string edit = legacy_.commit("<EDIT> RunID", kCollected);
  write_uuid_named(repo_, kG, "66052-02A");
  legacy_.commit("<IMPORT> initial", kDay2);
  for (const FileKind kind : {FileKind::Intercepts, FileKind::Baselines, FileKind::Blanks, FileKind::IcFactors})
    legacy_.write("66052-02A", kind, LegacyRepoBuilder::fixture_text(kind));
  legacy_.commit("<ISOEVO> default collection fits", kDay2);

  std::vector<std::string> first;
  for (const int batch_commits : {500, 1}) {
    auto world = fresh_world();
    auto stats = run_import(*world, adapter_config(repo_, batch_commits));
    ASSERT_TRUE(stats) << err(stats.error());
    EXPECT_TRUE(world->conflicts().empty()) << batch_commits;
    ASSERT_TRUE(world->store->load_analysis(kF)->has_value());
    EXPECT_EQ((*world->store->load_analysis(kF))->summary.runid, "66052-07B");
    EXPECT_EQ((*world->store->load_analysis(kG))->summary.runid, "66052-02A");
    // Folded under the identity it had by then: no identity revision, and the
    // rewrite is kept with its commit.
    EXPECT_TRUE(world->store->history(kF, Kind::Identity)->empty());
    EXPECT_EQ(json::parse(world->analysis_detail(kF)).at("identity_from"), edit);
    const auto of_edit = world->store->provenance_for(ingest::changeset_id(kUrl, edit));
    ASSERT_EQ(of_edit->size(), 1u);
    EXPECT_EQ(json::parse(of_edit->front().detail_json.value()).at("rewrites")[0].at("changed").at("aliquot"),
              json({{"old", 2}, {"new", 7}}));
    const auto rows = snapshot_of(*world);
    if (first.empty())
      first = rows;
    else
      EXPECT_EQ(rows, first);
  }
}

TEST_P(ProjectImportTest, RewritesOfACommitSplitByABatchAreAllKept) {
  // One commit rewrites the record of E (collected: reported at once) and of
  // P (pending: reported when P is folded, a batch later).
  legacy_.collect(kRunE, kE.str(), kCollected);
  const Uuid kP = *Uuid::parse("66666666-6666-4666-8666-666666666666");
  legacy_.write_record_files("66052-03A", kP.str());
  legacy_.commit("<COLLECTION>", kDay2);
  auto of_e = json::parse(LegacyRepoBuilder::record_text(kRunE, kE.str()));
  of_e["sample"] = "renamed by a sync";
  legacy_.write(kRunE, FileKind::Record, of_e.dump(4));
  auto of_p = json::parse(LegacyRepoBuilder::record_text("66052-03A", kP.str()));
  of_p["sample"] = "renamed by a sync";
  legacy_.write("66052-03A", FileKind::Record, of_p.dump(4));
  const std::string sync = legacy_.commit("<SYNC> Synced repository with database", kDay2);
  for (const FileKind kind : {FileKind::Intercepts, FileKind::Baselines, FileKind::Blanks, FileKind::IcFactors})
    legacy_.write("66052-03A", kind, LegacyRepoBuilder::fixture_text(kind));
  legacy_.commit("<ISOEVO> default collection fits", kRefit);

  std::string first;
  for (const int batch_commits : {500, 1}) {
    auto world = fresh_world();
    auto stats = run_import(*world, adapter_config(repo_, batch_commits));
    ASSERT_TRUE(stats) << err(stats.error());
    const auto rows = world->store->provenance_for(ingest::changeset_id(kUrl, sync));
    ASSERT_TRUE(rows);
    ASSERT_EQ(rows->size(), 1u);
    const json detail = json::parse(rows->front().detail_json.value_or("{}"));
    ASSERT_EQ(detail.value("rewrites", json::array()).size(), 2u) << batch_commits << ": " << detail.dump();
    EXPECT_EQ(detail.at("rewrites")[0].at("path"), LegacyRepoBuilder::path(kRunE, FileKind::Record));
    EXPECT_EQ(detail.at("rewrites")[1].at("path"), LegacyRepoBuilder::path("66052-03A", FileKind::Record));
    EXPECT_EQ(detail.at("rewrites")[1].at("changed").at("sample").at("new"), "renamed by a sync");
    if (first.empty())
      first = detail.dump();
    else
      EXPECT_EQ(detail.dump(), first);

    // A second run, and a replay, change nothing.
    const auto seq = *world->store->latest_change_seq();
    ASSERT_TRUE(run_import(*world, adapter_config(repo_, batch_commits)));
    auto replay = writer_config();
    replay.replay = true;
    ASSERT_TRUE(run_import(*world, adapter_config(repo_, 2), std::nullopt, replay));
    EXPECT_EQ(*world->store->latest_change_seq(), seq);
    EXPECT_EQ(json::parse(world->store->provenance_for(ingest::changeset_id(kUrl, sync))->front().detail_json.value())
                  .dump(),
              first);
  }
}

TEST_P(ProjectImportTest, ReplayAfterAFileWasRestoredAndThenChangedWritesNothing) {
  // The intercepts are removed, restored with what they had, then refit.
  legacy_.collect(kRunE, kE.str(), kCollected);
  repo_.remove(LegacyRepoBuilder::path(kRunE, FileKind::Intercepts));
  legacy_.commit("removed by hand", kDay2);
  legacy_.write(kRunE, FileKind::Intercepts, LegacyRepoBuilder::fixture_text(FileKind::Intercepts));
  legacy_.commit("restored", kDay2);
  legacy_.refit(kRunE, "Ar40", 12.5, kRefit);
  ASSERT_TRUE(run_import(*world_, adapter_config(repo_)));
  ASSERT_EQ(store().history(kE, Kind::Intercepts)->size(), 2u);
  ASSERT_EQ(head_intercept(*world_, kE, "Ar40"), std::optional<double>{12.5});
  const auto seq = *store().latest_change_seq();
  const auto rows = snapshot_of(*world_);

  // Replayed, the restore is met in a batch of its own: the store's head is
  // by then the refit, which says nothing about what the path held at the
  // restore.
  auto replay = writer_config();
  replay.replay = true;
  for (const int batch_commits : {1, 2, 3, 500}) {
    auto stats = run_import(*world_, adapter_config(repo_, batch_commits), std::nullopt, replay);
    ASSERT_TRUE(stats) << batch_commits << ": " << err(stats.error());
    EXPECT_EQ(*store().latest_change_seq(), seq) << batch_commits;
    EXPECT_EQ(snapshot_of(*world_), rows) << batch_commits;
    EXPECT_EQ(head_intercept(*world_, kE, "Ar40"), std::optional<double>{12.5}) << batch_commits;
    EXPECT_EQ(store().history(kE, Kind::Intercepts)->size(), 2u) << batch_commits;
  }
  // The same history imported in batches of one from the start.
  auto other = fresh_world();
  ASSERT_TRUE(run_import(*other, adapter_config(repo_, 1)));
  EXPECT_EQ(snapshot_of(*other), rows);
}

TEST_P(ProjectImportTest, GoodContentAfterAnUnreadableVersionIsARevisionAtAnyCut) {
  // Refit, then garbage, then the refit's content again. The garbage is a
  // conflict; the file after it differs from the version before it, so it is
  // a revision, whatever the store's head holds when it is met.
  legacy_.collect(kRunE, kE.str(), kCollected);
  legacy_.refit(kRunE, "Ar40", 12.5, kDay2);
  legacy_.write(kRunE, FileKind::Intercepts, "{\"Ar40\": ");
  legacy_.commit("<ISOEVO> broken", kRefit);
  const std::string mended = legacy_.refit(kRunE, "Ar40", 12.5, kLater);

  std::vector<std::string> first;
  for (const int batch_commits : {500, 1}) {
    auto world = fresh_world();
    auto stats = run_import(*world, adapter_config(repo_, batch_commits));
    ASSERT_TRUE(stats) << err(stats.error());
    auto history = world->store->history(kE, Kind::Intercepts);
    ASSERT_EQ(history->size(), 3u) << batch_commits;
    EXPECT_EQ(history->back().uuid,
              ingest::revision_id(kUrl, mended, LegacyRepoBuilder::path(kRunE, FileKind::Intercepts)));
    EXPECT_EQ(world->conflicts(ConflictKind::Unparseable).size(), 1u);
    const auto rows = snapshot_of(*world);
    if (first.empty())
      first = rows;
    else
      EXPECT_EQ(rows, first);
    const auto seq = *world->store->latest_change_seq();
    auto replay = writer_config();
    replay.replay = true;
    ASSERT_TRUE(run_import(*world, adapter_config(repo_, batch_commits == 1 ? 500 : 1), std::nullopt, replay));
    EXPECT_EQ(*world->store->latest_change_seq(), seq) << batch_commits;
    EXPECT_EQ(snapshot_of(*world), rows) << batch_commits;
  }
}

TEST_P(ProjectImportTest, FileRemovedAndRestoredAddsNothingAtAnyCut) {
  legacy_.collect(kRunE, kE.str(), kCollected);
  legacy_.refit(kRunE, "Ar40", 12.5, kDay2);
  for (const FileKind kind : {FileKind::Intercepts, FileKind::Baselines, FileKind::Extraction})
    repo_.remove(LegacyRepoBuilder::path(kRunE, kind));
  legacy_.commit("removed by hand", kRefit);
  legacy_.write(kRunE, FileKind::Intercepts, LegacyRepoBuilder::intercepts_text("Ar40", 12.5));
  legacy_.write(kRunE, FileKind::Baselines, LegacyRepoBuilder::fixture_text(FileKind::Baselines));
  legacy_.write(kRunE, FileKind::Extraction, LegacyRepoBuilder::fixture_text(FileKind::Extraction));
  legacy_.commit("restored", kLater);

  std::vector<std::string> first;
  for (const int batch_commits : {500, 5, 1}) {
    auto world = fresh_world();
    auto stats = run_import(*world, adapter_config(repo_, batch_commits));
    ASSERT_TRUE(stats) << err(stats.error());
    EXPECT_EQ(world->store->history(kE, Kind::Intercepts)->size(), 2u) << batch_commits;
    EXPECT_EQ(world->store->history(kE, Kind::Baselines)->size(), 1u) << batch_commits;
    EXPECT_EQ(world->count("changeset"), 2) << batch_commits;  // the collection and the refit
    EXPECT_TRUE(world->conflicts().empty());
    const auto rows = snapshot_of(*world);
    if (first.empty())
      first = rows;
    else
      EXPECT_EQ(rows, first) << batch_commits;
  }
}

TEST_P(ProjectImportTest, TagInsideAPendingCollectionDoesNotStallAStoppedRun) {
  // The tag sits on the record commit of a collection that is not complete.
  legacy_.write_record_files(kRunE, kE.str());
  const std::string tagged = legacy_.commit("<COLLECTION>", kCollected);
  repo_.tag("mid-collection");
  for (const FileKind kind : {FileKind::Intercepts, FileKind::Baselines, FileKind::Blanks, FileKind::IcFactors})
    legacy_.write(kRunE, kind, LegacyRepoBuilder::fixture_text(kind));
  legacy_.commit("<ISOEVO> default collection fits", kCollected);
  legacy_.refit(kRunE, "Ar40", 12.5, kRefit);

  // One batch per run: the first ends at the tag with the analysis pending,
  // and still moves the token.
  auto first = run_import(*world_, adapter_config(repo_), 1);
  ASSERT_TRUE(first) << err(first.error());
  EXPECT_EQ(world_->count("analysis"), 0);
  EXPECT_EQ(world_->source().progress_token, std::optional<std::string>{token_at(repo_, tagged, 0)});
  int runs = 1;
  for (bool finished = false; !finished && runs < 10; ++runs) {
    auto stats = run_import(*world_, adapter_config(repo_), 1);
    ASSERT_TRUE(stats) << err(stats.error());
    finished = stats->finished;
  }
  EXPECT_LE(runs, 4);
  EXPECT_EQ(store().history(kE, Kind::Intercepts)->size(), 2u);
  // Nothing was imported yet at the tagged commit: no bookmark, as in one run.
  auto other = fresh_world();
  ASSERT_TRUE(run_import(*other, adapter_config(repo_)));
  EXPECT_EQ(snapshot_of(*world_), snapshot_of(*other));
}

TEST_P(ProjectImportTest, RenumberToAnIdentifierTheCatalogLacks) {
  legacy_.collect(kRunE, kE.str(), kCollected);
  repo_.write(LegacyRepoBuilder::path(kRunE, FileKind::Record), LegacyRepoBuilder::record_text("66099-01A", kE.str()));
  const std::string edit = legacy_.commit("<EDIT> RunID", kDay2);

  // With a catalog that lacks 66099 the renumber waits for it.
  auto stats = run_import(*world_, adapter_config(repo_));
  ASSERT_TRUE(stats) << err(stats.error());
  EXPECT_EQ((*store().load_analysis(kE))->summary.runid, kRunE);
  auto waiting = store().import_conflict(
      ingest::conflict_id(kUrl, edit, LegacyRepoBuilder::path(kRunE, FileKind::Record)));
  ASSERT_TRUE(waiting && waiting->has_value());
  EXPECT_EQ((*waiting)->kind, ConflictKind::UnknownAnalysis);
  EXPECT_EQ((*waiting)->resolution, "pending");

  // Without a catalog the identifier is made up from the record, as for a
  // collected analysis, and reported the same way.
  auto bare = fresh_world(false);
  auto config = adapter_config(repo_);
  config.catalog_from_repos = true;
  auto made = run_import(*bare, config);
  ASSERT_TRUE(made) << err(made.error());
  EXPECT_EQ((*bare->store->load_analysis(kE))->summary.runid, "66099-01A");
  EXPECT_TRUE(bare->store->find_identifier("66099")->has_value());
  EXPECT_TRUE(bare->conflicts(ConflictKind::UnknownAnalysis).empty());
  const auto synthesized = bare->conflicts(ConflictKind::IdentityClash);
  ASSERT_EQ(synthesized.size(), 2u);  // 66052 and 66099
  EXPECT_TRUE(bare->store->import_conflict(ingest::conflict_id(kUrl, "", "catalog/identifier/66099"))->has_value());
}

// Spec 10.31. The renumber to 66099 is refused, a later one to a known
// identifier is stored. With 66099 in the catalog a replay could write the
// first: it would take the analysis back to a run id it has since left.
TEST_P(ProjectImportTest, ReplayDoesNotApplyARefusedRenumberBehindALaterOne) {
  legacy_.collect(kRunE, kE.str(), kCollected);
  const std::string path = LegacyRepoBuilder::path(kRunE, FileKind::Record);
  repo_.write(path, LegacyRepoBuilder::record_text("66099-01A", kE.str()));
  const std::string refused = legacy_.commit("<EDIT> RunID", kDay2);
  repo_.write(path, LegacyRepoBuilder::record_text("66052-07B", kE.str()));
  const std::string stored = legacy_.commit("<EDIT> RunID", kRefit);

  ASSERT_TRUE(run_import(*world_, adapter_config(repo_)));
  EXPECT_EQ((*store().load_analysis(kE))->summary.runid, "66052-07B");
  const Uuid head = ingest::revision_id(kUrl, stored, path);
  ASSERT_EQ(*store().head(kE, Kind::Identity), std::optional<Uuid>{head});
  const Uuid conflict = ingest::conflict_id(kUrl, refused, path);
  EXPECT_EQ((**store().import_conflict(conflict)).kind, ConflictKind::UnknownAnalysis);

  P::IdentifierSpec identifier;
  identifier.identifier = "66099";
  ASSERT_TRUE(store().add_identifier(world_->client, identifier));
  const auto seq = *store().latest_change_seq();

  auto replay = writer_config();
  replay.replay = true;
  for (const std::optional<int> batch : {std::optional<int>{1}, std::optional<int>{}}) {
    auto config = adapter_config(repo_);
    if (batch) config.batch_commits = *batch;
    auto stats = run_import(*world_, config, std::nullopt, replay);
    ASSERT_TRUE(stats) << err(stats.error());
    EXPECT_TRUE(stats->finished);
    EXPECT_EQ((*store().load_analysis(kE))->summary.runid, "66052-07B");
    EXPECT_EQ(*store().head(kE, Kind::Identity), std::optional<Uuid>{head});
    EXPECT_EQ(store().history(kE, Kind::Identity)->size(), 1u);
    EXPECT_EQ(*store().latest_change_seq(), seq);
    const auto conflicts = world_->conflicts();
    ASSERT_EQ(conflicts.size(), 1u);
    EXPECT_EQ(conflicts[0].uuid, conflict);
    EXPECT_EQ(conflicts[0].kind, ConflictKind::IdentityClash);
    EXPECT_EQ(conflicts[0].resolution, "pending");
    const json detail = json::parse(conflicts[0].detail_json);
    EXPECT_EQ(detail.at("reason"), "late_revision_not_applied");
    EXPECT_EQ(detail.at("commit"), refused);
    EXPECT_EQ(detail.at("path"), path);
    EXPECT_EQ(detail.at("kind"), "identity");
    EXPECT_EQ(detail.at("content").at("identifier"), "66099");
    EXPECT_EQ(detail.at("content").at("aliquot"), 1);
  }
}

TEST_P(ProjectImportTest, LargeRewrittenValueIsKeptByReference) {
  legacy_.collect(kRunE, kE.str(), kCollected);
  auto extraction = json::parse(LegacyRepoBuilder::fixture_text(FileKind::Extraction));
  const std::string small(64 * 1024 - 2, 'x');  // 64 KiB with its quotes: kept
  const std::string large(64 * 1024 - 1, 'y');  // one byte more: a reference
  extraction["measured_response"] = small;
  extraction["requested_output"] = large;
  legacy_.write(kRunE, FileKind::Extraction, extraction.dump(4));
  const std::string edit = legacy_.commit("<MANUAL> response", kDay2);
  const std::string blob =
      repo_.git({"rev-parse", "HEAD:" + LegacyRepoBuilder::path(kRunE, FileKind::Extraction)}).substr(0, 40);
  auto stats = run_import(*world_, adapter_config(repo_));
  ASSERT_TRUE(stats) << err(stats.error());

  const json changed =
      json::parse(store().provenance_for(ingest::changeset_id(kUrl, edit))->front().detail_json.value())
          .at("rewrites")[0]
          .at("changed");
  EXPECT_EQ(changed.at("measured_response").at("new"), small);
  EXPECT_TRUE(changed.at("measured_response").at("old").is_string());
  EXPECT_EQ(changed.at("requested_output").at("new"), json({{"blob_sha", blob}, {"bytes", 64 * 1024 + 1}}));
  EXPECT_TRUE(changed.at("requested_output").at("old").is_string());  // the fixture's is small
}

// ---------------------------------------------------------------- one history, one result

namespace {

const Uuid kL = *Uuid::parse("55555555-5555-4555-8555-555555555555");
const Uuid kP = *Uuid::parse("66666666-6666-4666-8666-666666666666");
const Uuid kH = *Uuid::parse("77777777-7777-4777-8777-777777777777");

// The commits a test needs to name.
struct History {
  std::string sync;  // rewrites the record of E (collected) and of P (still pending)
};

// One fixed history with everything that has gone wrong at a batch boundary.
// Part 1 ends with nothing pending; part 2 is what a later run finds.
void build_part_one(GitFixture& repo, LegacyRepoBuilder& legacy, History& history) {
  repo.write("README.md", "# IR1010\n");
  legacy.commit("Initial commit", "2018-02-16T10:00:00-07:00");
  // L never gets blanks or IC factors during part 1: folded by the bounded wait.
  legacy.import_without_collection("66052-04A", kL.str(), "2018-02-19T10:00:00-07:00", false);
  legacy.collect(kRunE, kE.str(), kCollected);
  legacy.collect("66052-02A", kF.str(), kDay2);
  legacy.refit(kRunE, "Ar40", 12.5, kDay2);
  // F is renumbered; its old run id is taken by another analysis right after.
  repo.write(LegacyRepoBuilder::path("66052-02A", FileKind::Record),
             LegacyRepoBuilder::record_text("66052-07B", kF.str()));
  legacy.commit("<EDIT> RunID", kDay2);
  write_uuid_named(repo, kG, "66052-02A");
  legacy.commit("<IMPORT> initial", kDay2);
  // P's record arrives together with a refit of E; a tag sits on that commit.
  legacy.write_record_files("66052-03A", kP.str());
  legacy.write(kRunE, FileKind::Intercepts, LegacyRepoBuilder::intercepts_text("Ar39", 3.25));
  legacy.commit("<COLLECTION>", kRefit);
  repo.tag("while-pending");
  // One commit rewrites E's record (collected), P's record and P's
  // extraction (both still pending), and refits F.
  auto of_e = json::parse(LegacyRepoBuilder::record_text(kRunE, kE.str()));
  of_e["sample"] = "renamed by a sync";
  legacy.write(kRunE, FileKind::Record, of_e.dump(4));
  auto of_p = json::parse(LegacyRepoBuilder::record_text("66052-03A", kP.str()));
  of_p["project"] = "renamed by a sync";
  legacy.write("66052-03A", FileKind::Record, of_p.dump(4));
  auto extraction = json::parse(LegacyRepoBuilder::fixture_text(FileKind::Extraction));
  extraction["extract_value"] = 5.0;
  legacy.write("66052-03A", FileKind::Extraction, extraction.dump(4));
  legacy.write("66052-02A", FileKind::Intercepts, LegacyRepoBuilder::intercepts_text("Ar38", 8.5));
  history.sync = legacy.commit("<SYNC> Synced repository with database", kRefit);
  legacy.write("66052-03A", FileKind::Intercepts, LegacyRepoBuilder::fixture_text(FileKind::Intercepts));
  legacy.write("66052-03A", FileKind::Baselines, LegacyRepoBuilder::fixture_text(FileKind::Baselines));
  legacy.commit("<ISOEVO> default collection fits", kRefit);
  legacy.write("66052-03A", FileKind::Blanks, LegacyRepoBuilder::fixture_text(FileKind::Blanks));
  legacy.commit("<BLANKS> preceding bu-FD-F-789", kRefit);
  legacy.write("66052-03A", FileKind::IcFactors, LegacyRepoBuilder::fixture_text(FileKind::IcFactors));
  legacy.commit("<ICFactor> default", kRefit);
  // A merge that keeps the first parent's version, and one that keeps the other's.
  repo.branch("side");
  legacy.refit(kRunE, "Ar40", 11.0, kLater);
  repo.checkout("side");
  legacy.refit(kRunE, "Ar40", 22.0, kLater);
  repo.checkout("main");
  repo.git({"merge", "--quiet", "--no-ff", "-s", "ours", "-m", "Merge branch 'side'", "side"}, kLater);
  repo.branch("side2");
  legacy.refit("66052-03A", "Ar36", 5.0, kLater);
  repo.checkout("side2");
  legacy.refit("66052-03A", "Ar36", 6.0, kLater);
  repo.checkout("main");
  repo.git({"merge", "--quiet", "--no-ff", "-X", "theirs", "-m", "Merge branch 'side2'", "side2"}, kLater);
  // A root file and a satellite file removed, added again with what they
  // had, and changed in a later commit.
  const std::string extraction_path = LegacyRepoBuilder::path("66052-03A", FileKind::Extraction);
  repo.remove(LegacyRepoBuilder::path("66052-03A", FileKind::Baselines));
  repo.remove(extraction_path);
  legacy.commit("removed by hand", kLater);
  legacy.write("66052-03A", FileKind::Baselines, LegacyRepoBuilder::fixture_text(FileKind::Baselines));
  repo.write(extraction_path, extraction.dump(4));
  legacy.commit("restored", kLater);
  auto baselines = json::parse(LegacyRepoBuilder::fixture_text(FileKind::Baselines));
  baselines.at("H1")["value"] = 0.125;
  legacy.write("66052-03A", FileKind::Baselines, baselines.dump(4));
  extraction["extract_value"] = 6.0;
  repo.write(extraction_path, extraction.dump(4));
  legacy.commit("<ISOEVO> fits=H1(Average)", kLater);
  repo.tag("part-one");
}

void build_part_two(GitFixture&, LegacyRepoBuilder& legacy) {
  const char* const later = "2019-01-10T10:00:00-07:00";
  legacy.collect("66052-05A", kH.str(), later);
  // L's blanks arrive long after it was folded without them.
  legacy.write("66052-04A", FileKind::Blanks, LegacyRepoBuilder::fixture_text(FileKind::Blanks));
  legacy.commit("<BLANKS> auto update blanks", later);
  legacy.refit("66052-05A", "Ar40", 9.75, later);
}

// Where two snapshots first differ, for a readable failure.
std::string first_difference(const std::vector<std::string>& got, const std::vector<std::string>& want) {
  for (std::size_t i = 0; i < std::max(got.size(), want.size()); ++i) {
    const std::string a = i < got.size() ? got[i] : "(nothing)";
    const std::string b = i < want.size() ? want[i] : "(nothing)";
    if (a != b) return "line " + std::to_string(i) + "\n  got:  " + a.substr(0, 600) + "\n  want: " + b.substr(0, 600);
  }
  return {};
}

}  // namespace

TEST_P(ProjectImportTest, OneHistoryOneResult) {
  History history;
  build_part_one(repo_, legacy_, history);

  // An incremental import: part one now, part two when it exists.
  auto incremental = fresh_world();
  auto half = run_import(*incremental, adapter_config(repo_));
  ASSERT_TRUE(half) << err(half.error());
  build_part_two(repo_, legacy_);
  auto rest = run_import(*incremental, adapter_config(repo_));
  ASSERT_TRUE(rest) << err(rest.error());

  // The reference: the whole history in one uninterrupted batch.
  auto whole = run_import(*world_, adapter_config(repo_));
  ASSERT_TRUE(whole) << err(whole.error());
  const auto want = snapshot_of(*world_);
  ASSERT_FALSE(want.empty());

  // What the reference must hold, whatever else it holds.
  EXPECT_EQ((*store().load_analysis(kF))->summary.runid, "66052-07B");
  ASSERT_TRUE(store().load_analysis(kG)->has_value());
  EXPECT_EQ((*store().load_analysis(kG))->summary.runid, "66052-02A");  // the freed run id
  for (const auto& conflict : world_->conflicts()) ADD_FAILURE() << conflict.path << " " << conflict.detail_json;
  EXPECT_EQ(head_intercept(*world_, kE, "Ar40"), std::optional<double>{11.0});  // the merge kept main's
  EXPECT_EQ(head_intercept(*world_, kP, "Ar36"), std::optional<double>{6.0});   // the merge kept the side's
  EXPECT_EQ(store().history(kP, Kind::Baselines)->size(), 2u);  // removed and restored: nothing; then changed
  EXPECT_EQ(store().history(kL, Kind::Blanks)->size(), 2u);                     // the late file is a revision
  EXPECT_EQ(json::parse(world_->analysis_detail(kL)).at("synthetic_collection"), true);
  EXPECT_EQ(world_->count("bookmark"), 2);
  // All three rewrites of the <SYNC> commit, though two were held until P was folded.
  const auto of_sync = store().provenance_for(ingest::changeset_id(kUrl, history.sync));
  ASSERT_TRUE(of_sync);
  ASSERT_EQ(of_sync->size(), 1u);
  const json rewrites = json::parse(of_sync->front().detail_json.value_or("{}")).value("rewrites", json::array());
  std::vector<std::string> rewritten;
  for (const auto& entry : rewrites) rewritten.push_back(entry.at("path").get<std::string>());
  EXPECT_EQ(rewritten, (std::vector<std::string>{LegacyRepoBuilder::path(kRunE, FileKind::Record),
                                                 LegacyRepoBuilder::path("66052-03A", FileKind::Record),
                                                 LegacyRepoBuilder::path("66052-03A", FileKind::Extraction)}));

  const auto same = [&](World& w, const std::string& what) {
    const auto got = snapshot_of(w);
    EXPECT_TRUE(got == want) << what << ": " << first_difference(got, want);
  };
  const auto replayed = [&](World& w, const std::string& what, int batch_commits) {
    auto replay = writer_config();
    replay.replay = true;
    const auto seq = *w.store->latest_change_seq();
    auto again = run_import(w, adapter_config(repo_, batch_commits), std::nullopt, replay);
    ASSERT_TRUE(again) << what << ": " << err(again.error());
    EXPECT_EQ(*w.store->latest_change_seq(), seq) << what << ": the replay wrote something";
    same(w, what + ", replayed");
  };

  replayed(*world_, "one batch", 500);
  replayed(*world_, "one batch, replayed in batches of 2", 2);

  // Analyses still incomplete when an incremental run ends are folded then
  // (a known limit); this history has none at the end of part one.
  same(*incremental, "part one, then part two");
  replayed(*incremental, "part one, then part two", 3);

  for (const int batch_commits : {1, 2, 3, 7}) {
    const std::string what = "batches of " + std::to_string(batch_commits);
    auto cut = fresh_world();
    auto stats = run_import(*cut, adapter_config(repo_, batch_commits));
    ASSERT_TRUE(stats) << what << ": " << err(stats.error());
    EXPECT_TRUE(stats->finished);
    same(*cut, what);
    replayed(*cut, what, batch_commits == 1 ? 500 : 1);

    // Stopped after every batch; each run is a new adapter resuming from the stored token.
    auto resumed = fresh_world();
    bool finished = false;
    int runs = 0;
    for (; !finished && runs < 200; ++runs) {
      auto one = run_import(*resumed, adapter_config(repo_, batch_commits), 1);
      ASSERT_TRUE(one) << what << ", run " << runs << ": " << err(one.error());
      finished = one->finished;
    }
    EXPECT_TRUE(finished) << what << ": no end after " << runs << " runs";
    same(*resumed, what + ", resumed after every batch");
    for (const int replay_commits : {1, 2, 3, 7, 500})
      replayed(*resumed, what + ", resumed after every batch, replay in " + std::to_string(replay_commits),
               replay_commits);
  }
}

// ---------------------------------------------------------------- verify

namespace {

ingest::VerifyReport verify_repo(World& w, const ProjectAdapterConfig& config,
                                 const ingest::AgeFn& age_fn = no_ages()) {
  auto adapter = ProjectRepoAdapter::open(config);
  EXPECT_TRUE(adapter) << (adapter ? "" : err(adapter.error()));
  return adapter ? verify_source(w, **adapter, age_fn) : ingest::VerifyReport{};
}

}  // namespace

// The whole of OneHistoryOneResult's history, and then an interpreted age, a
// run log and a file that is nothing: every file of every commit is accounted
// for, whatever the batch size of the import or of the walk that verifies it.
TEST_P(ProjectImportTest, VerifyAfterImportIsOk) {
  History history;
  build_part_one(repo_, legacy_, history);
  build_part_two(repo_, legacy_);
  legacy_.add_interpreted_age("2019-02-01T10:00:00-07:00");
  repo_.write("660/logs/52-05A.logs.log", "run log\n");
  repo_.write("notes.txt", "not a legacy file\n");
  const std::string stray = legacy_.commit("by hand", "2019-02-02T10:00:00-07:00");
  auto imported = run_import(*world_, adapter_config(repo_, 4));
  ASSERT_TRUE(imported) << err(imported.error());
  const auto rows = snapshot_of(*world_);
  const auto seq = *store().latest_change_seq();
  const Uuid source = world_->source().spec.uuid;

  std::optional<ingest::VerifyReport> first;
  for (const int batch_commits : {1, 3, 500}) {
    const auto report = verify_repo(*world_, adapter_config(repo_, batch_commits));
    EXPECT_EQ(unaccounted(report), std::vector<std::string>{}) << batch_commits;
    for (const auto& open : report.unaccounted)
      ADD_FAILURE() << batch_commits << ": " << open.unit.commit << " " << open.unit.path << " disposition "
                    << static_cast<int>(open.unit.disposition) << ", " << open.missing.size() << " missing";
    EXPECT_EQ(report.would_write, 0) << batch_commits;
    EXPECT_EQ(report.replay_would_write, 0) << batch_commits;
    // notes.txt is a pending conflict: that alone fails verify.
    EXPECT_EQ(report.pending_blocking, 1) << batch_commits;
    EXPECT_EQ(report.blocking_conflicts, std::vector<Uuid>{ingest::conflict_id(kUrl, stray, "notes.txt")});
    EXPECT_EQ(report.pending_warnings, 0);
    EXPECT_FALSE(report.ok());
    EXPECT_EQ(report.ignored, 2) << batch_commits;  // README.md and the run log
    EXPECT_GT(report.units, 60) << batch_commits;
    // The interpreted age: its members are not reduced here, and most are in no repository this store has.
    EXPECT_EQ(report.parity_pass, 0);
    EXPECT_EQ(report.parity_fail, 0);
    EXPECT_GT(report.parity_not_comparable, 1) << batch_commits;
    EXPECT_EQ(report.not_comparable_reasons.count("not reduced in this test"), 1u);
    EXPECT_EQ(report.not_comparable_reasons.count("analysis is not in the store"), 1u);
    if (first) {
      EXPECT_EQ(report.units, first->units) << batch_commits;
      EXPECT_EQ(report.parity_not_comparable, first->parity_not_comparable) << batch_commits;
      EXPECT_EQ(report.not_comparable_reasons, first->not_comparable_reasons) << batch_commits;
    }
    first = report;
  }
  const auto after = snapshot_of(*world_);
  EXPECT_TRUE(after == rows) << "verify wrote something: " << first_difference(after, rows);
  EXPECT_EQ(*store().latest_change_seq(), seq);
  EXPECT_EQ(world_->source().status, "finished");

  resolve_pending(*world_, source);
  EXPECT_TRUE(verify_repo(*world_, adapter_config(repo_)).ok());

  // The same history imported in one batch verifies the same.
  auto whole = fresh_world();
  ASSERT_TRUE(run_import(*whole, adapter_config(repo_)));
  const auto other = verify_repo(*whole, adapter_config(repo_, 2));
  EXPECT_EQ(unaccounted(other), std::vector<std::string>{});
  EXPECT_EQ(other.units, first->units);
  EXPECT_EQ(other.replay_would_write, 0);
}

// Take one row out of the store: verify names the file it came from.
TEST_P(ProjectImportTest, VerifyReportsAMissingRevisionAnalysisOrConflict) {
  const auto collected = legacy_.collect(kRunE, kE.str(), kCollected);
  const std::string refit = legacy_.refit(kRunE, "Ar40", 12.5, kRefit);
  repo_.write("notes.txt", "not a legacy file\n");
  const std::string stray = legacy_.commit("by hand", kLater);
  ASSERT_TRUE(run_import(*world_, adapter_config(repo_)));
  const auto path = [](FileKind kind) { return LegacyRepoBuilder::path(kRunE, kind); };
  const std::string spectrometer = std::string(LegacyRepoBuilder::kSpecSha) + ".json";

  auto report = verify_repo(*world_, adapter_config(repo_, 2));
  EXPECT_EQ(unaccounted(report), std::vector<std::string>{});
  EXPECT_EQ(report.units, 10);  // record, data, extraction, spectrometer, four reduced files, the refit, the note

  // A revision's provenance row.
  ASSERT_EQ(forget(*world_, "DELETE FROM import_provenance WHERE commit_sha = ? AND path = ?",
                   {pd::qv(refit), pd::qv(path(FileKind::Intercepts))}),
            1);
  report = verify_repo(*world_, adapter_config(repo_, 2));
  EXPECT_EQ(unaccounted(report), std::vector<std::string>{unit_name(refit, path(FileKind::Intercepts))});
  EXPECT_EQ(report.replay_would_write, 1);

  // A conflict row.
  ASSERT_EQ(forget(*world_, "DELETE FROM import_conflict WHERE path = 'notes.txt'"), 1);
  report = verify_repo(*world_, adapter_config(repo_, 2));
  EXPECT_EQ(unaccounted(report),
            sorted({unit_name(refit, path(FileKind::Intercepts)), unit_name(stray, "notes.txt")}));
  EXPECT_EQ(report.pending_blocking, 0);
  EXPECT_FALSE(report.ok());

  // The row of the analysis: the host of what was folded into it. Its
  // record, its extraction file and the spectrometer file it names go with it;
  // the files that are revisions of their own do not.
  ASSERT_EQ(forget(*world_, "DELETE FROM import_provenance WHERE entity_type = 'analysis'"), 1);
  report = verify_repo(*world_, adapter_config(repo_, 2));
  EXPECT_EQ(unaccounted(report),
            sorted({unit_name(refit, path(FileKind::Intercepts)), unit_name(stray, "notes.txt"),
                    unit_name(collected.collection, path(FileKind::Record)),
                    unit_name(collected.collection, path(FileKind::Extraction)),
                    unit_name(collected.collection, spectrometer)}));
  EXPECT_EQ(unaccounted_unit(report, collected.collection, path(FileKind::Extraction)).unit.disposition,
            ingest::UnitDisposition::Folded);
}

// A file that goes X, Y, X has three revisions. The third has the content of
// the first: a row for that blob at another commit must not stand in for it.
TEST_P(ProjectImportTest, VerifyReportsARevisionDroppedFromAnXYXHistory) {
  legacy_.collect(kRunE, kE.str(), kCollected);
  legacy_.refit(kRunE, "Ar40", 12.5, kDay2);
  legacy_.write(kRunE, FileKind::Intercepts, LegacyRepoBuilder::fixture_text(FileKind::Intercepts));
  const std::string back = legacy_.commit("<ISOEVO> back to the collection fits", kRefit);
  const std::string path = LegacyRepoBuilder::path(kRunE, FileKind::Intercepts);
  ASSERT_TRUE(run_import(*world_, adapter_config(repo_)));
  ASSERT_EQ(store().history(kE, Kind::Intercepts)->size(), 3u);
  EXPECT_EQ(unaccounted(verify_repo(*world_, adapter_config(repo_, 1))), std::vector<std::string>{});

  ASSERT_EQ(forget(*world_, "DELETE FROM import_provenance WHERE commit_sha = ? AND path = ?",
                   {pd::qv(back), pd::qv(path)}),
            1);
  for (const int batch_commits : {1, 500}) {
    const auto report = verify_repo(*world_, adapter_config(repo_, batch_commits));
    EXPECT_EQ(unaccounted(report), std::vector<std::string>{unit_name(back, path)}) << batch_commits;
    EXPECT_FALSE(report.ok());
  }
}

// A spectrometer file that first appears after the analysis naming it was
// folded cannot become its snapshot: it is a conflict, at any cut, and verify
// lists the file when that conflict is missing. One no record names is ignored.
TEST_P(ProjectImportTest, LateSpectrometerFileIsAConflict) {
  write_uuid_named(repo_, kF, "66052-03B");  // names kSpecSha; the file is not there
  legacy_.commit("<IMPORT> initial", kCollected);
  legacy_.refit(kRunE, "Ar40", 12.5, kDay2);  // a file of an analysis without a record, for one more commit
  const std::string spectrometer = std::string(LegacyRepoBuilder::kSpecSha) + ".json";
  repo_.write(spectrometer, fixture(kUnknown + spectrometer));
  const std::string unnamed = std::string(40, 'a') + ".json";
  repo_.write(unnamed, fixture(kUnknown + spectrometer));
  const std::string late = legacy_.commit("settings, late", kRefit);

  std::vector<std::string> first;
  for (const int batch_commits : {500, 1}) {
    auto world = fresh_world();
    auto stats = run_import(*world, adapter_config(repo_, batch_commits));
    ASSERT_TRUE(stats) << err(stats.error());
    const auto conflicts = world->conflicts(ConflictKind::Unparseable);
    ASSERT_EQ(conflicts.size(), 1u) << batch_commits;
    EXPECT_EQ(conflicts[0].uuid, ingest::conflict_id(kUrl, late, spectrometer));
    const json detail = json::parse(conflicts[0].detail_json);
    EXPECT_EQ(detail.at("reason"), "spectrometer_file_after_collection");
    EXPECT_EQ(detail.at("analyses"), json::array({kF.str()}));
    // The analysis was stored without the snapshot, and says which it lacks.
    EXPECT_EQ(json::parse(world->analysis_detail(kF)).at("spectrometer_file_unavailable"),
              std::string(LegacyRepoBuilder::kSpecSha))
        << batch_commits;
    const auto rows = snapshot_of(*world);
    if (first.empty())
      first = rows;
    else
      EXPECT_TRUE(rows == first) << batch_commits << ": " << first_difference(rows, first);

    // A replay writes nothing.
    auto replay = writer_config();
    replay.replay = true;
    const auto seq = *world->store->latest_change_seq();
    ASSERT_TRUE(run_import(*world, adapter_config(repo_, batch_commits == 1 ? 500 : 1), std::nullopt, replay));
    EXPECT_EQ(*world->store->latest_change_seq(), seq) << batch_commits;

    auto report = verify_repo(*world, adapter_config(repo_, batch_commits == 1 ? 500 : 1));
    EXPECT_EQ(unaccounted(report), std::vector<std::string>{}) << batch_commits;
    EXPECT_EQ(report.replay_would_write, 0);
    EXPECT_EQ(report.ignored, 1) << "the settings file no record names";
    EXPECT_FALSE(report.ok());  // the late file and the record-less intercepts are pending conflicts
    ASSERT_EQ(forget(*world, "DELETE FROM import_conflict WHERE path = ?", {pd::qv(spectrometer)}), 1);
    report = verify_repo(*world, adapter_config(repo_, batch_commits));
    EXPECT_EQ(unaccounted(report), std::vector<std::string>{unit_name(late, spectrometer)}) << batch_commits;
    EXPECT_EQ(unaccounted_unit(report, late, spectrometer).unit.disposition, ingest::UnitDisposition::Conflict);
  }

  // The file arrives in a later run: the analysis was folded by an earlier one.
  auto resumed = fresh_world();
  ASSERT_TRUE(run_import(*resumed, adapter_config(repo_, 1), 2));
  ASSERT_TRUE(run_import(*resumed, adapter_config(repo_, 1)));
  const auto rows = snapshot_of(*resumed);
  EXPECT_TRUE(rows == first) << first_difference(rows, first);
}

// Good content after a version that could not be read is a revision at its
// own commit: verify looks for it there, not at the commit that first had it.
TEST_P(ProjectImportTest, VerifyLooksForGoodContentAfterAnUnreadableVersionAtItsOwnCommit) {
  legacy_.collect(kRunE, kE.str(), kCollected);
  const std::string good = legacy_.refit(kRunE, "Ar40", 12.5, kDay2);
  legacy_.write(kRunE, FileKind::Intercepts, "{ not json");
  const std::string bad = legacy_.commit("interrupted", kRefit);
  legacy_.write(kRunE, FileKind::Intercepts, LegacyRepoBuilder::intercepts_text("Ar40", 12.5));
  const std::string again = legacy_.commit("repaired", kLater);
  const std::string path = LegacyRepoBuilder::path(kRunE, FileKind::Intercepts);
  ASSERT_TRUE(run_import(*world_, adapter_config(repo_)));

  auto report = verify_repo(*world_, adapter_config(repo_, 1));
  EXPECT_EQ(unaccounted(report), std::vector<std::string>{});
  EXPECT_EQ(report.pending_blocking, 1);  // the unreadable version
  ASSERT_EQ(forget(*world_, "DELETE FROM import_provenance WHERE commit_sha = ? AND path = ?",
                   {pd::qv(again), pd::qv(path)}),
            1);
  for (const int batch_commits : {1, 500}) {
    report = verify_repo(*world_, adapter_config(repo_, batch_commits));
    EXPECT_EQ(unaccounted(report), std::vector<std::string>{unit_name(again, path)}) << batch_commits;
    EXPECT_EQ(unaccounted_unit(report, again, path).unit.disposition, ingest::UnitDisposition::Imported);
  }
  (void)good;
  (void)bad;
}

// A file removed and restored unchanged repeats the unit before the removal:
// it is accounted for by that unit's row, and by nothing else.
TEST_P(ProjectImportTest, VerifyAccountsForARestoredFileByTheUnitItRepeats) {
  const auto collected = legacy_.collect(kRunE, kE.str(), kCollected);
  const std::string refit = legacy_.refit(kRunE, "Ar40", 12.5, kDay2);
  const std::string path = LegacyRepoBuilder::path(kRunE, FileKind::Intercepts);
  repo_.remove(path);
  const std::string removed = legacy_.commit("removed by hand", kRefit);
  legacy_.write(kRunE, FileKind::Intercepts, LegacyRepoBuilder::intercepts_text("Ar40", 12.5));
  const std::string restored = legacy_.commit("restored", kLater);
  ASSERT_TRUE(run_import(*world_, adapter_config(repo_)));
  EXPECT_EQ(unaccounted(verify_repo(*world_, adapter_config(repo_, 1))), std::vector<std::string>{});

  // The root's row has the path too, but the restored file repeats the refit.
  ASSERT_EQ(forget(*world_, "DELETE FROM import_provenance WHERE commit_sha = ? AND path = ?",
                   {pd::qv(refit), pd::qv(path)}),
            1);
  for (const int batch_commits : {1, 500}) {
    const auto report = verify_repo(*world_, adapter_config(repo_, batch_commits));
    EXPECT_EQ(unaccounted(report), sorted({unit_name(refit, path), unit_name(restored, path)})) << batch_commits;
    const auto& repeat = unaccounted_unit(report, restored, path);
    EXPECT_EQ(repeat.unit.disposition, ingest::UnitDisposition::Unchanged);
    EXPECT_EQ(repeat.unit.repeats, refit);
    ASSERT_EQ(repeat.missing.size(), 1u);
    EXPECT_EQ(repeat.missing[0].commit, refit);
  }
  (void)collected;
  (void)removed;
}

// An analysis the store does not have: a history longer than what was imported.
TEST_P(ProjectImportTest, VerifyReportsAnAnalysisThatIsNotImported) {
  legacy_.collect(kRunE, kE.str(), kCollected);
  legacy_.refit(kRunE, "Ar40", 12.5, kRefit);
  ASSERT_TRUE(run_import(*world_, adapter_config(repo_)));
  const std::string token = *world_->source().progress_token;
  EXPECT_TRUE(verify_repo(*world_, adapter_config(repo_)).ok());

  const auto added = legacy_.collect("66052-02A", kF.str(), kLater);
  const auto path = [](FileKind kind) { return LegacyRepoBuilder::path("66052-02A", kind); };
  for (const int batch_commits : {1, 500}) {
    const auto report = verify_repo(*world_, adapter_config(repo_, batch_commits));
    EXPECT_FALSE(report.ok());
    EXPECT_EQ(unaccounted(report), sorted({unit_name(added.collection, path(FileKind::Record)),
                                           unit_name(added.collection, path(FileKind::Data)),
                                           unit_name(added.collection, path(FileKind::Extraction)),
                                           unit_name(added.isoevo, path(FileKind::Intercepts)),
                                           unit_name(added.isoevo, path(FileKind::Baselines)),
                                           unit_name(added.blanks, path(FileKind::Blanks)),
                                           unit_name(added.icfactors, path(FileKind::IcFactors))}))
        << batch_commits;
    EXPECT_GT(report.would_write, 0);
    EXPECT_EQ(report.would_write, report.replay_would_write) << batch_commits;
    // The import finished, but the repository has a head it did not see.
    EXPECT_EQ(report.source.status, "finished");
    EXPECT_NE(report.source.stored_head, std::optional<std::string>{report.source.current_head});
    EXPECT_EQ(report.source.current_head, repo_.head());
    EXPECT_FALSE(report.source.finished_and_current());
  }
  // Verify did not import it, nor move the token.
  EXPECT_EQ(world_->count("analysis"), 1);
  EXPECT_EQ(world_->source().progress_token, std::optional<std::string>{token});

  ASSERT_TRUE(run_import(*world_, adapter_config(repo_)));
  EXPECT_TRUE(verify_repo(*world_, adapter_config(repo_)).ok());
}

// Copies of an analysis have no rows of their own: a second copy in the same
// source points at the analysis, a copy in another source at its membership.
TEST_P(ProjectImportTest, VerifyAccountsForCopiesOfAnAnalysis) {
  write_uuid_named(repo_, kF, "66052-03B");
  legacy_.commit("<IMPORT> initial", kCollected);
  const std::string same = legacy_.import_without_collection("66052-03B", kF.str(), kDay2);  // the same record text
  legacy_.import_without_collection("66052-08A", kF.str(), kRefit);
  auto record = json::parse(LegacyRepoBuilder::record_text("66052-03B", kF.str()));
  record["comment"] = "edited in the copy";
  legacy_.write("66052-08A", FileKind::Record, record.dump(4));
  const std::string edited = legacy_.commit("edited", kRefit);
  GitFixture second;
  second.init();
  LegacyRepoBuilder copy(second);
  const std::string shared = copy.import_without_collection("66052-03B", kF.str(), kLater);
  auto second_config = adapter_config(second);
  second_config.url = "https://github.com/NMGRLData/Shared";
  second_config.repository_name = "Shared";

  for (const int batch_commits : {500, 1}) {
    auto world = fresh_world();
    ASSERT_TRUE(run_import(*world, adapter_config(repo_, batch_commits)));
    ASSERT_TRUE(run_import(*world, second_config));
    EXPECT_EQ(world->count("analysis"), 1);

    // This source: nothing unaccounted. The copy that differs, the edit of its
    // record, which is not applied, and the spectrometer file that came after
    // F was folded are pending conflicts.
    auto report = verify_repo(*world, adapter_config(repo_, batch_commits == 1 ? 500 : 1));
    EXPECT_EQ(unaccounted(report), std::vector<std::string>{}) << batch_commits;
    EXPECT_EQ(report.would_write, 0);
    EXPECT_EQ(report.replay_would_write, 0);
    EXPECT_EQ(report.pending_blocking, 3) << batch_commits;
    // The other source: the record is the membership row, the rest is folded into it.
    report = verify_repo(*world, second_config);
    EXPECT_EQ(unaccounted(report), std::vector<std::string>{}) << batch_commits;
    EXPECT_TRUE(report.ok());
    EXPECT_EQ(report.units, 8);  // seven files and the spectrometer settings

    // Without the membership row every file of the copy is unaccounted for but
    // the spectrometer file, which no analysis of that source uses.
    ASSERT_EQ(forget(*world, "DELETE FROM import_provenance WHERE commit_sha = ?", {pd::qv(shared)}), 1);
    report = verify_repo(*world, second_config);
    std::vector<std::string> files;
    for (const FileKind kind : {FileKind::Record, FileKind::Data, FileKind::Extraction, FileKind::Intercepts,
                                FileKind::Baselines, FileKind::Blanks, FileKind::IcFactors})
      files.push_back(unit_name(shared, LegacyRepoBuilder::path("66052-03B", kind)));
    EXPECT_EQ(unaccounted(report), sorted(files)) << batch_commits;
    EXPECT_EQ(unaccounted_unit(report, shared, LegacyRepoBuilder::path("66052-03B", FileKind::Blanks)).unit.disposition,
              ingest::UnitDisposition::Folded);
    EXPECT_EQ(report.ignored, 1);
  }
  (void)same;
  (void)edited;
}

// The fixture's interpreted age holds the age legacy pychron stored for
// 66052-01E; the age function is asked for it as of the commit that saved it.
TEST_P(ProjectImportTest, VerifyComparesTheStoredAgeOfAMember) {
  legacy_.collect(kRunE, kE.str(), kCollected);
  const std::string saved = legacy_.add_interpreted_age(kRefit);
  legacy_.refit(kRunE, "Ar40", 12.5, kLater);  // after the age was saved
  ASSERT_TRUE(run_import(*world_, adapter_config(repo_)));

  const json document = json::parse(fixture("ia/IR1010/660/ia/52.ia.json"));
  std::optional<double> legacy_age, legacy_err;
  for (const auto& member : document.at("analyses"))
    if (member.value("uuid", "") == kE.str()) {
      legacy_age = member.at("age").get<double>();
      legacy_err = member.at("age_err").get<double>();
    }
  ASSERT_TRUE(legacy_age && legacy_err);

  std::vector<ingest::AsOf> asked;
  double factor = 1.0;
  const ingest::AgeFn fn = [&](Uuid analysis, const ingest::AsOf& as_of) -> Result<ingest::ParityAge> {
    EXPECT_EQ(analysis, kE);
    asked.push_back(as_of);
    return ingest::ParityAge{ingest::ComputedAge{*legacy_age * factor, *legacy_err}};
  };
  auto report = verify_repo(*world_, adapter_config(repo_), fn);
  EXPECT_TRUE(report.ok());
  EXPECT_EQ(report.parity_pass, 1);
  EXPECT_EQ(report.parity_fail, 0);
  EXPECT_EQ(report.not_comparable_reasons.count("analysis is not in the store"), 1u);
  ASSERT_EQ(asked.size(), 1u);
  const std::string path(LegacyRepoBuilder::kInterpretedAgePath);
  EXPECT_EQ(asked[0].interpreted_age, ingest::interpreted_age_id(kUrl, path));
  EXPECT_EQ(asked[0].revision, ingest::revision_id(kUrl, saved, path));
  EXPECT_EQ(asked[0].changeset, ingest::changeset_id(kUrl, saved));
  EXPECT_EQ(asked[0].created, *UtcTime::parse(kRefitUtc));

  factor = 1.001;
  report = verify_repo(*world_, adapter_config(repo_), fn);
  EXPECT_FALSE(report.ok());
  ASSERT_EQ(report.parity_failures.size(), 1u);
  EXPECT_EQ(report.parity_failures[0].analysis, kE);
  const auto conflicts = world_->conflicts(ConflictKind::ValueMismatch);
  ASSERT_EQ(conflicts.size(), 1u);
  EXPECT_EQ(conflicts[0].uuid, report.parity_failures[0].conflict);
  EXPECT_EQ(conflicts[0].path, path);
  EXPECT_EQ(json::parse(conflicts[0].detail_json).at("legacy").at("age").get<double>(), *legacy_age);
  EXPECT_EQ(json::parse(conflicts[0].detail_json).at("record_id").get<std::string>(), kRunE);
}

INSTANTIATE_TEST_SUITE_P(Engines, ProjectImportTest, ::testing::ValuesIn(P::testing::engines()));
