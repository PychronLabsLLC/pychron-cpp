// MetaRepoAdapter: the history of a legacy MetaData repository, built in a
// GitFixture from the real files under fixtures/meta, imported into a real
// store through BatchWriter. Expected values are the fixtures' literals.

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
#include "pychron/core/sha256.hpp"
#include "pychron/dvc/meta_adapter.hpp"
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

// The fixture level file with the J of some positions (by hole number) replaced.
std::string level_text(const std::map<int, double>& j) {
  json level = json::parse(fixture("meta/NM-293/G.json"));
  for (auto& entry : level.at("positions"))
    if (const auto it = j.find(entry.at("position").get<int>()); it != j.end()) entry["j"] = it->second;
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
  rows("flux", QStringLiteral("SELECT revision_uuid, j, j_err, mean_j, extra FROM flux_value"),
       {"revision_uuid", "j", "j_err", "mean_j"}, "extra");
  rows("flux_analysis", QStringLiteral("SELECT revision_uuid, record_id, is_omitted FROM flux_value_analysis"),
       {"revision_uuid", "record_id", "is_omitted"});
  rows("level_z", QStringLiteral("SELECT revision_uuid, z FROM level_z_value"), {"revision_uuid", "z"});
  rows("production", QStringLiteral("SELECT revision_uuid, key, value, error FROM production_value"),
       {"revision_uuid", "key", "value", "error"});
  rows("level_production", QStringLiteral("SELECT revision_uuid, production_ref_uuid, note FROM level_production_value"),
       {"revision_uuid", "production_ref_uuid", "note"});
  rows("dose", QStringLiteral("SELECT revision_uuid, ordinal, power FROM chronology_dose"),
       {"revision_uuid", "ordinal", "power"});
  rows("gain", QStringLiteral("SELECT revision_uuid, detector, gain FROM detector_gain"),
       {"revision_uuid", "detector", "gain"});
  rows("sensitivity", QStringLiteral("SELECT revision_uuid, sensitivity, extra FROM sensitivity_value"),
       {"revision_uuid", "sensitivity"}, "extra");
  rows("hole", QStringLiteral("SELECT revision_uuid, ordinal, hole_id, x, y FROM holder_hole"),
       {"revision_uuid", "ordinal", "hole_id", "x", "y"});
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
  EXPECT_EQ(world_->count("level", "WHERE name = 'G' AND z = 0"), 1);
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

TEST_P(MetaImportTest, DeletedFileAddsNothingAndRestoredIsNotRepeated) {
  commit_file(kLevel, fixture("meta/NM-293/G.json"), kDay1);
  repo_.remove(kLevel);
  repo_.commit("removed by hand", kDay2);
  commit_file(kLevel, fixture("meta/NM-293/G.json"), kDay3, "restored");
  auto stats = run_import(*world_, adapter_config(repo_, 1));
  ASSERT_TRUE(stats) << err(stats.error());
  EXPECT_EQ(stats->batches, 3);
  EXPECT_EQ(world_->count("changeset"), 1);
  EXPECT_EQ(world_->count("revision"), 24);
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
  EXPECT_EQ(std::get<ingest::LevelItem>((*batch)->catalog[1]).z, std::optional<double>{0.0});
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
}

void build_part_two(GitFixture& repo) {
  const char* const later = "2019-01-10T10:00:00-07:00";
  std::map<int, double> j{{16, 0.0019}, {2, 0.0021}, {3, 0.0031},  {12, 0.0122},
                          {20, 0.0201}, {21, 0.0212}, {9, 0.0099}, {10, 0.0101}};
  repo.write(kLevel, level_text(j));
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
  const std::map<int, double> fitted{{16, 0.0019}, {2, 0.0021}, {3, 0.0031},  {12, 0.0122},
                                     {20, 0.0201}, {21, 0.0212}, {9, 0.0099}, {10, 0.0101}};
  const json original = json::parse(fixture("meta/NM-293/G.json"));
  for (const auto& entry : original.at("positions")) {
    const int position = entry.at("position").get<int>();
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

INSTANTIATE_TEST_SUITE_P(Engines, MetaImportTest, ::testing::ValuesIn(P::testing::engines()));
