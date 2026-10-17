// D1: migrations, statement splitting, PostgreSQL/SQLite DDL parity and the
// append-only triggers (DVC schema spec, sections 11, 12.6 "ddl/").

#include <gtest/gtest.h>

#include <map>
#include <set>

#include "migrate.hpp"
#include "sql/browse.hpp"
#include "sql/catalog.hpp"
#include "sql/statements.hpp"
#include "store_fixture.hpp"
#include "store_impl.hpp"

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
    ASSERT_EQ(status->size(), 6u);
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
    EXPECT_EQ((*status)[5].version, 6);
    EXPECT_EQ((*status)[5].description, "pi_identity");
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

// The next aliquot of an identifier is read from the end of its unique index
// on (identifier_uuid, aliquot, increment): one seek, however many analyses.
TEST(SchemaIndexes, TheLastAnalysisOfAnIdentifierIsASeek) {
  TestDatabase tdb("sqlite", true);
  ASSERT_TRUE(open_or_die(tdb.url()));
  auto db = raw(tdb.url());
  ASSERT_TRUE(db);
  const std::vector<std::string> plan =
      column(*db, QStringLiteral("EXPLAIN QUERY PLAN ") + pd::sql::kLastAnalysis, "detail", {QStringLiteral("66573")});
  ASSERT_FALSE(plan.empty());
  std::string all;
  for (const auto& step : plan) all += step + "\n";
  EXPECT_NE(all.find("sqlite_autoindex_analysis"), std::string::npos) << all;
  EXPECT_NE(all.find("identifier_uuid=?"), std::string::npos) << all;
  EXPECT_EQ(all.find("SCAN"), std::string::npos) << all;
  EXPECT_EQ(all.find("TEMP B-TREE"), std::string::npos) << all;
}

// A PI given by its ORCID (an entry sheet's principal_investigator_orcid
// column) is found by the partial unique index on it, not by reading every PI.
TEST(SchemaIndexes, APiIsFoundByOrcid) {
  TestDatabase tdb("sqlite", true);
  ASSERT_TRUE(open_or_die(tdb.url()));
  auto db = raw(tdb.url());
  ASSERT_TRUE(db);
  const std::vector<std::string> plan = column(*db, QStringLiteral("EXPLAIN QUERY PLAN ") + pd::sql::kPrincipalInvestigatorByOrcid,
                                               "detail", {QStringLiteral("0000-0002-1825-0097")});
  ASSERT_FALSE(plan.empty());
  std::string all;
  for (const auto& step : plan) all += step + "\n";
  EXPECT_NE(all.find("principal_investigator_orcid"), std::string::npos) << all;
  EXPECT_EQ(all.find("SCAN"), std::string::npos) << all;
}

// The age sink reads the cached fingerprints of a batch before it writes:
// each analysis is a seek on derived_value's primary key (analysis_uuid,
// fingerprint, name), not a walk of every cached age.
TEST(SchemaIndexes, CachedFingerprintsAreFoundByAnalysis) {
  TestDatabase tdb("sqlite", true);
  ASSERT_TRUE(open_or_die(tdb.url()));
  auto db = raw(tdb.url());
  ASSERT_TRUE(db);
  const pd::Bindings ids{pd::qv(Uuid::v7()), pd::qv(Uuid::v7()), pd::qv(Uuid::v7())};
  const std::vector<std::string> plan =
      column(*db, QStringLiteral("EXPLAIN QUERY PLAN ") + pd::sql::derived_fingerprints(3), "detail", ids);
  ASSERT_FALSE(plan.empty());
  std::string all;
  for (const auto& step : plan) all += step + "\n";
  EXPECT_NE(all.find("sqlite_autoindex_derived_value_1"), std::string::npos) << all;
  EXPECT_NE(all.find("analysis_uuid=?"), std::string::npos) << all;
  EXPECT_EQ(all.find("derived_value_name_ix"), std::string::npos) << "found by name, across every analysis:\n" << all;
  EXPECT_EQ(all.find("SCAN"), std::string::npos) << all;
}

// The experiment window's References dock reads, after every saved run, the
// newest analyses of some types on the session's spectrometer since a time,
// leaving out the invalid ones (browse.cpp composes the statement this way).
// The index (mass_spectrometer_uuid, analysis_type, timestamp_utc) cannot give
// time order across several types, so two shapes are good: a search on it, or
// a walk of analysis_ts_ix that stops at the limit. A scan of analysis with no
// index, or a sort of every match in a temp tree, is what a lab pays per run.
namespace {

BrowseFilter reference_browse(const std::vector<std::string>& types) {
  BrowseFilter f;
  f.analysis_types = types;
  f.mass_spectrometers = {"jan"};
  f.from = *UtcTime::parse("2026-10-01T00:00:00Z");
  f.exclude_tags = {"invalid"};
  return f;
}

std::string plan_of(pd::Db& db, const QString& sql, const pd::Bindings& bindings) {
  std::string all;
  for (const auto& step : column(db, QStringLiteral("EXPLAIN QUERY PLAN ") + sql, "detail", bindings)) all += step + "\n";
  return all;
}

std::string plan_of(pd::Db& db, const Result<pd::BrowseStatement>& st) {
  EXPECT_TRUE(st) << (st ? "" : to_string(st.error()));
  return st ? plan_of(db, st->sql, st->bindings) : std::string{};
}

Result<pd::BrowseStatement> page_statement(pd::Db& db, const BrowseFilter& f, int limit = 200) {
  return pd::browse_page_statement(db, Dialect::Sqlite, BrowseRequest{f, limit, std::nullopt, true});
}

// A step that reads `analysis a` whole, in no index's order.
bool scans_analysis_without_an_index(const std::string& plan) {
  std::size_t at = 0;
  while ((at = plan.find("SCAN a", at)) != std::string::npos) {
    const std::size_t end = plan.find('\n', at);
    if (plan.substr(at, end - at).find("USING") == std::string::npos) return true;
    at = end;
  }
  return false;
}

const std::vector<std::string> kBlankTypes{"blank_unknown", "blank_air", "blank_cocktail", "blank_extraction_line",
                                           "blank_laser"};

}  // namespace

TEST(SchemaIndexes, TheReferenceSeriesBrowseUsesAnIndexAndDoesNotSortInATempTree) {
  TestDatabase tdb("sqlite", true);
  ASSERT_TRUE(open_or_die(tdb.url()));
  auto db = raw(tdb.url());
  ASSERT_TRUE(db);
  for (const auto& types : {kBlankTypes, std::vector<std::string>{"air"}}) {
    const std::string plan = plan_of(*db, page_statement(*db, reference_browse(types), 50));
    ASSERT_FALSE(plan.empty());
    const bool by_type = plan.find("analysis_ms_type_ts_ix") != std::string::npos;
    const bool by_time = plan.find("analysis_ts_ix") != std::string::npos;
    EXPECT_TRUE(by_type || by_time) << types.size() << " type(s):\n" << plan;
    EXPECT_FALSE(scans_analysis_without_an_index(plan)) << types.size() << " type(s):\n" << plan;
    // In time order off analysis_ts_ix nothing is sorted; found by type, only the matches are.
    if (by_time) {
      EXPECT_EQ(plan.find("TEMP B-TREE FOR ORDER BY"), std::string::npos) << types.size() << " type(s):\n" << plan;
    }
  }
}

TEST(SchemaIndexes, TheReferenceSeriesCountDoesNotScanAnalysis) {
  TestDatabase tdb("sqlite", true);
  ASSERT_TRUE(open_or_die(tdb.url()));
  auto db = raw(tdb.url());
  ASSERT_TRUE(db);
  for (const auto& types : {kBlankTypes, std::vector<std::string>{"air"}}) {
    const std::string plan = plan_of(*db, pd::browse_count_statement(*db, Dialect::Sqlite, reference_browse(types)));
    ASSERT_FALSE(plan.empty());
    EXPECT_FALSE(scans_analysis_without_an_index(plan)) << types.size() << " type(s):\n" << plan;
  }
}

// A browse page shows each row's cached age: one probe of derived_value's
// primary key (analysis_uuid, fingerprint, name) per row. Found by
// derived_value_name_ix instead, every cached age of the store was read for
// each row of the page (0.5 s a page on 8.7 thousand analyses, against 1 ms).
TEST(SchemaIndexes, ABrowsePageFindsItsAgesByPrimaryKey) {
  TestDatabase tdb("sqlite", true);
  ASSERT_TRUE(open_or_die(tdb.url()));
  auto db = raw(tdb.url());
  ASSERT_TRUE(db);
  BrowseFilter valid;
  valid.exclude_tags = {"invalid"};
  for (const auto& f : {BrowseFilter{}, valid, reference_browse(kBlankTypes)}) {
    const std::string plan = plan_of(*db, page_statement(*db, f));
    ASSERT_FALSE(plan.empty());
    EXPECT_NE(plan.find("SEARCH dv USING INDEX sqlite_autoindex_derived_value_1 (analysis_uuid=?"), std::string::npos)
        << plan;
    EXPECT_EQ(plan.find("derived_value_name_ix"), std::string::npos) << "found by name, across every analysis:\n" << plan;
    EXPECT_EQ(plan.find("SCAN dv"), std::string::npos) << plan;
    EXPECT_FALSE(scans_analysis_without_an_index(plan)) << plan;
  }
  // An analysis's detail reads its age the same way.
  const std::string detail = plan_of(*db,
                                     pd::sql::kBrowseSelect.arg(QStringLiteral("a.timestamp_utc")) + pd::sql::kBrowseFrom +
                                         pd::sql::kBrowseAgeJoin + QStringLiteral("WHERE a.uuid = ?"),
                                     {pd::qv(Uuid::v7())});
  EXPECT_NE(detail.find("sqlite_autoindex_derived_value_1"), std::string::npos) << detail;
  EXPECT_EQ(detail.find("SCAN"), std::string::npos) << detail;
}

// A count or a facet runs over every analysis that matches: with no age bound
// it does not join the cache at all, so a filter that is off costs nothing.
TEST(SchemaIndexes, ACountWithoutAnAgeBoundLeavesTheCacheAlone) {
  TestDatabase tdb("sqlite", true);
  ASSERT_TRUE(open_or_die(tdb.url()));
  auto db = raw(tdb.url());
  ASSERT_TRUE(db);
  for (const auto& f : {BrowseFilter{}, reference_browse(kBlankTypes)}) {
    auto count = pd::browse_count_statement(*db, Dialect::Sqlite, f);
    ASSERT_TRUE(count) << to_string(count.error());
    EXPECT_FALSE(count->sql.contains(QStringLiteral("derived_value"))) << count->sql.toStdString();
    for (BrowseFacet facet : {BrowseFacet::AnalysisType, BrowseFacet::Sample, BrowseFacet::Identifier,
                              BrowseFacet::Repository}) {
      auto st = pd::facet_statement(*db, Dialect::Sqlite, facet, f);
      ASSERT_TRUE(st) << to_string(st.error());
      EXPECT_FALSE(st->sql.contains(QStringLiteral("derived_value"))) << st->sql.toStdString();
    }
  }
}

// Under an age bound the bound is a range on derived_value_name_ix (name,
// value), which drives the statement; each match finds its analysis by primary
// key. Neither table is read whole: not the count, not a facet, not the page.
TEST(SchemaIndexes, ACountWithAnAgeBoundScansNeitherTable) {
  TestDatabase tdb("sqlite", true);
  ASSERT_TRUE(open_or_die(tdb.url()));
  auto db = raw(tdb.url());
  ASSERT_TRUE(db);
  BrowseFilter both;
  both.age_min = 25.0;
  both.age_max = 30.0;
  both.exclude_tags = {"invalid"};
  BrowseFilter low;
  low.age_min = 25.0;
  BrowseFilter high;
  high.age_max = 30.0;
  auto scans_whole = [](const std::string& plan) {
    return plan.find("SCAN a\n") != std::string::npos || plan.find("SCAN a ") != std::string::npos ||
           plan.find("SCAN dv") != std::string::npos;
  };
  for (const auto& f : {both, low, high}) {
    std::vector<std::pair<std::string, std::string>> plans;
    plans.emplace_back("count", plan_of(*db, pd::browse_count_statement(*db, Dialect::Sqlite, f)));
    plans.emplace_back("page", plan_of(*db, page_statement(*db, f)));
    plans.emplace_back("sample facet", plan_of(*db, pd::facet_statement(*db, Dialect::Sqlite, BrowseFacet::Sample, f)));
    for (const auto& [what, plan] : plans) {
      ASSERT_FALSE(plan.empty()) << what;
      EXPECT_NE(plan.find("SEARCH dv USING INDEX derived_value_name_ix (name=? AND value"), std::string::npos)
          << what << ":\n" << plan;
      EXPECT_NE(plan.find("SEARCH a USING INDEX sqlite_autoindex_analysis_1 (uuid=?)"), std::string::npos)
          << what << ":\n" << plan;
      EXPECT_FALSE(scans_whole(plan)) << what << ":\n" << plan;
    }
  }
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
    if (!b.contains(x)) ADD_FAILURE() << "only in PostgreSQL: " << x;
  for (const auto& x : b)
    if (!a.contains(x)) ADD_FAILURE() << "only in SQLite: " << x;
}

INSTANTIATE_TEST_SUITE_P(Engines, SchemaTest, ::testing::ValuesIn(engines()),
                         [](const auto& p) { return p.param; });
