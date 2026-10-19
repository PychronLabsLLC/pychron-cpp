// elctl db: whether a store's schema is current, and bringing one up to date.
#include <gtest/gtest.h>

#include "db.hpp"
#include "elctl_fixture.hpp"

using elctl::testing::contains;
using elctl::testing::Outcome;
using elctl::testing::run_raw;

#ifndef PYCHRON_ELCTL_HAS_STORE

TEST(DbCmd, StubWithoutPersistence) {
  const Outcome o = run_raw({"db", "status", "--db", "sqlite::memory:"});
  EXPECT_EQ(o.code, elctl::kUsage);
  EXPECT_TRUE(contains(o.err, "elctl was built without persistence")) << o.err;
}

#else

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include <QSqlDatabase>
#include <QSqlQuery>
#include <QString>

#include "pychron/persistence/store.hpp"
#include "store_fixture.hpp"

namespace {

namespace ps = pychron::persistence;
namespace fs = std::filesystem;

class DbCmd : public elctl::testing::ElctlTest {
 protected:
  void SetUp() override {
    ElctlTest::SetUp();
    file_ = path("store.db");
    db_ = "sqlite:" + file_.string();
    auto store = ps::open_store(ps::StoreConfig{db_, true});
    ASSERT_TRUE(store) << to_string(store.error());
  }

  // The store as an older pychron left it: without the last migration.
  void take_back_the_last_migration() {
    {
      QSqlDatabase d = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("db-cmd-test"));
      d.setDatabaseName(QString::fromStdString(file_.string()));
      ASSERT_TRUE(d.open());
      QSqlQuery q(d);
      for (const char* index : {"ref_object_position_ix", "ref_object_level_ix", "ref_object_irradiation_ix",
                                "ref_object_mass_spectrometer_ix"}) {
        ASSERT_TRUE(q.exec(QStringLiteral("DROP INDEX %1").arg(QString::fromLatin1(index))));
      }
      ASSERT_TRUE(q.exec(QStringLiteral("DELETE FROM schema_version WHERE version = 5")));
      d.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("db-cmd-test"));
  }

  fs::path file_;
  std::string db_;
};

}  // namespace

TEST_F(DbCmd, StatusOfACurrentStore) {
  const Outcome o = run_raw({"db", "status", "--db", db_});
  EXPECT_EQ(o.code, elctl::kOk) << o.err;
  EXPECT_TRUE(contains(o.out, "up to date")) << o.out;
  EXPECT_TRUE(contains(o.out, "0005  ref_scope_indexes")) << o.out;
  EXPECT_TRUE(contains(o.out, "0001  init")) << o.out;
}

TEST_F(DbCmd, StatusOfAStoreThatIsBehindSaysWhatToRun) {
  take_back_the_last_migration();
  const Outcome o = run_raw({"db", "status", "--db", db_});
  EXPECT_EQ(o.code, elctl::kFailed);
  EXPECT_TRUE(contains(o.out, "behind")) << o.out;
  EXPECT_TRUE(contains(o.out, "migration 5")) << o.out;
  EXPECT_TRUE(contains(o.out, "elctl db migrate")) << o.out;
  // Asking changed nothing.
  EXPECT_FALSE(ps::open_store(ps::StoreConfig{db_, false}));
}

TEST_F(DbCmd, MigrateBringsAStoreUpToDate) {
  take_back_the_last_migration();
  ASSERT_FALSE(ps::open_store(ps::StoreConfig{db_, false}));

  const Outcome o = run_raw({"db", "migrate", "--db", db_});
  EXPECT_EQ(o.code, elctl::kOk) << o.err;
  EXPECT_TRUE(contains(o.out, "migrated")) << o.out;
  EXPECT_TRUE(contains(o.out, "0005  ref_scope_indexes")) << o.out;
  EXPECT_TRUE(ps::open_store(ps::StoreConfig{db_, false}));  // what pychron-ui does

  const Outcome again = run_raw({"db", "migrate", "--db", db_});
  EXPECT_EQ(again.code, elctl::kOk);
  EXPECT_TRUE(contains(again.out, "up to date")) << again.out;
  EXPECT_FALSE(contains(again.out, "migrated")) << again.out;
}

TEST_F(DbCmd, NeitherMakesADatabase) {
  const fs::path nowhere = path("typo.db");
  for (const char* action : {"status", "migrate"}) {
    const Outcome o = run_raw({"db", action, "--db", "sqlite:" + nowhere.string()});
    EXPECT_EQ(o.code, elctl::kUsage) << action;
    EXPECT_TRUE(contains(o.err, "no database at")) << o.err;
    EXPECT_FALSE(fs::exists(nowhere)) << action;
  }
}

TEST_F(DbCmd, AnEmptyFileIsNotAStore) {
  const fs::path empty = path("empty.db");
  { std::ofstream make(empty); }
  const Outcome o = run_raw({"db", "migrate", "--db", "sqlite:" + empty.string()});
  EXPECT_EQ(o.code, elctl::kUsage);
  EXPECT_TRUE(contains(o.err, "not a pychron store")) << o.err;
  EXPECT_EQ(fs::file_size(empty), 0u);
}

// No --db and no install to use: the usage error says why there is none.
TEST_F(DbCmd, WithoutDbTheReasonThereIsNoInstallIsSaid) {
  const Outcome none = run_raw({"db", "status"});
  EXPECT_EQ(none.code, elctl::kUsage);
  EXPECT_TRUE(contains(none.err, "nothing is installed yet")) << none.err;

  // Two installs, neither the default.
  ASSERT_EQ(run_raw({"init", "data-reduction", "--root", path("a").string(), "--name", "a", "--yes"}).code, elctl::kOk);
  ASSERT_EQ(run_raw({"init", "data-reduction", "--root", path("b").string(), "--name", "b", "--yes"}).code, elctl::kOk);
  {
    std::ifstream in(path("site.toml"));
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const auto at = text.find("default = ");
    ASSERT_NE(at, std::string::npos) << text;
    text.erase(at, text.find('\n', at) - at + 1);
    std::ofstream(path("site.toml"), std::ios::trunc) << text;
  }
  const Outcome several = run_raw({"db", "status"});
  EXPECT_EQ(several.code, elctl::kUsage);
  EXPECT_TRUE(contains(several.err, "several installs and no default")) << several.err;
}

TEST_F(DbCmd, UsageErrors) {
  EXPECT_EQ(run_raw({"db"}).code, elctl::kUsage);
  EXPECT_EQ(run_raw({"db", "status"}).code, elctl::kUsage);
  EXPECT_EQ(run_raw({"db", "vacuum", "--db", db_}).code, elctl::kUsage);
  EXPECT_EQ(run_raw({"db", "status", "--db", db_, "--force"}).code, elctl::kUsage);
  EXPECT_EQ(run_raw({"db", "help"}).code, elctl::kOk);
}

// With --install and no --db: the install's database.
TEST_F(DbCmd, DbStatusUsesTheInstallsDatabase) {
  const fs::path root = path("dr");
  ASSERT_EQ(run_raw({"init", "data-reduction", "--root", root.string(), "--name", "dr", "--yes"}).code, elctl::kOk);
  const std::string url = "sqlite:" + (root / "data" / "pychron.db").generic_string();
  const Outcome by_url = run_raw({"db", "status", "--db", url});
  ASSERT_EQ(by_url.code, elctl::kOk) << by_url.err;
  const Outcome by_install = run_raw({"--install", "dr", "db", "status"});
  EXPECT_EQ(by_install.code, elctl::kOk) << by_install.err;
  EXPECT_EQ(by_install.out, by_url.out);
  // The default install, when --install is not given.
  const Outcome by_default = run_raw({"db", "status"});
  EXPECT_EQ(by_default.code, elctl::kOk) << by_default.err;
  EXPECT_EQ(by_default.out, by_url.out);
  // An install that is not there is an error, as for every command, not the default one.
  const Outcome nowhere = run_raw({"--install", "nowhere", "db", "status"});
  EXPECT_EQ(nowhere.code, elctl::kFailed);
  EXPECT_FALSE(contains(nowhere.out, "up to date")) << nowhere.out;
}

// elctl db derive, on the seeded store: two heating steps of an irradiated
// unknown, which have ages, and an air, which has none.
class DbDerive : public elctl::testing::StoreTest {};

TEST_F(DbDerive, FillsTheCacheAndSaysSo) {
  const Outcome o = run_raw({"db", "derive", "--db", db_});
  EXPECT_EQ(o.code, elctl::kOk) << o.err;
  EXPECT_TRUE(contains(o.out, "  3 seen, 3 written")) << o.out;  // the page's line
  EXPECT_TRUE(contains(o.out, "derived: 3 seen, 3 written (1 without an age), 0 current, 0 failed")) << o.out;
  EXPECT_TRUE(o.err.empty()) << o.err;

  const Outcome again = run_raw({"db", "derive", "--db", db_});
  EXPECT_EQ(again.code, elctl::kOk) << again.err;
  EXPECT_TRUE(contains(again.out, "derived: 3 seen, 0 written (0 without an age), 3 current, 0 failed")) << again.out;
}

TEST_F(DbDerive, AllWritesAgainAndQuietSaysOnlyTheSummary) {
  ASSERT_EQ(run_raw({"db", "derive", "--db", db_}).code, elctl::kOk);
  const Outcome o = run_raw({"db", "derive", "--db", db_, "--all", "--quiet"});
  EXPECT_EQ(o.code, elctl::kOk) << o.err;
  EXPECT_EQ(o.out, "derived: 3 seen, 3 written (1 without an age), 0 current, 0 failed\n");
}

TEST_F(DbDerive, TheLimitIsHowManyAreWalked) {
  const Outcome o = run_raw({"db", "derive", "--db", db_, "--limit", "1", "--quiet"});
  EXPECT_EQ(o.code, elctl::kOk) << o.err;
  EXPECT_TRUE(contains(o.out, "derived: 1 seen")) << o.out;
  for (const char* bad : {"0", "-2", "x", "1.5"})
    EXPECT_EQ(run_raw({"db", "derive", "--db", db_, "--limit", bad}).code, elctl::kUsage) << bad;
}

TEST_F(DbDerive, AStoreBehindItsMigrationsIsRefused) {
  store_.reset();
  {
    QSqlDatabase d = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("db-derive-test"));
    d.setDatabaseName(QString::fromStdString(path("store.db").string()));
    ASSERT_TRUE(d.open());
    QSqlQuery q(d);
    ASSERT_TRUE(q.exec(QStringLiteral("DELETE FROM schema_version WHERE version = 5")));
    d.close();
  }
  QSqlDatabase::removeDatabase(QStringLiteral("db-derive-test"));
  const Outcome o = run_raw({"db", "derive", "--db", db_});
  EXPECT_EQ(o.code, elctl::kUsage);
  EXPECT_TRUE(contains(o.err, "migration")) << o.err;
  EXPECT_FALSE(ps::open_store(ps::StoreConfig{db_, false}));  // still behind: derive never migrates
}

TEST_F(DbDerive, ItsOptionsAreOnlyDerives) {
  EXPECT_EQ(run_raw({"db", "status", "--db", db_, "--all"}).code, elctl::kUsage);
  EXPECT_EQ(run_raw({"db", "migrate", "--db", db_, "--limit", "3"}).code, elctl::kUsage);
}

#endif
