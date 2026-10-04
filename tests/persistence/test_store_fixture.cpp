// The test fixture's own promises (store_fixture.hpp): database names that
// parallel test processes cannot share, and no file left behind or inherited.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <set>
#include <string>

#include <QCoreApplication>

#include "store_fixture.hpp"

using pychron::persistence::testing::TestDatabase;

// ctest runs one process per test, so a per-process counter is always 0: the
// name must be unique across processes on its own.
TEST(StoreFixture, DatabaseNamesCarryThePidAndAFullWidthRandomValue) {
  const std::string pid = std::to_string(QCoreApplication::applicationPid());
  std::set<std::string> tags;
  for (int i = 0; i < 2000; ++i) {
    const std::string tag = TestDatabase::unique_tag();
    // "<pid>_<16 hex digits>_<counter>"
    ASSERT_EQ(tag.rfind(pid + "_", 0), 0u) << tag;
    const auto random = tag.substr(pid.size() + 1, 16);
    ASSERT_EQ(random.find_first_not_of("0123456789abcdef"), std::string::npos) << tag;
    ASSERT_EQ(tag[pid.size() + 17], '_') << tag;
    ASSERT_TRUE(tags.insert(tag).second) << tag;
  }
  // The random part differs from call to call, not only the counter.
  std::set<std::string> randoms;
  for (const auto& tag : tags) randoms.insert(tag.substr(pid.size() + 1, 16));
  EXPECT_GT(randoms.size(), 1990u);
}

TEST(StoreFixture, SqliteSideFilesGoWithTheMainFile) {
  const std::filesystem::path stale =
      std::filesystem::temp_directory_path() / ("pychron_store_" + TestDatabase::unique_tag() + ".sqlite");
  for (const char* suffix : {"", "-wal", "-shm"}) std::ofstream(stale.string() + suffix) << "stale";
  TestDatabase::remove_sqlite_files(stale);
  for (const char* suffix : {"", "-wal", "-shm"}) {
    EXPECT_FALSE(std::filesystem::exists(stale.string() + suffix)) << suffix;
  }

  // A file-backed database leaves nothing behind.
  std::string url;
  {
    TestDatabase database("sqlite", true);
    url = database.url();
    auto store = pychron::persistence::testing::open_or_die(url);
    ASSERT_TRUE(store);
    ASSERT_TRUE(store->register_client({"c", "importer", std::nullopt, "test"}));
  }
  const std::string file = url.substr(std::string("sqlite:").size());
  for (const char* suffix : {"", "-wal", "-shm"}) {
    EXPECT_FALSE(std::filesystem::exists(file + suffix)) << suffix;
  }
}
