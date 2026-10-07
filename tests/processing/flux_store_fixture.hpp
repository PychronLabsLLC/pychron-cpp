#pragma once

// A temp-file SQLite store with a reduction client and user, for the flux
// monitor set tests. SQLite only: the persistence tests' engine fixture needs
// Qt, which the processing tests do not link.

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <memory>
#include <random>
#include <string>

#include "pychron/persistence/store.hpp"

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
    store_.reset();
    std::error_code ec;
    for (const char* suffix : {"", "-wal", "-shm"}) std::filesystem::remove(path_.string() + suffix, ec);
  }

  persistence::IStore& store() { return *store_; }
  const persistence::Actor& actor() const { return actor_; }
  const std::string& url() const { return url_; }

 private:
  std::filesystem::path path_;
  std::string url_;
  std::unique_ptr<persistence::IStore> store_;
  persistence::Actor actor_;
};

}  // namespace pychron::processing::testing
