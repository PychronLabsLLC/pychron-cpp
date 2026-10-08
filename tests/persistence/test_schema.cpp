// D1: migrations, statement splitting, PostgreSQL/SQLite DDL parity and the
// append-only triggers (DVC schema spec, sections 11, 12.6 "ddl/").

#include <gtest/gtest.h>

#include <map>
#include <set>

#include "migrate.hpp"
#include "sql/statements.hpp"
#include "store_fixture.hpp"

using namespace pychron;
using namespace pychron::persistence;
using namespace pychron::persistence::testing;
namespace pd = pychron::persistence::detail;

namespace {

const char* const kAppendOnly[] = {
    "changeset",          "revision",           "head_move",         "bookmark_entry",
    "intercept_value",    "baseline_value",     "blank_value",       "blank_reference",
    "icfactor_value",     "icfactor_reference", "signal_ref",        "tag_value",
    "annotation_value",   "refpin_value",       "identity_value",    "cosmogenic_value",
    "ia_value",           "ia_member",          "flux_value",        "flux_value_analysis",
    "level_z_value",      "production_meta",    "production_value",  "level_production_value",
    "chronology_dose",    "detector_gain",      "sensitivity_value", "holder_meta",
    "holder_hole",        "script_version",     "ref_document",      "signal_blob",
    "script_text",        "spectrometer_snapshot", "change_log",     "change_entity",
    "ingest_receipt"};

std::unique_ptr<pd::Db> raw(const std::string& url) {
  auto db = pd::Db::open(StoreConfig{url, false});
  EXPECT_TRUE(db) << (db ? "" : to_string(db.error()));
  return db ? std::move(*db) : nullptr;
}

std::vector<std::string> column(pd::Db& db, const QString& sql, const char* name, const pd::Bindings& b = {}) {
  std::vector<std::string> out;
  auto rows = db.select(sql, b);
  EXPECT_TRUE(rows) << (rows ? "" : to_string(rows.error()));
  if (rows)
    for (const auto& r : *rows) out.push_back(pd::to_std(r.value(name)));
  return out;
}

// "table|column", "table|p|a,b", "table|u|a,b", "table|f|a,b->ref"
std::set<std::string> introspect(pd::Db& db) {
  std::set<std::string> out;
  if (db.dialect() == Dialect::Sqlite) {
    for (const auto& t : column(db, "SELECT name FROM sqlite_master WHERE type = 'table' AND name NOT LIKE 'sqlite_%'",
                                "name")) {
      const pd::Bindings tb{pd::qv(t)};
      std::map<int, std::string> pk;
      auto cols = db.select("SELECT name, pk FROM pragma_table_xinfo(?)", tb);
      for (const auto& r : *cols) {
        out.insert(t + "|" + pd::to_std(r.value("name")));
        if (r.value("pk").toInt() > 0) pk[r.value("pk").toInt()] = pd::to_std(r.value("name"));
      }
      std::string p;
      for (const auto& [i, c] : pk) p += (p.empty() ? "" : ",") + c;
      if (!p.empty()) out.insert(t + "|p|" + p);
      for (const auto& idx : column(db, "SELECT name FROM pragma_index_list(?) WHERE origin = 'u'", "name", tb)) {
        std::string u;
        for (const auto& c : column(db, "SELECT name FROM pragma_index_info(?) ORDER BY seqno", "name",
                                    {pd::qv(idx)}))
          u += (u.empty() ? "" : ",") + c;
        out.insert(t + "|u|" + u);
      }
      std::map<int, std::pair<std::string, std::string>> fks;
      auto fk = db.select(R"(SELECT id, "from" AS col, "table" AS ref FROM pragma_foreign_key_list(?) ORDER BY id, seq)",
                          tb);
      for (const auto& r : *fk) {
        auto& [cols_, ref] = fks[r.value("id").toInt()];
        cols_ += (cols_.empty() ? "" : ",") + pd::to_std(r.value("col"));
        ref = pd::to_std(r.value("ref"));
      }
      for (const auto& [id, f] : fks) out.insert(t + "|f|" + f.first + "->" + f.second);
    }
  } else {
    for (const auto& c : column(db,
                                "SELECT table_name || '|' || column_name AS c FROM information_schema.columns "
                                "WHERE table_schema = current_schema()",
                                "c"))
      out.insert(c);
    auto rows = db.select(
        "SELECT t.relname AS tbl, c.contype AS type, c.conname AS name, a.attname AS col, rt.relname AS ref "
        "FROM pg_constraint c JOIN pg_class t ON t.oid = c.conrelid "
        "JOIN pg_namespace n ON n.oid = t.relnamespace "
        "CROSS JOIN LATERAL unnest(c.conkey) WITH ORDINALITY AS k(attnum, ord) "
        "JOIN pg_attribute a ON a.attrelid = t.oid AND a.attnum = k.attnum "
        "LEFT JOIN pg_class rt ON rt.oid = c.confrelid "
        "WHERE n.nspname = current_schema() AND c.contype IN ('p', 'u', 'f') ORDER BY c.conname, k.ord");
    EXPECT_TRUE(rows);
    std::map<std::string, std::tuple<std::string, std::string, std::string, std::string>> cons;
    for (const auto& r : *rows) {
      auto& [tbl, type, cols_, ref] = cons[pd::to_std(r.value("name")) + "@" + pd::to_std(r.value("tbl"))];
      tbl = pd::to_std(r.value("tbl"));
      type = pd::to_std(r.value("type"));
      cols_ += (cols_.empty() ? "" : ",") + pd::to_std(r.value("col"));
      ref = pd::to_std(r.value("ref"));
    }
    for (const auto& [name, c] : cons) {
      const auto& [tbl, type, cols_, ref] = c;
      out.insert(tbl + "|" + type + "|" + cols_ + (type == "f" ? "->" + ref : ""));
    }
  }
  return out;
}

class SchemaTest : public ::testing::TestWithParam<std::string> {};

}  // namespace

TEST(SplitSql, HandlesQuotesCommentsDollarBodiesAndTriggers) {
  const auto s = pd::split_sql(
      "CREATE TABLE a (x text DEFAULT ';'); -- a comment; with semicolon\n"
      "CREATE FUNCTION f() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN RAISE EXCEPTION 'x;y'; END $$;\n"
      "CREATE TRIGGER t BEFORE UPDATE ON a BEGIN SELECT RAISE(ABORT, 'no;'); END;\n"
      "CREATE TRIGGER u BEFORE UPDATE ON a FOR EACH ROW EXECUTE FUNCTION f();\n"
      "INSERT INTO a VALUES ('it''s');");
  ASSERT_EQ(s.size(), 5u);
  EXPECT_EQ(s[0], "CREATE TABLE a (x text DEFAULT ';')");
  EXPECT_NE(s[1].find("END $$"), std::string::npos);
  EXPECT_EQ(s[2], "CREATE TRIGGER t BEFORE UPDATE ON a BEGIN SELECT RAISE(ABORT, 'no;'); END");
  EXPECT_EQ(s[3], "CREATE TRIGGER u BEFORE UPDATE ON a FOR EACH ROW EXECUTE FUNCTION f()");
  EXPECT_EQ(s[4], "INSERT INTO a VALUES ('it''s')");
}

TEST_P(SchemaTest, MigrateIsIdempotentAndRecordsChecksums) {
  TestDatabase tdb(GetParam(), true);
  {
    auto store = open_or_die(tdb.url());
    ASSERT_TRUE(store);
    auto status = store->schema_status();
    ASSERT_TRUE(status) << to_string(status.error());
    ASSERT_EQ(status->size(), 5u);
    EXPECT_EQ((*status)[0].version, 1);
    EXPECT_EQ((*status)[0].description, "init");
    EXPECT_EQ((*status)[0].checksum_hex.size(), 64u);
    EXPECT_EQ((*status)[1].version, 2);
    EXPECT_EQ((*status)[1].description, "import_detail");
    EXPECT_EQ((*status)[2].version, 3);
    EXPECT_EQ((*status)[2].description, "entry");
    EXPECT_EQ((*status)[3].version, 4);
    EXPECT_EQ((*status)[3].description, "sample_geometry");
    EXPECT_EQ((*status)[4].version, 5);
    EXPECT_EQ((*status)[4].description, "ref_scope_indexes");
  }
  // Reopen: nothing pending, so migrate = false opens fine.
  auto again = open_store(StoreConfig{tdb.url(), false});
  ASSERT_TRUE(again) << to_string(again.error());
}

TEST_P(SchemaTest, RefusesAnOutdatedSchemaWithoutMigrate) {
  TestDatabase tdb(GetParam(), true);
  auto store = open_store(StoreConfig{tdb.url(), false});
  ASSERT_FALSE(store);
  EXPECT_EQ(store.error().kind, ErrorKind::Config);
  // Whoever reads this is at a window that will not open their data: it says what to run.
  EXPECT_NE(store.error().what.find("elctl db migrate"), std::string::npos) << store.error().what;
}

// The reference values of an analysis (its flux, its level's production, its
// irradiation's chronology, its spectrometer's gains) are found by what they
// belong to. Without an index on each of those columns every reference
// object of the type was read for every analysis loaded: 21 000 of them, on
// a real store, 2.6 ms an analysis.
TEST(SchemaIndexes, AnAnalysissReferencesAreFoundByScope) {
  TestDatabase tdb("sqlite", true);
  ASSERT_TRUE(open_or_die(tdb.url()));
  auto db = raw(tdb.url());
  ASSERT_TRUE(db);
  const pd::Bindings scope{QStringLiteral("p"), QStringLiteral("l"), QStringLiteral("i"), QStringLiteral("m")};
  const std::vector<std::string> plan =
      column(*db, QStringLiteral("EXPLAIN QUERY PLAN ") + pd::sql::kRefCandidates, "detail", scope);
  ASSERT_FALSE(plan.empty());
  std::string all;
  for (const auto& step : plan) all += step + "\n";
  for (const char* index : {"ref_object_position_ix", "ref_object_level_ix", "ref_object_irradiation_ix",
                            "ref_object_mass_spectrometer_ix"}) {
    EXPECT_NE(all.find(index), std::string::npos) << index << " is not used:\n" << all;
  }
  EXPECT_EQ(all.find("SCAN"), std::string::npos) << all;
  EXPECT_EQ(all.find("(ref_type=?)"), std::string::npos) << "found by type alone:\n" << all;
}

TEST_P(SchemaTest, ChecksumMismatchIsFatal) {
  TestDatabase tdb(GetParam(), true);
  ASSERT_TRUE(open_or_die(tdb.url()));
  auto db = raw(tdb.url());
  ASSERT_TRUE(db);
  ASSERT_TRUE(db->affecting("UPDATE schema_version SET checksum = ? WHERE version = 1",
                            {pd::qv(pychron::sha256(std::string_view{"tampered"}))}));
  auto store = open_store(StoreConfig{tdb.url(), true});
  ASSERT_FALSE(store);
  EXPECT_EQ(store.error().kind, ErrorKind::Config);
}

TEST_P(SchemaTest, EveryAppendOnlyTableHasItsTriggers) {
  TestDatabase tdb(GetParam(), true);
  ASSERT_TRUE(open_or_die(tdb.url()));
  auto db = raw(tdb.url());
  ASSERT_TRUE(db);
  for (const char* table : kAppendOnly) {
    const QString sql =
        db->dialect() == Dialect::Sqlite
            ? QStringLiteral("SELECT count(*) AS n FROM sqlite_master WHERE type = 'trigger' AND tbl_name = ?")
            : QStringLiteral("SELECT count(*) AS n FROM pg_trigger g JOIN pg_class c ON c.oid = g.tgrelid "
                             "JOIN pg_namespace s ON s.oid = c.relnamespace "
                             "WHERE s.nspname = current_schema() AND c.relname = ? AND NOT g.tgisinternal");
    auto n = db->select_one(sql, {pd::qv(table)});
    ASSERT_TRUE(n);
    EXPECT_GE((*n)->value("n").toInt(), 1) << table;
  }
}

TEST_P(SchemaTest, AppendOnlyRowsRejectUpdateAndDelete) {
  TestDatabase tdb(GetParam(), true);
  auto store = open_or_die(tdb.url());
  ASSERT_TRUE(store);
  const Lab lab = seed_lab(*store);
  const Bytes signal = series(1), baseline = series(0);
  auto item = analysis_item(lab, 1, signal, baseline);
  ASSERT_TRUE(store->ingest(item));
  ASSERT_TRUE(store->ingest(IngestItem{Uuid::v7(), {}, lab.acquisition_client, BlobIngest{"f32le-tv/1", signal, 4}}));
  auto db = raw(tdb.url());
  ASSERT_TRUE(db);

  const std::pair<const char*, const char*> attempts[] = {
      {"UPDATE changeset SET message = 'x'", "DELETE FROM changeset"},
      {"UPDATE revision SET created_utc = created_utc", "DELETE FROM revision"},
      {"UPDATE head_move SET reason = 'commit'", "DELETE FROM head_move"},
      {"UPDATE intercept_value SET value = 0", "DELETE FROM intercept_value"},
      {"UPDATE signal_ref SET n_points = 1", "DELETE FROM signal_ref"},
      {"UPDATE signal_blob SET codec = 'x'", "DELETE FROM signal_blob"},
      {"UPDATE change_log SET kind = 'catalog'", "DELETE FROM change_log"},
      {"UPDATE change_entity SET op = 'x'", "DELETE FROM change_entity"},
      {"UPDATE ingest_receipt SET kind = 'x'", "DELETE FROM ingest_receipt"},
  };
  for (const auto& [update, del] : attempts) {
    auto u = db->affecting(update);
    EXPECT_FALSE(u) << update;
    if (!u) {
      EXPECT_EQ(u.error().kind, ErrorKind::Protocol) << update << ": " << u.error().what;
    }
    EXPECT_FALSE(db->affecting(del)) << del;
  }

  // analysis: identity columns, provisional, runid_text and signals_state may change; nothing else.
  EXPECT_TRUE(db->affecting("UPDATE analysis SET signals_state = 'complete', provisional = ?", {pd::qv(false)}));
  EXPECT_TRUE(db->affecting("UPDATE analysis SET aliquot = 2, runid_text = '66573-02'"));
  EXPECT_FALSE(db->affecting("UPDATE analysis SET analysis_type = 'blank'"));
  EXPECT_FALSE(db->affecting("UPDATE analysis SET weight = 1.5"));
  EXPECT_FALSE(db->affecting("DELETE FROM analysis"));
}

TEST(SchemaParity, PostgresAndSqliteHaveTheSameTablesColumnsAndKeys) {
  if (pg_url().empty()) GTEST_SKIP() << "PYCHRON_TEST_PG_URL not set";
  TestDatabase pg("pg", false), lite("sqlite", true);
  ASSERT_TRUE(open_or_die(pg.url()));
  ASSERT_TRUE(open_or_die(lite.url()));
  auto pg_db = raw(pg.url());
  auto lite_db = raw(lite.url());
  ASSERT_TRUE(pg_db && lite_db);
  const auto a = introspect(*pg_db);
  const auto b = introspect(*lite_db);
  EXPECT_GT(a.size(), 500u);
  for (const auto& x : a)
    if (!b.count(x)) ADD_FAILURE() << "only in PostgreSQL: " << x;
  for (const auto& x : b)
    if (!a.count(x)) ADD_FAILURE() << "only in SQLite: " << x;
}

INSTANTIATE_TEST_SUITE_P(Engines, SchemaTest, ::testing::ValuesIn(engines()),
                         [](const auto& p) { return p.param; });
