// The password stores (secret_store.hpp). The memory store is tested as it
// is. The system's store (the keychain here on a Mac) is tested only when
// asked for, with PYCHRON_TEST_SECRET_STORE=1: it writes to the credential
// store of whoever runs the test, and a locked keychain answers with a
// dialog. It uses a service name of its own, made up for the run, and takes
// away what it put there. The account and the passwords are made up.

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <gtest/gtest.h>

#include "pychron/core/env.hpp"
#include "pychron/core/error.hpp"
#include "pychron/earthbank/profiles.hpp"
#include "pychron/earthbank/secret_store.hpp"
#include "secret_store_detail.hpp"

namespace pychron::earthbank {
namespace {

constexpr std::string_view kAccount = "fixture@earthbank.test";

bool has(std::string_view text, std::string_view part) { return text.find(part) != std::string_view::npos; }

std::string random_word() {
  std::random_device device;
  static constexpr std::string_view kHex = "0123456789abcdef";
  std::string word;
  for (int i = 0; i < 4; ++i) {
    unsigned int bits = device();
    for (int j = 0; j < 4; ++j, bits >>= 4) word.push_back(kHex[bits & 0xF]);
  }
  return word;
}

// set, get, overwrite, erase: what every store does.
void round_trip(ISecretStore& store, std::string_view account) {
  auto got = store.get(account);
  ASSERT_TRUE(got.has_value()) << to_string(got.error());
  EXPECT_EQ(*got, std::nullopt);

  auto set = store.set(account, "made-up one");
  ASSERT_TRUE(set.has_value()) << to_string(set.error());
  got = store.get(account);
  ASSERT_TRUE(got.has_value()) << to_string(got.error());
  EXPECT_EQ(*got, "made-up one");

  // Not ASCII, and longer: bytes in are the bytes out.
  const std::string second = "made-up tw\xC3\xB6, with a longer tail \\ \" '";
  set = store.set(account, second);
  ASSERT_TRUE(set.has_value()) << to_string(set.error());
  got = store.get(account);
  ASSERT_TRUE(got.has_value()) << to_string(got.error());
  EXPECT_EQ(*got, second);

  // Another account under the same service is another password.
  const std::string other = std::string(account) + ".other";
  got = store.get(other);
  ASSERT_TRUE(got.has_value()) << to_string(got.error());
  EXPECT_EQ(*got, std::nullopt);

  auto erased = store.erase(account);
  ASSERT_TRUE(erased.has_value()) << to_string(erased.error());
  got = store.get(account);
  ASSERT_TRUE(got.has_value()) << to_string(got.error());
  EXPECT_EQ(*got, std::nullopt);
  // Erasing what is not there is not an error.
  erased = store.erase(account);
  EXPECT_TRUE(erased.has_value()) << (erased ? "" : to_string(erased.error()));
}

// What no store takes: nothing is stored, and the message has no password.
void expect_refusals(ISecretStore& store) {
  for (const auto& refused : {store.set("", "made-up one"), store.set(kAccount, ""),
                              store.set(kAccount, std::string_view("made-up\0one", 11))}) {
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().kind, ErrorKind::Config);
    EXPECT_TRUE(has(refused.error().what, store.backend())) << refused.error().what;
    EXPECT_FALSE(has(refused.error().what, "made-up")) << refused.error().what;
  }
  EXPECT_FALSE(store.get("").has_value());
  EXPECT_FALSE(store.erase("").has_value());
}

TEST(SecretStore, MemoryRoundTrip) {
  const auto store = make_memory_secret_store();
  ASSERT_NE(store, nullptr);
  EXPECT_EQ(store->backend(), "memory");
  EXPECT_FALSE(store->persistent());
  round_trip(*store, kAccount);
  expect_refusals(*store);
  const auto nothing = store->get(kAccount);
  ASSERT_TRUE(nothing.has_value());
  EXPECT_EQ(*nothing, std::nullopt);
}

TEST(SecretStore, AMemoryStoreOfItsOwnSharesNothing) {
  const auto one = make_memory_secret_store();
  const auto two = make_memory_secret_store();
  ASSERT_TRUE(one->set(kAccount, "made-up one").has_value());
  EXPECT_EQ(two->get(kAccount).value_or(std::string("?")), std::nullopt);
}

// What make_secret_store falls back to: one store a service for the process,
// so that a password typed once is there for the rest of the session.
TEST(SecretStore, TheProcessMemoryStoreIsOneAService) {
  const std::string service = "pychron.earthbank.test." + random_word();
  {
    const auto first = detail::process_memory_secret_store(service);
    EXPECT_EQ(first->backend(), "memory");
    EXPECT_FALSE(first->persistent());
    ASSERT_TRUE(first->set(kAccount, "made-up one").has_value());
  }
  const auto again = detail::process_memory_secret_store(service);
  EXPECT_EQ(again->get(kAccount).value_or(std::nullopt), "made-up one");
  const auto elsewhere = detail::process_memory_secret_store(service + ".other");
  EXPECT_EQ(elsewhere->get(kAccount).value_or(std::string("?")), std::nullopt);
  ASSERT_TRUE(again->erase(kAccount).has_value());
}

TEST(SecretStore, TheBuildSaysWhichStoreItHas) {
  const std::set<std::string_view> names{"keychain", "credential-manager", "libsecret", "memory"};
  EXPECT_TRUE(names.contains(built_in_secret_store())) << built_in_secret_store();
  // A build may be made with none (PYCHRON_EARTHBANK_SYSTEM_SECRET_STORE=OFF);
  // with one, it is the platform's.
  if (built_in_secret_store() == "memory") return;
#if defined(__APPLE__)
  EXPECT_EQ(built_in_secret_store(), "keychain");
#elif defined(_WIN32)
  EXPECT_EQ(built_in_secret_store(), "credential-manager");
#else
  EXPECT_EQ(built_in_secret_store(), "libsecret");
#endif
}

TEST(SecretStore, AccountNameIsProfileAtHost) {
  Profile p;
  p.name = "mylab";
  p.base_url = "https://earthbank.test";
  EXPECT_EQ(secret_account(p), "mylab@earthbank.test");
  // The host in lower case; the profile's name as it is.
  p.name = "My-Lab_2";
  p.base_url = "https://EarthBank.Test";
  EXPECT_EQ(secret_account(p), "My-Lab_2@earthbank.test");
  // A port that is not the scheme's own is part of it.
  p.name = "dev";
  p.base_url = "https://earthbank.test:8443";
  EXPECT_EQ(secret_account(p), "dev@earthbank.test:8443");
  p.base_url = "https://earthbank.test:443";
  EXPECT_EQ(secret_account(p), "dev@earthbank.test");
  p.base_url = "http://localhost:8080";
  EXPECT_EQ(secret_account(p), "dev@localhost:8080");
  p.base_url = "http://127.0.0.1:80";
  EXPECT_EQ(secret_account(p), "dev@127.0.0.1");
  p.base_url = "http://[::1]:5000";
  EXPECT_EQ(secret_account(p), "dev@[::1]:5000");
  // A slash at the end, as a file may have been given it, changes nothing.
  p.base_url = "https://earthbank.test/";
  EXPECT_EQ(secret_account(p), "dev@earthbank.test");
  // The default profile is on the service, whose name is not spelled here.
  Profile fresh;
  fresh.name = "mylab";
  EXPECT_EQ(secret_account(fresh), "mylab@" + std::string(kDefaultBaseUrl.substr(kDefaultBaseUrl.find("://") + 3)));
  // Two profiles on one server have two accounts.
  Profile other = p;
  other.name = "other";
  EXPECT_NE(secret_account(p), secret_account(other));
}

// Ends the process when the system's store has not answered in time: a
// locked keychain asks with a dialog, and nobody may be there to answer.
class DeadMan {
 public:
  explicit DeadMan(std::string service) : thread_([this, service = std::move(service)] {
    std::unique_lock lock(mutex_);
    if (done_.wait_for(lock, std::chrono::seconds(20), [this] { return finished_; })) return;
    std::fprintf(stderr,
                 "SecretStore.PlatformRoundTrip: the system's store did not answer in 20 s (is it asking for "
                 "something?). Aborting. An item of service '%s' may be left behind.\n",
                 service.c_str());
    std::abort();
  }) {}
  ~DeadMan() {
    {
      const std::lock_guard lock(mutex_);
      finished_ = true;
    }
    done_.notify_all();
    thread_.join();
  }

 private:
  std::mutex mutex_;
  std::condition_variable done_;
  bool finished_ = false;
  std::thread thread_;
};

TEST(SecretStore, PlatformRoundTrip) {
  if (env_var("PYCHRON_TEST_SECRET_STORE").value_or("") != "1")
    GTEST_SKIP() << "not asked for: set PYCHRON_TEST_SECRET_STORE=1 to test the system's store (built in: "
                 << built_in_secret_store()
                 << "). It writes one item under a made-up service name to your own credential store and removes it.";
  if (env_var("CI").has_value()) GTEST_SKIP() << "CI is set: there is no session with a credential store to use";

  const std::string service = "pychron.earthbank.test." + random_word();
  const DeadMan dead_man(service);
  const auto store = make_secret_store(service);
  ASSERT_NE(store, nullptr);
  if (built_in_secret_store() == "memory") GTEST_SKIP() << "this build has no system store to test";
  if (store->backend() == "memory")
    GTEST_SKIP() << "the " << built_in_secret_store() << " store cannot be used in this session";
  EXPECT_EQ(store->backend(), built_in_secret_store());
  EXPECT_TRUE(store->persistent());
  std::printf("testing the %s store with service %s\n", store->backend().c_str(), service.c_str());

  // Whatever fails below, nothing of this service stays in the store.
  struct Eraser {
    ISecretStore& store;
    ~Eraser() {
      (void)store.erase(kAccount);
      (void)store.erase(std::string(kAccount) + ".other");
    }
  } const eraser{*store};

  round_trip(*store, kAccount);
  expect_refusals(*store);

  // It is the system that holds it: a second store of the same service sees
  // what the first set, and one of another service does not.
  ASSERT_TRUE(store->set(kAccount, "made-up three").has_value());
  const auto second = make_secret_store(service);
  EXPECT_EQ(second->get(kAccount).value_or(std::nullopt), "made-up three");
  const auto elsewhere = make_secret_store(service + ".other");
  EXPECT_EQ(elsewhere->get(kAccount).value_or(std::string("?")), std::nullopt);
  ASSERT_TRUE(second->erase(kAccount).has_value());
  EXPECT_EQ(store->get(kAccount).value_or(std::string("?")), std::nullopt);
}

}  // namespace
}  // namespace pychron::earthbank
