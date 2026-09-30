#include "pychron/systems/spectrometer/field_table_store.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <random>

namespace fs = std::filesystem;
namespace ps = pychron::spectrometer;
using pychron::ErrorKind;
using namespace std::chrono_literals;

namespace {

class FieldTableStoreTest : public ::testing::Test {
 protected:
  void SetUp() override {
    std::random_device rd;
    root_ = fs::temp_directory_path() / ("pychron_ftstore_" + std::to_string(rd()));
    fs::create_directories(root_);
  }
  void TearDown() override {
    std::error_code ec;
    fs::remove_all(root_, ec);
  }

  static ps::FieldTable table(double h1) {
    return ps::FieldTable(ps::FitKind::Discrete, ps::TableAxis::Dac,
                          {{"Ar40", 39.962383, {{"H1", h1}, {"AX", h1 - 0.1}}}, {"Ar36", 35.967545, {{"H1", 4.5}}}});
  }

  // 2026-09-30T12:00:00Z
  static ps::FieldTableStore::WallTime t0() { return ps::FieldTableStore::WallTime{} + 1790769600s; }

  fs::path root_;
};

}  // namespace

TEST(FieldTableVersion, FormatsUtcTimestamp) {
  EXPECT_EQ(ps::format_version(ps::FieldTableStore::WallTime{} + 1790769600s), "20260930T120000Z");
}

TEST_F(FieldTableStoreTest, EmptyStoreHasNoCurrent) {
  ps::FieldTableStore store(root_, "argon");
  auto v = store.versions();
  ASSERT_TRUE(v.has_value());
  EXPECT_TRUE(v->empty());
  EXPECT_FALSE(store.current_version().has_value());
  EXPECT_FALSE(store.load().has_value());
}

TEST_F(FieldTableStoreTest, SaveWritesVersionAndCurrentPointer) {
  ps::FieldTableStore store(root_, "argon");
  auto v = store.save(table(5.0), t0());
  ASSERT_TRUE(v.has_value()) << pychron::to_string(v.error());
  EXPECT_EQ(*v, "20260930T120000Z");
  EXPECT_TRUE(fs::exists(root_ / "argon" / "20260930T120000Z.toml"));
  EXPECT_TRUE(fs::exists(root_ / "argon" / "current"));
  EXPECT_EQ(*store.current_version(), *v);
  auto loaded = store.load();
  ASSERT_TRUE(loaded.has_value());
  EXPECT_EQ(*loaded, table(5.0));
}

TEST_F(FieldTableStoreTest, VersionsAreChronologicalAndCollisionsSuffixed) {
  ps::FieldTableStore store(root_, "argon");
  auto a = store.save(table(5.0), t0());
  auto b = store.save(table(5.1), t0());  // same second
  auto c = store.save(table(5.2), t0() + 60s);
  ASSERT_TRUE(a && b && c);
  EXPECT_NE(*a, *b);
  EXPECT_EQ(*b, "20260930T120000Z-1");
  EXPECT_EQ(*store.versions(), (std::vector<std::string>{*a, *b, *c}));
  EXPECT_EQ(*store.current_version(), *c);
  EXPECT_DOUBLE_EQ(store.load(*a)->points()[0].values.at("H1"), 5.0);
}

TEST_F(FieldTableStoreTest, RestoreMovesCurrentOnly) {
  ps::FieldTableStore store(root_, "argon");
  auto a = store.save(table(5.0), t0());
  auto b = store.save(table(5.1), t0() + 1s);
  ASSERT_TRUE(a && b);
  ASSERT_TRUE(store.restore(*a).has_value());
  EXPECT_EQ(*store.current_version(), *a);
  EXPECT_DOUBLE_EQ(store.load()->points()[0].values.at("H1"), 5.0);
  EXPECT_EQ(store.versions()->size(), 2u);  // nothing deleted
}

TEST_F(FieldTableStoreTest, RestoreUnknownVersionFailsAndKeepsCurrent) {
  ps::FieldTableStore store(root_, "argon");
  auto a = store.save(table(5.0), t0());
  ASSERT_TRUE(a.has_value());
  auto r = store.restore("19990101T000000Z");
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().kind, ErrorKind::Config);
  EXPECT_EQ(*store.current_version(), *a);
}

TEST_F(FieldTableStoreTest, RestoreRejectsUnparseableVersion) {
  ps::FieldTableStore store(root_, "argon");
  auto a = store.save(table(5.0), t0());
  ASSERT_TRUE(a.has_value());
  std::ofstream(root_ / "argon" / "20200101T000000Z.toml") << "fit = [";
  EXPECT_FALSE(store.restore("20200101T000000Z").has_value());
  EXPECT_EQ(*store.current_version(), *a);
}

TEST_F(FieldTableStoreTest, RestoreRejectsPathTraversal) {
  ps::FieldTableStore store(root_, "argon");
  ASSERT_TRUE(store.save(table(5.0), t0()).has_value());
  EXPECT_FALSE(store.restore("../other").has_value());
  EXPECT_FALSE(store.load("../../etc/passwd").has_value());
}

TEST_F(FieldTableStoreTest, NamedTablesAreIndependent) {
  ps::FieldTableStore argon(root_, "argon");
  ps::FieldTableStore ic(root_, "ic");
  ASSERT_TRUE(argon.save(table(5.0), t0()).has_value());
  ASSERT_TRUE(ic.save(table(6.0), t0()).has_value());
  EXPECT_DOUBLE_EQ(argon.load()->points()[0].values.at("H1"), 5.0);
  EXPECT_DOUBLE_EQ(ic.load()->points()[0].values.at("H1"), 6.0);
}
