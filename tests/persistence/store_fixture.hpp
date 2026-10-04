#pragma once

// Engine-parameterised store fixture. Every test gets a fresh database: an
// in-memory or temp-file SQLite database, or a fresh PostgreSQL schema when
// PYCHRON_TEST_PG_URL is set.

#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include <QCoreApplication>

#include "pychron/core/env.hpp"
#include "pychron/persistence/store.hpp"
#include "tiny/db.hpp"

namespace pychron::persistence::testing {

inline std::string pg_url() {
  return env_var("PYCHRON_TEST_PG_URL").value_or("");
}

// "sqlite" always; "pg" when PYCHRON_TEST_PG_URL is set.
inline std::vector<std::string> engines() {
  std::vector<std::string> e{"sqlite"};
  if (!pg_url().empty()) e.push_back("pg");
  return e;
}

// A database that exists for the lifetime of this object.
class TestDatabase {
 public:
  // file_backed: SQLite in a temp file (several connections can share it).
  TestDatabase(const std::string& engine, bool file_backed) {
    const std::string tag = unique_tag();
    if (engine == "pg") {
      schema_ = "pychron_t_" + tag;
      const std::string base = pg_url();
      url_ = base + (base.find('?') == std::string::npos ? "?" : "&") + "search_path=" + schema_;
      admin_ = std::move(*detail::Db::open(StoreConfig{base, false}));
      (void)admin_->unprepared(detail::qs("DROP SCHEMA IF EXISTS " + schema_ + " CASCADE"));
      (void)admin_->unprepared(detail::qs("CREATE SCHEMA " + schema_));
    } else if (file_backed) {
      path_ = std::filesystem::temp_directory_path() / ("pychron_store_" + tag + ".sqlite");
      remove_sqlite_files(path_);  // a file a killed test left under the same name
      url_ = "sqlite:" + path_.string();
    } else {
      url_ = "sqlite::memory:";
    }
  }

  ~TestDatabase() {
    if (admin_) (void)admin_->unprepared(detail::qs("DROP SCHEMA IF EXISTS " + schema_ + " CASCADE"));
    if (!path_.empty()) remove_sqlite_files(path_);
  }

  // "<pid>_<64 random bits, hex>_<n>": a name no other database of this
  // process, and of no other test process running beside it, has. ctest runs
  // one process per test, so the counter alone is always 0, and a short
  // random number is shared sooner or later by two of the binaries that run
  // in parallel.
  static std::string unique_tag() {
    static std::atomic<int> counter{0};
    std::random_device device;
    const std::uint64_t random = (static_cast<std::uint64_t>(device()) << 32) | static_cast<std::uint64_t>(device());
    static const char kHex[] = "0123456789abcdef";
    std::string hex(16, '0');
    for (int i = 0; i < 16; ++i) hex[static_cast<std::size_t>(i)] = kHex[(random >> (60 - 4 * i)) & 0xf];
    return std::to_string(QCoreApplication::applicationPid()) + "_" + hex + "_" + std::to_string(counter.fetch_add(1));
  }

  // A SQLite database file and the write-ahead log and shared-memory files
  // SQLite keeps beside it: left behind, they would be replayed into the next
  // database of that name.
  static void remove_sqlite_files(const std::filesystem::path& database) {
    for (const char* suffix : {"", "-wal", "-shm"}) {
      std::error_code ignored;
      std::filesystem::remove(database.string() + suffix, ignored);
    }
  }

  const std::string& url() const { return url_; }

 private:
  std::string url_;
  std::string schema_;
  std::filesystem::path path_;
  std::unique_ptr<detail::Db> admin_;
};

inline std::unique_ptr<IStore> open_or_die(const std::string& url) {
  auto store = open_store(StoreConfig{url, true});
  if (!store) {
    ADD_FAILURE() << "open_store(" << url << "): " << to_string(store.error());
    return nullptr;
  }
  return std::move(*store);
}

// Catalog rows every ingest test needs.
struct Lab {
  Uuid acquisition_client;
  Uuid reduction_client;
  Uuid analyst;
  Uuid reducer;
  Uuid mass_spectrometer;
  Uuid irradiation;
  Uuid level;
  Uuid position;
  Uuid identifier;
  Uuid identifier2;  // "66574", no irradiation position
};

// Catalog writes made by seed_lab (each is one change_log entry).
inline constexpr int kSeedLabChanges = 10;

inline Lab seed_lab(IStore& store) {
  Lab lab;
  lab.acquisition_client = *store.register_client({"acq-1", "acquisition", std::nullopt, "test"});
  lab.reduction_client = *store.register_client({"red-1", "reduction", std::nullopt, "test"});
  lab.analyst = *store.ensure_user(lab.acquisition_client, "jross");
  lab.reducer = *store.ensure_user(lab.reduction_client, "jsmith");
  lab.mass_spectrometer = *store.add_mass_spectrometer(lab.acquisition_client, {"jan", "argus", "j", std::nullopt});
  lab.irradiation = *store.add_irradiation(lab.acquisition_client, "NM-300");
  lab.level = *store.add_level(lab.acquisition_client, {lab.irradiation, "A", std::nullopt, 0.5, std::nullopt, std::nullopt});
  lab.position = *store.add_irradiation_position(lab.acquisition_client, {lab.level, 1, std::nullopt, std::nullopt, {}, {}, std::nullopt});
  lab.identifier =
      *store.add_identifier(lab.acquisition_client, {"66573", "unknown", std::nullopt, std::nullopt, lab.position, std::nullopt, std::nullopt});
  lab.identifier2 =
      *store.add_identifier(lab.acquisition_client, {"66574", "unknown", std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt});
  return lab;
}

inline Bytes series(float offset, int n = 4) {
  std::vector<TvPoint> p;
  for (int i = 0; i < n; ++i) p.push_back({static_cast<float>(i), offset + static_cast<float>(i) * 0.5f});
  return encode_tv(p);
}

// A complete analysis ingest with one signal and one baseline series.
inline IngestItem analysis_item(const Lab& lab, int aliquot, const Bytes& signal, const Bytes& baseline,
                                const std::string& identifier = "66573") {
  AnalysisIngest a;
  a.analysis = Uuid::v7();
  a.changeset = Uuid::v7();
  a.created = UtcTime::now();
  a.identifier = identifier;
  a.aliquot = aliquot;
  a.analysis_type = "unknown";
  a.timestamp = *UtcTime::parse("2026-10-02T12:00:00.000001Z");
  a.mass_spectrometer = "jan";
  a.analyst = "jross";
  a.load_name = "load-1";
  a.isotopes = {{"Ar40", "H1", "fA", std::nullopt, std::nullopt, std::nullopt},
                {"Ar39", "AX", "fA", std::nullopt, std::nullopt, std::nullopt}};
  a.detectors = {{"H1", 0.0, 1.0}, {"AX", 0.0, 1.0}};
  auto& r = a.roots;
  r.signals = Uuid::v7();
  r.intercepts = Uuid::v7();
  r.baselines = Uuid::v7();
  r.blanks = Uuid::v7();
  r.icfactors = Uuid::v7();
  r.tags = Uuid::v7();
  // Canonical (series_kind, series_key) order, as the store returns them.
  r.signal_refs = {{"baseline", "H1", "H1", blob_sha256(kCodecTv, baseline), 4, std::nullopt, std::nullopt},
                   {"signal", "Ar40", "H1", blob_sha256(kCodecTv, signal), 4, std::nullopt, std::nullopt}};
  InterceptRow ar40;
  ar40.isotope = "Ar40";
  ar40.detector = "H1";
  ar40.value = 100.5;
  ar40.error = 0.25;
  ar40.fit = "linear";
  r.intercepts_rows = {ar40};
  BaselineRow h1;
  h1.detector = "H1";
  h1.value = 0.01;
  h1.error = 0.001;
  h1.fit = "average";
  r.baselines_rows = {h1};
  BlankRow blank;
  blank.isotope = "Ar40";
  blank.value = 0.5;
  blank.error = 0.05;
  blank.fit = "preceding";
  r.blanks_rows = {blank};
  IcFactorRow ic;
  ic.detector = "H1";
  ic.value = 1.0;
  ic.error = 0.0;
  ic.fit = "default";
  r.icfactors_rows = {ic};
  return IngestItem{a.analysis, sha256(std::string_view{"payload-" + a.analysis.str()}), lab.acquisition_client,
                    std::move(a)};
}

// Makes every UPDATE of `table` fail with an error that is no constraint
// violation, on either engine: a trigger that writes to a table that does
// not exist (SQLite: SQLITE_ERROR; PostgreSQL: 42P01).
inline Result<void> break_updates_of(detail::Db& db, const char* table) {
  const QString name = QString::fromUtf8(table);
  if (db.dialect() == Dialect::Sqlite)
    return db.unprepared(QStringLiteral("CREATE TRIGGER pychron_test_break BEFORE UPDATE ON %1 BEGIN "
                                        "INSERT INTO pychron_test_nowhere VALUES (1); END")
                             .arg(name));
  if (auto r = db.unprepared(QStringLiteral(
          "CREATE FUNCTION pychron_test_break() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN "
          "INSERT INTO pychron_test_nowhere VALUES (1); RETURN NEW; END $$"));
      !r)
    return r;
  return db.unprepared(QStringLiteral("CREATE TRIGGER pychron_test_break BEFORE UPDATE ON %1 FOR EACH ROW "
                                      "EXECUTE FUNCTION pychron_test_break()")
                           .arg(name));
}

class StoreTest : public ::testing::TestWithParam<std::string> {
 protected:
  void SetUp() override {
    db_ = std::make_unique<TestDatabase>(GetParam(), false);
    store_ = open_or_die(db_->url());
    ASSERT_TRUE(store_);
    lab_ = seed_lab(*store_);
  }

  // Declared first so it is destroyed last: the store holds a connection to it.
  std::unique_ptr<TestDatabase> db_;
  std::unique_ptr<IStore> store_;
  Lab lab_;
};

}  // namespace pychron::persistence::testing
