// MetaRepoAdapter: the history of a legacy MetaData repository, built in a
// GitFixture from the real files under fixtures/meta, imported into a real
// store through BatchWriter. Expected values are the fixtures' literals.

#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "fixture_files.hpp"
#include "git_fixture.hpp"
#include "pychron/core/sha256.hpp"
#include "pychron/dvc/meta_adapter.hpp"
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
using P::RefType;
using P::Uuid;

namespace {

// The normalized form of the url the adapter is given.
const std::string kUrl = "https://github.com/NMGRLData/MetaData";
const std::string kLevel = "NM-293/G.json";
const std::string kSens = "spectrometers/felix.sens.json";

const char* const kDay1 = "2017-12-20T10:00:00-07:00";
const char* const kDay2 = "2018-01-05T09:30:00-07:00";
const char* const kDay3 = "2018-06-05T14:57:22-06:00";
const char* const kDay4 = "2018-06-07T14:20:47-06:00";

std::string err(const Error& e) { return to_string(e); }

// A fresh database: the store under test and a white-box connection to it.
struct World {
  explicit World(const std::string& engine) : database(engine, true) {
    store = P::testing::open_or_die(database.url());
    if (!store) return;
    client = *store->register_client({"import-1", "importer", std::nullopt, "test"});
    auto opened = pd::Db::open(P::StoreConfig{database.url(), false});
    if (opened) db = std::move(*opened);
  }

  long long count(const std::string& table, const std::string& where = "") {
    auto row = db->select_one(QStringLiteral("SELECT count(*) AS n FROM %1 %2")
                                  .arg(QString::fromStdString(table), QString::fromStdString(where)));
    return row && *row ? (*row)->value("n").toLongLong() : -1;
  }

  // The uuid the importer gives a reference object it creates.
  static Uuid object(RefType type, const std::string& key) {
    return ingest::catalog_id("ref_object", std::string(P::to_string(type)) + "\n" + key);
  }

  std::vector<P::RevisionInfo> history(RefType type, const std::string& key) {
    auto found = store->history(object(type, key), Kind::RefValue);
    EXPECT_TRUE(found) << key;
    return found ? *found : std::vector<P::RevisionInfo>{};
  }

  template <class Value>
  Value payload(Uuid revision) {
    auto found = store->load_payload(revision);
    if (!found || !*found) {
      ADD_FAILURE() << "no payload for revision " << revision.str();
      return {};
    }
    return std::get<Value>(std::get<P::RefPayload>(**found));
  }

  // The value at the head of a reference object.
  template <class Value>
  Value head(RefType type, const std::string& key) {
    auto found = store->head(object(type, key), Kind::RefValue);
    if (!found || !*found) {
      ADD_FAILURE() << "no head for " << P::to_string(type) << " " << key;
      return {};
    }
    return payload<Value>(**found);
  }

  // The provenance detail of the changeset of a commit; null: it has none.
  json changeset_detail(const std::string& commit) {
    auto rows = store->provenance_for(ingest::changeset_id(kUrl, commit));
    if (!rows || rows->empty() || !rows->front().detail_json) return nullptr;
    return json::parse(*rows->front().detail_json);
  }

  json revision_detail(Uuid revision) {
    auto rows = store->provenance_for(revision);
    if (!rows || rows->empty()) return nullptr;
    return json::parse(rows->front().detail_json.value_or("{}"));
  }

  std::vector<P::ImportConflictRow> conflicts() {
    auto rows = store->import_conflicts({std::nullopt, std::nullopt, std::nullopt});
    EXPECT_TRUE(rows);
    return rows ? *rows : std::vector<P::ImportConflictRow>{};
  }

  P::ImportSourceInfo source() {
    auto all = store->import_sources();
    if (all)
      for (const auto& s : *all)
        if (s.spec.url_or_path == kUrl) return s;
    ADD_FAILURE() << "no import source " << kUrl;
    return {};
  }

  // Declared first so it is destroyed last: the connections point into it.
  P::testing::TestDatabase database;
  std::unique_ptr<P::IStore> store;
  Uuid client;
  std::unique_ptr<pd::Db> db;
};

MetaAdapterConfig adapter_config(GitFixture& repo, int batch_commits = 500) {
  MetaAdapterConfig c;
  c.git.repo = repo.path();
  c.git.branch = "main";
  c.git.scratch = repo.temp("scratch");
  c.url = "https://GitHub.com/NMGRLData/MetaData.git";
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
Result<RunStats> run_import(World& w, const MetaAdapterConfig& config, std::optional<int> max_batches = std::nullopt,
                            ingest::WriterConfig writer = writer_config()) {
  auto adapter = MetaRepoAdapter::open(config);
  if (!adapter) return fail(adapter.error());
  ingest::BatchWriter batches(*w.store, w.client, std::move(writer));
  return batches.run(**adapter, max_batches, {}, {});
}

std::string flux_key(int position) { return "NM-293/G/" + std::to_string(position); }

// The fixture level file with the J of some positions (by hole number)
// replaced, and without the positions of `without`.
std::string level_text(const std::map<int, double>& j, const std::set<int>& without = {}) {
  const json fixed = json::parse(fixture("meta/NM-293/G.json"));
  json level = fixed;
  level["positions"] = json::array();
  for (json entry : fixed.at("positions")) {
    const int position = entry.at("position").get<int>();
    if (without.contains(position)) continue;
    if (const auto it = j.find(position); it != j.end()) entry["j"] = it->second;
    level["positions"].push_back(std::move(entry));
  }
  return level.dump(4);
}

// The first `count` entries of the fixture's sensitivity list.
json sensitivities(std::size_t count) {
  json all = json::parse(fixture("meta/spectrometers/felix.sens.json"));
  json some = json::array();
  for (std::size_t i = 0; i < count; ++i) some.push_back(all.at(i));
  return some;
}

// Every reference file of the fixture, uncommitted.
void write_fixture_files(GitFixture& repo) {
  for (const char* path : {"NM-293/G.json", "NM-293/productions.json", "NM-293/productions/Triga_PR.json",
                           "NM-293/chronology.txt", "spectrometers/jan.gain.json", "spectrometers/felix.sens.json",
                           "irradiation_holders/24_hole.txt", "load_holders/37-hole.txt"})
    repo.write(path, fixture(std::string("meta/") + path));
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

// Every stored row an import decides, without what differs by design from
// run to run (change sequence numbers, write times).
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
  // An irradiation's uuid is the store's own (random); it is named instead.
  rows("irradiation", QStringLiteral("SELECT name FROM irradiation"), {"name"});
  rows("level",
       QStringLiteral("SELECT l.uuid AS uuid, i.name AS irradiation, l.name AS name, l.z AS z FROM level l "
                      "JOIN irradiation i ON i.uuid = l.irradiation_uuid"),
       {"uuid", "irradiation", "name", "z"});
  rows("position", QStringLiteral("SELECT uuid, level_uuid, position FROM irradiation_position"),
       {"uuid", "level_uuid", "position"});
  rows("mass_spectrometer", QStringLiteral("SELECT uuid, name FROM mass_spectrometer"), {"uuid", "name"});
  rows("ref_object",
       QStringLiteral("SELECT r.uuid AS uuid, r.ref_type AS ref_type, r.key AS key, i.name AS irradiation, "
                      "r.level_uuid AS level_uuid, r.position_uuid AS position_uuid, "
                      "r.mass_spectrometer_uuid AS mass_spectrometer_uuid FROM ref_object r "
                      "LEFT JOIN irradiation i ON i.uuid = r.irradiation_uuid"),
       {"uuid", "ref_type", "key", "irradiation", "level_uuid", "position_uuid", "mass_spectrometer_uuid"});
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
  rows("flux",
       QStringLiteral("SELECT revision_uuid, j, j_err, mean_j, mean_j_err, mean_j_mswd, position_jerr, "
                      "lambda_k_total, lambda_k_total_err, monitor_name, monitor_material, monitor_age, "
                      "monitor_age_err, options, extra FROM flux_value"),
       {"revision_uuid", "j", "j_err", "mean_j", "mean_j_err", "mean_j_mswd", "position_jerr", "lambda_k_total",
        "lambda_k_total_err", "monitor_name", "monitor_material", "monitor_age", "monitor_age_err", "options"},
       "extra");
  rows("flux_analysis",
       QStringLiteral("SELECT revision_uuid, record_id, analysis_uuid, is_omitted FROM flux_value_analysis"),
       {"revision_uuid", "record_id", "analysis_uuid", "is_omitted"});
  rows("level_z", QStringLiteral("SELECT revision_uuid, z FROM level_z_value"), {"revision_uuid", "z"});
  rows("production_meta", QStringLiteral("SELECT revision_uuid, reactor, note FROM production_meta"),
       {"revision_uuid", "reactor", "note"});
  rows("production", QStringLiteral("SELECT revision_uuid, key, value, error FROM production_value"),
       {"revision_uuid", "key", "value", "error"});
  rows("level_production", QStringLiteral("SELECT revision_uuid, production_ref_uuid, note FROM level_production_value"),
       {"revision_uuid", "production_ref_uuid", "note"});
  rows("dose", QStringLiteral("SELECT revision_uuid, ordinal, power, start_utc, end_utc FROM chronology_dose"),
       {"revision_uuid", "ordinal", "power", "start_utc", "end_utc"});
  rows("gain", QStringLiteral("SELECT revision_uuid, detector, gain FROM detector_gain"),
       {"revision_uuid", "detector", "gain"});
  rows("sensitivity", QStringLiteral("SELECT revision_uuid, sensitivity, create_date_utc, extra FROM sensitivity_value"),
       {"revision_uuid", "sensitivity", "create_date_utc"}, "extra");
  rows("holder", QStringLiteral("SELECT revision_uuid, shape, radius, has_hole_numbers FROM holder_meta"),
       {"revision_uuid", "shape", "radius", "has_hole_numbers"});
  rows("hole", QStringLiteral("SELECT revision_uuid, ordinal, hole_id, x, y, radius FROM holder_hole"),
       {"revision_uuid", "ordinal", "hole_id", "x", "y", "radius"});
  return out;
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

class MetaImportTest : public ::testing::TestWithParam<std::string> {
 protected:
  void SetUp() override {
    if (!GitFixture::available()) GTEST_SKIP() << "git not on PATH";
    repo_.init();
    world_ = std::make_unique<World>(GetParam());
    ASSERT_TRUE(world_->store);
    ASSERT_TRUE(world_->db);
  }

  P::IStore& store() { return *world_->store; }
  std::unique_ptr<World> fresh_world() { return std::make_unique<World>(GetParam()); }

  std::string commit_file(const std::string& path, const std::string& text, const char* date,
                          const std::string& message = "modified") {
    repo_.write(path, text);
    return repo_.commit(message, date);
  }

  // The repository first: the adapters read it.
  GitFixture repo_;
  std::unique_ptr<World> world_;
};

}  // namespace

TEST_P(MetaImportTest, LevelFileMakesOneRevisionPerPosition) {
  const std::string added = commit_file(kLevel, fixture("meta/NM-293/G.json"), kDay1, "Added level G to NM-293");
  auto stats = run_import(*world_, adapter_config(repo_));
  ASSERT_TRUE(stats) << err(stats.error());
  EXPECT_TRUE(stats->finished);
  EXPECT_EQ(stats->changesets, 1);
  EXPECT_EQ(stats->revisions, 23 + 1);  // the positions, and the level's z
  EXPECT_EQ(stats->conflicts, 0);

  EXPECT_EQ(world_->count("ref_object", "WHERE ref_type = 'flux_position'"), 23);
  for (int position = 1; position <= 23; ++position) {
    const auto history = world_->history(RefType::FluxPosition, flux_key(position));
    ASSERT_EQ(history.size(), 1u) << position;
    EXPECT_EQ(history[0].uuid, ingest::revision_id(kUrl, added, kLevel + "#" + std::to_string(position)));
    EXPECT_EQ(history[0].changeset.uuid, ingest::changeset_id(kUrl, added));
    EXPECT_EQ(history[0].changeset.kind, P::ChangesetKind::Reference);
    EXPECT_EQ(history[0].changeset.message, "Added level G to NM-293");
    EXPECT_EQ(history[0].changeset.created, *P::UtcTime::parse("2017-12-20T17:00:00Z"));
  }
  const auto monitor = world_->head<P::FluxValue>(RefType::FluxPosition, flux_key(1));
  EXPECT_EQ(monitor.j, std::optional<double>{0.0018634713371872254});
  EXPECT_EQ(monitor.j_err, std::optional<double>{2.0155546457173005e-05});
  EXPECT_EQ(monitor.analyses.size(), 6u);
  const auto unknown = world_->head<P::FluxValue>(RefType::FluxPosition, flux_key(16));
  EXPECT_EQ(unknown.j, std::optional<double>{0.0018848683037985877});
  EXPECT_EQ(json::parse(unknown.extra_json.value_or("{}")).at("identifier"), "66052");

  // Where each came from: the file, with the position after '#'.
  auto provenance = store().provenance_for(ingest::revision_id(kUrl, added, kLevel + "#16"));
  ASSERT_TRUE(provenance);
  ASSERT_EQ(provenance->size(), 1u);
  EXPECT_EQ(provenance->front().path, kLevel + "#16");
  EXPECT_EQ(provenance->front().commit_sha, added);
  EXPECT_EQ(provenance->front().git_blob_sha, "99558c94a9b426161c59c3d42b396eff772de1ac");

  // The irradiation, its level and the positions exist, and each object is scoped to its own.
  EXPECT_EQ(world_->count("irradiation", "WHERE name = 'NM-293'"), 1);
  // The level's z is the level_geometry value below, and nowhere else.
  EXPECT_EQ(world_->count("level", "WHERE name = 'G' AND z IS NULL"), 1);
  EXPECT_EQ(world_->count("irradiation_position"), 23);
  EXPECT_EQ(world_->count("ref_object", "WHERE ref_type = 'flux_position' AND position_uuid IS NOT NULL"), 23);
  // No identifier is made up from a level file: that is the catalog's.
  EXPECT_EQ(world_->count("identifier"), 0);

  const auto geometry = world_->history(RefType::LevelGeometry, "NM-293/G");
  ASSERT_EQ(geometry.size(), 1u);
  EXPECT_EQ(geometry[0].uuid, ingest::revision_id(kUrl, added, kLevel + "#z"));
  EXPECT_EQ(world_->payload<P::LevelZValue>(geometry[0].uuid).z, std::optional<double>{0.0});
}

TEST_P(MetaImportTest, ChangedPositionOnly) {
  commit_file(kLevel, fixture("meta/NM-293/G.json"), kDay1);
  // The whole file is written again, differently laid out; one J differs.
  const std::string fit = commit_file(kLevel, level_text({{16, 0.0019}}), kDay2, "fit flux for NM-293G");
  auto stats = run_import(*world_, adapter_config(repo_));
  ASSERT_TRUE(stats) << err(stats.error());
  EXPECT_EQ(stats->changesets, 2);
  EXPECT_EQ(stats->revisions, 24 + 1);

  for (int position = 1; position <= 23; ++position)
    EXPECT_EQ(world_->history(RefType::FluxPosition, flux_key(position)).size(), position == 16 ? 2u : 1u) << position;
  const auto history = world_->history(RefType::FluxPosition, flux_key(16));
  ASSERT_EQ(history.size(), 2u);
  EXPECT_EQ(history[1].uuid, ingest::revision_id(kUrl, fit, kLevel + "#16"));
  EXPECT_EQ(history[1].parent, std::optional<Uuid>{history[0].uuid});
  EXPECT_EQ(world_->head<P::FluxValue>(RefType::FluxPosition, flux_key(16)).j, std::optional<double>{0.0019});
  EXPECT_EQ(world_->history(RefType::LevelGeometry, "NM-293/G").size(), 1u);

  // Found again by a later run that starts after the first two commits.
  commit_file(kLevel, level_text({{16, 0.0019}, {2, 0.0021}}), kDay3);
  auto later = run_import(*world_, adapter_config(repo_));
  ASSERT_TRUE(later) << err(later.error());
  EXPECT_EQ(later->revisions, 1);
  EXPECT_EQ(world_->history(RefType::FluxPosition, flux_key(2)).size(), 2u);
  EXPECT_EQ(world_->history(RefType::FluxPosition, flux_key(16)).size(), 2u);
}

TEST_P(MetaImportTest, ProductionAndChronology) {
  repo_.write("NM-293/productions.json", fixture("meta/NM-293/productions.json"));
  repo_.write("NM-293/productions/Triga_PR.json", fixture("meta/NM-293/productions/Triga_PR.json"));
  repo_.write("NM-293/chronology.txt", fixture("meta/NM-293/chronology.txt"));
  const std::string added = repo_.commit("Added irradiation NM-293", kDay1);
  auto stats = run_import(*world_, adapter_config(repo_));
  ASSERT_TRUE(stats) << err(stats.error());
  EXPECT_EQ(stats->changesets, 1);
  EXPECT_EQ(stats->revisions, 12 + 1 + 1);
  EXPECT_EQ(world_->count("changeset", "WHERE kind = 'reference'"), 1);
  EXPECT_EQ(world_->count("changeset"), 1);

  auto production = world_->head<P::ProductionValue>(RefType::Production, "NM-293/Triga_PR");
  std::sort(production.ratios.begin(), production.ratios.end(),
            [](const auto& a, const auto& b) { return a.key < b.key; });
  const std::vector<P::ProductionRatio> ratios{
      {"Ca3637", 0.000286, 5e-07}, {"Ca3837", 4e-05, 2e-05}, {"Ca3937", 0.000758, 7e-06},
      {"Ca_K", 1.96, 0.0},         {"Cl3638", 250.0, 0.0},   {"Cl_K", 0.227, 0.0},
      {"K3739", 0.0, 0.0},         {"K3839", 0.013, 0.0},    {"K4039", 0.00873, 0.00017}};
  EXPECT_EQ(production.ratios, ratios);
  EXPECT_FALSE(production.reactor);
  EXPECT_EQ(world_->history(RefType::Production, "NM-293/Triga_PR")[0].uuid,
            ingest::revision_id(kUrl, added, "NM-293/productions/Triga_PR.json"));

  const auto chronology = world_->head<P::ChronologyValue>(RefType::Chronology, "NM-293");
  ASSERT_EQ(chronology.doses.size(), 1u);
  EXPECT_EQ(chronology.doses[0].ordinal, 0);
  EXPECT_EQ(chronology.doses[0].power, 1.0);
  EXPECT_EQ(chronology.doses[0].start, *P::UtcTime::parse("2017-12-21T13:28:00Z"));
  EXPECT_EQ(chronology.doses[0].end, *P::UtcTime::parse("2017-12-21T21:28:00Z"));

  // Each of the twelve levels names the production object.
  EXPECT_EQ(world_->count("ref_object", "WHERE ref_type = 'level_production'"), 12);
  for (const char* level : {"A", "G", "M"}) {
    const std::string key = std::string("NM-293/") + level;
    const auto history = world_->history(RefType::LevelProduction, key);
    ASSERT_EQ(history.size(), 1u) << level;
    EXPECT_EQ(history[0].uuid, ingest::revision_id(kUrl, added, std::string("NM-293/productions.json#") + level));
    EXPECT_EQ(world_->payload<P::LevelProductionValue>(history[0].uuid).production,
              World::object(RefType::Production, "NM-293/Triga_PR"));
  }
  EXPECT_EQ(world_->count("level"), 12);
  EXPECT_EQ(world_->count("ref_object", "WHERE ref_type = 'production' AND irradiation_uuid IS NOT NULL"), 1);

  // One level moves to another production: one revision, and the production
  // is an object though it has no file yet.
  json map = json::parse(fixture("meta/NM-293/productions.json"));
  map["G"] = "Cd_shielded";
  commit_file("NM-293/productions.json", map.dump(4), kDay2);
  auto later = run_import(*world_, adapter_config(repo_));
  ASSERT_TRUE(later) << err(later.error());
  EXPECT_EQ(later->revisions, 1);
  EXPECT_EQ(world_->head<P::LevelProductionValue>(RefType::LevelProduction, "NM-293/G").production,
            World::object(RefType::Production, "NM-293/Cd_shielded"));
  EXPECT_EQ(world_->history(RefType::LevelProduction, "NM-293/A").size(), 1u);
  EXPECT_TRUE(world_->history(RefType::Production, "NM-293/Cd_shielded").empty());
}

TEST_P(MetaImportTest, SensitivityEntriesInOrder) {
  const std::string three = commit_file(kSens, sensitivities(3).dump(4), kDay1);
  auto stats = run_import(*world_, adapter_config(repo_));
  ASSERT_TRUE(stats) << err(stats.error());
  EXPECT_EQ(stats->revisions, 3);

  // The list is not in date order (2020, 2000, 2020); the revisions follow the list.
  auto history = world_->history(RefType::Sensitivity, "felix");
  ASSERT_EQ(history.size(), 3u);
  const double listed[] = {2.13e-16, 4e-16, 2.13e-16};
  const char* const dated[] = {"2020-01-27T19:54:30Z", "2000-05-18T18:54:30Z", "2020-01-27T19:54:30Z"};
  for (std::size_t i = 0; i < 3; ++i) {
    EXPECT_EQ(history[i].uuid, ingest::revision_id(kUrl, three, kSens + "#" + std::to_string(i)));
    const auto value = world_->payload<P::SensitivityValue>(history[i].uuid);
    EXPECT_EQ(value.sensitivity, listed[i]);
    ASSERT_TRUE(value.create_date);
    EXPECT_EQ(*value.create_date, *P::UtcTime::parse(dated[i]));
    EXPECT_EQ(json::parse(value.extra_json.value_or("{}")).at("units"), "mol/fA");
  }
  EXPECT_EQ(history[1].parent, std::optional<Uuid>{history[0].uuid});
  EXPECT_EQ(history[2].parent, std::optional<Uuid>{history[1].uuid});
  EXPECT_EQ(world_->count("ref_object", "WHERE ref_type = 'sensitivity' AND mass_spectrometer_uuid IS NOT NULL"), 1);
  EXPECT_EQ(world_->count("mass_spectrometer", "WHERE name = 'felix'"), 1);

  // The fixture as it is: a fourth entry.
  const std::string four = commit_file(kSens, fixture("meta/spectrometers/felix.sens.json"), kDay2);
  auto later = run_import(*world_, adapter_config(repo_));
  ASSERT_TRUE(later) << err(later.error());
  EXPECT_EQ(later->revisions, 1);
  history = world_->history(RefType::Sensitivity, "felix");
  ASSERT_EQ(history.size(), 4u);
  EXPECT_EQ(history[3].uuid, ingest::revision_id(kUrl, four, kSens + "#3"));
  EXPECT_EQ(world_->head<P::SensitivityValue>(RefType::Sensitivity, "felix").sensitivity, 5e-16);
}

TEST_P(MetaImportTest, SensitivityHeadFollowsTheLastEntry) {
  commit_file(kSens, fixture("meta/spectrometers/felix.sens.json"), kDay1);
  // An entry in the middle is corrected: the legacy code still uses the last one.
  json list = sensitivities(4);
  list[1]["sensitivity"] = 4.5e-16;
  const std::string corrected = commit_file(kSens, list.dump(4), kDay2);
  // Then the last entry is taken away.
  list.erase(3);
  const std::string shortened = commit_file(kSens, list.dump(4), kDay3);
  ASSERT_TRUE(run_import(*world_, adapter_config(repo_)));

  const auto history = world_->history(RefType::Sensitivity, "felix");
  ASSERT_EQ(history.size(), 4u + 2u + 1u);
  EXPECT_EQ(history[4].uuid, ingest::revision_id(kUrl, corrected, kSens + "#1"));
  EXPECT_EQ(world_->payload<P::SensitivityValue>(history[4].uuid).sensitivity, 4.5e-16);
  EXPECT_EQ(history[5].uuid, ingest::revision_id(kUrl, corrected, kSens + "#3"));
  EXPECT_EQ(world_->payload<P::SensitivityValue>(history[5].uuid).sensitivity, 5e-16);
  EXPECT_EQ(world_->revision_detail(history[5].uuid).at("restated"), true);
  EXPECT_EQ(history[6].uuid, ingest::revision_id(kUrl, shortened, kSens + "#2"));
  EXPECT_EQ(world_->head<P::SensitivityValue>(RefType::Sensitivity, "felix").sensitivity, 2.13e-16);
}

TEST_P(MetaImportTest, GainsAndHolders) {
  repo_.write("spectrometers/jan.gain.json", fixture("meta/spectrometers/jan.gain.json"));
  repo_.write("irradiation_holders/24_hole.txt", fixture("meta/irradiation_holders/24_hole.txt"));
  repo_.write("load_holders/37-hole.txt", fixture("meta/load_holders/37-hole.txt"));
  const std::string added = repo_.commit("holders", kDay1);
  // A spectrometer is named in lower case, as the analyses name it.
  const std::string gains = commit_file("spectrometers/Obama.gain.json", R"({"H1": 1.0025, "AX": 0.998, "note": "x"})",
                                        kDay2);
  auto stats = run_import(*world_, adapter_config(repo_));
  ASSERT_TRUE(stats) << err(stats.error());
  EXPECT_EQ(stats->revisions, 4);
  EXPECT_EQ(stats->conflicts, 0);

  // The fixture's gains file is `{}`: a value without gains.
  const auto history = world_->history(RefType::Gains, "jan");
  ASSERT_EQ(history.size(), 1u);
  EXPECT_EQ(history[0].uuid, ingest::revision_id(kUrl, added, "spectrometers/jan.gain.json"));
  EXPECT_TRUE(world_->payload<P::GainsValue>(history[0].uuid).gains.empty());
  const auto named = world_->history(RefType::Gains, "obama");
  ASSERT_EQ(named.size(), 1u);
  EXPECT_EQ(named[0].uuid, ingest::revision_id(kUrl, gains, "spectrometers/Obama.gain.json"));
  auto detectors = world_->payload<P::GainsValue>(named[0].uuid).gains;
  std::sort(detectors.begin(), detectors.end(), [](const auto& a, const auto& b) { return a.detector < b.detector; });
  EXPECT_EQ(detectors, (std::vector<P::DetectorGain>{{"AX", 0.998}, {"H1", 1.0025}}));
  EXPECT_EQ(world_->revision_detail(named[0].uuid),
            json::parse(R"({"extra": {"note": "x"}, "name_as_written": "Obama"})"));
  EXPECT_EQ(world_->count("mass_spectrometer"), 2);
  EXPECT_EQ(world_->count("mass_spectrometer", "WHERE name = 'jan' OR name = 'obama'"), 2);
  EXPECT_EQ(world_->count("ref_object", "WHERE ref_type = 'gains' AND mass_spectrometer_uuid IS NOT NULL"), 2);

  const auto irradiation = world_->head<P::HolderValue>(RefType::IrradiationHolder, "24_hole");
  EXPECT_EQ(irradiation.radius, std::optional<double>{0.0175});
  ASSERT_EQ(irradiation.holes.size(), 56u);
  EXPECT_EQ(irradiation.holes[1], (P::HolderHole{1, "2", 0.1048, 0.3912, 0.0175}));
  EXPECT_EQ(world_->revision_detail(world_->history(RefType::IrradiationHolder, "24_hole")[0].uuid),
            json::parse(R"({"header": "56,0.0175"})"));

  const auto load = world_->head<P::HolderValue>(RefType::LoadHolder, "37-hole");
  EXPECT_EQ(load.shape, std::optional<std::string>{"circle"});
  EXPECT_EQ(load.radius, std::optional<double>{1.75});
  EXPECT_FALSE(load.has_hole_numbers);
  ASSERT_EQ(load.holes.size(), 37u);
  EXPECT_EQ(load.holes[18], (P::HolderHole{18, "19", 0.0, 0.0, std::nullopt}));
}

TEST_P(MetaImportTest, UnparseableLevelIsConflict) {
  const std::string garbage = "{\"positions\": [{\"position\": 1, \"j\": 0.001";
  repo_.write(kLevel, garbage);
  repo_.write("NM-293/chronology.txt", fixture("meta/NM-293/chronology.txt"));
  const std::string bad = repo_.commit("truncated by a crash", kDay1);
  auto stats = run_import(*world_, adapter_config(repo_));
  ASSERT_TRUE(stats) << err(stats.error());
  EXPECT_TRUE(stats->finished);
  EXPECT_EQ(stats->conflicts, 1);
  EXPECT_EQ(stats->revisions, 1);  // the chronology beside it is imported

  auto conflict = store().import_conflict(ingest::conflict_id(kUrl, bad, kLevel));
  ASSERT_TRUE(conflict && conflict->has_value());
  EXPECT_EQ((*conflict)->kind, ConflictKind::Unparseable);
  EXPECT_EQ((*conflict)->path, kLevel);
  EXPECT_EQ((*conflict)->file_sha256, std::optional<Sha256Digest>{sha256(std::string_view{garbage})});
  EXPECT_TRUE(json::parse((*conflict)->detail_json).contains("reason"));
  EXPECT_EQ(world_->count("ref_object", "WHERE ref_type = 'flux_position'"), 0);
  EXPECT_EQ(world_->history(RefType::Chronology, "NM-293").size(), 1u);

  // Repaired: every position is new. Broken again and repaired with one J
  // changed: the position is compared with the last version that could be read.
  commit_file(kLevel, fixture("meta/NM-293/G.json"), kDay2);
  commit_file(kLevel, "", kDay3);
  commit_file(kLevel, level_text({{4, 0.0044}}), kDay4);
  auto later = run_import(*world_, adapter_config(repo_));
  ASSERT_TRUE(later) << err(later.error());
  EXPECT_EQ(later->conflicts, 1);
  EXPECT_EQ(later->revisions, 23 + 1 + 1);
  EXPECT_EQ(world_->conflicts().size(), 2u);
  for (int position = 1; position <= 23; ++position)
    EXPECT_EQ(world_->history(RefType::FluxPosition, flux_key(position)).size(), position == 4 ? 2u : 1u) << position;
}

TEST_P(MetaImportTest, UnparseableFilesOfEveryKindAreConflicts) {
  const std::vector<std::string> paths{"NM-293/G.json",
                                       "NM-293/chronology.txt",
                                       "NM-293/productions.json",
                                       "NM-293/productions/Triga_PR.json",
                                       "irradiation_holders/24_hole.txt",
                                       "load_holders/37-hole.txt",
                                       "spectrometers/felix.sens.json",
                                       "spectrometers/jan.gain.json"};
  for (const auto& path : paths) repo_.write(path, "<<<<<<< HEAD");
  const std::string bad = repo_.commit("a merge gone wrong", kDay1);
  auto stats = run_import(*world_, adapter_config(repo_));
  ASSERT_TRUE(stats) << err(stats.error());
  EXPECT_EQ(stats->conflicts, 8);
  EXPECT_EQ(stats->revisions, 0);
  for (const auto& path : paths) {
    auto conflict = store().import_conflict(ingest::conflict_id(kUrl, bad, path));
    ASSERT_TRUE(conflict && conflict->has_value()) << path;
    EXPECT_EQ((*conflict)->kind, ConflictKind::Unparseable) << path;
    EXPECT_TRUE((*conflict)->file_sha256.has_value()) << path;
  }
  EXPECT_EQ(world_->count("changeset"), 0);
  EXPECT_EQ(world_->count("ref_object"), 0);
}

TEST_P(MetaImportTest, FilesThatAreNotReferenceDataAreIgnored) {
  repo_.write("README.md", "# MetaData\n");
  repo_.write(".gitignore", "*.pyc\n");
  repo_.write("reactors.json", "{\"Triga\": {}}");
  repo_.write("molecular_weights.json", "not even json");
  repo_.write("productions/Triga_PR.json", "not even json");
  repo_.write("scripts/felix/measurement/unknown.py", "def main(): pass\n");
  repo_.write("experiments/felix/template.txt", "queue\n");
  repo_.write("irradiation_holders/24_hole.xml", "<holder/>");
  repo_.write("irradiation_holders/.txt", "");
  repo_.write("NM-293/notes.txt", "remember the cadmium");
  const std::string only = repo_.commit("everything else", kDay1);
  auto stats = run_import(*world_, adapter_config(repo_));
  ASSERT_TRUE(stats) << err(stats.error());
  EXPECT_TRUE(stats->finished);
  EXPECT_EQ(stats->changesets, 0);
  EXPECT_EQ(stats->conflicts, 0);
  EXPECT_EQ(world_->count("changeset"), 0);
  EXPECT_EQ(world_->count("import_provenance"), 0);
  EXPECT_EQ(world_->count("import_conflict"), 0);
  EXPECT_EQ(world_->count("irradiation"), 0);
  EXPECT_EQ(world_->source().head_sha, std::optional<std::string>{only});
  EXPECT_EQ(world_->source().status, "finished");
}

TEST_P(MetaImportTest, RemovedPositionHasNoValue) {
  commit_file(kLevel, level_text({}), kDay1);
  const std::string dropped = commit_file(kLevel, level_text({}, {5}), kDay2, "position 5 emptied");
  auto stats = run_import(*world_, adapter_config(repo_));
  ASSERT_TRUE(stats) << err(stats.error());
  EXPECT_EQ(stats->revisions, 24 + 1);

  // The legacy system has no J there any more; nor has the head.
  auto history = world_->history(RefType::FluxPosition, flux_key(5));
  ASSERT_EQ(history.size(), 2u);
  EXPECT_EQ(history[1].uuid, ingest::revision_id(kUrl, dropped, kLevel + "#5"));
  EXPECT_EQ(world_->head<P::FluxValue>(RefType::FluxPosition, flux_key(5)), P::FluxValue{});
  EXPECT_EQ(world_->revision_detail(history[1].uuid), json::parse(R"({"removed": true})"));
  EXPECT_EQ(world_->count("flux_value", "WHERE j IS NULL AND j_err IS NULL AND lambda_k_total IS NULL"), 1);
  for (int position = 1; position <= 23; ++position) {
    if (position == 5) continue;
    EXPECT_EQ(world_->history(RefType::FluxPosition, flux_key(position)).size(), 1u) << position;
  }

  // Nothing more while it stays away; a normal revision when it is back.
  commit_file(kLevel, level_text({{4, 0.0044}}, {5}), kDay3);
  const std::string back = commit_file(kLevel, level_text({{4, 0.0044}}), kDay4, "position 5 filled again");
  auto later = run_import(*world_, adapter_config(repo_));
  ASSERT_TRUE(later) << err(later.error());
  EXPECT_EQ(later->revisions, 1 + 1);
  history = world_->history(RefType::FluxPosition, flux_key(5));
  ASSERT_EQ(history.size(), 3u);
  EXPECT_EQ(history[2].uuid, ingest::revision_id(kUrl, back, kLevel + "#5"));
  EXPECT_EQ(world_->head<P::FluxValue>(RefType::FluxPosition, flux_key(5)).j,
            std::optional<double>{json::parse(fixture("meta/NM-293/G.json")).at("positions").at(4).at("j").get<double>()});
  EXPECT_FALSE(world_->revision_detail(history[2].uuid).contains("removed"));
}

TEST_P(MetaImportTest, PositionRemovedOnOneSideOfAMerge) {
  commit_file(kLevel, level_text({}), kDay1);
  // The merge takes the side's removal along with main's fit.
  repo_.branch("side");
  const std::string fit = commit_file(kLevel, level_text({{12, 0.0122}}), kDay2, "main fits 12");
  repo_.checkout("side");
  const std::string emptied = commit_file(kLevel, level_text({}, {5}), kDay2, "side empties 5");
  repo_.checkout("main");
  const std::string merged = repo_.merge("side", "Merge branch 'side'", kDay3);
  // Another side empties position 7; this merge keeps main's tree.
  repo_.branch("other");
  commit_file(kLevel, level_text({{12, 0.0122}, {2, 0.0021}}, {5}), kDay3, "main fits 2");
  repo_.checkout("other");
  const std::string discarded = commit_file(kLevel, level_text({{12, 0.0122}}, {5, 7}), kDay3, "other empties 7");
  repo_.checkout("main");
  repo_.git({"merge", "--quiet", "--no-ff", "-s", "ours", "-m", "Merge branch 'other'", "other"}, kDay4);
  const std::string kept = repo_.head();

  for (const int batch_commits : {500, 1}) {
    auto world = fresh_world();
    auto stats = run_import(*world, adapter_config(repo_, batch_commits));
    ASSERT_TRUE(stats) << err(stats.error());
    // Kept by the merge: removed.
    EXPECT_EQ(world->head<P::FluxValue>(RefType::FluxPosition, flux_key(5)), P::FluxValue{});
    const auto of_5 = world->history(RefType::FluxPosition, flux_key(5));
    ASSERT_EQ(of_5.size(), 2u);
    EXPECT_EQ(of_5[1].uuid, ingest::revision_id(kUrl, emptied, kLevel + "#5"));
    EXPECT_EQ(world->revision_detail(of_5[1].uuid), json::parse(R"({"removed": true})"));
    // Discarded by the merge: not removed. The walk is one line, so the
    // position is emptied at the other's commit and filled at the merge.
    EXPECT_EQ(world->head<P::FluxValue>(RefType::FluxPosition, flux_key(7)).j,
              std::optional<double>{json::parse(fixture("meta/NM-293/G.json")).at("positions").at(6).at("j").get<double>()});
    const auto of_7 = world->history(RefType::FluxPosition, flux_key(7));
    ASSERT_EQ(of_7.size(), 3u);
    EXPECT_EQ(of_7[1].uuid, ingest::revision_id(kUrl, discarded, kLevel + "#7"));
    EXPECT_EQ(world->revision_detail(of_7[1].uuid), json::parse(R"({"removed": true})"));
    EXPECT_EQ(of_7[2].uuid, ingest::revision_id(kUrl, kept, kLevel + "#7"));
    EXPECT_EQ(world->revision_detail(of_7[2].uuid), json::parse(R"({"walk": "merge"})"));

    // What the walk itself did to position 12 is told apart from main's fit:
    // put back where the side's commit comes after main's, and stated again
    // at the merge.
    EXPECT_EQ(world->head<P::FluxValue>(RefType::FluxPosition, flux_key(12)).j, std::optional<double>{0.0122});
    const auto of_12 = world->history(RefType::FluxPosition, flux_key(12));
    ASSERT_EQ(of_12.size(), 4u);
    EXPECT_EQ(of_12[1].uuid, ingest::revision_id(kUrl, fit, kLevel + "#12"));
    EXPECT_FALSE(world->revision_detail(of_12[1].uuid).contains("walk"));
    EXPECT_EQ(of_12[2].uuid, ingest::revision_id(kUrl, emptied, kLevel + "#12"));
    EXPECT_EQ(world->revision_detail(of_12[2].uuid), json::parse(R"({"walk": "branch"})"));
    EXPECT_EQ(of_12[3].uuid, ingest::revision_id(kUrl, merged, kLevel + "#12"));
    EXPECT_EQ(world->revision_detail(of_12[3].uuid), json::parse(R"({"walk": "merge"})"));
    // main's second fit comes before the other's commit, which puts it back.
    EXPECT_EQ(world->head<P::FluxValue>(RefType::FluxPosition, flux_key(2)).j, std::optional<double>{0.0021});
    EXPECT_EQ(world->history(RefType::FluxPosition, flux_key(2)).size(), 4u);
  }
}

TEST_P(MetaImportTest, LevelDroppedFromProductionsIsNotedAndKeepsItsHead) {
  commit_file("NM-293/productions.json", fixture("meta/NM-293/productions.json"), kDay1);
  json map = json::parse(fixture("meta/NM-293/productions.json"));
  map.erase("M");
  map.erase("L");
  const std::string dropped = commit_file("NM-293/productions.json", map.dump(4), kDay2);
  auto stats = run_import(*world_, adapter_config(repo_));
  ASSERT_TRUE(stats) << err(stats.error());
  EXPECT_EQ(stats->revisions, 12);

  // A level_production value must name a production: it cannot say "none".
  // The commit says what went, and the head stays (a documented limit).
  EXPECT_EQ(world_->changeset_detail(dropped),
            json::parse(R"({"removed": ["NM-293/productions.json#L", "NM-293/productions.json#M"]})"));
  EXPECT_EQ(world_->history(RefType::LevelProduction, "NM-293/M").size(), 1u);
  EXPECT_EQ(world_->head<P::LevelProductionValue>(RefType::LevelProduction, "NM-293/M").production,
            World::object(RefType::Production, "NM-293/Triga_PR"));
  EXPECT_EQ(world_->count("changeset"), 2);

  // Deleted altogether: every level it still had.
  repo_.remove("NM-293/productions.json");
  const std::string deleted = repo_.commit("removed", kDay3);
  ASSERT_TRUE(run_import(*world_, adapter_config(repo_)));
  const json detail = world_->changeset_detail(deleted);
  ASSERT_EQ(detail.at("removed").size(), 10u);
  EXPECT_EQ(detail.at("removed")[0], "NM-293/productions.json#A");
}

TEST_P(MetaImportTest, DeletedFilesLeaveNoValue) {
  write_fixture_files(repo_);
  repo_.commit("initial import", kDay1);
  for (const char* path : {"NM-293/productions/Triga_PR.json", "NM-293/chronology.txt", "spectrometers/jan.gain.json",
                           "spectrometers/felix.sens.json", "irradiation_holders/24_hole.txt",
                           "load_holders/37-hole.txt"})
    repo_.remove(path);
  const std::string deleted = repo_.commit("cleared out", kDay2);
  auto stats = run_import(*world_, adapter_config(repo_));
  ASSERT_TRUE(stats) << err(stats.error());
  EXPECT_EQ(stats->conflicts, 0);

  const auto removed_at = [&](RefType type, const std::string& key, const std::string& path) {
    const auto history = world_->history(type, key);
    ASSERT_EQ(history.size(), 2u) << key;
    EXPECT_EQ(history[1].uuid, ingest::revision_id(kUrl, deleted, path)) << key;
    EXPECT_EQ(world_->revision_detail(history[1].uuid), json::parse(R"({"removed": true})")) << key;
    auto provenance = store().provenance_for(history[1].uuid);
    ASSERT_TRUE(provenance && provenance->size() == 1u) << key;
    EXPECT_EQ(provenance->front().git_blob_sha, "") << key;  // there is no file
  };
  removed_at(RefType::Chronology, "NM-293", "NM-293/chronology.txt");
  EXPECT_EQ(world_->head<P::ChronologyValue>(RefType::Chronology, "NM-293"), P::ChronologyValue{});
  removed_at(RefType::Production, "NM-293/Triga_PR", "NM-293/productions/Triga_PR.json");
  EXPECT_EQ(world_->head<P::ProductionValue>(RefType::Production, "NM-293/Triga_PR"), P::ProductionValue{});
  removed_at(RefType::Gains, "jan", "spectrometers/jan.gain.json");
  EXPECT_EQ(world_->head<P::GainsValue>(RefType::Gains, "jan"), P::GainsValue{});
  removed_at(RefType::IrradiationHolder, "24_hole", "irradiation_holders/24_hole.txt");
  EXPECT_EQ(world_->head<P::HolderValue>(RefType::IrradiationHolder, "24_hole"), P::HolderValue{});
  removed_at(RefType::LoadHolder, "37-hole", "load_holders/37-hole.txt");
  EXPECT_EQ(world_->head<P::HolderValue>(RefType::LoadHolder, "37-hole"), P::HolderValue{});

  // A sensitivity value is a number and cannot be "none": the commit says the
  // list went, and the head stays (a documented limit).
  EXPECT_EQ(world_->history(RefType::Sensitivity, "felix").size(), 4u);
  EXPECT_EQ(world_->head<P::SensitivityValue>(RefType::Sensitivity, "felix").sensitivity, 5e-16);
  EXPECT_EQ(world_->changeset_detail(deleted), json::parse(R"({"removed": ["spectrometers/felix.sens.json"]})"));

  // A file that never had a value leaves nothing when it goes.
  commit_file("NM-293/productions/Other.json", "not json", kDay3);
  repo_.remove("NM-293/productions/Other.json");
  repo_.commit("gone again", kDay4);
  auto later = run_import(*world_, adapter_config(repo_));
  ASSERT_TRUE(later) << err(later.error());
  EXPECT_EQ(later->revisions, 0);
  EXPECT_EQ(later->conflicts, 1);
  EXPECT_EQ(world_->count("ref_object", "WHERE key = 'NM-293/Other'"), 0);
}

TEST_P(MetaImportTest, EmptiedSensitivityListIsNoted) {
  commit_file(kSens, fixture("meta/spectrometers/felix.sens.json"), kDay1);
  const std::string emptied = commit_file(kSens, "[]", kDay2);
  ASSERT_TRUE(run_import(*world_, adapter_config(repo_)));
  EXPECT_EQ(world_->history(RefType::Sensitivity, "felix").size(), 4u);
  EXPECT_EQ(world_->changeset_detail(emptied), json::parse(R"({"removed": ["spectrometers/felix.sens.json"]})"));
}

TEST_P(MetaImportTest, DeletedLevelFileRemovesItsPositionsAndRestoringBringsThemBack) {
  commit_file(kLevel, fixture("meta/NM-293/G.json"), kDay1);
  repo_.remove(kLevel);
  const std::string deleted = repo_.commit("removed by hand", kDay2);
  const std::string restored = commit_file(kLevel, fixture("meta/NM-293/G.json"), kDay3, "restored");
  auto stats = run_import(*world_, adapter_config(repo_, 1));
  ASSERT_TRUE(stats) << err(stats.error());
  EXPECT_EQ(stats->batches, 3);
  EXPECT_EQ(world_->count("changeset"), 3);
  EXPECT_EQ(world_->count("revision"), 3 * 24);
  const auto history = world_->history(RefType::FluxPosition, flux_key(16));
  ASSERT_EQ(history.size(), 3u);
  EXPECT_EQ(history[1].uuid, ingest::revision_id(kUrl, deleted, kLevel + "#16"));
  EXPECT_EQ(world_->payload<P::FluxValue>(history[1].uuid), P::FluxValue{});
  EXPECT_EQ(history[2].uuid, ingest::revision_id(kUrl, restored, kLevel + "#16"));
  EXPECT_EQ(world_->head<P::FluxValue>(RefType::FluxPosition, flux_key(16)).j,
            std::optional<double>{0.0018848683037985877});
  const auto geometry = world_->history(RefType::LevelGeometry, "NM-293/G");
  ASSERT_EQ(geometry.size(), 3u);
  EXPECT_FALSE(world_->payload<P::LevelZValue>(geometry[1].uuid).z.has_value());
  EXPECT_EQ(world_->revision_detail(geometry[1].uuid), json::parse(R"({"removed": true})"));
  EXPECT_EQ(world_->head<P::LevelZValue>(RefType::LevelGeometry, "NM-293/G").z, std::optional<double>{0.0});
}

TEST_P(MetaImportTest, RenamedIrradiationRemovesTheOldObjectsAndMakesNewOnes) {
  write_fixture_files(repo_);
  repo_.commit("initial import", kDay1);
  for (const char* file : {"G.json", "productions.json", "productions/Triga_PR.json", "chronology.txt"}) {
    repo_.remove(std::string("NM-293/") + file);
    repo_.write(std::string("NM-294/") + file, fixture(std::string("meta/NM-293/") + file));
  }
  const std::string renamed = repo_.commit("it was NM-294 all along", kDay2);
  auto stats = run_import(*world_, adapter_config(repo_));
  ASSERT_TRUE(stats) << err(stats.error());
  EXPECT_EQ(stats->conflicts, 0);

  // Under the old name nothing has a value any more.
  for (int position = 1; position <= 23; ++position)
    EXPECT_EQ(world_->head<P::FluxValue>(RefType::FluxPosition, flux_key(position)), P::FluxValue{}) << position;
  EXPECT_FALSE(world_->head<P::LevelZValue>(RefType::LevelGeometry, "NM-293/G").z.has_value());
  EXPECT_TRUE(world_->head<P::ChronologyValue>(RefType::Chronology, "NM-293").doses.empty());
  EXPECT_TRUE(world_->head<P::ProductionValue>(RefType::Production, "NM-293/Triga_PR").ratios.empty());
  const json detail = world_->changeset_detail(renamed);
  ASSERT_EQ(detail.at("removed").size(), 12u);  // the level productions, which cannot say "none"
  EXPECT_EQ(detail.at("removed")[6], "NM-293/productions.json#G");
  // Under the new one everything has.
  EXPECT_EQ(world_->head<P::FluxValue>(RefType::FluxPosition, "NM-294/G/16").j,
            std::optional<double>{0.0018848683037985877});
  EXPECT_EQ(world_->head<P::LevelZValue>(RefType::LevelGeometry, "NM-294/G").z, std::optional<double>{0.0});
  EXPECT_EQ(world_->head<P::ChronologyValue>(RefType::Chronology, "NM-294").doses.size(), 1u);
  EXPECT_EQ(world_->head<P::ProductionValue>(RefType::Production, "NM-294/Triga_PR").ratios.size(), 9u);
  EXPECT_EQ(world_->head<P::LevelProductionValue>(RefType::LevelProduction, "NM-294/G").production,
            World::object(RefType::Production, "NM-294/Triga_PR"));
  EXPECT_EQ(world_->count("irradiation"), 2);
  EXPECT_EQ(world_->history(RefType::FluxPosition, "NM-294/G/16").size(), 1u);
  EXPECT_EQ(world_->history(RefType::FluxPosition, flux_key(16)).size(), 2u);
}

TEST_P(MetaImportTest, ChronologyWithAnUnreadableDoseIsAConflict) {
  commit_file("NM-293/chronology.txt", fixture("meta/NM-293/chronology.txt"), kDay1);
  const std::string text = fixture("meta/NM-293/chronology.txt") + "1.0,2017-12-22 06:28,2017-12-22 14:28:00\n";
  const std::string bad = commit_file("NM-293/chronology.txt", text, kDay2);
  auto stats = run_import(*world_, adapter_config(repo_));
  ASSERT_TRUE(stats) << err(stats.error());
  EXPECT_EQ(stats->conflicts, 1);
  auto conflict = store().import_conflict(ingest::conflict_id(kUrl, bad, "NM-293/chronology.txt"));
  ASSERT_TRUE(conflict && conflict->has_value());
  EXPECT_EQ((*conflict)->kind, ConflictKind::Unparseable);
  EXPECT_EQ((*conflict)->file_sha256, std::optional<Sha256Digest>{sha256(std::string_view{text})});
  // The chronology is not stored with a dose short.
  EXPECT_EQ(world_->history(RefType::Chronology, "NM-293").size(), 1u);
  EXPECT_EQ(world_->head<P::ChronologyValue>(RefType::Chronology, "NM-293").doses.size(), 1u);
}

TEST_P(MetaImportTest, MergeLeavesEveryHeadAsTheMergeTreeHasIt) {
  commit_file(kLevel, level_text({}), kDay1);
  repo_.branch("side");
  commit_file(kLevel, level_text({{3, 0.0031}}), kDay2, "main fits 3");
  repo_.checkout("side");
  commit_file(kLevel, level_text({{12, 0.0122}}), kDay2, "side fits 12");
  repo_.checkout("main");
  const std::string merged = repo_.merge("side", "Merge branch 'side'", kDay3);
  // Both sides fit position 20; the merge keeps main's.
  repo_.branch("other");
  commit_file(kLevel, level_text({{3, 0.0031}, {12, 0.0122}, {20, 0.0201}}), kDay3, "main fits 20");
  repo_.checkout("other");
  commit_file(kLevel, level_text({{3, 0.0031}, {12, 0.0122}, {20, 0.0202}}), kDay3, "other fits 20");
  repo_.checkout("main");
  repo_.git({"merge", "--quiet", "--no-ff", "-s", "ours", "-m", "Merge branch 'other'", "other"}, kDay4);
  const std::string kept = repo_.head();

  for (const int batch_commits : {500, 1}) {
    auto world = fresh_world();
    auto stats = run_import(*world, adapter_config(repo_, batch_commits));
    ASSERT_TRUE(stats) << err(stats.error());
    EXPECT_EQ(stats->conflicts, 0);
    const auto j = [&](int position) { return world->head<P::FluxValue>(RefType::FluxPosition, flux_key(position)).j; };
    EXPECT_EQ(j(3), std::optional<double>{0.0031});
    EXPECT_EQ(j(12), std::optional<double>{0.0122});
    EXPECT_EQ(j(20), std::optional<double>{0.0201});
    EXPECT_EQ(j(1), std::optional<double>{0.0018634713371872254});
    // The walk is one line, main's commit before the side's: the side's file
    // puts main's position back, and the merge brings it forward again.
    const auto of_3 = world->history(RefType::FluxPosition, flux_key(3));
    ASSERT_EQ(of_3.size(), 4u);  // as added, main's fit, back at the side's commit, main's again at the merge
    EXPECT_EQ(of_3[3].uuid, ingest::revision_id(kUrl, merged, kLevel + "#3"));
    EXPECT_EQ(world->revision_detail(of_3[1].uuid), json::object());  // an edit
    EXPECT_EQ(world->revision_detail(of_3[2].uuid), json::parse(R"({"walk": "branch"})"));
    EXPECT_EQ(world->revision_detail(of_3[3].uuid), json::parse(R"({"walk": "merge"})"));
    // The side's own fit, at the same commit as that put-back, is an edit.
    EXPECT_EQ(world->revision_detail(world->history(RefType::FluxPosition, flux_key(12))[1].uuid), json::object());
    EXPECT_EQ(world->history(RefType::FluxPosition, flux_key(12)).size(), 2u);
    const auto of_20 = world->history(RefType::FluxPosition, flux_key(20));
    ASSERT_EQ(of_20.size(), 4u);  // as added, main's, the other's, and main's again at the merge
    EXPECT_EQ(of_20[3].uuid, ingest::revision_id(kUrl, kept, kLevel + "#20"));
    EXPECT_EQ(world->history(RefType::FluxPosition, flux_key(1)).size(), 1u);
  }
}

TEST_P(MetaImportTest, SecondRunIsNoOp) {
  write_fixture_files(repo_);
  repo_.write("README.md", "# MetaData\n");
  repo_.commit("initial import", kDay1);
  commit_file(kLevel, level_text({{16, 0.0019}}), kDay2, "fit flux for NM-293G");
  commit_file(kLevel, "garbage", kDay3);
  ASSERT_TRUE(run_import(*world_, adapter_config(repo_)));
  const auto seq = *store().latest_change_seq();
  const auto snapshot = snapshot_of(*world_);
  EXPECT_EQ(world_->conflicts().size(), 1u);

  auto again = run_import(*world_, adapter_config(repo_));
  ASSERT_TRUE(again) << err(again.error());
  EXPECT_TRUE(again->finished);
  EXPECT_EQ(again->batches, 0);
  EXPECT_EQ(*store().latest_change_seq(), seq);
  EXPECT_EQ(snapshot_of(*world_), snapshot);

  // A replay walks everything again and still writes nothing.
  auto replay = writer_config();
  replay.replay = true;
  auto replayed = run_import(*world_, adapter_config(repo_, 1), std::nullopt, replay);
  ASSERT_TRUE(replayed) << err(replayed.error());
  EXPECT_EQ(replayed->batches, 3);
  EXPECT_EQ(*store().latest_change_seq(), seq);
  EXPECT_EQ(snapshot_of(*world_), snapshot);

  // And a dry run finds nothing left to write.
  auto dry = writer_config();
  dry.dry_run = true;
  dry.replay = true;
  auto counted = run_import(*world_, adapter_config(repo_), std::nullopt, dry);
  ASSERT_TRUE(counted) << err(counted.error());
  EXPECT_EQ(counted->would_write, 0);
}

TEST_P(MetaImportTest, ResumeTokenAndRewrittenHistory) {
  const std::string first = commit_file(kLevel, fixture("meta/NM-293/G.json"), kDay1);
  const std::string second = commit_file(kLevel, level_text({{16, 0.0019}}), kDay2);
  auto one = run_import(*world_, adapter_config(repo_, 1), 1);
  ASSERT_TRUE(one) << err(one.error());
  EXPECT_FALSE(one->finished);
  const std::string token = world_->source().progress_token.value_or("");
  EXPECT_EQ(token.substr(0, 43), first + "@0~");
  EXPECT_EQ(world_->source().done, 1);
  EXPECT_EQ(world_->source().total, 2);

  auto rest = run_import(*world_, adapter_config(repo_, 1));
  ASSERT_TRUE(rest) << err(rest.error());
  EXPECT_TRUE(rest->finished);
  EXPECT_EQ(rest->batches, 1);
  EXPECT_EQ(rest->revisions, 1);  // the previous version was not imported again, and was still compared with
  const std::string end = world_->source().progress_token.value_or("");
  EXPECT_EQ(end.substr(0, 43), second + "@1~");
  EXPECT_TRUE(end.ends_with("+end"));

  const auto seq = *store().latest_change_seq();
  repo_.git({"commit", "--quiet", "--amend", "-m", "rewritten"}, kDay3);
  auto stats = run_import(*world_, adapter_config(repo_));
  ASSERT_FALSE(stats);
  EXPECT_NE(stats.error().what.find("history was rewritten"), std::string::npos) << stats.error().what;
  EXPECT_EQ(*store().latest_change_seq(), seq);
  EXPECT_EQ(world_->source().status, "failed");

  // Nor with --replay, which walks from the first commit (fix wave A2).
  auto replay = writer_config();
  replay.replay = true;
  auto replayed = run_import(*world_, adapter_config(repo_), std::nullopt, replay);
  ASSERT_FALSE(replayed);
  EXPECT_NE(replayed.error().what.find("history was rewritten"), std::string::npos) << replayed.error().what;
  EXPECT_EQ(*store().latest_change_seq(), seq);
}

TEST_P(MetaImportTest, OpenAndPlanErrors) {
  commit_file(kLevel, fixture("meta/NM-293/G.json"), kDay1);
  auto no_url = adapter_config(repo_);
  no_url.url.clear();
  EXPECT_FALSE(MetaRepoAdapter::open(no_url));
  auto no_zone = adapter_config(repo_);
  no_zone.lab_time_zone = "Mars/Olympus_Mons";
  auto refused = MetaRepoAdapter::open(no_zone);
  ASSERT_FALSE(refused);
  EXPECT_NE(refused.error().what.find("Mars/Olympus_Mons"), std::string::npos) << refused.error().what;
  auto no_repo = adapter_config(repo_);
  no_repo.git.repo = repo_.temp("nowhere");
  EXPECT_FALSE(MetaRepoAdapter::open(no_repo));

  auto adapter = MetaRepoAdapter::open(adapter_config(repo_));
  ASSERT_TRUE(adapter) << err(adapter.error());
  auto described = (*adapter)->describe();
  ASSERT_TRUE(described);
  EXPECT_EQ(described->kind, P::ImportSourceKind::MetaRepo);
  EXPECT_EQ(described->url, "https://GitHub.com/NMGRLData/MetaData.git");
  EXPECT_EQ(described->branch, "main");
  EXPECT_EQ(described->head, repo_.head());
  EXPECT_FALSE((*adapter)->next_batch());  // not planned
  NoState state;
  EXPECT_FALSE((*adapter)->plan(std::string("yesterday"), state));
  auto planned = (*adapter)->plan(std::nullopt, state);
  ASSERT_TRUE(planned) << err(planned.error());
  EXPECT_EQ(*planned, 1);
  auto batch = (*adapter)->next_batch();
  ASSERT_TRUE(batch && batch->has_value());
  EXPECT_EQ((*batch)->done, 1);
  EXPECT_EQ((*batch)->total, 1);
  ASSERT_EQ((*batch)->changesets.size(), 1u);
  EXPECT_EQ((*batch)->changesets[0].kind, P::ChangesetKind::Reference);
  // Before the first use of each: the irradiation, the level, then the objects.
  ASSERT_GE((*batch)->catalog.size(), 3u);
  EXPECT_TRUE(std::holds_alternative<ingest::IrradiationItem>((*batch)->catalog[0]));
  ASSERT_TRUE(std::holds_alternative<ingest::LevelItem>((*batch)->catalog[1]));
  EXPECT_FALSE(std::get<ingest::LevelItem>((*batch)->catalog[1]).z.has_value());
  EXPECT_TRUE(std::holds_alternative<ingest::RefObjectItem>((*batch)->catalog[2]));
  auto end = (*adapter)->next_batch();
  ASSERT_TRUE(end);
  EXPECT_FALSE(end->has_value());
}

namespace {

// One fixed history with what can go wrong where a walk is cut. Part two is
// what a later run finds.
void build_part_one(GitFixture& repo) {
  const auto put = [&](const std::string& path, const std::string& text, const char* date, const char* message) {
    repo.write(path, text);
    repo.commit(message, date);
  };
  repo.write("README.md", "# MetaData\n");
  repo.write("scripts/felix/measurement/unknown.py", "def main(): pass\n");
  repo.commit("Initial commit", "2017-12-19T10:00:00-07:00");
  write_fixture_files(repo);
  repo.write(kSens, sensitivities(2).dump(4));
  repo.commit("Added irradiation NM-293", kDay1);
  // The level file changes one position, then another.
  std::map<int, double> j{{16, 0.0019}};
  put(kLevel, level_text(j), kDay2, "fit flux for NM-293G");
  j[2] = 0.0021;
  put(kLevel, level_text(j), kDay2, "fit flux for NM-293G");
  json production = json::parse(fixture("meta/NM-293/productions/Triga_PR.json"));
  production["K4039"] = json::array({0.0089, 0.0002});
  put("NM-293/productions/Triga_PR.json", production.dump(4), kDay2, "modified - Triga_PR.json");
  put(kSens, fixture("meta/spectrometers/felix.sens.json"), kDay2, "added sensitivity");
  // Two branches fit different positions.
  repo.branch("side");
  j[3] = 0.0031;
  put(kLevel, level_text(j), kDay3, "main fits 3");
  repo.checkout("side");
  std::map<int, double> on_side = j;
  on_side.erase(3);
  on_side[12] = 0.0122;
  put(kLevel, level_text(on_side), kDay3, "side fits 12");
  repo.checkout("main");
  repo.merge("side", "Merge branch 'side'", kDay3);
  j[12] = 0.0122;
  // Two branches fit the same position; the merge keeps main's. Then again, keeping the other's.
  repo.branch("side2");
  j[20] = 0.0201;
  put(kLevel, level_text(j), kDay3, "main fits 20");
  repo.checkout("side2");
  std::map<int, double> on_side2 = j;
  on_side2[20] = 0.0202;
  put(kLevel, level_text(on_side2), kDay3, "side2 fits 20");
  repo.checkout("main");
  repo.git({"merge", "--quiet", "--no-ff", "-s", "ours", "-m", "Merge branch 'side2'", "side2"}, kDay3);
  repo.branch("side3");
  j[21] = 0.0211;
  put(kLevel, level_text(j), kDay4, "main fits 21");
  repo.checkout("side3");
  std::map<int, double> on_side3 = j;
  on_side3[21] = 0.0212;
  put(kLevel, level_text(on_side3), kDay4, "side3 fits 21");
  repo.checkout("main");
  repo.git({"merge", "--quiet", "--no-ff", "-X", "theirs", "-m", "Merge branch 'side3'", "side3"}, kDay4);
  j[21] = 0.0212;
  // A garbage commit, then the repair, which also fits a position.
  put(kLevel, "{\"positions\": [", kDay4, "interrupted");
  j[9] = 0.0099;
  put(kLevel, level_text(j), kDay4, "fit flux for NM-293G");
  // A sensitivity entry in the middle is corrected.
  json list = sensitivities(4);
  list[1]["sensitivity"] = 4.5e-16;
  put(kSens, list.dump(4), kDay4, "corrected sensitivity");
  // A level moves to a production that has no file.
  json map = json::parse(fixture("meta/NM-293/productions.json"));
  map["G"] = "Cd_shielded";
  put("NM-293/productions.json", map.dump(4), kDay4, "modified - productions.json");
  // Removals. Two positions are emptied, one is filled again; a level leaves
  // productions.json; a holder file is deleted.
  put(kLevel, level_text(j, {22, 23}), kDay4, "emptied 22 and 23");
  put(kLevel, level_text(j, {23}), kDay4, "22 again");
  map.erase("M");
  put("NM-293/productions.json", map.dump(4), kDay4, "level M is gone");
  repo.remove("irradiation_holders/24_hole.txt");
  repo.commit("holder removed", kDay4);
  // One branch fits a position, the other empties one; the merge has both.
  repo.branch("side4");
  j[14] = 0.0141;
  put(kLevel, level_text(j, {23}), kDay4, "main fits 14");
  repo.checkout("side4");
  std::map<int, double> on_side4 = j;
  on_side4.erase(14);
  put(kLevel, level_text(on_side4, {19, 23}), kDay4, "side4 empties 19");
  repo.checkout("main");
  repo.merge("side4", "Merge branch 'side4'", kDay4);
}

void build_part_two(GitFixture& repo) {
  const char* const later = "2019-01-10T10:00:00-07:00";
  std::map<int, double> j{{16, 0.0019}, {2, 0.0021},  {3, 0.0031},  {12, 0.0122}, {20, 0.0201},
                          {21, 0.0212}, {9, 0.0099},  {10, 0.0101}, {14, 0.0141}};
  repo.write(kLevel, level_text(j, {19, 23}));
  repo.commit("fit flux for NM-293G", later);
  json list = sensitivities(4);
  list[1]["sensitivity"] = 4.5e-16;
  json newest = list[3];
  newest["sensitivity"] = 6e-16;
  list.push_back(newest);
  repo.write(kSens, list.dump(4));
  repo.commit("added sensitivity", later);
  repo.write("NM-293/chronology.txt",
             fixture("meta/NM-293/chronology.txt") + "1.0,2017-12-22 06:28:00,2017-12-22 14:28:00\n");
  repo.commit("second day", later);
  // A commit that leaves nothing but a note: the sensitivity list is deleted.
  repo.remove(kSens);
  repo.commit("sensitivities moved to the database", later);
}

}  // namespace

TEST_P(MetaImportTest, OneHistoryOneResult) {
  build_part_one(repo_);

  // An incremental import: part one now, part two when it exists.
  auto incremental = fresh_world();
  auto half = run_import(*incremental, adapter_config(repo_));
  ASSERT_TRUE(half) << err(half.error());
  build_part_two(repo_);
  auto rest = run_import(*incremental, adapter_config(repo_));
  ASSERT_TRUE(rest) << err(rest.error());

  // The reference: the whole history in one uninterrupted batch.
  auto whole = run_import(*world_, adapter_config(repo_));
  ASSERT_TRUE(whole) << err(whole.error());
  EXPECT_EQ(whole->batches, 1);
  const auto want = snapshot_of(*world_);
  ASSERT_FALSE(want.empty());

  // What the reference must hold, whatever else it holds: every head is what
  // the files at the branch head say.
  const std::map<int, double> fitted{{16, 0.0019}, {2, 0.0021},  {3, 0.0031},  {12, 0.0122}, {20, 0.0201},
                                     {21, 0.0212}, {9, 0.0099},  {10, 0.0101}, {14, 0.0141}};
  const json original = json::parse(fixture("meta/NM-293/G.json"));
  for (const auto& entry : original.at("positions")) {
    const int position = entry.at("position").get<int>();
    if (position == 19 || position == 23) {  // emptied, and still empty at the branch head
      EXPECT_EQ(world_->head<P::FluxValue>(RefType::FluxPosition, flux_key(position)), P::FluxValue{}) << position;
      continue;
    }
    if (position == 22) {  // emptied and filled again
      EXPECT_EQ(world_->history(RefType::FluxPosition, flux_key(position)).size(), 3u);
      EXPECT_EQ(world_->head<P::FluxValue>(RefType::FluxPosition, flux_key(position)).j,
                std::optional<double>{entry.at("j").get<double>()});
      continue;
    }
    const auto it = fitted.find(position);
    const double j = it != fitted.end() ? it->second : entry.at("j").get<double>();
    EXPECT_EQ(world_->head<P::FluxValue>(RefType::FluxPosition, flux_key(position)).j, std::optional<double>{j})
        << position;
    // A position no commit changed has the one revision the first commit gave it.
    if (it == fitted.end()) {
      EXPECT_EQ(world_->history(RefType::FluxPosition, flux_key(position)).size(), 1u) << position;
    }
  }
  EXPECT_EQ(world_->history(RefType::FluxPosition, flux_key(9)).size(), 2u);  // the garbage commit cost nothing
  EXPECT_EQ(world_->history(RefType::FluxPosition, flux_key(16)).size(), 2u);
  const auto conflicts = world_->conflicts();
  ASSERT_EQ(conflicts.size(), 1u);
  EXPECT_EQ(conflicts[0].path, kLevel);
  EXPECT_EQ(conflicts[0].kind, ConflictKind::Unparseable);
  EXPECT_EQ(world_->head<P::SensitivityValue>(RefType::Sensitivity, "felix").sensitivity, 6e-16);
  EXPECT_EQ(world_->history(RefType::Sensitivity, "felix").size(), 2u + 2u + 2u + 1u);
  const auto production = world_->head<P::ProductionValue>(RefType::Production, "NM-293/Triga_PR");
  EXPECT_EQ(std::count(production.ratios.begin(), production.ratios.end(), P::ProductionRatio{"K4039", 0.0089, 0.0002}),
            1);
  EXPECT_EQ(world_->history(RefType::Production, "NM-293/Triga_PR").size(), 2u);
  EXPECT_EQ(world_->head<P::LevelProductionValue>(RefType::LevelProduction, "NM-293/G").production,
            World::object(RefType::Production, "NM-293/Cd_shielded"));
  EXPECT_EQ(world_->head<P::ChronologyValue>(RefType::Chronology, "NM-293").doses.size(), 2u);
  EXPECT_EQ(world_->head<P::HolderValue>(RefType::IrradiationHolder, "24_hole"), P::HolderValue{});
  EXPECT_EQ(world_->history(RefType::LevelProduction, "NM-293/M").size(), 1u);

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
  same(*incremental, "part one, then part two");
  replayed(*incremental, "part one, then part two", 3);

  for (const int batch_commits : {1, 2, 500}) {
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
  }
}

// ---------------------------------------------------------------- verify

// The whole of OneHistoryOneResult's history (merges, a garbage commit, files
// that change nothing, removed positions, a deleted list): every file of every
// commit is accounted for, whatever the batch size of the import or of the
// walk that verifies it.
TEST_P(MetaImportTest, VerifyAfterImportIsOk) {
  build_part_one(repo_);
  build_part_two(repo_);
  auto imported = run_import(*world_, adapter_config(repo_, 3));
  ASSERT_TRUE(imported) << err(imported.error());
  const auto rows = snapshot_of(*world_);
  const Uuid source = world_->source().spec.uuid;

  std::optional<ingest::VerifyReport> first;
  for (const int batch_commits : {1, 4, 500}) {
    auto adapter = MetaRepoAdapter::open(adapter_config(repo_, batch_commits));
    ASSERT_TRUE(adapter) << err(adapter.error());
    const auto report = verify_source(*world_, **adapter);
    EXPECT_EQ(unaccounted(report), std::vector<std::string>{}) << batch_commits;
    EXPECT_EQ(report.would_write, 0);
    EXPECT_EQ(report.replay_would_write, 0);
    // The garbage level file is a pending conflict: that alone fails verify.
    EXPECT_EQ(report.pending_blocking, 1) << batch_commits;
    EXPECT_EQ(report.pending_warnings, 0);
    EXPECT_FALSE(report.ok());
    // README.md and the script of the first commit.
    EXPECT_EQ(report.ignored, 2) << batch_commits;
    EXPECT_GT(report.units, 30) << batch_commits;
    if (first) {
      EXPECT_EQ(report.units, first->units) << batch_commits;
    }
    first = report;
  }
  const auto after = snapshot_of(*world_);
  EXPECT_TRUE(after == rows) << "verify wrote something: " << first_difference(after, rows);
  EXPECT_EQ(world_->source().status, "finished");

  resolve_pending(*world_, source);
  auto adapter = MetaRepoAdapter::open(adapter_config(repo_));
  ASSERT_TRUE(adapter) << err(adapter.error());
  EXPECT_TRUE(verify_source(*world_, **adapter).ok());

  // A store that has part of the history: what is missing is listed, and a
  // run would write it.
  auto partial = fresh_world();
  auto some = run_import(*partial, adapter_config(repo_, 2), 3);
  ASSERT_TRUE(some) << err(some.error());
  auto again = MetaRepoAdapter::open(adapter_config(repo_, 2));
  ASSERT_TRUE(again) << err(again.error());
  const auto behind = verify_source(*partial, **again);
  EXPECT_FALSE(behind.ok());
  EXPECT_FALSE(behind.unaccounted.empty());
  EXPECT_GT(behind.would_write, 0);
  EXPECT_GT(behind.replay_would_write, 0);
  EXPECT_EQ(partial->source().status, "paused");
  EXPECT_EQ(behind.source.status, "paused");
  EXPECT_FALSE(behind.source.finished_and_current());
}

// Take one row out of the store: verify names the file version it came from.
TEST_P(MetaImportTest, VerifyReportsAMissingRevisionNoteOrConflict) {
  const std::string added = commit_file(kLevel, fixture("meta/NM-293/G.json"), kDay1, "Added level G to NM-293");
  const std::string fitted = commit_file(kLevel, level_text({{3, 0.0031}}), kDay2, "fit 3");
  const std::string garbage = commit_file(kLevel, "{\"positions\": [", kDay3, "interrupted");
  // The same content in another layout: a version that changes nothing.
  const std::string reformatted =
      commit_file(kLevel, json::parse(level_text({{3, 0.0031}})).dump(1), kDay3, "reformatted");
  repo_.write(kSens, sensitivities(2).dump(4));
  const std::string listed = repo_.commit("sensitivities", kDay3);
  repo_.remove(kSens);
  const std::string emptied = repo_.commit("sensitivities moved", kDay4);
  ASSERT_TRUE(run_import(*world_, adapter_config(repo_)));

  const auto open = [&] {
    auto adapter = MetaRepoAdapter::open(adapter_config(repo_, 2));
    EXPECT_TRUE(adapter);
    return adapter ? verify_source(*world_, **adapter) : ingest::VerifyReport{};
  };
  auto report = open();
  EXPECT_EQ(unaccounted(report), std::vector<std::string>{});
  EXPECT_EQ(report.units, 6);
  EXPECT_EQ(report.pending_blocking, 1);

  // One revision of a file that holds many: the provenance row of position 3 at the fit.
  ASSERT_EQ(forget(*world_, "DELETE FROM import_provenance WHERE commit_sha = ? AND path = ?",
                   {pd::qv(fitted), pd::qv(kLevel + "#3")}),
            1);
  report = open();
  // The fit, and the reformatted version, whose content is in the fit's rows.
  EXPECT_EQ(unaccounted(report), sorted({unit_name(fitted, kLevel), unit_name(reformatted, kLevel)}));
  for (const std::string& commit : {fitted, reformatted}) {
    const auto& unit = unaccounted_unit(report, commit, kLevel);
    ASSERT_EQ(unit.missing.size(), 1u);
    EXPECT_EQ(unit.missing[0].commit, fitted);
    EXPECT_EQ(unit.missing[0].path, kLevel + "#3");
  }
  EXPECT_EQ(unaccounted_unit(report, fitted, kLevel).unit.disposition, ingest::UnitDisposition::Imported);
  EXPECT_EQ(unaccounted_unit(report, reformatted, kLevel).unit.disposition, ingest::UnitDisposition::Unchanged);
  EXPECT_EQ(report.replay_would_write, 1);
  EXPECT_EQ(report.would_write, 0);  // the token is at the end: only a replay sees it

  // The conflict of the file that could not be read.
  ASSERT_EQ(forget(*world_, "DELETE FROM import_conflict WHERE path = ?", {pd::qv(kLevel)}), 1);
  EXPECT_EQ(unaccounted(open()),
            sorted({unit_name(fitted, kLevel), unit_name(garbage, kLevel), unit_name(reformatted, kLevel)}));

  // The note of a removal that has no revision: the deleted sensitivity list.
  ASSERT_EQ(forget(*world_, "DELETE FROM import_provenance WHERE entity_type = 'changeset' AND commit_sha = ?",
                   {pd::qv(emptied)}),
            1);
  report = open();
  EXPECT_EQ(unaccounted(report), sorted({unit_name(fitted, kLevel), unit_name(garbage, kLevel),
                                         unit_name(reformatted, kLevel), unit_name(emptied, kSens)}));
  const auto& gone = unaccounted_unit(report, emptied, kSens);
  EXPECT_TRUE(gone.unit.deleted);
  ASSERT_EQ(gone.missing.size(), 1u);
  EXPECT_EQ(gone.missing[0].kind, ingest::Evidence::Kind::Note);
  EXPECT_EQ(gone.missing[0].list, "removed");
  (void)added;
  (void)listed;
}

// A position that goes A, B, A has three revisions; the third is accounted
// for by its own row only, though the file is then byte for byte the first.
TEST_P(MetaImportTest, VerifyReportsARevisionDroppedFromAnABAHistory) {
  commit_file(kLevel, level_text({}), kDay1, "Added level G to NM-293");
  commit_file(kLevel, level_text({{3, 0.0031}}), kDay2, "fit 3");
  const std::string back = commit_file(kLevel, level_text({}), kDay3, "back");
  ASSERT_TRUE(run_import(*world_, adapter_config(repo_)));
  ASSERT_EQ(world_->history(RefType::FluxPosition, flux_key(3)).size(), 3u);
  const auto listed = [&](int batch_commits) {
    auto adapter = MetaRepoAdapter::open(adapter_config(repo_, batch_commits));
    EXPECT_TRUE(adapter);
    return adapter ? unaccounted(verify_source(*world_, **adapter)) : std::vector<std::string>{};
  };
  EXPECT_EQ(listed(1), std::vector<std::string>{});
  ASSERT_EQ(forget(*world_, "DELETE FROM import_provenance WHERE commit_sha = ? AND path = ?",
                   {pd::qv(back), pd::qv(kLevel + "#3")}),
            1);
  for (const int batch_commits : {1, 500})
    EXPECT_EQ(listed(batch_commits), std::vector<std::string>{unit_name(back, kLevel)}) << batch_commits;
}

INSTANTIATE_TEST_SUITE_P(Engines, MetaImportTest, ::testing::ValuesIn(P::testing::engines()));
