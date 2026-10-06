// CatalogAdapter end to end: a converted dump of the legacy catalog, read by
// the adapter, written by the BatchWriter, read back from the store.
//
// fixtures/catalog is what tools/legacy_dump_to_jsonl.py writes for
// tools/tests/fixtures/legacy_catalog.sql (a test of the tool checks that it
// is current). The dump has rows that must be refused, and rows that are
// imported without a link; the comments in it say which and why.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

#include "pychron/core/sha256.hpp"
#include "pychron/dvc/catalog_adapter.hpp"
#include "pychron/ingest/ids.hpp"
#include "pychron/ingest/writer.hpp"
#include "sql/geometry.hpp"
#include "store_fixture.hpp"
#include "verify_support.hpp"

using namespace pychron;
using namespace pychron::dvc;
namespace P = pychron::persistence;
namespace pd = pychron::persistence::detail;
using ingest::RunStats;
using nlohmann::json;
using P::ConflictKind;
using P::Uuid;

namespace {

const std::filesystem::path kFixture = std::filesystem::path(PYCHRON_DVC_FIXTURES_DIR) / "catalog";
const char* const kZone = "America/Denver";
// Rows of the fixture's catalog tables: one unit each.
constexpr int kRows = 47;

std::string err(const Error& e) { return to_string(e); }

std::string fixture_sha() {
  std::ifstream in(kFixture / "MANIFEST.json", std::ios::binary);
  return json::parse(in).at("sha256").get<std::string>();
}

// A converted dump written by the test: table files and their manifest.
class DumpDir {
 public:
  DumpDir() {
    static std::atomic<int> counter{0};
    path_ = std::filesystem::temp_directory_path() /
            ("pychron_catalog_" + std::to_string(std::random_device{}() % 1000000) + "_" +
             std::to_string(counter.fetch_add(1)));
    std::filesystem::create_directories(path_);
  }
  ~DumpDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }
  DumpDir(const DumpDir&) = delete;
  DumpDir& operator=(const DumpDir&) = delete;

  const std::filesystem::path& path() const { return path_; }

  // One line per row, as given.
  DumpDir& table(const std::string& name, const std::vector<std::string>& lines) {
    std::ofstream out(path_ / (name + ".jsonl"), std::ios::binary);
    for (const auto& line : lines) out << line << "\n";
    manifest_["tables"][name] = lines.size();
    return *this;
  }
  DumpDir& set(const std::string& key, json value) {
    manifest_[key] = std::move(value);
    return *this;
  }
  // The manifest is written last, as the converter does.
  DumpDir& done() {
    std::ofstream out(path_ / "MANIFEST.json", std::ios::binary);
    out << manifest_.dump(2) << "\n";
    return *this;
  }

 private:
  std::filesystem::path path_;
  json manifest_ = {{"tables", json::object()}, {"sha256", std::string(64, 'a')}, {"time_zone", "+00:00"}};
};

// A fresh database: the store under test and a white-box connection to it.
struct World {
  explicit World(const std::string& engine) : database(engine, true) {
    store = P::testing::open_or_die(database.url());
    if (!store) return;
    client = *store->register_client({"import-1", "importer", std::nullopt, "test"});
    auto opened = pd::Db::open(P::StoreConfig{database.url(), false});
    if (opened) db = std::move(*opened);
  }

  long long count(const char* table) {
    auto row = db->select_one(QStringLiteral("SELECT count(*) AS n FROM %1").arg(QString::fromUtf8(table)));
    return row && *row ? (*row)->value("n").toLongLong() : -1;
  }

  // The one row a query finds.
  pd::Row one(const std::string& sql) {
    auto rows = db->select(QString::fromStdString(sql));
    EXPECT_TRUE(rows) << sql;
    if (!rows) return {};
    EXPECT_EQ(rows->size(), 1u) << sql;
    return rows->size() == 1 ? rows->front() : pd::Row{};
  }

  std::string text(const std::string& sql) { return pd::to_std(one(sql).value("v")); }
  bool is_null(const std::string& sql) { return one(sql).value("v").isNull(); }
  std::string time(const std::string& sql) { return pd::to_time(one(sql).value("v")).iso(); }

  std::vector<P::ImportConflictRow> conflicts() {
    auto rows = store->import_conflicts({std::nullopt, std::nullopt, std::nullopt});
    EXPECT_TRUE(rows);
    return rows ? *rows : std::vector<P::ImportConflictRow>{};
  }

  P::ImportSourceInfo source() {
    auto all = store->import_sources();
    EXPECT_TRUE(all && all->size() == 1);
    return all && all->size() == 1 ? all->front() : P::ImportSourceInfo{};
  }

  // Declared first so it is destroyed last: the connections point into it.
  P::testing::TestDatabase database;
  std::unique_ptr<P::IStore> store;
  Uuid client;
  std::unique_ptr<pd::Db> db;
};

CatalogAdapterConfig adapter_config(const std::filesystem::path& dir = kFixture, int batch_rows = 2000) {
  return {dir, kZone, batch_rows};
}

ingest::WriterConfig writer_config() {
  ingest::WriterConfig c;
  c.importer_version = "pychron-import/test";
  c.lab_time_zone = kZone;
  return c;
}

Result<RunStats> run_import(World& w, const CatalogAdapterConfig& config, std::optional<int> max_batches = std::nullopt,
                            ingest::WriterConfig writer = writer_config()) {
  auto adapter = CatalogAdapter::open(config);
  if (!adapter) return fail(adapter.error());
  ingest::BatchWriter batches(*w.store, w.client, std::move(writer));
  return batches.run(**adapter, max_batches, {}, {});
}

// Every stored row a catalog import decides. Left out is what differs by
// design from run to run: write times, change sequence numbers, and the ids
// the store makes up itself (rows are joined by name instead).
std::vector<std::string> snapshot_of(World& w) {
  std::vector<std::string> out;
  const auto rows = [&](const char* label, const char* sql, const std::vector<const char*>& columns,
                        const char* json_column = nullptr) {
    auto found = w.db->select(QString::fromUtf8(sql));
    ASSERT_TRUE(found) << label;
    std::vector<std::string> lines;
    for (const auto& row : *found) {
      std::string line = label;
      for (const char* column : columns) {
        const QVariant value = row.value(column);
        line += " | " + (value.isNull() ? std::string("NULL") : pd::to_std(value));
      }
      if (json_column) line += " | " + json::parse(pd::to_std(row.value(json_column))).dump();
      lines.push_back(std::move(line));
    }
    std::sort(lines.begin(), lines.end());
    out.insert(out.end(), lines.begin(), lines.end());
  };
  rows("pi", "SELECT uuid, last_name, first_initial, affiliation, email FROM principal_investigator",
       {"uuid", "last_name", "first_initial", "affiliation", "email"});
  rows("project",
       "SELECT p.uuid AS uuid, p.name AS name, p.pi_uuid AS pi_uuid, p.checkin_date AS checkin_date, "
       "p.comment AS comment, p.lab_contact AS lab_contact, p.institution AS institution FROM project p",
       {"uuid", "name", "pi_uuid", "checkin_date", "comment", "lab_contact", "institution"});
  rows("material", "SELECT uuid, name, grainsize FROM material", {"uuid", "name", "grainsize"});
  // The fixture's one sample with legacy times is HH-1; the other's are write times.
  rows("sample",
       "SELECT uuid, name, project_uuid, material_uuid, note, igsn, geom, elevation, storage_location, location, "
       "unit, lithology, lithology_class, lithology_type, lithology_group, approximate_age, "
       "CASE WHEN name = 'HH-1' THEN created_utc END AS created, "
       "CASE WHEN name = 'HH-1' THEN updated_utc END AS updated FROM sample",
       {"uuid", "name", "project_uuid", "material_uuid", "note", "igsn", "geom", "elevation", "storage_location",
        "location", "unit", "lithology", "lithology_class", "lithology_type", "lithology_group", "approximate_age",
        "created", "updated"});
  rows("irradiation", "SELECT name, CASE WHEN name = 'NM-300' THEN created_utc END AS created FROM irradiation",
       {"name", "created"});
  rows("level",
       "SELECT l.uuid AS uuid, i.name AS irradiation, l.name AS name, l.holder_ref_uuid AS holder, l.z AS z, "
       "l.note AS note FROM level l JOIN irradiation i ON i.uuid = l.irradiation_uuid",
       {"uuid", "irradiation", "name", "holder", "z", "note"});
  rows("position", "SELECT uuid, level_uuid, position, sample_uuid, weight, packet, note FROM irradiation_position",
       {"uuid", "level_uuid", "position", "sample_uuid", "weight", "packet", "note"});
  rows("identifier", "SELECT uuid, identifier, kind, position_uuid, sample_uuid FROM identifier",
       {"uuid", "identifier", "kind", "position_uuid", "sample_uuid"});
  rows("ref_object", "SELECT uuid, ref_type, key FROM ref_object", {"uuid", "ref_type", "key"});
  rows("user", "SELECT name, email, affiliation, category FROM app_user", {"name", "email", "affiliation", "category"});
  rows("mass_spectrometer", "SELECT uuid, name, kind, code FROM mass_spectrometer", {"uuid", "name", "kind", "code"});
  rows("extract_device", "SELECT name FROM extract_device", {"name"});
  rows("load",
       "SELECT l.uuid AS uuid, l.name AS name, l.holder_ref_uuid AS holder, u.name AS creator, l.archived AS archived, "
       "l.created_utc AS created FROM load l LEFT JOIN app_user u ON u.uuid = l.created_by_user_uuid",
       {"uuid", "name", "holder", "creator", "archived", "created"});
  rows("load_position", "SELECT load_uuid, position, identifier_uuid, weight, nxtals, note FROM load_position",
       {"load_uuid", "position", "identifier_uuid", "weight", "nxtals", "note"});
  rows("conflict", "SELECT uuid, path, conflict_kind, resolution, detail FROM import_conflict",
       {"uuid", "path", "conflict_kind", "resolution"}, "detail");
  rows("source",
       "SELECT uuid, kind, url_or_path, head_commit_sha, progress_commit_sha, commits_total, commits_done, status "
       "FROM import_source",
       {"uuid", "kind", "url_or_path", "head_commit_sha", "progress_commit_sha", "commits_total", "commits_done",
        "status"});
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

// plan() takes a state the adapter does not ask anything of.
struct NoState final : ingest::IImportState {
  Result<std::optional<std::string>> head_blob_sha(const ingest::SubjectRef&, P::Kind) override {
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

// Every batch of an adapter, from the start.
std::vector<ingest::ImportBatch> all_batches(CatalogAdapter& adapter) {
  NoState state;
  std::vector<ingest::ImportBatch> out;
  EXPECT_TRUE(adapter.plan(std::nullopt, state));
  for (;;) {
    auto batch = adapter.next_batch();
    EXPECT_TRUE(batch);
    if (!batch || !*batch) return out;
    out.push_back(std::move(**batch));
  }
}

// The one batch of a small dump.
ingest::ImportBatch only_batch(const DumpDir& dir) {
  auto adapter = CatalogAdapter::open(adapter_config(dir.path()));
  EXPECT_TRUE(adapter) << (adapter ? "" : err(adapter.error()));
  if (!adapter) return {};
  auto batches = all_batches(**adapter);
  EXPECT_EQ(batches.size(), 1u);
  return batches.empty() ? ingest::ImportBatch{} : batches.front();
}

template <class Item>
std::vector<Item> items_of(const ingest::ImportBatch& batch) {
  std::vector<Item> out;
  for (const auto& item : batch.catalog)
    if (const auto* typed = std::get_if<Item>(&item)) out.push_back(*typed);
  return out;
}

json detail_of(const ingest::ConflictItem& conflict) { return json::parse(conflict.detail_json); }

// The rows a batch accounts for: a row is an item (with a conflict for each
// thing of it that was not applied, "imported": true) or a refusal.
std::size_t rows_of(const ingest::ImportBatch& batch) {
  std::size_t refusals = 0;
  for (const auto& conflict : batch.conflicts)
    if (detail_of(conflict).at("imported") == false) ++refusals;
  return batch.catalog.size() + refusals;
}

const char* name_of(const ingest::CatalogItem& item) {
  struct Name {
    const char* operator()(const ingest::PiItem&) const { return "pi"; }
    const char* operator()(const ingest::ProjectItem&) const { return "project"; }
    const char* operator()(const ingest::MaterialItem&) const { return "material"; }
    const char* operator()(const ingest::SampleItem&) const { return "sample"; }
    const char* operator()(const ingest::IrradiationItem&) const { return "irradiation"; }
    const char* operator()(const ingest::LevelItem&) const { return "level"; }
    const char* operator()(const ingest::PositionItem&) const { return "position"; }
    const char* operator()(const ingest::SpecialIdentifierItem&) const { return "special"; }
    const char* operator()(const ingest::UserItem&) const { return "user"; }
    const char* operator()(const ingest::MassSpecItem&) const { return "mass_spectrometer"; }
    const char* operator()(const ingest::ExtractDeviceItem&) const { return "extract_device"; }
    const char* operator()(const ingest::LoadItem&) const { return "load"; }
    const char* operator()(const ingest::RepositoryItem&) const { return "repository"; }
    const char* operator()(const ingest::RefObjectItem&) const { return "ref_object"; }
    const char* operator()(const ingest::InterpretedAgeItem&) const { return "interpreted_age"; }
    const char* operator()(const ingest::LoadPositionItem&) const { return "load_position"; }
  };
  return std::visit(Name{}, item);
}

class CatalogDb : public ::testing::TestWithParam<std::string> {
 protected:
  void SetUp() override {
    world_ = std::make_unique<World>(GetParam());
    ASSERT_TRUE(world_->store);
    ASSERT_TRUE(world_->db);
  }

  std::unique_ptr<World> fresh_world() { return std::make_unique<World>(GetParam()); }

  void import_fixture() {
    auto stats = run_import(*world_, adapter_config());
    ASSERT_TRUE(stats) << err(stats.error());
    ASSERT_TRUE(stats->finished);
  }

  std::unique_ptr<World> world_;
};

}  // namespace

// ---------------------------------------------------------------- the fixture, stored

TEST_P(CatalogDb, ImportsInForeignKeyOrder) {
  auto stats = run_import(*world_, adapter_config());
  ASSERT_TRUE(stats) << err(stats.error());
  EXPECT_TRUE(stats->finished);
  EXPECT_EQ(stats->batches, 1);
  EXPECT_EQ(stats->conflicts, 11);

  // The third material is the placeholder of the sample that has none.
  const std::map<std::string, long long> want{
      {"principal_investigator", 3}, {"project", 4},  {"material", 3},          {"sample", 4},
      {"irradiation", 2},            {"level", 3},    {"irradiation_position", 6}, {"identifier", 5},
      {"app_user", 2},               {"mass_spectrometer", 2}, {"extract_device", 2}, {"load", 4},
      {"load_position", 4},          {"ref_object", 2}, {"import_conflict", 11}};
  for (const auto& [table, n] : want) EXPECT_EQ(world_->count(table.c_str()), n) << table;

  // A sample is linked to its project (and that to its investigator) and its material.
  auto r = world_->one("SELECT p.name AS project, i.last_name AS pi, m.name AS material, m.grainsize AS grainsize "
                       "FROM sample s JOIN project p ON p.uuid = s.project_uuid "
                       "JOIN principal_investigator i ON i.uuid = p.pi_uuid "
                       "JOIN material m ON m.uuid = s.material_uuid WHERE s.name = 'HH-1'");
  EXPECT_EQ(pd::to_std(r.value("project")), "Henry Hill");
  EXPECT_EQ(pd::to_std(r.value("pi")), "Ross");
  EXPECT_EQ(pd::to_std(r.value("material")), "Sanidine");
  EXPECT_EQ(pd::to_std(r.value("grainsize")), "");
  // A project may have no investigator; the material named by its repeated row is the one material.
  r = world_->one("SELECT p.name AS project, p.pi_uuid AS pi, m.name AS material FROM sample s "
                  "JOIN project p ON p.uuid = s.project_uuid JOIN material m ON m.uuid = s.material_uuid "
                  "WHERE s.name = 'FC-2'");
  EXPECT_EQ(pd::to_std(r.value("project")), "REFERENCES");
  EXPECT_TRUE(r.value("pi").isNull());
  EXPECT_EQ(pd::to_std(r.value("material")), "Sanidine");

  // An identifier is linked to its position, that to its level, sample and irradiation.
  r = world_->one("SELECT d.kind AS kind, p.position AS position, l.name AS level, i.name AS irradiation, "
                  "s.name AS sample FROM identifier d JOIN irradiation_position p ON p.uuid = d.position_uuid "
                  "JOIN level l ON l.uuid = p.level_uuid JOIN irradiation i ON i.uuid = l.irradiation_uuid "
                  "JOIN sample s ON s.uuid = p.sample_uuid WHERE d.identifier = '66573'");
  EXPECT_EQ(pd::to_std(r.value("kind")), "unknown");
  EXPECT_EQ(r.value("position").toInt(), 1);
  EXPECT_EQ(pd::to_std(r.value("level")), "A");
  EXPECT_EQ(pd::to_std(r.value("irradiation")), "NM-300");
  EXPECT_EQ(pd::to_std(r.value("sample")), "HH-1");
  EXPECT_EQ(world_->text("SELECT l.name AS v FROM identifier d JOIN irradiation_position p ON p.uuid = "
                         "d.position_uuid JOIN level l ON l.uuid = p.level_uuid WHERE d.identifier = '66600'"),
            "B");
  // An empty hole is a position without an identifier or a sample.
  r = world_->one("SELECT p.sample_uuid AS sample, (SELECT count(*) FROM identifier d WHERE d.position_uuid = p.uuid) "
                  "AS identifiers FROM irradiation_position p WHERE p.position = 3");
  EXPECT_TRUE(r.value("sample").isNull());
  EXPECT_EQ(r.value("identifiers").toInt(), 0);

  // A level keeps its holder, height and note.
  r = world_->one("SELECT o.ref_type AS ref_type, o.key AS holder, l.z AS z, l.note AS note FROM level l "
                  "JOIN irradiation i ON i.uuid = l.irradiation_uuid JOIN ref_object o ON o.uuid = l.holder_ref_uuid "
                  "WHERE i.name = 'NM-300' AND l.name = 'A'");
  EXPECT_EQ(pd::to_std(r.value("ref_type")), "irradiation_holder");
  EXPECT_EQ(pd::to_std(r.value("holder")), "24Spokes");
  EXPECT_DOUBLE_EQ(r.value("z").toDouble(), 0.5);
  EXPECT_EQ(pd::to_std(r.value("note")), "bottom of the can");

  // The ids are the ones every importer derives from the natural keys.
  EXPECT_EQ(world_->text("SELECT uuid AS v FROM identifier WHERE identifier = '66573'"),
            ingest::catalog_id("identifier", "66573").str());
  EXPECT_EQ(world_->text("SELECT uuid AS v FROM mass_spectrometer WHERE name = 'jan'"),
            ingest::catalog_id("mass_spectrometer", "jan").str());

  const auto source = world_->source();
  EXPECT_EQ(source.spec.kind, P::ImportSourceKind::LegacyDb);
  EXPECT_EQ(source.spec.url_or_path, std::filesystem::absolute(kFixture).lexically_normal().string());
  EXPECT_EQ(source.head_sha, std::optional<std::string>{fixture_sha()});
  EXPECT_EQ(source.status, "finished");
  EXPECT_EQ(source.done, kRows);
  EXPECT_EQ(source.total, kRows);
}

TEST_P(CatalogDb, SampleKeepsLatLonAndNote) {
  import_fixture();
  const auto r = world_->one("SELECT * FROM sample WHERE name = 'HH-1'");
  EXPECT_EQ(pd::to_std(r.value("note")), "collected at the base, north side");
  EXPECT_EQ(pd::to_std(r.value("igsn")), "IGSN001");
  const auto point = pd::parse_point(pd::to_std(r.value("geom")));  // the legacy lat and lon, one point
  ASSERT_TRUE(point);
  EXPECT_DOUBLE_EQ(point->lat, 34.0722);
  EXPECT_DOUBLE_EQ(point->lon, -106.905);
  EXPECT_DOUBLE_EQ(r.value("elevation").toDouble(), 1890.5);
  EXPECT_EQ(pd::to_std(r.value("storage_location")), "shelf 3");
  EXPECT_EQ(pd::to_std(r.value("location")), "Socorro, NM");
  EXPECT_EQ(pd::to_std(r.value("unit")), "Tuff of Henry Hill");
  EXPECT_EQ(pd::to_std(r.value("lithology")), "ignimbrite");
  EXPECT_EQ(pd::to_std(r.value("lithology_class")), "volcanic");
  EXPECT_EQ(pd::to_std(r.value("lithology_type")), "pyroclastic");
  EXPECT_EQ(pd::to_std(r.value("lithology_group")), "Mogollon");
  EXPECT_DOUBLE_EQ(r.value("approximate_age").toDouble(), 28.2);
  // DATETIME columns are naive lab time: 09:30 MST, and 01:30 on the night
  // the clocks went back, read as the earlier (MDT) instant.
  EXPECT_EQ(pd::to_time(r.value("created_utc")).iso(), "2016-03-01T16:30:00.000000Z");
  EXPECT_EQ(pd::to_time(r.value("updated_utc")).iso(), "2016-11-06T07:30:00.000000Z");
  // A sample without them has no legacy columns at all.
  EXPECT_TRUE(world_->is_null("SELECT note AS v FROM sample WHERE name = 'FC-2'"));
  EXPECT_TRUE(world_->is_null("SELECT geom AS v FROM sample WHERE name = 'FC-2'"));
}

TEST_P(CatalogDb, ProjectKeepsLegacyColumns) {
  import_fixture();
  const auto r = world_->one("SELECT * FROM project WHERE name = 'Henry Hill'");
  EXPECT_EQ(pd::to_std(r.value("checkin_date")).substr(0, 10), "2016-02-29");
  EXPECT_EQ(pd::to_std(r.value("comment")), "two crates; 'handle' with care");
  EXPECT_EQ(pd::to_std(r.value("lab_contact")), "mheizler");
  EXPECT_EQ(pd::to_std(r.value("institution")), "NMT");
  // MySQL's zero date is no date.
  EXPECT_TRUE(world_->is_null("SELECT checkin_date AS v FROM project WHERE name = 'J-Curve'"));
  EXPECT_EQ(world_->text("SELECT i.last_name AS v FROM project p JOIN principal_investigator i ON i.uuid = p.pi_uuid "
                         "WHERE p.name = 'J-Curve'"),
            "Heizler");
  const auto pi = world_->one("SELECT * FROM principal_investigator WHERE last_name = 'Ross'");
  EXPECT_EQ(pd::to_std(pi.value("first_initial")), "J");
  EXPECT_EQ(pd::to_std(pi.value("affiliation")), "NMT");
  EXPECT_EQ(pd::to_std(pi.value("email")), "ross@nmt.edu");
}

TEST_P(CatalogDb, PositionUserSpectrometerAndLoadKeepTheirColumns) {
  import_fixture();
  auto r = world_->one("SELECT p.weight AS weight, p.packet AS packet, p.note AS note FROM irradiation_position p "
                       "JOIN identifier d ON d.position_uuid = p.uuid WHERE d.identifier = '66573'");
  EXPECT_DOUBLE_EQ(r.value("weight").toDouble(), 12.5);
  EXPECT_EQ(pd::to_std(r.value("packet")), "p1");
  EXPECT_EQ(pd::to_std(r.value("note")), "chipped");

  r = world_->one("SELECT * FROM app_user WHERE name = 'mheizler'");
  EXPECT_EQ(pd::to_std(r.value("email")), "m@nmt.edu");
  EXPECT_EQ(pd::to_std(r.value("affiliation")), "NMT");
  EXPECT_EQ(pd::to_std(r.value("category")), "staff");

  // Spectrometer names are lower case, as the analyses and reference data name them.
  EXPECT_EQ(world_->text("SELECT kind AS v FROM mass_spectrometer WHERE name = 'jan'"), "Argus VI");
  EXPECT_EQ(world_->text("SELECT kind AS v FROM mass_spectrometer WHERE name = 'felix'"), "Helix SFT");
  EXPECT_EQ(world_->count("extract_device"), 2);
  EXPECT_EQ(world_->text("SELECT name AS v FROM extract_device WHERE name LIKE '%CO2'"), "Fusions CO2");

  // TIMESTAMP columns of a dump that set its session zone to +00:00 are UTC.
  EXPECT_EQ(world_->time("SELECT created_utc AS v FROM irradiation WHERE name = 'NM-300'"),
            "2018-01-15T17:00:00.000000Z");
  r = world_->one("SELECT l.created_utc AS created, l.archived AS archived, u.name AS creator, o.key AS holder, "
                  "o.ref_type AS ref_type FROM load l JOIN app_user u ON u.uuid = l.created_by_user_uuid "
                  "JOIN ref_object o ON o.uuid = l.holder_ref_uuid WHERE l.name = 'L-101'");
  EXPECT_EQ(pd::to_time(r.value("created")).iso(), "2018-03-01T18:00:00.000000Z");
  EXPECT_FALSE(r.value("archived").toBool());
  EXPECT_EQ(pd::to_std(r.value("creator")), "mheizler");
  EXPECT_EQ(pd::to_std(r.value("holder")), "221-hole");
  EXPECT_EQ(pd::to_std(r.value("ref_type")), "load_holder");
  r = world_->one("SELECT archived, created_by_user_uuid, holder_ref_uuid FROM load WHERE name = 'L-102'");
  EXPECT_TRUE(r.value("archived").toBool());
  EXPECT_TRUE(r.value("created_by_user_uuid").isNull());
  EXPECT_TRUE(r.value("holder_ref_uuid").isNull());

  // String keys match as MySQL's collations match them: 'JRoss ' is the user jross, 'l-101 ' the load L-101.
  EXPECT_EQ(world_->text("SELECT u.name AS v FROM load l JOIN app_user u ON u.uuid = l.created_by_user_uuid "
                         "WHERE l.name = 'L-104'"),
            "jross");
  EXPECT_EQ(world_->text("SELECT d.identifier AS v FROM load_position p JOIN load l ON l.uuid = p.load_uuid "
                         "JOIN identifier d ON d.uuid = p.identifier_uuid WHERE l.name = 'L-101' AND p.position = 4"),
            "66600");
  EXPECT_EQ(world_->count("app_user"), 2);

  r = world_->one("SELECT p.weight AS weight, p.nxtals AS nxtals, p.note AS note, d.identifier AS identifier "
                  "FROM load_position p JOIN load l ON l.uuid = p.load_uuid "
                  "JOIN identifier d ON d.uuid = p.identifier_uuid WHERE l.name = 'L-101' AND p.position = 1");
  EXPECT_DOUBLE_EQ(r.value("weight").toDouble(), 1.5);
  EXPECT_EQ(r.value("nxtals").toInt(), 2);
  EXPECT_EQ(pd::to_std(r.value("note")), "big");
  EXPECT_EQ(pd::to_std(r.value("identifier")), "66573");
}

TEST_P(CatalogDb, DanglingForeignKeyIsConflict) {
  import_fixture();
  const std::string sha = fixture_sha();
  const std::string url = std::filesystem::absolute(kFixture).lexically_normal().string();
  // path -> what the reason says. A path that ends in "@<column>" is a row
  // that was imported without the link that column makes.
  const std::map<std::string, std::string> want{
      {"ProjectTbl.jsonl#3@principal_investigatorID",
       "principal_investigatorID 99 is not in PrincipalInvestigatorTbl; imported without it"},
      {"SampleTbl.jsonl#3", "projectID 42 is not in ProjectTbl"},
      {"SampleTbl.jsonl#5", "has the natural key of SampleTbl 1 and other values for note; those of that row are kept"},
      {"SampleTbl.jsonl#6@materialID", "materialID is missing; imported under material 'unknown'"},
      {"LevelTbl.jsonl#4", "irradiationID 7 is not in IrradiationTbl"},
      {"IrradiationPositionTbl.jsonl#5@sampleID",
       "sampleID 3 names a SampleTbl row that was not imported; imported without it"},
      {"IrradiationPositionTbl.jsonl#6", "identifier 66573 already sits at the position of IrradiationPositionTbl 1"},
      {"IrradiationPositionTbl.jsonl#7", "position is missing"},
      {"LoadTbl.jsonl#L-103@username", "username 'nobody' is not in UserTbl; imported without it"},
      {"LoadPositionTbl.jsonl#3", "identifier 99999 is not in IrradiationPositionTbl"},
      {"LoadPositionTbl.jsonl#4", "loadName 'L-999' is not in LoadTbl"},
  };
  const auto conflicts = world_->conflicts();
  ASSERT_EQ(conflicts.size(), want.size());
  for (const auto& conflict : conflicts) {
    const auto expected = want.find(conflict.path);
    ASSERT_NE(expected, want.end()) << conflict.path;
    EXPECT_EQ(conflict.kind, ConflictKind::IdentityClash) << conflict.path;
    EXPECT_EQ(conflict.resolution, "pending") << conflict.path;
    EXPECT_EQ(conflict.uuid, ingest::conflict_id(url, sha, conflict.path)) << conflict.path;
    EXPECT_TRUE(conflict.file_sha256.has_value()) << conflict.path;
    const json detail = json::parse(conflict.detail_json);
    const std::string table = conflict.path.substr(0, conflict.path.find(".jsonl"));
    const auto hash = conflict.path.find('#'), link = conflict.path.find('@');
    EXPECT_EQ(detail.at("table"), table) << conflict.path;
    EXPECT_EQ(detail.at("legacy_id"), conflict.path.substr(hash + 1, link == std::string::npos ? link : link - hash - 1))
        << conflict.path;
    EXPECT_EQ(detail.at("reason"), expected->second) << conflict.path;
    EXPECT_TRUE(detail.at("row").is_object()) << conflict.path;
    // A row that repeats another with other values is imported too: only those values are not.
    const bool repeat = conflict.path == "SampleTbl.jsonl#5";
    EXPECT_EQ(detail.at("imported"), link != std::string::npos || repeat) << conflict.path;
    if (repeat) {
      EXPECT_EQ(detail.at("columns"),
                json::parse(R"({"note":{"kept":"collected at the base, north side","given":"another note"}})"));
    }
    if (link != std::string::npos) {
      EXPECT_EQ(detail.at("column"), conflict.path.substr(link + 1)) << conflict.path;
      EXPECT_EQ(detail.at("value"), detail.at("row").at(conflict.path.substr(link + 1))) << conflict.path;
    }
  }
  // The refused row is kept whole in the conflict.
  const auto lost = std::find_if(conflicts.begin(), conflicts.end(),
                                 [](const auto& c) { return c.path == "SampleTbl.jsonl#3"; });
  ASSERT_NE(lost, conflicts.end());
  EXPECT_EQ(json::parse(lost->detail_json).at("row").at("name"), "Lost");
  // Nothing of a refused row is stored, nor made up for it.
  EXPECT_EQ(world_->count("sample"), 4);
  // A repeat that brings what the first row lacks fills it.
  EXPECT_EQ(world_->text("SELECT igsn AS v FROM sample WHERE name = 'FC-2'"), "IGSN002");
  EXPECT_EQ(world_->text("SELECT note AS v FROM sample WHERE name = 'HH-1'"), "collected at the base, north side");
  // A sample without a material is stored under the placeholder, and its position keeps it.
  EXPECT_EQ(world_->text("SELECT m.name AS v FROM sample s JOIN material m ON m.uuid = s.material_uuid "
                         "WHERE s.name = 'NoMat'"),
            "unknown");
  EXPECT_EQ(world_->text("SELECT s.name AS v FROM identifier d JOIN irradiation_position p ON p.uuid = "
                         "d.position_uuid JOIN sample s ON s.uuid = p.sample_uuid WHERE d.identifier = '66602'"),
            "NoMat");
  EXPECT_EQ(world_->one("SELECT count(*) AS n FROM sample WHERE name = 'Lost'").value("n").toInt(), 0);
  EXPECT_EQ(world_->one("SELECT count(*) AS n FROM identifier WHERE identifier IN ('66700', '99999')")
                .value("n")
                .toInt(),
            0);
  EXPECT_EQ(world_->one("SELECT count(*) AS n FROM load WHERE name = 'L-999'").value("n").toInt(), 0);
  EXPECT_EQ(world_->one("SELECT count(*) AS n FROM app_user WHERE name = 'nobody'").value("n").toInt(), 0);
}

// A link the store can do without does not cost the row, nor the rows under
// it (spec section 10.23).
TEST_P(CatalogDb, BrokenOptionalLinkIsImportedWithoutIt) {
  import_fixture();
  // A project whose investigator is not in the dump, and the sample in it.
  EXPECT_TRUE(world_->is_null("SELECT pi_uuid AS v FROM project WHERE name = 'Orphan'"));
  EXPECT_EQ(world_->text("SELECT p.name AS v FROM sample s JOIN project p ON p.uuid = s.project_uuid "
                         "WHERE s.name = 'Orphan-1'"),
            "Orphan");
  // A position whose sample was refused: the identifier still sits in it, and can be loaded.
  auto r = world_->one("SELECT p.position AS position, p.sample_uuid AS sample, l.name AS level FROM identifier d "
                       "JOIN irradiation_position p ON p.uuid = d.position_uuid JOIN level l ON l.uuid = p.level_uuid "
                       "WHERE d.identifier = '66601'");
  EXPECT_EQ(r.value("position").toInt(), 2);
  EXPECT_EQ(pd::to_std(r.value("level")), "B");
  EXPECT_TRUE(r.value("sample").isNull());
  EXPECT_EQ(world_->text("SELECT l.name AS v FROM load_position p JOIN load l ON l.uuid = p.load_uuid "
                         "JOIN identifier d ON d.uuid = p.identifier_uuid WHERE d.identifier = '66601'"),
            "L-102");
  // A load whose user is not in the dump.
  r = world_->one("SELECT created_by_user_uuid, created_utc FROM load WHERE name = 'L-103'");
  EXPECT_TRUE(r.value("created_by_user_uuid").isNull());
  EXPECT_EQ(pd::to_time(r.value("created_utc")).iso(), "2018-05-01T18:00:00.000000Z");
}

// One sample that cannot be stored (no project) does not take its positions,
// their identifiers and the loads they sit in with it.
TEST_P(CatalogDb, RefusedSampleKeepsItsPositionsAndLoads) {
  DumpDir dir;
  dir.table("ProjectTbl", {R"({"id":1,"name":"P","principal_investigatorID":null})"})
      .table("MaterialTbl", {R"({"id":1,"name":"M","grainsize":null})"})
      .table("SampleTbl", {R"({"id":1,"name":"S","materialID":1,"projectID":null})"})
      .table("IrradiationTbl", {R"({"id":1,"name":"NM-1","create_date":null})"})
      .table("LevelTbl", {R"({"id":1,"name":"A","irradiationID":1})"})
      .table("IrradiationPositionTbl",
             {
                 R"({"id":1,"identifier":"100","sampleID":1,"levelID":1,"position":1,"weight":2.5})",
                 R"({"id":2,"identifier":"101","sampleID":1,"levelID":1,"position":2})",
             })
      .table("LoadTbl", {R"({"name":"L","create_date":null,"archived":0,"username":null,"holderName":null})"})
      .table("LoadPositionTbl",
             {
                 R"({"id":1,"identifier":"100","position":1,"loadName":"L"})",
                 R"({"id":2,"identifier":"101","position":2,"loadName":"L"})",
             })
      .done();
  auto stats = run_import(*world_, adapter_config(dir.path()));
  ASSERT_TRUE(stats) << err(stats.error());
  EXPECT_TRUE(stats->finished);

  std::map<std::string, std::string> reasons;
  for (const auto& conflict : world_->conflicts())
    reasons[conflict.path] = json::parse(conflict.detail_json).at("reason").get<std::string>();
  EXPECT_EQ(reasons, (std::map<std::string, std::string>{
                         {"SampleTbl.jsonl#1", "projectID is missing"},
                         {"IrradiationPositionTbl.jsonl#1@sampleID",
                          "sampleID 1 names a SampleTbl row that was not imported; imported without it"},
                         {"IrradiationPositionTbl.jsonl#2@sampleID",
                          "sampleID 1 names a SampleTbl row that was not imported; imported without it"},
                     }));
  EXPECT_EQ(world_->count("sample"), 0);
  EXPECT_EQ(world_->count("irradiation_position"), 2);
  EXPECT_EQ(world_->count("identifier"), 2);
  EXPECT_EQ(world_->count("load_position"), 2);
  const auto r = world_->one("SELECT p.weight AS weight, p.sample_uuid AS sample, l.name AS level, i.name AS irradiation "
                             "FROM identifier d JOIN irradiation_position p ON p.uuid = d.position_uuid "
                             "JOIN level l ON l.uuid = p.level_uuid JOIN irradiation i ON i.uuid = l.irradiation_uuid "
                             "WHERE d.identifier = '100'");
  EXPECT_DOUBLE_EQ(r.value("weight").toDouble(), 2.5);
  EXPECT_TRUE(r.value("sample").isNull());
  EXPECT_EQ(pd::to_std(r.value("level")), "A");
  EXPECT_EQ(pd::to_std(r.value("irradiation")), "NM-1");
  EXPECT_EQ(world_->one("SELECT count(*) AS n FROM load_position p JOIN identifier d ON d.uuid = p.identifier_uuid "
                        "WHERE d.identifier IN ('100', '101')")
                .value("n")
                .toInt(),
            2);

  // Again: nothing new, the conflicts are not duplicated.
  auto replay = writer_config();
  replay.replay = true;
  ASSERT_TRUE(run_import(*world_, adapter_config(dir.path()), std::nullopt, replay));
  EXPECT_EQ(world_->count("import_conflict"), 3);
}

// Legacy allows a sample without a material; the store does not. Such a
// sample is imported under the placeholder material and reported, and so is
// one whose material is not in the dump (spec section 10.41).
TEST_P(CatalogDb, SampleWithoutMaterialIsImportedUnderThePlaceholder) {
  DumpDir dir;
  dir.table("ProjectTbl", {R"({"id":1,"name":"P","principal_investigatorID":null})"})
      .table("MaterialTbl", {R"({"id":1,"name":"M","grainsize":null})"})
      .table("SampleTbl",
             {
                 R"({"id":1,"name":"S","materialID":null,"projectID":1,"note":"a note"})",
                 R"({"id":2,"name":"T","materialID":77,"projectID":1})",
                 R"({"id":3,"name":"S","materialID":1,"projectID":1})",
                 R"({"id":4,"name":"U","materialID":null,"projectID":null})",
             })
      .table("IrradiationTbl", {R"({"id":1,"name":"NM-1","create_date":null})"})
      .table("LevelTbl", {R"({"id":1,"name":"A","irradiationID":1})"})
      .table("IrradiationPositionTbl",
             {
                 R"({"id":1,"identifier":"100","sampleID":1,"levelID":1,"position":1})",
                 R"({"id":2,"identifier":"101","sampleID":2,"levelID":1,"position":2})",
             })
      .done();
  auto stats = run_import(*world_, adapter_config(dir.path()));
  ASSERT_TRUE(stats) << err(stats.error());
  EXPECT_TRUE(stats->finished);
  EXPECT_EQ(stats->conflicts, 3);

  std::map<std::string, json> details;
  for (const auto& conflict : world_->conflicts()) {
    EXPECT_EQ(conflict.kind, ConflictKind::IdentityClash) << conflict.path;
    details[conflict.path] = json::parse(conflict.detail_json);
  }
  ASSERT_EQ(details.size(), 3u);
  // One warning for each sample; a sample without a project is still refused.
  const json& none = details.at("SampleTbl.jsonl#1@materialID");
  EXPECT_EQ(none.at("imported"), true);
  EXPECT_EQ(none.at("table"), "SampleTbl");
  EXPECT_EQ(none.at("legacy_id"), "1");
  EXPECT_EQ(none.at("column"), "materialID");
  EXPECT_EQ(none.at("reason"), "materialID is missing; imported under material 'unknown'");
  const json& dangling = details.at("SampleTbl.jsonl#2@materialID");
  EXPECT_EQ(dangling.at("imported"), true);
  EXPECT_EQ(dangling.at("legacy_id"), "2");
  EXPECT_EQ(dangling.at("value"), 77);
  EXPECT_EQ(dangling.at("reason"), "materialID 77 is not in MaterialTbl; imported under material 'unknown'");
  const json& refused = details.at("SampleTbl.jsonl#4");
  EXPECT_EQ(refused.at("imported"), false);
  EXPECT_EQ(refused.at("reason"), "projectID is missing; materialID is missing");

  // The placeholder is one material, made the way any material is.
  EXPECT_EQ(world_->count("material"), 2);
  EXPECT_EQ(world_->text("SELECT grainsize AS v FROM material WHERE name = 'unknown'"), "");
  EXPECT_EQ(world_->text("SELECT uuid AS v FROM material WHERE name = 'unknown'"),
            ingest::catalog_id("material", std::string("unknown") + "\n").str());
  // The sample of that name with a material is another sample.
  EXPECT_EQ(world_->count("sample"), 3);
  auto r = world_->one("SELECT s.note AS note, p.name AS project FROM sample s JOIN material m ON m.uuid = "
                       "s.material_uuid JOIN project p ON p.uuid = s.project_uuid "
                       "WHERE s.name = 'S' AND m.name = 'unknown'");
  EXPECT_EQ(pd::to_std(r.value("note")), "a note");
  EXPECT_EQ(pd::to_std(r.value("project")), "P");
  EXPECT_EQ(world_->text("SELECT m.name AS v FROM sample s JOIN material m ON m.uuid = s.material_uuid "
                         "WHERE s.name = 'T'"),
            "unknown");
  // The positions keep their samples.
  for (const auto& [identifier, sample] : std::map<std::string, std::string>{{"100", "S"}, {"101", "T"}}) {
    r = world_->one("SELECT s.name AS sample, m.name AS material FROM identifier d "
                    "JOIN irradiation_position p ON p.uuid = d.position_uuid JOIN sample s ON s.uuid = p.sample_uuid "
                    "JOIN material m ON m.uuid = s.material_uuid WHERE d.identifier = '" +
                    identifier + "'");
    EXPECT_EQ(pd::to_std(r.value("sample")), sample) << identifier;
    EXPECT_EQ(pd::to_std(r.value("material")), "unknown") << identifier;
  }

  // Verify counts the two under warnings and the refusal as blocking.
  {
    auto adapter = CatalogAdapter::open(adapter_config(dir.path()));
    ASSERT_TRUE(adapter) << err(adapter.error());
    const auto report = dvc::testing::verify_source(*world_, **adapter);
    EXPECT_EQ(dvc::testing::unaccounted(report), std::vector<std::string>{});
    EXPECT_EQ(report.pending_warnings, 2);
    EXPECT_EQ(report.pending_blocking, 1);
    EXPECT_EQ(report.replay_would_write, 0);
  }

  // A replay writes nothing.
  const auto before = snapshot_of(*world_);
  const auto seq = *world_->store->latest_change_seq();
  auto replay = writer_config();
  replay.replay = true;
  auto replayed = run_import(*world_, adapter_config(dir.path()), std::nullopt, replay);
  ASSERT_TRUE(replayed) << err(replayed.error());
  EXPECT_EQ(replayed->conflicts, 3);
  const auto after = snapshot_of(*world_);
  EXPECT_TRUE(after == before) << first_difference(after, before);
  EXPECT_EQ(*world_->store->latest_change_seq(), seq);
  EXPECT_EQ(world_->count("import_conflict"), 3);
}

// A store imported before a rule changed holds conflicts the dump, as it is
// read now, no longer has: the refusal of a row that is now imported, the
// link a row lost and now keeps. A replay supersedes them; it leaves alone
// what someone decided and what is not a catalog row's conflict.
TEST_P(CatalogDb, ReplaySupersedesConflictsTheDumpNoLongerHas) {
  DumpDir dir;
  dir.table("ProjectTbl", {R"({"id":1,"name":"P","principal_investigatorID":null})"})
      .table("SampleTbl", {R"({"id":1,"name":"S","materialID":null,"projectID":1})"})
      .table("IrradiationTbl", {R"({"id":1,"name":"NM-1","create_date":null})"})
      .table("LevelTbl", {R"({"id":1,"name":"A","irradiationID":1})"})
      .table("IrradiationPositionTbl",
             {
                 R"({"id":1,"identifier":"100","sampleID":1,"levelID":1,"position":1})",
                 R"({"id":2,"identifier":"100","sampleID":null,"levelID":1,"position":2})",
             })
      .done();
  ASSERT_TRUE(run_import(*world_, adapter_config(dir.path())));
  const auto source = world_->source();
  const std::string sha(64, 'a');
  const auto id = [&](const std::string& path) { return ingest::conflict_id(source.spec.url_or_path, sha, path); };
  const auto resolution = [&](const std::string& path) {
    auto row = world_->store->import_conflict(id(path));
    return row && *row ? (*row)->resolution : std::string("absent");
  };
  ASSERT_EQ(resolution("SampleTbl.jsonl#1@materialID"), "pending");
  ASSERT_EQ(resolution("IrradiationPositionTbl.jsonl#2"), "pending");

  // What the importer wrote before samples without a material were imported.
  const auto digest = sha256(std::string_view{"row"});
  const auto old_row = [&](const std::string& path, const std::string& detail) {
    return P::ImportConflictRow{id(path), path, std::nullopt, ConflictKind::IdentityClash, std::nullopt, digest, detail,
                                "pending"};
  };
  {
    auto uow = world_->store->begin_import_batch(source.spec.uuid, world_->client);
    ASSERT_TRUE(uow);
    ASSERT_TRUE((*uow)->add_conflict(
        old_row("SampleTbl.jsonl#1", R"({"table":"SampleTbl","imported":false,"reason":"materialID is missing"})")));
    ASSERT_TRUE((*uow)->add_conflict(old_row(
        "IrradiationPositionTbl.jsonl#1@sampleID",
        R"({"table":"IrradiationPositionTbl","imported":true,"column":"sampleID","reason":"imported without it"})")));
    // Decided by someone: left as it is.
    ASSERT_TRUE((*uow)->add_conflict(old_row("ProjectTbl.jsonl#1", R"({"table":"ProjectTbl","imported":false})")));
    ASSERT_TRUE((*uow)->resolve_conflict(id("ProjectTbl.jsonl#1"), "accepted"));
    ASSERT_TRUE((*uow)->commit());
  }
  {
    auto adapter = CatalogAdapter::open(adapter_config(dir.path()));
    ASSERT_TRUE(adapter) << err(adapter.error());
    const auto report = dvc::testing::verify_source(*world_, **adapter);
    EXPECT_EQ(report.pending_blocking, 2);
    EXPECT_EQ(report.pending_warnings, 2);
    EXPECT_EQ(report.replay_would_write, 2);  // the two a replay would supersede
  }

  auto replay = writer_config();
  replay.replay = true;
  auto replayed = run_import(*world_, adapter_config(dir.path()), std::nullopt, replay);
  ASSERT_TRUE(replayed) << err(replayed.error());
  EXPECT_EQ(replayed->conflicts, 2);  // what the run leaves pending
  EXPECT_EQ(resolution("SampleTbl.jsonl#1"), "superseded");
  EXPECT_EQ(resolution("IrradiationPositionTbl.jsonl#1@sampleID"), "superseded");
  EXPECT_EQ(resolution("ProjectTbl.jsonl#1"), "accepted");
  // What the dump still has stays pending.
  EXPECT_EQ(resolution("SampleTbl.jsonl#1@materialID"), "pending");
  EXPECT_EQ(resolution("IrradiationPositionTbl.jsonl#2"), "pending");

  auto adapter = CatalogAdapter::open(adapter_config(dir.path()));
  ASSERT_TRUE(adapter) << err(adapter.error());
  const auto report = dvc::testing::verify_source(*world_, **adapter);
  EXPECT_EQ(report.pending_blocking, 1);
  EXPECT_EQ(report.pending_warnings, 1);
  EXPECT_EQ(report.replay_would_write, 0);

  // Again: nothing changes.
  const auto seq = *world_->store->latest_change_seq();
  ASSERT_TRUE(run_import(*world_, adapter_config(dir.path()), std::nullopt, replay));
  EXPECT_EQ(resolution("SampleTbl.jsonl#1"), "superseded");
  EXPECT_EQ(*world_->store->latest_change_seq(), seq);
}

// The other half: a row refused under an old rule whose conflict says
// something else today keeps its conflict, which then says what is so today;
// and a conflict that was superseded and that the dump has again is pending
// again. What someone decided is not touched.
TEST_P(CatalogDb, ReplayRestatesAConflictThatSaysSomethingElseNow) {
  DumpDir dir;
  dir.table("ProjectTbl", {R"({"id":1,"name":"P","principal_investigatorID":null})"})
      .table("SampleTbl",
             {
                 R"({"id":1,"name":"S","materialID":null,"projectID":1,"note":"one"})",
                 R"({"id":2,"name":"S","materialID":null,"projectID":1,"note":"two"})",
                 R"({"id":3,"name":"S","materialID":null,"projectID":1,"note":"three"})",
             })
      .done();
  ASSERT_TRUE(run_import(*world_, adapter_config(dir.path())));
  const auto source = world_->source();
  const std::string sha(64, 'a');
  const auto id = [&](const std::string& path) { return ingest::conflict_id(source.spec.url_or_path, sha, path); };
  const auto stored = [&](const std::string& path) {
    auto row = world_->store->import_conflict(id(path));
    EXPECT_TRUE(row && *row) << path;
    return row && *row ? **row : P::ImportConflictRow{};
  };
  const std::string repeats = "has the natural key of SampleTbl 1 and other values for note; those of that row are kept";
  ASSERT_EQ(json::parse(stored("SampleTbl.jsonl#2").detail_json).at("reason"), repeats);

  // As a store imported under the old rule has them, one of them set aside.
  {
    auto uow = world_->store->begin_import_batch(source.spec.uuid, world_->client);
    ASSERT_TRUE(uow);
    for (const char* path : {"SampleTbl.jsonl#2", "SampleTbl.jsonl#3"}) {
      auto row = stored(path);
      row.detail_json = R"({"table":"SampleTbl","imported":false,"reason":"materialID is missing"})";
      ASSERT_TRUE((*uow)->restate_conflict(row));
    }
    ASSERT_TRUE((*uow)->resolve_conflict(id("SampleTbl.jsonl#3"), "ignored"));
    ASSERT_TRUE((*uow)->resolve_conflict(id("SampleTbl.jsonl#1@materialID"), "superseded"));
    ASSERT_TRUE((*uow)->commit());
  }
  {
    auto adapter = CatalogAdapter::open(adapter_config(dir.path()));
    ASSERT_TRUE(adapter) << err(adapter.error());
    EXPECT_EQ(dvc::testing::verify_source(*world_, **adapter).replay_would_write, 2);
  }

  auto replay = writer_config();
  replay.replay = true;
  auto replayed = run_import(*world_, adapter_config(dir.path()), std::nullopt, replay);
  ASSERT_TRUE(replayed) << err(replayed.error());
  // Pending after it: the repeat that was restated, and the placeholder material of all three.
  EXPECT_EQ(replayed->conflicts, 4);
  auto row = stored("SampleTbl.jsonl#2");
  EXPECT_EQ(row.resolution, "pending");
  EXPECT_EQ(json::parse(row.detail_json).at("reason"), repeats);
  EXPECT_EQ(json::parse(row.detail_json).at("imported"), true);
  EXPECT_EQ(json::parse(row.detail_json).at("legacy_id"), "2");
  row = stored("SampleTbl.jsonl#3");
  EXPECT_EQ(row.resolution, "ignored");
  EXPECT_EQ(json::parse(row.detail_json).at("reason"), "materialID is missing");
  EXPECT_EQ(stored("SampleTbl.jsonl#1@materialID").resolution, "pending");

  // Again: nothing changes.
  const auto seq = *world_->store->latest_change_seq();
  const auto before = snapshot_of(*world_);
  ASSERT_TRUE(run_import(*world_, adapter_config(dir.path()), std::nullopt, replay));
  EXPECT_EQ(snapshot_of(*world_), before);
  EXPECT_EQ(*world_->store->latest_change_seq(), seq);
  auto adapter = CatalogAdapter::open(adapter_config(dir.path()));
  ASSERT_TRUE(adapter) << err(adapter.error());
  EXPECT_EQ(dvc::testing::verify_source(*world_, **adapter).replay_would_write, 0);
}

// The store keeps a sample's location as one point (migration 0004): a
// legacy row with a latitude but no longitude, or the reverse, is imported
// without a location, and the half it had is reported as a warning.
TEST_P(CatalogDb, HalfALocationIsImportedWithoutItAndReported) {
  DumpDir dir;
  dir.table("ProjectTbl", {R"({"id":1,"name":"P","principal_investigatorID":null})"})
      .table("MaterialTbl", {R"({"id":1,"name":"M","grainsize":null})"})
      .table("SampleTbl",
             {
                 R"({"id":1,"name":"S","materialID":1,"projectID":1,"lat":34.5})",
                 R"({"id":2,"name":"T","materialID":1,"projectID":1,"lon":-106.5})",
                 R"({"id":3,"name":"U","materialID":1,"projectID":1,"lat":34.5,"lon":-106.5})",
             })
      .table("IrradiationTbl", {R"({"id":1,"name":"NM-1","create_date":null})"})
      .table("LevelTbl", {R"({"id":1,"name":"A","irradiationID":1})"})
      .table("IrradiationPositionTbl", {R"({"id":1,"identifier":"100","sampleID":3,"levelID":1,"position":1})"})
      .done();
  auto stats = run_import(*world_, adapter_config(dir.path()));
  ASSERT_TRUE(stats) << err(stats.error());
  EXPECT_TRUE(stats->finished);
  EXPECT_EQ(stats->conflicts, 2);
  EXPECT_EQ(world_->count("sample"), 3);
  EXPECT_TRUE(world_->is_null("SELECT geom AS v FROM sample WHERE name = 'S'"));
  EXPECT_TRUE(world_->is_null("SELECT geom AS v FROM sample WHERE name = 'T'"));
  const auto point = pd::parse_point(world_->text("SELECT geom AS v FROM sample WHERE name = 'U'"));
  ASSERT_TRUE(point);
  EXPECT_DOUBLE_EQ(point->lat, 34.5);
  EXPECT_DOUBLE_EQ(point->lon, -106.5);

  std::map<std::string, std::string> reasons;
  for (const auto& conflict : world_->conflicts()) {
    EXPECT_TRUE(ingest::is_warning_conflict(conflict)) << conflict.path;
    reasons[conflict.path] = json::parse(conflict.detail_json).at("reason").get<std::string>();
  }
  EXPECT_EQ(reasons, (std::map<std::string, std::string>{
                         {"SampleTbl.jsonl#1@lat", "lat without lon is not a location; imported without a location"},
                         {"SampleTbl.jsonl#2@lon", "lon without lat is not a location; imported without a location"}}));
}

// Rows with one natural key are one row (spec section 10.43). What decides
// is what would be imported, not the text of the dump: a repeat that says the
// same, or brings what the first lacks, is no conflict; one that says
// something else for a column is imported without that value, with a warning.
TEST_P(CatalogDb, RepeatedRowsAreOneRowWithTheUnionOfTheirValues) {
  DumpDir dir;
  dir.table("ProjectTbl", {R"({"id":1,"name":"P","principal_investigatorID":null})"})
      .table("MaterialTbl", {R"({"id":1,"name":"M","grainsize":null})"})
      .table("SampleTbl",
             {
                 R"({"id":1,"name":"S","materialID":1,"projectID":1,"note":"one","igsn":null,"lat":34.5,"lon":-106.5})",
                 // The same after the none rule: "---------" and NULL are both no value.
                 R"({"id":2,"name":"S","materialID":1,"projectID":1,"note":"---------","igsn":"","lat":"34.5","lon":"-106.5"})",
                 R"({"id":3,"name":"S","materialID":1,"projectID":1,"note":null,"igsn":null,"lat":null,"lon":null})",
                 // Brings what the first lacks.
                 R"({"id":4,"name":"S","materialID":1,"projectID":1,"note":null,"igsn":"IG-1"})",
                 // Says something else, and brings a location.
                 R"({"id":5,"name":"S","materialID":1,"projectID":1,"note":"five","igsn":"IG-1","location":"here"})",
             })
      .table("IrradiationTbl", {R"({"id":1,"name":"NM-1","create_date":null})"})
      .table("LevelTbl", {R"({"id":1,"name":"A","irradiationID":1})"})
      .table("IrradiationPositionTbl",
             {
                 R"({"id":1,"identifier":"100","sampleID":null,"levelID":1,"position":1,"weight":null})",
                 R"({"id":2,"identifier":"100","sampleID":5,"levelID":1,"position":1,"weight":2.5})",
                 // Another identifier for the hole: the hole keeps the first.
                 R"({"id":3,"identifier":"200","sampleID":5,"levelID":1,"position":1,"weight":2.5})",
             })
      .done();
  auto stats = run_import(*world_, adapter_config(dir.path()));
  ASSERT_TRUE(stats) << err(stats.error());
  EXPECT_TRUE(stats->finished);
  EXPECT_EQ(stats->conflicts, 2);

  EXPECT_EQ(world_->count("sample"), 1);
  auto r = world_->one("SELECT note, igsn, geom, location FROM sample");
  EXPECT_EQ(pd::to_std(r.value("note")), "one");
  EXPECT_EQ(pd::to_std(r.value("igsn")), "IG-1");
  const auto point = pd::parse_point(pd::to_std(r.value("geom")));
  ASSERT_TRUE(point);
  EXPECT_DOUBLE_EQ(point->lat, 34.5);
  EXPECT_DOUBLE_EQ(point->lon, -106.5);
  EXPECT_EQ(pd::to_std(r.value("location")), "here");
  EXPECT_EQ(world_->count("irradiation_position"), 1);
  EXPECT_EQ(world_->count("identifier"), 1);
  r = world_->one("SELECT d.identifier AS identifier, p.weight AS weight, s.name AS sample FROM irradiation_position p "
                  "JOIN identifier d ON d.position_uuid = p.uuid JOIN sample s ON s.uuid = p.sample_uuid");
  EXPECT_EQ(pd::to_std(r.value("identifier")), "100");
  EXPECT_DOUBLE_EQ(r.value("weight").toDouble(), 2.5);
  EXPECT_EQ(pd::to_std(r.value("sample")), "S");

  std::map<std::string, json> details;
  for (const auto& conflict : world_->conflicts()) {
    EXPECT_EQ(conflict.kind, ConflictKind::IdentityClash) << conflict.path;
    EXPECT_TRUE(ingest::is_warning_conflict(conflict)) << conflict.path;
    details[conflict.path] = json::parse(conflict.detail_json);
  }
  ASSERT_EQ(details.size(), 2u);
  const json& sample = details.at("SampleTbl.jsonl#5");
  EXPECT_EQ(sample.at("imported"), true);
  EXPECT_EQ(sample.at("table"), "SampleTbl");
  EXPECT_EQ(sample.at("legacy_id"), "5");
  EXPECT_EQ(sample.at("repeats"), "1");
  EXPECT_EQ(sample.at("columns"), json::parse(R"({"note":{"kept":"one","given":"five"}})"));
  EXPECT_EQ(sample.at("reason"), "has the natural key of SampleTbl 1 and other values for note; those of that row are kept");
  const json& hole = details.at("IrradiationPositionTbl.jsonl#3");
  EXPECT_EQ(hole.at("imported"), true);
  EXPECT_EQ(hole.at("columns"), json::parse(R"({"identifier":{"kept":"100","given":"200"}})"));

  {
    auto adapter = CatalogAdapter::open(adapter_config(dir.path()));
    ASSERT_TRUE(adapter) << err(adapter.error());
    const auto report = dvc::testing::verify_source(*world_, **adapter);
    EXPECT_EQ(dvc::testing::unaccounted(report), std::vector<std::string>{});
    EXPECT_EQ(report.pending_blocking, 0);
    EXPECT_EQ(report.pending_warnings, 2);
    EXPECT_EQ(report.replay_would_write, 0);
    EXPECT_TRUE(report.ok());
  }

  // A replay writes nothing, and one row at a time ends the same.
  const auto before = snapshot_of(*world_);
  const auto seq = *world_->store->latest_change_seq();
  auto replay = writer_config();
  replay.replay = true;
  ASSERT_TRUE(run_import(*world_, adapter_config(dir.path()), std::nullopt, replay));
  const auto after = snapshot_of(*world_);
  EXPECT_TRUE(after == before) << first_difference(after, before);
  EXPECT_EQ(*world_->store->latest_change_seq(), seq);
  auto other = fresh_world();
  for (bool finished = false; !finished;) {
    auto step = run_import(*other, adapter_config(dir.path(), 1), 1);
    ASSERT_TRUE(step) << err(step.error());
    finished = step->finished;
  }
  // The two worlds read the dump from one directory: the same source, the same ids.
  const auto cut = snapshot_of(*other);
  EXPECT_TRUE(cut == before) << first_difference(cut, before);
}

TEST_P(CatalogDb, ResumeFromToken) {
  auto first = run_import(*world_, adapter_config(kFixture, 2), 1);
  ASSERT_TRUE(first) << err(first.error());
  EXPECT_EQ(first->batches, 1);
  EXPECT_FALSE(first->finished);
  auto paused = world_->source();
  EXPECT_EQ(paused.status, "paused");
  EXPECT_EQ(paused.progress_token, std::optional<std::string>{"0:2@" + fixture_sha()});
  EXPECT_EQ(paused.done, 2);
  EXPECT_EQ(world_->count("principal_investigator"), 2);
  EXPECT_EQ(world_->count("project"), 0);

  auto rest = run_import(*world_, adapter_config(kFixture, 2));
  ASSERT_TRUE(rest) << err(rest.error());
  EXPECT_TRUE(rest->finished);
  EXPECT_EQ(rest->batches, (kRows - 2 + 1) / 2);
  EXPECT_EQ(world_->source().progress_token, std::optional<std::string>{"12:0@" + fixture_sha()});

  auto whole = fresh_world();
  auto one_run = run_import(*whole, adapter_config());
  ASSERT_TRUE(one_run) << err(one_run.error());
  const auto got = snapshot_of(*world_), want = snapshot_of(*whole);
  ASSERT_FALSE(want.empty());
  EXPECT_TRUE(got == want) << first_difference(got, want);
}

TEST_P(CatalogDb, SecondRunIsNoOp) {
  import_fixture();
  const auto before = snapshot_of(*world_);
  const auto seq = *world_->store->latest_change_seq();

  auto again = run_import(*world_, adapter_config());
  ASSERT_TRUE(again) << err(again.error());
  EXPECT_TRUE(again->finished);
  EXPECT_EQ(again->batches, 0);
  EXPECT_EQ(again->conflicts, 0);
  EXPECT_EQ(snapshot_of(*world_), before);

  // A replay walks every row again and writes nothing.
  auto replay = writer_config();
  replay.replay = true;
  auto replayed = run_import(*world_, adapter_config(kFixture, 5), std::nullopt, replay);
  ASSERT_TRUE(replayed) << err(replayed.error());
  EXPECT_TRUE(replayed->finished);
  EXPECT_EQ(replayed->batches, (kRows + 4) / 5);
  const auto after = snapshot_of(*world_);
  EXPECT_TRUE(after == before) << first_difference(after, before);
  EXPECT_EQ(*world_->store->latest_change_seq(), seq);
}

// However the rows are cut into batches, and wherever the import stops and
// starts again, the store ends up the same (spec section 10.16).
TEST_P(CatalogDb, OneHistoryOneResult) {
  import_fixture();
  const auto want = snapshot_of(*world_);
  ASSERT_EQ(want.size(), 58u);  // every stored row and the source

  for (const int batch_rows : {1, 2, 2000}) {
    {
      auto w = fresh_world();
      auto stats = run_import(*w, adapter_config(kFixture, batch_rows));
      ASSERT_TRUE(stats) << batch_rows << ": " << err(stats.error());
      EXPECT_TRUE(stats->finished);
      EXPECT_EQ(stats->conflicts, 11) << batch_rows;
      const auto got = snapshot_of(*w);
      EXPECT_TRUE(got == want) << "batch_rows " << batch_rows << ": " << first_difference(got, want);
    }
    {
      // Stopped after every batch; each run opens the dump and the source anew.
      auto w = fresh_world();
      int runs = 0;
      for (bool finished = false; !finished; ++runs) {
        ASSERT_LT(runs, 100);
        auto stats = run_import(*w, adapter_config(kFixture, batch_rows), 1);
        ASSERT_TRUE(stats) << batch_rows << ": " << err(stats.error());
        finished = stats->finished;
      }
      EXPECT_EQ(runs, (kRows + batch_rows - 1) / batch_rows + 1) << batch_rows;
      const auto got = snapshot_of(*w);
      EXPECT_TRUE(got == want) << "batch_rows " << batch_rows << ", resumed: " << first_difference(got, want);
    }
  }
}

// ---------------------------------------------------------------- verify

// Every row of the dump is a catalog row in the store or a conflict that says
// why not, however the rows were cut into batches.
TEST_P(CatalogDb, VerifyAfterImportIsOk) {
  auto imported = run_import(*world_, adapter_config(kFixture, 7));
  ASSERT_TRUE(imported) << err(imported.error());
  const auto rows = snapshot_of(*world_);

  for (const int batch_rows : {1, 5, 2000}) {
    auto adapter = CatalogAdapter::open(adapter_config(kFixture, batch_rows));
    ASSERT_TRUE(adapter) << err(adapter.error());
    const auto report = dvc::testing::verify_source(*world_, **adapter);
    EXPECT_EQ(report.units, kRows) << batch_rows;
    EXPECT_EQ(report.ignored, 0);
    EXPECT_EQ(dvc::testing::unaccounted(report), std::vector<std::string>{}) << batch_rows;
    EXPECT_EQ(report.would_write, 0);
    EXPECT_EQ(report.replay_would_write, 0);
    // Six rows were refused; three were imported without a link, one under
    // the placeholder material and one without a value an earlier row has.
    EXPECT_EQ(report.pending_blocking, 6) << batch_rows;
    EXPECT_EQ(report.pending_warnings, 5) << batch_rows;
    EXPECT_FALSE(report.ok());
    EXPECT_EQ(report.parity_pass + report.parity_fail + report.parity_not_comparable, 0);
  }
  EXPECT_EQ(snapshot_of(*world_), rows) << "verify wrote something";

  // With the refusals dealt with, the warnings alone do not fail it.
  auto refused = world_->store->import_conflicts({world_->source().spec.uuid, std::nullopt, std::string("pending")});
  ASSERT_TRUE(refused);
  auto uow = world_->store->begin_import_batch(world_->source().spec.uuid, world_->client);
  ASSERT_TRUE(uow);
  for (const auto& row : *refused) {
    if (!ingest::is_warning_conflict(row)) {
      ASSERT_TRUE((*uow)->resolve_conflict(row.uuid, "ignored"));
    }
  }
  ASSERT_TRUE((*uow)->commit());
  auto adapter = CatalogAdapter::open(adapter_config());
  ASSERT_TRUE(adapter) << err(adapter.error());
  const auto report = dvc::testing::verify_source(*world_, **adapter);
  EXPECT_TRUE(report.ok());
  EXPECT_EQ(report.pending_blocking, 0);
  EXPECT_EQ(report.pending_warnings, 5);
}

// Catalog rows are found by natural key, whoever made them, and a dry run does
// not count catalog rows: a dump that was never imported can look complete.
// What says it was imported is the source: registered, finished, same dump.
TEST_P(CatalogDb, VerifyOfADumpThatWasNotImportedIsNotOk) {
  import_fixture();
  DumpDir other;
  other.table("ExtractDeviceTbl", {R"({"name":"Fusions CO2"})", R"({"name":"Fusions Diode"})"}).done();
  auto adapter = CatalogAdapter::open(adapter_config(other.path()));
  ASSERT_TRUE(adapter) << err(adapter.error());
  auto report = dvc::testing::verify_source(*world_, **adapter);
  EXPECT_EQ(report.units, 2);
  EXPECT_EQ(dvc::testing::unaccounted(report), std::vector<std::string>{});
  EXPECT_EQ(report.would_write, 0);
  EXPECT_EQ(report.replay_would_write, 0);
  EXPECT_EQ(report.pending_blocking, 0);
  EXPECT_FALSE(report.source.registered);
  EXPECT_FALSE(report.ok());

  ASSERT_TRUE(run_import(*world_, adapter_config(other.path())));
  auto again = CatalogAdapter::open(adapter_config(other.path()));
  ASSERT_TRUE(again) << err(again.error());
  report = dvc::testing::verify_source(*world_, **again);
  EXPECT_TRUE(report.source.registered);
  EXPECT_EQ(report.source.status, "finished");
  EXPECT_TRUE(report.ok());

  // The directory converted again from a newer dump: not the dump that was imported.
  other.set("sha256", std::string(64, 'b')).done();
  auto newer = CatalogAdapter::open(adapter_config(other.path()));
  ASSERT_TRUE(newer) << err(newer.error());
  report = dvc::testing::verify_source(*world_, **newer);
  EXPECT_EQ(report.source.status, "finished");
  EXPECT_EQ(report.source.current_head, std::string(64, 'b'));
  EXPECT_FALSE(report.ok());
}

// Take one row out of the store: verify names the dump row it belongs to.
TEST_P(CatalogDb, VerifyReportsAMissingCatalogRowOrConflict) {
  import_fixture();
  const std::string sha = fixture_sha();
  const auto name = [&](const char* path) { return dvc::testing::unit_name(sha, path); };
  const auto listed = [&] {
    auto adapter = CatalogAdapter::open(adapter_config());
    EXPECT_TRUE(adapter);
    return adapter ? dvc::testing::unaccounted(dvc::testing::verify_source(*world_, **adapter))
                   : std::vector<std::string>{};
  };
  ASSERT_EQ(listed(), std::vector<std::string>{});

  // A catalog row.
  ASSERT_EQ(dvc::testing::forget(*world_, "DELETE FROM extract_device WHERE name = 'Fusions CO2'"), 1);
  EXPECT_EQ(listed(), std::vector<std::string>{name("ExtractDeviceTbl.jsonl#Fusions CO2")});

  // A row with parents: the load position of 66600 in L-101.
  ASSERT_EQ(dvc::testing::forget(*world_, "DELETE FROM load_position WHERE position = 4"), 1);
  EXPECT_EQ(listed(),
            (std::vector<std::string>{name("ExtractDeviceTbl.jsonl#Fusions CO2"), name("LoadPositionTbl.jsonl#6")}));

  // The conflict of a refused row, and the conflict of a link an imported row lost.
  ASSERT_EQ(dvc::testing::forget(*world_, "DELETE FROM import_conflict WHERE path = 'SampleTbl.jsonl#3'"), 1);
  ASSERT_EQ(
      dvc::testing::forget(*world_, "DELETE FROM import_conflict WHERE path = 'LoadTbl.jsonl#L-103@username'"), 1);
  auto adapter = CatalogAdapter::open(adapter_config());
  ASSERT_TRUE(adapter) << err(adapter.error());
  const auto report = dvc::testing::verify_source(*world_, **adapter);
  EXPECT_EQ(dvc::testing::unaccounted(report),
            (std::vector<std::string>{name("ExtractDeviceTbl.jsonl#Fusions CO2"), name("LoadPositionTbl.jsonl#6"),
                                      name("LoadTbl.jsonl#L-103"), name("SampleTbl.jsonl#3")}));
  // What is missing is named: the load is there, its link's conflict is not.
  const auto& load = dvc::testing::unaccounted_unit(report, sha, "LoadTbl.jsonl#L-103");
  ASSERT_EQ(load.missing.size(), 1u);
  EXPECT_EQ(load.missing[0].kind, ingest::Evidence::Kind::Conflict);
  EXPECT_EQ(load.missing[0].path, "LoadTbl.jsonl#L-103@username");
  const auto& device = dvc::testing::unaccounted_unit(report, sha, "ExtractDeviceTbl.jsonl#Fusions CO2");
  ASSERT_EQ(device.missing.size(), 1u);
  EXPECT_EQ(device.missing[0].kind, ingest::Evidence::Kind::CatalogRow);
  // The dry run sees the two conflicts it would write again; catalog rows it does not count.
  EXPECT_EQ(report.replay_would_write, 2);
  EXPECT_FALSE(report.ok());
}

INSTANTIATE_TEST_SUITE_P(Engines, CatalogDb, ::testing::ValuesIn(P::testing::engines()),
                         [](const auto& p) { return p.param; });

// ---------------------------------------------------------------- the adapter alone

TEST(CatalogDbAdapter, DescribesTheDump) {
  auto adapter = CatalogAdapter::open(adapter_config());
  ASSERT_TRUE(adapter) << err(adapter.error());
  const auto described = (*adapter)->describe();
  ASSERT_TRUE(described);
  EXPECT_EQ(described->kind, P::ImportSourceKind::LegacyDb);
  EXPECT_EQ(described->url, std::filesystem::absolute(kFixture).lexically_normal().string());
  EXPECT_EQ(described->branch, "");
  EXPECT_EQ(described->head, fixture_sha());
}

TEST(CatalogDbAdapter, SendsParentsBeforeChildren) {
  auto adapter = CatalogAdapter::open(adapter_config());
  ASSERT_TRUE(adapter) << err(adapter.error());
  const auto batches = all_batches(**adapter);
  ASSERT_EQ(batches.size(), 1u);
  std::vector<std::string> order;
  for (const auto& item : batches[0].catalog)
    if (order.empty() || order.back() != name_of(item)) order.emplace_back(name_of(item));
  EXPECT_EQ(order, (std::vector<std::string>{"pi", "project", "material", "sample", "irradiation", "level", "position",
                                              "user", "mass_spectrometer", "extract_device", "load", "load_position"}));
  EXPECT_EQ(rows_of(batches[0]), static_cast<std::size_t>(kRows));
  EXPECT_EQ(batches[0].conflicts.size(), 11u);
  EXPECT_EQ(batches[0].done, kRows);
  EXPECT_EQ(batches[0].total, kRows);
  EXPECT_EQ(batches[0].head, fixture_sha());
  EXPECT_EQ(batches[0].resume_token, "12:0@" + fixture_sha());
}

TEST(CatalogDbAdapter, BatchesAreTheSameRowsHoweverTheyAreCut) {
  auto whole = CatalogAdapter::open(adapter_config());
  ASSERT_TRUE(whole);
  const auto reference = all_batches(**whole);
  for (const int batch_rows : {1, 3, 7, kRows - 1, kRows, kRows + 1}) {
    auto adapter = CatalogAdapter::open(adapter_config(kFixture, batch_rows));
    ASSERT_TRUE(adapter) << err(adapter.error());
    const auto batches = all_batches(**adapter);
    EXPECT_EQ(batches.size(), static_cast<std::size_t>((kRows + batch_rows - 1) / batch_rows)) << batch_rows;
    std::vector<std::string> items, conflicts;
    for (const auto& batch : batches) {
      EXPECT_LE(rows_of(batch), static_cast<std::size_t>(batch_rows));
      for (const auto& item : batch.catalog) items.emplace_back(name_of(item));
      for (const auto& conflict : batch.conflicts) conflicts.push_back(conflict.key.path);
    }
    std::vector<std::string> want_items, want_conflicts;
    for (const auto& item : reference[0].catalog) want_items.emplace_back(name_of(item));
    for (const auto& conflict : reference[0].conflicts) want_conflicts.push_back(conflict.key.path);
    EXPECT_EQ(items, want_items) << batch_rows;
    EXPECT_EQ(conflicts, want_conflicts) << batch_rows;
    EXPECT_EQ(batches.back().resume_token, "12:0@" + fixture_sha());
  }
}

TEST(CatalogDbAdapter, PlanResumesAtTheTokenOfThisDump) {
  auto adapter = CatalogAdapter::open(adapter_config(kFixture, 4));
  ASSERT_TRUE(adapter) << err(adapter.error());
  NoState state;
  const std::string sha = fixture_sha();
  EXPECT_EQ(*(*adapter)->plan(std::nullopt, state), kRows);
  auto first = (*adapter)->next_batch();
  ASSERT_TRUE(first && *first);
  EXPECT_EQ((*first)->resume_token, "1:1@" + sha);  // three investigators, then one project
  EXPECT_EQ((*first)->done, 4);

  EXPECT_EQ(*(*adapter)->plan("1:1@" + sha, state), kRows - 4);
  auto second = (*adapter)->next_batch();
  ASSERT_TRUE(second && *second);
  ASSERT_FALSE((*second)->catalog.empty());
  EXPECT_EQ(std::string(name_of((*second)->catalog.front())), "project");
  EXPECT_EQ(std::get<ingest::ProjectItem>((*second)->catalog.front()).name, "REFERENCES");

  EXPECT_EQ(*(*adapter)->plan("12:0@" + sha, state), 0);
  auto end = (*adapter)->next_batch();
  ASSERT_TRUE(end);
  EXPECT_FALSE(end->has_value());
  // The end of one table is the start of the next.
  EXPECT_EQ(*(*adapter)->plan("0:3@" + sha, state), kRows - 3);

  // A token of another dump, or one that names no row, starts from the first row.
  for (const std::string& token :
       {"1:1@" + std::string(64, '0'), std::string("1:1"), std::string("garbage"), "99:0@" + sha, "0:4@" + sha,
        "12:1@" + sha, "-1:0@" + sha, ":@" + sha, std::string()})
    EXPECT_EQ(*(*adapter)->plan(token, state), kRows) << token;
}

TEST(CatalogDbAdapter, MissingManifestIsError) {
  DumpDir dir;
  dir.table("MaterialTbl", {R"({"id":1,"name":"Sanidine","grainsize":null})"});
  auto adapter = CatalogAdapter::open(adapter_config(dir.path()));
  ASSERT_FALSE(adapter);
  EXPECT_EQ(adapter.error().kind, ErrorKind::Io);
  EXPECT_NE(adapter.error().what.find("MANIFEST.json"), std::string::npos) << adapter.error().what;

  dir.done();
  EXPECT_TRUE(CatalogAdapter::open(adapter_config(dir.path())));

  auto nowhere = CatalogAdapter::open(adapter_config(dir.path() / "nope"));
  ASSERT_FALSE(nowhere);
  EXPECT_NE(nowhere.error().what.find("not a directory"), std::string::npos) << nowhere.error().what;
}

TEST(CatalogDbAdapter, DirectoryThatIsNotOneWholeConversionIsError) {
  {
    // The manifest counts a row the file does not have.
    DumpDir dir;
    dir.table("MaterialTbl", {R"({"id":1,"name":"Sanidine"})"}).set("tables", {{"MaterialTbl", 2}}).done();
    auto adapter = CatalogAdapter::open(adapter_config(dir.path()));
    ASSERT_FALSE(adapter);
    EXPECT_NE(adapter.error().what.find("has 1 rows"), std::string::npos) << adapter.error().what;
  }
  {
    // The manifest lists a file that is not there.
    DumpDir dir;
    dir.set("tables", {{"MaterialTbl", 1}}).done();
    auto adapter = CatalogAdapter::open(adapter_config(dir.path()));
    ASSERT_FALSE(adapter);
    EXPECT_NE(adapter.error().what.find("MaterialTbl.jsonl"), std::string::npos) << adapter.error().what;
  }
  {
    DumpDir dir;
    dir.set("sha256", nullptr).done();
    EXPECT_FALSE(CatalogAdapter::open(adapter_config(dir.path())));
  }
  {
    DumpDir dir;
    std::ofstream(dir.path() / "MANIFEST.json", std::ios::binary) << "{\"tables\": ";
    auto adapter = CatalogAdapter::open(adapter_config(dir.path()));
    ASSERT_FALSE(adapter);
    EXPECT_EQ(adapter.error().kind, ErrorKind::Protocol);
  }
}

TEST(CatalogDbAdapter, BadConfigIsError) {
  auto zone = CatalogAdapter::open({kFixture, "Mars/Olympus_Mons", 2000});
  ASSERT_FALSE(zone);
  EXPECT_EQ(zone.error().kind, ErrorKind::Config);
  auto rows = CatalogAdapter::open({kFixture, kZone, 0});
  ASSERT_FALSE(rows);
  EXPECT_EQ(rows.error().kind, ErrorKind::Config);
}

TEST(CatalogDbAdapter, DumpWithoutCatalogTablesIsEmpty) {
  DumpDir dir;
  dir.table("VersionTbl", {R"({"version":"1"})"}).done();
  auto adapter = CatalogAdapter::open(adapter_config(dir.path()));
  ASSERT_TRUE(adapter) << err(adapter.error());
  NoState state;
  EXPECT_EQ(*(*adapter)->plan(std::nullopt, state), 0);
  auto batch = (*adapter)->next_batch();
  ASSERT_TRUE(batch);
  EXPECT_FALSE(batch->has_value());
}

TEST(CatalogDbAdapter, TableNamesAreMatchedWithoutCase) {
  // MySQL on Windows lower-cases table names; column names stay as written.
  DumpDir dir;
  dir.table("irradiationtbl", {R"({"id":1,"name":"NM-300","create_date":null})"})
      .table("leveltbl", {R"({"id":5,"name":"A","irradiationID":1,"holder":null,"z":null,"note":null})"})
      .done();
  const auto batch = only_batch(dir);
  ASSERT_EQ(batch.catalog.size(), 2u);
  EXPECT_TRUE(batch.conflicts.empty());
  const auto levels = items_of<ingest::LevelItem>(batch);
  ASSERT_EQ(levels.size(), 1u);
  EXPECT_EQ(levels[0].irradiation, "NM-300");
  EXPECT_EQ(levels[0].name, "A");
}

TEST(CatalogDbAdapter, RowThatCannotBeReadIsConflictAndTheRestGoesOn) {
  DumpDir dir;
  dir.table("SampleTbl",
            {
                R"({"id":1,"name":"good","materialID":1,"projectID":1})",
                R"(this is not JSON)",
                R"([1, 2])",
                R"({"id":4,"name":null,"materialID":1,"projectID":1})",
                R"({"id":5,"name":"","materialID":1,"projectID":1})",
                R"({"id":6,"name":"no material","materialID":null,"projectID":1})",
                R"({"id":7,"name":"lat","materialID":1,"projectID":1,"lat":"north","lon":{"a":1}})",
                R"({"id":8,"name":"when","materialID":1,"projectID":1,"create_date":"last tuesday"})",
                R"({"id":9,"name":"nul","materialID":1,"projectID":1,"note":"a\u0000b"})",
                R"({"id":1,"name":"again","materialID":1,"projectID":1})",
                R"({"name":"no id","materialID":1,"projectID":1})",
                R"({"id":"x12","name":"odd id","materialID":1,"projectID":1})",
                R"({"id":13,"name":"text key","materialID":"one","projectID":1})",
                R"({"id":14,"name":"numbers as text","materialID":"1","projectID":"1","lat":"34.5","lon":""})",
                R"({"id":15,"name":"blob","materialID":1,"projectID":1,"lat__base64":"AAEC"})",
            })
      .table("ProjectTbl", {R"({"id":1,"name":"P","principal_investigatorID":null})"})
      .table("MaterialTbl", {R"({"id":1,"name":"M","grainsize":null})"})
      .done();
  const auto batch = only_batch(dir);
  const auto samples = items_of<ingest::SampleItem>(batch);
  ASSERT_EQ(samples.size(), 3u);
  EXPECT_EQ(samples[0].fields.name, "good");
  EXPECT_EQ(samples[0].material, "M");
  // No material: imported under the placeholder, and reported (below).
  EXPECT_EQ(samples[1].fields.name, "no material");
  EXPECT_EQ(samples[1].material, ingest::kPlaceholderMaterial);
  EXPECT_EQ(samples[1].grainsize, "");
  EXPECT_EQ(samples[2].fields.name, "numbers as text");
  // "34.5" reads as a number, but a latitude without a longitude is no
  // location: the row comes through without one, and says so below.
  EXPECT_EQ(samples[2].fields.lat, std::nullopt);
  EXPECT_EQ(samples[2].fields.lon, std::nullopt);
  EXPECT_EQ(samples[2].project, "P");

  std::map<std::string, json> details;
  for (const auto& conflict : batch.conflicts) {
    EXPECT_EQ(conflict.kind, ConflictKind::IdentityClash);
    EXPECT_EQ(conflict.key.commit, std::string(64, 'a'));
    EXPECT_TRUE(details.emplace(conflict.key.path, detail_of(conflict)).second) << conflict.key.path;
  }
  const std::map<std::string, std::string> want{
      {"SampleTbl.jsonl#line2", "the line is not a JSON object"},
      {"SampleTbl.jsonl#line3", "the line is not a JSON object"},
      {"SampleTbl.jsonl#4", "name is missing"},
      {"SampleTbl.jsonl#5", "name is missing"},
      {"SampleTbl.jsonl#6@materialID", "materialID is missing; imported under material 'unknown'"},
      {"SampleTbl.jsonl#7", "lat is not a number; lon is not a number"},
      {"SampleTbl.jsonl#8", "create_date 'last tuesday' is not a time"},
      {"SampleTbl.jsonl#9", "note holds a NUL character"},
      {"SampleTbl.jsonl#1~line10", "id repeats that of an earlier row"},
      {"SampleTbl.jsonl#line11", "id is missing"},
      {"SampleTbl.jsonl#x12", "id is not a whole number"},
      {"SampleTbl.jsonl#13", "materialID is not a whole number"},
      {"SampleTbl.jsonl#14@lat", "lat without lon is not a location; imported without a location"},
      {"SampleTbl.jsonl#15", "lat is not a number"},
  };
  ASSERT_EQ(details.size(), want.size());
  for (const auto& [path, reason] : want) {
    ASSERT_TRUE(details.contains(path)) << path;
    EXPECT_EQ(details.at(path).at("reason"), reason) << path;
    EXPECT_EQ(details.at(path).at("table"), "SampleTbl") << path;
  }
  EXPECT_EQ(details.at("SampleTbl.jsonl#line2").at("text"), "this is not JSON");
  EXPECT_TRUE(details.at("SampleTbl.jsonl#line2").at("legacy_id").is_null());
  EXPECT_EQ(details.at("SampleTbl.jsonl#1~line10").at("legacy_id"), "1");
  EXPECT_EQ(details.at("SampleTbl.jsonl#1~line10").at("line"), 10);
  // A NUL cannot be stored in a jsonb detail: it is spelled out.
  EXPECT_EQ(details.at("SampleTbl.jsonl#9").at("row").at("note"), "a\\0b");
  for (const auto& conflict : batch.conflicts)
    EXPECT_EQ(conflict.detail_json.find("\\u0000"), std::string::npos) << conflict.key.path;
}

TEST(CatalogDbAdapter, RepeatedNaturalKeys) {
  DumpDir dir;
  dir.table("PrincipalInvestigatorTbl",
            {
                R"({"id":1,"affiliation":"NMT","email":null,"last_name":"Ross","first_initial":"J"})",
                R"({"id":2,"affiliation":"NMT","email":null,"last_name":"Ross","first_initial":"J"})",
                R"({"id":3,"affiliation":"UNM","email":null,"last_name":"Ross","first_initial":"J"})",
            })
      .table("ProjectTbl",
             {
                 // Each names a different row of the one investigator.
                 R"({"id":1,"name":"P","principal_investigatorID":1})",
                 R"({"id":2,"name":"P","principal_investigatorID":2})",
                 R"({"id":3,"name":"Q","principal_investigatorID":3})",
             })
      .table("MassSpectrometerTbl",
             {
                 R"({"name":"Jan","kind":"Argus"})",
                 R"({"name":"jan","kind":"Argus"})",
                 R"({"name":"JAN","kind":"Helix"})",
             })
      .done();
  const auto batch = only_batch(dir);
  // A repeat with another value is imported; the value is reported and not sent.
  ASSERT_EQ(batch.conflicts.size(), 2u);
  EXPECT_EQ(batch.conflicts[0].key.path, "PrincipalInvestigatorTbl.jsonl#3");
  json detail = detail_of(batch.conflicts[0]);
  EXPECT_EQ(detail.at("reason"), "has the natural key of PrincipalInvestigatorTbl 1 and other values for affiliation; "
                                 "those of that row are kept");
  EXPECT_EQ(detail.at("imported"), true);
  EXPECT_EQ(detail.at("columns"), json::parse(R"({"affiliation":{"kept":"NMT","given":"UNM"}})"));
  EXPECT_EQ(batch.conflicts[1].key.path, "MassSpectrometerTbl.jsonl#JAN");
  detail = detail_of(batch.conflicts[1]);
  EXPECT_EQ(detail.at("reason"),
            "has the natural key of MassSpectrometerTbl Jan and other values for kind; those of that row are kept");
  EXPECT_EQ(detail.at("columns"), json::parse(R"({"kind":{"kept":"Argus","given":"Helix"}})"));
  const auto investigators = items_of<ingest::PiItem>(batch);
  ASSERT_EQ(investigators.size(), 3u);
  EXPECT_EQ(investigators[1].affiliation, std::optional<std::string>{"NMT"});
  EXPECT_FALSE(investigators[2].affiliation.has_value());
  // A row that names the repeat means the investigator that was kept.
  const auto projects = items_of<ingest::ProjectItem>(batch);
  ASSERT_EQ(projects.size(), 3u);
  EXPECT_EQ(projects[2].name, "Q");
  EXPECT_EQ(projects[2].pi_last_name, std::optional<std::string>{"Ross"});
  EXPECT_EQ(projects[2].pi_first_initial, std::optional<std::string>{"J"});
  const auto spectrometers = items_of<ingest::MassSpecItem>(batch);
  ASSERT_EQ(spectrometers.size(), 3u);
  for (const auto& spectrometer : spectrometers) EXPECT_EQ(spectrometer.spec.name, "jan");
  EXPECT_EQ(spectrometers[1].spec.kind, std::optional<std::string>{"Argus"});
  EXPECT_FALSE(spectrometers[2].spec.kind.has_value());
}

TEST(CatalogDbAdapter, TimesFollowTheColumnTypeAndTheDumpsZone) {
  const std::vector<std::string> irradiations{
      R"({"id":1,"name":"winter","create_date":"2018-01-15 17:00:00"})",
      R"({"id":2,"name":"gap","create_date":"2018-03-11 02:30:00"})",
      R"({"id":3,"name":"day","create_date":"2018-01-15"})",
      R"({"id":4,"name":"zero","create_date":"0000-00-00 00:00:00"})",
      R"({"id":5,"name":"fraction","create_date":"2018-07-01 12:00:00.250000"})",
  };
  const auto created = [](const DumpDir& dir) {
    std::map<std::string, std::string> out;
    for (const auto& item : items_of<ingest::IrradiationItem>(only_batch(dir)))
      out[item.name] = item.created ? item.created->iso() : "none";
    return out;
  };
  {
    // TIMESTAMP in a dump that set its session zone to +00:00: UTC as written.
    DumpDir dir;
    dir.table("IrradiationTbl", irradiations)
        .set("columns", {{"IrradiationTbl", {{"create_date", "timestamp"}}}})
        .done();
    const auto got = created(dir);
    EXPECT_EQ(got.at("winter"), "2018-01-15T17:00:00.000000Z");
    EXPECT_EQ(got.at("gap"), "2018-03-11T02:30:00.000000Z");
    EXPECT_EQ(got.at("day"), "2018-01-15T00:00:00.000000Z");
    EXPECT_EQ(got.at("zero"), "none");
    EXPECT_EQ(got.at("fraction"), "2018-07-01T12:00:00.250000Z");
  }
  {
    // DATETIME: naive lab time. 02:30 on the night the clocks went forward
    // does not exist; the instant the gap begins is taken.
    DumpDir dir;
    dir.table("IrradiationTbl", irradiations).set("columns", {{"IrradiationTbl", {{"create_date", "datetime"}}}}).done();
    const auto got = created(dir);
    EXPECT_EQ(got.at("winter"), "2018-01-16T00:00:00.000000Z");
    EXPECT_EQ(got.at("gap"), "2018-03-11T09:00:00.000000Z");
    EXPECT_EQ(got.at("day"), "2018-01-15T07:00:00.000000Z");
    EXPECT_EQ(got.at("fraction"), "2018-07-01T18:00:00.250000Z");
  }
  {
    // TIMESTAMP in a dump that did not say its zone (--skip-tz-utc): lab time.
    DumpDir dir;
    dir.table("IrradiationTbl", irradiations)
        .set("columns", {{"IrradiationTbl", {{"create_date", "timestamp"}}}})
        .set("time_zone", nullptr)
        .done();
    EXPECT_EQ(created(dir).at("winter"), "2018-01-16T00:00:00.000000Z");
  }
  {
    // No CREATE TABLE in the dump (--no-create-info): the type is the one the
    // legacy ORM declares, TIMESTAMP for an irradiation's and a load's
    // create_date, DATETIME for a sample's dates.
    DumpDir dir;
    dir.table("IrradiationTbl", irradiations)
        .table("LoadTbl", {R"({"name":"L","create_date":"2018-01-15 17:00:00","archived":0})"})
        .table("ProjectTbl", {R"({"id":1,"name":"P"})"})
        .table("MaterialTbl", {R"({"id":1,"name":"M"})"})
        .table("SampleTbl",
               {R"({"id":1,"name":"S","materialID":1,"projectID":1,"create_date":"2018-01-15 17:00:00","update_date":"2018-01-15 18:00:00"})"})
        .done();
    const auto batch = only_batch(dir);
    const auto made = items_of<ingest::IrradiationItem>(batch);
    ASSERT_EQ(made.size(), 5u);
    EXPECT_EQ(made[0].created->iso(), "2018-01-15T17:00:00.000000Z");
    const auto loads = items_of<ingest::LoadItem>(batch);
    ASSERT_EQ(loads.size(), 1u);
    EXPECT_EQ(loads[0].spec.created->iso(), "2018-01-15T17:00:00.000000Z");
    const auto samples = items_of<ingest::SampleItem>(batch);
    ASSERT_EQ(samples.size(), 1u);
    EXPECT_EQ(samples[0].fields.created->iso(), "2018-01-16T00:00:00.000000Z");
    EXPECT_EQ(samples[0].fields.updated->iso(), "2018-01-16T01:00:00.000000Z");
  }
  {
    // Neither a CREATE TABLE nor a session zone: lab time.
    DumpDir dir;
    dir.table("IrradiationTbl", irradiations).set("time_zone", nullptr).done();
    EXPECT_EQ(created(dir).at("winter"), "2018-01-16T00:00:00.000000Z");
  }
  {
    // A type the dump declares wins over the ORM's.
    DumpDir dir;
    dir.table("SampleTbl", {R"({"id":1,"name":"S","materialID":1,"projectID":1,"create_date":"2018-01-15 17:00:00"})"})
        .table("ProjectTbl", {R"({"id":1,"name":"P"})"})
        .table("MaterialTbl", {R"({"id":1,"name":"M"})"})
        .set("columns", {{"SampleTbl", {{"create_date", "timestamp"}}}})
        .done();
    const auto samples = items_of<ingest::SampleItem>(only_batch(dir));
    ASSERT_EQ(samples.size(), 1u);
    EXPECT_EQ(samples[0].fields.created->iso(), "2018-01-15T17:00:00.000000Z");
  }
}

TEST(CatalogDbAdapter, BrokenOptionalLinkGivesTheItemAndAConflict) {
  DumpDir dir;
  dir.table("PrincipalInvestigatorTbl", {R"({"id":1,"last_name":null})"})
      .table("ProjectTbl",
             {
                 R"({"id":1,"name":"dangling","principal_investigatorID":9})",
                 R"({"id":2,"name":"refused parent","principal_investigatorID":1})",
                 // A key that cannot be read is not a broken link: the row is refused.
                 R"({"id":3,"name":"unreadable","principal_investigatorID":"abc"})",
             })
      .table("IrradiationTbl", {R"({"id":1,"name":"NM-1"})"})
      .table("LevelTbl", {R"({"id":1,"name":"A","irradiationID":1})"})
      .table("IrradiationPositionTbl",
             {
                 R"({"id":1,"identifier":"100","sampleID":7,"levelID":1,"position":1})",
                 // Refused for another reason: one conflict, which names the link too.
                 R"({"id":2,"identifier":"101","sampleID":7,"levelID":1,"position":null})",
             })
      .table("LoadTbl", {R"({"name":"L","username":"ghost","archived":1})"})
      .done();
  const auto batch = only_batch(dir);

  const auto projects = items_of<ingest::ProjectItem>(batch);
  ASSERT_EQ(projects.size(), 2u);
  EXPECT_EQ(projects[0].name, "dangling");
  EXPECT_EQ(projects[0].pi_last_name, std::nullopt);
  EXPECT_EQ(projects[1].name, "refused parent");
  EXPECT_EQ(projects[1].pi_last_name, std::nullopt);
  const auto positions = items_of<ingest::PositionItem>(batch);
  ASSERT_EQ(positions.size(), 1u);
  EXPECT_EQ(positions[0].identifier, "100");
  EXPECT_EQ(positions[0].sample, std::nullopt);
  EXPECT_EQ(positions[0].project, std::nullopt);
  const auto loads = items_of<ingest::LoadItem>(batch);
  ASSERT_EQ(loads.size(), 1u);
  EXPECT_EQ(loads[0].created_by, std::nullopt);
  EXPECT_TRUE(loads[0].spec.archived);

  std::map<std::string, json> details;
  for (const auto& conflict : batch.conflicts) {
    EXPECT_EQ(conflict.kind, ConflictKind::IdentityClash);
    EXPECT_TRUE(details.emplace(conflict.key.path, detail_of(conflict)).second) << conflict.key.path;
  }
  const std::map<std::string, std::string> want{
      {"PrincipalInvestigatorTbl.jsonl#1", "last_name is missing"},
      {"ProjectTbl.jsonl#1@principal_investigatorID",
       "principal_investigatorID 9 is not in PrincipalInvestigatorTbl; imported without it"},
      {"ProjectTbl.jsonl#2@principal_investigatorID",
       "principal_investigatorID 1 names a PrincipalInvestigatorTbl row that was not imported; imported without it"},
      {"ProjectTbl.jsonl#3", "principal_investigatorID is not a whole number"},
      {"IrradiationPositionTbl.jsonl#1@sampleID", "sampleID 7 is not in SampleTbl; imported without it"},
      {"IrradiationPositionTbl.jsonl#2", "position is missing; sampleID 7 is not in SampleTbl"},
      {"LoadTbl.jsonl#L@username", "username 'ghost' is not in UserTbl; imported without it"},
  };
  ASSERT_EQ(details.size(), want.size());
  for (const auto& [path, reason] : want) {
    ASSERT_TRUE(details.contains(path)) << path;
    EXPECT_EQ(details.at(path).at("reason"), reason) << path;
    EXPECT_EQ(details.at(path).at("imported"), path.find('@') != std::string::npos) << path;
  }
  const json& link = details.at("ProjectTbl.jsonl#1@principal_investigatorID");
  EXPECT_EQ(link.at("table"), "ProjectTbl");
  EXPECT_EQ(link.at("legacy_id"), "1");
  EXPECT_EQ(link.at("column"), "principal_investigatorID");
  EXPECT_EQ(link.at("value"), 9);
  EXPECT_EQ(link.at("row").at("name"), "dangling");
  EXPECT_EQ(details.at("LoadTbl.jsonl#L@username").at("value"), "ghost");
  // One row is still one unit.
  EXPECT_EQ(batch.done, 9);
}

// MySQL's default collations compare text without regard to case or trailing
// spaces, so a foreign key may spell its parent differently (spec section 10.24).
TEST(CatalogDbAdapter, StringKeysMatchWithoutCaseOrTrailingSpaces) {
  DumpDir dir;
  dir.table("IrradiationTbl", {R"({"id":1,"name":"NM-1"})"})
      .table("LevelTbl", {R"({"id":1,"name":"A","irradiationID":1})"})
      .table("IrradiationPositionTbl",
             {
                 R"({"id":1,"identifier":"ba-01","levelID":1,"position":1})",
                 // The same identifier, as MySQL compares it, in another hole.
                 R"({"id":2,"identifier":"BA-01 ","levelID":1,"position":2})",
             })
      .table("UserTbl",
             {
                 R"({"name":"jross","affiliation":"NMT","category":null,"email":null})",
                 // The same user again, saying the same: nothing new.
                 R"({"name":"JRoss","affiliation":"NMT","category":null,"email":null})",
                 // The same user again, saying something else.
                 R"({"name":"JROSS  ","affiliation":"UNM","category":null,"email":null})",
                 // A leading space is part of a name.
                 R"({"name":" jross","affiliation":null,"category":null,"email":null})",
             })
      .table("ExtractDeviceTbl", {R"({"name":"Fusions CO2"})", R"({"name":"fusions co2 "})"})
      .table("LoadTbl",
             {
                 R"({"name":"L-1","username":"JROSS","archived":0})",
                 R"({"name":"L-2","username":"jross   ","archived":0})",
                 R"({"name":"l-2","username":"jross","archived":0})",
                 R"({"name":"L-3","username":"Jross","archived":1})",
                 R"({"name":"l-3 ","username":"jross","archived":0})",
             })
      .table("LoadPositionTbl",
             {
                 R"({"id":1,"identifier":"BA-01","position":1,"loadName":"l-1 "})",
                 R"({"id":2,"identifier":"ba-01  ","position":2,"loadName":"L-2"})",
                 R"({"id":3,"identifier":" ba-01","position":3,"loadName":"L-2"})",
             })
      .done();
  const auto batch = only_batch(dir);

  const auto users = items_of<ingest::UserItem>(batch);
  ASSERT_EQ(users.size(), 4u);
  EXPECT_EQ(users[0].name, "jross");
  EXPECT_EQ(users[1].name, "jross");  // the first row's spelling
  EXPECT_EQ(users[2].name, "jross");
  EXPECT_FALSE(users[2].affiliation.has_value());  // another one than the first row's: not sent
  EXPECT_EQ(users[3].name, " jross");
  const auto devices = items_of<ingest::ExtractDeviceItem>(batch);
  ASSERT_EQ(devices.size(), 2u);
  EXPECT_EQ(devices[1].name, "Fusions CO2");
  // A reference is stored in its parent's own spelling.
  const auto loads = items_of<ingest::LoadItem>(batch);
  ASSERT_EQ(loads.size(), 5u);
  EXPECT_EQ(loads[0].spec.name, "L-1");
  EXPECT_EQ(loads[2].spec.name, "L-2");
  for (const auto& load : loads) EXPECT_EQ(load.created_by, std::optional<std::string>{"jross"}) << load.spec.name;
  const auto loaded = items_of<ingest::LoadPositionItem>(batch);
  ASSERT_EQ(loaded.size(), 2u);
  EXPECT_EQ(loaded[0].load, "L-1");
  EXPECT_EQ(loaded[0].identifier, "ba-01");
  EXPECT_EQ(loaded[1].load, "L-2");
  EXPECT_EQ(loaded[1].identifier, "ba-01");

  std::map<std::string, std::string> reasons;
  for (const auto& conflict : batch.conflicts) reasons[conflict.key.path] = detail_of(conflict).at("reason");
  EXPECT_EQ(reasons, (std::map<std::string, std::string>{
                         {"IrradiationPositionTbl.jsonl#2",
                          "identifier BA-01  already sits at the position of IrradiationPositionTbl 1"},
                         {"UserTbl.jsonl#JROSS  ",
                          "has the natural key of UserTbl jross and other values for affiliation; those of that row are "
                          "kept"},
                         {"LoadTbl.jsonl#l-3 ",
                          "has the natural key of LoadTbl L-3 and other values for archived; those of that row are kept"},
                         {"LoadPositionTbl.jsonl#3", "identifier  ba-01 is not in IrradiationPositionTbl"},
                     }));
}

// Legacy pychron writes "---------" (NULL_STR) for no value. In an optional
// link or free text that is no value; a name that is a natural key is kept.
TEST(CatalogDbAdapter, LegacyNoneIsNoValueExceptInANaturalKey) {
  DumpDir dir;
  dir.table("MaterialTbl", {R"({"id":1,"name":"---------","grainsize":null})"})
      .table("IrradiationTbl", {R"({"id":1,"name":"NM-1","create_date":null})"})
      .table("LevelTbl", {R"({"id":1,"name":"A","irradiationID":1,"holder":"---------","note":"  "})"})
      .table("IrradiationPositionTbl",
             {R"({"id":1,"identifier":"---------","sampleID":null,"levelID":1,"position":1,"packet":"---------"})"})
      .table("UserTbl", {R"({"name":"ann","email":"---------","affiliation":" ","category":null})"})
      .table("LoadTbl",
             {
                 R"({"name":"L","create_date":null,"archived":0,"username":"---------","holderName":"---------"})",
                 R"({"name":"L2","create_date":null,"archived":0,"username":null,"holderName":"---"})",
                 R"({"name":"-","create_date":null,"archived":0,"username":null,"holderName":" - "})",
             })
      .done();
  const auto batch = only_batch(dir);
  EXPECT_TRUE(batch.conflicts.empty());
  const auto materials = items_of<ingest::MaterialItem>(batch);
  ASSERT_EQ(materials.size(), 1u);
  EXPECT_EQ(materials[0].name, "---------");
  const auto levels = items_of<ingest::LevelItem>(batch);
  ASSERT_EQ(levels.size(), 1u);
  EXPECT_FALSE(levels[0].holder.has_value());
  EXPECT_FALSE(levels[0].note.has_value());
  const auto positions = items_of<ingest::PositionItem>(batch);
  ASSERT_EQ(positions.size(), 1u);
  EXPECT_TRUE(positions[0].identifier.empty());
  EXPECT_FALSE(positions[0].packet.has_value());
  const auto users = items_of<ingest::UserItem>(batch);
  ASSERT_EQ(users.size(), 1u);
  EXPECT_FALSE(users[0].email.has_value());
  EXPECT_FALSE(users[0].affiliation.has_value());
  const auto loads = items_of<ingest::LoadItem>(batch);
  ASSERT_EQ(loads.size(), 3u);
  EXPECT_FALSE(loads[0].created_by.has_value());
  for (const auto& load : loads) {
    EXPECT_FALSE(load.holder_name.has_value()) << load.spec.name;  // any run of hyphens
  }
  EXPECT_EQ(loads[2].spec.name, "-");  // a natural key is kept as written
}

TEST(CatalogDbAdapter, WarnsWhenTheDumpHasNoCompletionMarker) {
  auto whole = CatalogAdapter::open(adapter_config());
  ASSERT_TRUE(whole) << err(whole.error());
  EXPECT_TRUE((*whole)->warnings().empty());

  DumpDir cut;
  cut.table("MaterialTbl", {R"({"id":1,"name":"M"})"}).set("dump_completed", false).done();
  auto adapter = CatalogAdapter::open(adapter_config(cut.path()));
  ASSERT_TRUE(adapter) << err(adapter.error());  // not an error: mysqldump --skip-comments writes no marker
  const auto warnings = (*adapter)->warnings();
  ASSERT_EQ(warnings.size(), 1u);
  EXPECT_NE(warnings[0].find("no completion marker"), std::string::npos) << warnings[0];
  EXPECT_NE(warnings[0].find("truncated"), std::string::npos) << warnings[0];
  NoState state;
  EXPECT_EQ(*(*adapter)->plan(std::nullopt, state), 1);

  // A manifest that does not say (written by hand, or by an older converter) is not warned about.
  DumpDir silent;
  silent.table("MaterialTbl", {R"({"id":1,"name":"M"})"}).done();
  auto quiet = CatalogAdapter::open(adapter_config(silent.path()));
  ASSERT_TRUE(quiet);
  EXPECT_TRUE((*quiet)->warnings().empty());
}

TEST(CatalogDbAdapter, ProjectDatesAndBinaryText) {
  DumpDir dir;
  dir.table("ProjectTbl",
            {
                R"({"id":1,"name":"day","checkin_date":"2019-03-04"})",
                R"({"id":2,"name":"datetime","checkin_date":"2019-03-04 23:59:59"})",
                R"({"id":3,"name":"zero","checkin_date":"0000-00-00"})",
                R"({"id":4,"name":"bad","checkin_date":"2019-02-30"})",
                R"({"id":5,"name":"worse","checkin_date":"spring"})",
                // A BLOB column the dump wrote in hex: UTF-8, then Latin-1 bytes.
                R"({"id":6,"name":"blob","comment__base64":"Y2Fmw6k="})",
                R"({"id":7,"name":"latin1","comment__base64":"Y2Fm6Q=="})",
                R"({"id":8,"name":"not base64","comment__base64":"***"})",
                R"({"id":9,"name":"empty","comment":"","lab_contact":"","institution":null})",
            })
      .done();
  const auto batch = only_batch(dir);
  std::map<std::string, ingest::ProjectItem> projects;
  for (const auto& item : items_of<ingest::ProjectItem>(batch)) projects[item.name] = item;
  ASSERT_EQ(projects.size(), 6u);
  EXPECT_EQ(projects.at("day").checkin_date, std::optional<std::string>{"2019-03-04"});
  // The calendar day as the lab wrote it, not shifted to UTC.
  EXPECT_EQ(projects.at("datetime").checkin_date, std::optional<std::string>{"2019-03-04"});
  EXPECT_EQ(projects.at("zero").checkin_date, std::nullopt);
  EXPECT_EQ(projects.at("blob").comment, std::optional<std::string>{"caf\xC3\xA9"});
  EXPECT_EQ(projects.at("latin1").comment, std::optional<std::string>{"caf\xC3\xA9"});
  EXPECT_EQ(projects.at("empty").comment, std::nullopt);
  EXPECT_EQ(projects.at("empty").lab_contact, std::nullopt);
  ASSERT_EQ(batch.conflicts.size(), 3u);
  EXPECT_EQ(detail_of(batch.conflicts[0]).at("reason"), "checkin_date '2019-02-30' is not a date");
  EXPECT_EQ(detail_of(batch.conflicts[1]).at("reason"), "checkin_date 'spring' is not a date");
  EXPECT_EQ(detail_of(batch.conflicts[2]).at("reason"), "comment is not base64");
}

// ---------------------------------------------------------------- tags

TEST(CatalogDbTags, LookupJoinsChangeRowsToAnalysisUuids) {
  auto lookup = load_tag_lookup(kFixture);
  ASSERT_TRUE(lookup) << err(lookup.error());
  ASSERT_TRUE(static_cast<bool>(*lookup));
  // Stored without dashes; its later change row counts.
  EXPECT_EQ((*lookup)(*Uuid::parse("0f8fad5b-d9cb-469f-a165-70867728950e")), std::optional<std::string>{"omit"});
  // Stored with dashes.
  EXPECT_EQ((*lookup)(*Uuid::parse("7c9e6679-7425-40de-944b-e07fc1f90ae7")), std::optional<std::string>{"invalid"});
  // No change row; a change row without a tag (stored in upper case); not in the dump.
  EXPECT_EQ((*lookup)(*Uuid::parse("11111111-2222-4333-8444-555555555555")), std::nullopt);
  EXPECT_EQ((*lookup)(*Uuid::parse("aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee")), std::nullopt);
  EXPECT_EQ((*lookup)(Uuid::v7()), std::nullopt);
}

TEST(CatalogDbTags, LatestChangeRowCountsWhateverTheOrder) {
  DumpDir dir;
  dir.table("AnalysisTbl",
            {
                R"({"id":1,"uuid":"AAAAAAAABBBB4CCC8DDDEEEEEEEEEEEE"})",
                R"({"id":2,"uuid":"{7c9e6679-7425-40de-944b-e07fc1f90ae7}"})",
                R"({"id":3,"uuid":"not a uuid"})",
                R"({"id":4,"uuid":null})",
                R"({"uuid":"11111111-2222-4333-8444-555555555555"})",
            })
      .table("AnalysisChangeTbl",
             {
                 R"({"idanalysischangeTbl":9,"tag":"invalid","analysisID":1})",
                 R"({"idanalysischangeTbl":4,"tag":"ok","analysisID":1})",
                 R"({"idanalysischangeTbl":5,"tag":"omit","analysisID":2})",
                 R"({"idanalysischangeTbl":6,"tag":"","analysisID":2})",
                 R"({"idanalysischangeTbl":7,"tag":"ok","analysisID":3})",
                 R"({"idanalysischangeTbl":8,"tag":"ok","analysisID":4})",
                 R"({"idanalysischangeTbl":10,"tag":"ok","analysisID":null})",
             })
      .done();
  auto lookup = load_tag_lookup(dir.path());
  ASSERT_TRUE(lookup) << err(lookup.error());
  EXPECT_EQ((*lookup)(*Uuid::parse("aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee")), std::optional<std::string>{"invalid"});
  // Its latest change row has no tag.
  EXPECT_EQ((*lookup)(*Uuid::parse("7c9e6679-7425-40de-944b-e07fc1f90ae7")), std::nullopt);
  EXPECT_EQ((*lookup)(*Uuid::parse("11111111-2222-4333-8444-555555555555")), std::nullopt);
}

TEST(CatalogDbTags, MissingFilesGiveAnEmptyLookup) {
  const Uuid any = *Uuid::parse("0f8fad5b-d9cb-469f-a165-70867728950e");
  {
    DumpDir empty;
    auto lookup = load_tag_lookup(empty.path());
    ASSERT_TRUE(lookup) << err(lookup.error());
    ASSERT_TRUE(static_cast<bool>(*lookup));
    EXPECT_EQ((*lookup)(any), std::nullopt);
    auto nowhere = load_tag_lookup(empty.path() / "nope");
    ASSERT_TRUE(nowhere) << err(nowhere.error());
    EXPECT_EQ((*nowhere)(any), std::nullopt);
  }
  {
    // A dump without the two tables, and one with only one of them.
    DumpDir dir;
    dir.table("MaterialTbl", {R"({"id":1,"name":"M"})"}).done();
    auto lookup = load_tag_lookup(dir.path());
    ASSERT_TRUE(lookup) << err(lookup.error());
    EXPECT_EQ((*lookup)(any), std::nullopt);
    dir.table("AnalysisTbl", {R"({"id":1,"uuid":"0f8fad5bd9cb469fa16570867728950e"})"}).done();
    lookup = load_tag_lookup(dir.path());
    ASSERT_TRUE(lookup) << err(lookup.error());
    EXPECT_EQ((*lookup)(any), std::nullopt);
  }
}

TEST(CatalogDbTags, UnfinishedConversionAndBrokenFilesAreErrors) {
  {
    DumpDir dir;
    dir.table("AnalysisTbl", {R"({"id":1,"uuid":"0f8fad5bd9cb469fa16570867728950e"})"});
    auto lookup = load_tag_lookup(dir.path());
    ASSERT_FALSE(lookup);
    EXPECT_NE(lookup.error().what.find("MANIFEST.json"), std::string::npos) << lookup.error().what;
  }
  {
    DumpDir dir;
    dir.table("AnalysisTbl", {R"({"id":1,"uuid":"0f8fad5bd9cb469fa16570867728950e"})"})
        .table("AnalysisChangeTbl", {R"({"idanalysischangeTbl":1,"tag":"ok","analysisID":1})", "{\"idanalysis"})
        .done();
    auto lookup = load_tag_lookup(dir.path());
    ASSERT_FALSE(lookup);
    EXPECT_NE(lookup.error().what.find("AnalysisChangeTbl.jsonl line 2"), std::string::npos) << lookup.error().what;
  }
}
