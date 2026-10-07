#pragma once

// A temp-file SQLite store with a reduction client and user, for the flux
// store tests, and (seed_level) the seeded level of flux_seed.hpp with a
// StoreSource over the same file. SQLite only: the persistence tests' engine
// fixture needs Qt, which the processing tests do not link.

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <memory>
#include <random>
#include <string>

#include "flux_seed.hpp"
#include "pychron/persistence/store.hpp"
#include "pychron/processing/store_source.hpp"

namespace pychron::processing::testing {

class FluxStoreTest : public ::testing::Test {
 protected:
  void SetUp() override {
    static std::atomic<int> counter{0};
    path_ = std::filesystem::temp_directory_path() /
            ("pychron_flux_store_" + std::to_string(std::random_device{}() % 1000000) + "_" +
             std::to_string(counter.fetch_add(1)) + ".sqlite");
    std::filesystem::remove(path_);
    url_ = "sqlite:" + path_.string();
    auto store = persistence::open_store(persistence::StoreConfig{url_, true});
    ASSERT_TRUE(store) << to_string(store.error());
    store_ = std::move(*store);
    auto client = store_->register_client({"red-1", "reduction", std::nullopt, "test"});
    ASSERT_TRUE(client) << to_string(client.error());
    auto user = store_->ensure_user(*client, "jsmith");
    ASSERT_TRUE(user) << to_string(user.error());
    actor_ = persistence::Actor{*user, *client};
  }

  void TearDown() override {
    source_.reset();
    store_.reset();
    std::error_code ec;
    for (const char* suffix : {"", "-wal", "-shm"}) std::filesystem::remove(path_.string() + suffix, ec);
  }

  persistence::IStore& store() { return *store_; }
  const persistence::Actor& actor() const { return actor_; }
  const std::string& url() const { return url_; }

  // NM-300 level A (flux_seed.hpp). Call once, before source(). Only the
  // first `analysed` monitor holes get their analyses.
  void seed_level(int analysed = 8) {
    auto seeded = seed_flux_level(*store_, actor_, "FC-2", analysed);
    ASSERT_TRUE(seeded) << to_string(seeded.error());
    level_ = std::move(*seeded);
  }

  // The three analyses of a monitor hole seeded without them; an open
  // source sees them.
  void add_monitor_analyses(int hole) {
    auto added = seed_monitor_analyses(*store_, level_, hole);
    ASSERT_TRUE(added) << to_string(added.error());
    if (source_) {
      auto refreshed = source_->refresh();
      EXPECT_TRUE(refreshed) << (refreshed ? "" : to_string(refreshed.error()));
    }
  }
  const SeededLevel& seeded() const { return level_; }

  // A flux_position revision of a hole; the revision's uuid.
  persistence::Uuid save_flux(int hole, persistence::FluxValue value) {
    auto revision = seed_save_flux(*store_, actor_, level_, hole, std::move(value));
    EXPECT_TRUE(revision) << (revision ? "" : to_string(revision.error()));
    return revision ? *revision : persistence::Uuid{};
  }

  void tag(const std::string& record_id, const std::string& name) {
    auto tagged = seed_tag(*store_, actor_, level_, record_id, name);
    EXPECT_TRUE(tagged) << (tagged ? "" : to_string(tagged.error()));
  }

  void publish_holder(std::vector<persistence::HolderHole> holes) {
    auto revision = seed_publish_holder(*store_, actor_, level_, std::move(holes));
    EXPECT_TRUE(revision) << (revision ? "" : to_string(revision.error()));
  }

  // Opened on first use, over the same file: it sees what was written before.
  // nullptr (and a failure) when it cannot be opened.
  StoreSource* source() {
    if (!source_) {
      auto s = StoreSource::open(persistence::StoreConfig{url_, false}, StoreSourceOptions{2, "tester", "test-host"});
      EXPECT_TRUE(s) << (s ? "" : to_string(s.error()));
      if (s) source_ = std::move(*s);
    }
    return source_.get();
  }

 private:
  std::filesystem::path path_;
  std::string url_;
  std::unique_ptr<persistence::IStore> store_;
  persistence::Actor actor_;
  SeededLevel level_;
  std::unique_ptr<StoreSource> source_;
};

}  // namespace pychron::processing::testing
