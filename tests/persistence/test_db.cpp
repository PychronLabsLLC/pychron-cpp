// The TinyORM connection wrapper (src/tiny/db.cpp): error mapping and
// transaction safety when the server connection drops.

#include <gtest/gtest.h>

#include "store_fixture.hpp"

using namespace pychron;
using namespace pychron::persistence;
using namespace pychron::persistence::testing;
namespace pd = pychron::persistence::detail;

TEST(Db, RejectsUnknownUrls) {
  auto db = pd::Db::open(StoreConfig{"mysql://localhost/x", false});
  ASSERT_FALSE(db);
  EXPECT_EQ(db.error().kind, ErrorKind::Config);
  EXPECT_FALSE(pd::Db::open(StoreConfig{"sqlite:", false}));
}

TEST(Db, UnreachableServerIsNotConnected) {
  auto db = pd::Db::open(StoreConfig{"postgresql://nobody:x@127.0.0.1:1/none?connect_timeout=2", false});
  ASSERT_FALSE(db);
  EXPECT_EQ(db.error().kind, ErrorKind::NotConnected) << db.error().what;
}

TEST(Db, ConstraintViolationIsProtocol) {
  TestDatabase tdb("sqlite", false);
  auto db = std::move(*pd::Db::open(StoreConfig{tdb.url(), false}));
  ASSERT_TRUE(db->unprepared("CREATE TABLE t (x INTEGER PRIMARY KEY) STRICT"));
  ASSERT_TRUE(db->affecting("INSERT INTO t VALUES (1)"));
  auto dup = db->affecting("INSERT INTO t VALUES (1)");
  ASSERT_FALSE(dup);
  EXPECT_EQ(dup.error().kind, ErrorKind::Protocol) << dup.error().what;
  auto strict = db->affecting("INSERT INTO t VALUES ('not a number')");
  ASSERT_FALSE(strict);
  EXPECT_EQ(strict.error().kind, ErrorKind::Protocol) << strict.error().what;
}

TEST(Db, FailedCommitIsRolledBackAndTheConnectionStaysUsable) {
  TestDatabase tdb("sqlite", false);
  auto db = std::move(*pd::Db::open(StoreConfig{tdb.url(), false}));
  ASSERT_TRUE(db->unprepared("PRAGMA foreign_keys = ON"));
  ASSERT_TRUE(db->unprepared("CREATE TABLE p (x INTEGER PRIMARY KEY) STRICT"));
  ASSERT_TRUE(db->unprepared(
      "CREATE TABLE c (x INTEGER PRIMARY KEY, p INTEGER REFERENCES p DEFERRABLE INITIALLY DEFERRED) STRICT"));
  {
    pd::WriteTx tx(*db);
    ASSERT_TRUE(tx.begin());
    ASSERT_TRUE(db->affecting("INSERT INTO c VALUES (1, 42)"));  // checked at COMMIT
    auto r = tx.commit();
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().kind, ErrorKind::Protocol) << r.error().what;
  }
  pd::WriteTx tx(*db);
  ASSERT_TRUE(tx.begin()) << "the failed transaction must have been rolled back";
  ASSERT_TRUE(db->affecting("INSERT INTO p VALUES (42)"));
  ASSERT_TRUE(db->affecting("INSERT INTO c VALUES (1, 42)"));
  EXPECT_TRUE(tx.commit());
  EXPECT_EQ((*db->select_one("SELECT count(*) AS n FROM c"))->value("n").toInt(), 1);
}

TEST(Db, ConnectionLostInsideATransactionNeverCommitsHalfOfIt) {
  if (pg_url().empty()) GTEST_SKIP() << "PYCHRON_TEST_PG_URL not set";
  TestDatabase tdb("pg", false);
  auto admin = std::move(*pd::Db::open(StoreConfig{tdb.url(), false}));
  ASSERT_TRUE(admin->unprepared("CREATE TABLE t (x int PRIMARY KEY)"));

  auto db = std::move(*pd::Db::open(StoreConfig{tdb.url(), false}));
  auto pid = db->select_one("SELECT pg_backend_pid() AS pid");
  ASSERT_TRUE(pid && *pid);
  {
    pd::WriteTx tx(*db);
    ASSERT_TRUE(tx.begin());
    ASSERT_TRUE(db->affecting("INSERT INTO t VALUES (1)"));
    ASSERT_TRUE(admin->select("SELECT pg_terminate_backend(?)", {(*pid)->value("pid")}));
    // Without transaction tracking TinyORM would reconnect and run this in
    // autocommit on a new session.
    auto second = db->affecting("INSERT INTO t VALUES (2)");
    ASSERT_FALSE(second);
    EXPECT_EQ(second.error().kind, ErrorKind::NotConnected) << second.error().what;
    EXPECT_FALSE(tx.commit());
  }
  auto rows = admin->select_one("SELECT count(*) AS n FROM t");
  ASSERT_TRUE(rows && *rows);
  EXPECT_EQ((*rows)->value("n").toInt(), 0);

  // Outside a transaction the wrapper reconnects and carries on.
  auto again = db->affecting("INSERT INTO t VALUES (3)");
  EXPECT_TRUE(again) << (again ? "" : again.error().what);
  pd::WriteTx tx(*db);
  ASSERT_TRUE(tx.begin());
  ASSERT_TRUE(db->affecting("INSERT INTO t VALUES (4)"));
  EXPECT_TRUE(tx.commit());
}
