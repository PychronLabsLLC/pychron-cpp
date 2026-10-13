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
#include <string>

#include <QSqlDatabase>
#include <QSqlQuery>
#include <QString>

#include "pychron/persistence/store.hpp"

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

TEST_F(DbCmd, UsageErrors) {
  EXPECT_EQ(run_raw({"db"}).code, elctl::kUsage);
  EXPECT_EQ(run_raw({"db", "status"}).code, elctl::kUsage);
  EXPECT_EQ(run_raw({"db", "vacuum", "--db", db_}).code, elctl::kUsage);
  EXPECT_EQ(run_raw({"db", "status", "--db", db_, "--force"}).code, elctl::kUsage);
  EXPECT_EQ(run_raw({"db", "help"}).code, elctl::kOk);
}

#endif
