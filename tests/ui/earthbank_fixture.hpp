#pragma once

// What the EarthBank suites of the UI share: the service in memory behind a
// gate a test can hold requests at, a memory password store and a profiles
// file in a temp folder. Nothing here reaches EarthBank, the keychain or the
// user's own profiles.

#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

#include <QTemporaryDir>
#include <QtTest/QtTest>

#include "earthbank_worker.hpp"
#include "fake_earthbank.hpp"
#include "pychron/core/error.hpp"
#include "pychron/earthbank/http.hpp"
#include "pychron/earthbank/profiles.hpp"
#include "pychron/earthbank/secret_store.hpp"
#include "session_support.hpp"

namespace pychron::ui::test {

// Passes requests on. While held, a request (every one, or with hold_at only
// those of one method and path) waits here, on the worker's thread, until the
// test lets it go.
class Gate final : public earthbank::IHttpClient {
 public:
  explicit Gate(earthbank::IHttpClient& next) : next_(next) {}
  Result<earthbank::HttpResponse> send(const earthbank::HttpRequest& request) override {
    {
      std::unique_lock lock(mutex_);
      const bool mine = method_.empty() ||
                        (request.method == method_ && earthbank::url_path(request.url).starts_with(path_));
      if (held_ && mine) {
        ++waiting_;
        changed_.wait(lock, [this] { return !held_; });
        --waiting_;
      }
    }
    return next_.send(request);
  }
  void hold() { hold_at({}, {}); }
  void hold_at(std::string method, std::string path_prefix) {
    const std::scoped_lock lock(mutex_);
    held_ = true;
    method_ = std::move(method);
    path_ = std::move(path_prefix);
  }
  void release() {
    const std::scoped_lock lock(mutex_);
    held_ = false;
    changed_.notify_all();
  }
  bool waiting() {
    const std::scoped_lock lock(mutex_);
    return held_ && waiting_ > 0;
  }

 private:
  earthbank::IHttpClient& next_;
  std::mutex mutex_;
  std::condition_variable changed_;
  bool held_ = false;
  std::string method_, path_;
  int waiting_ = 0;
};

// The service, a spy that remembers every secret that passed, and the gate
// before them: shared with the worker's client, which may outlive a test that
// closes its window with a request still out.
struct EarthbankService {
  earthbank::testing::FakeEarthBank bank;
  earthbank::testing::Spy spy{bank};
  Gate gate{spy};
};

class Through final : public earthbank::IHttpClient {
 public:
  explicit Through(std::shared_ptr<EarthbankService> service) : service_(std::move(service)) {}
  Result<earthbank::HttpResponse> send(const earthbank::HttpRequest& request) override {
    return service_->gate.send(request);
  }

 private:
  std::shared_ptr<EarthbankService> service_;
};

// A service, a store and a folder of its own for each test.
struct EarthbankWorld {
  std::shared_ptr<EarthbankService> service = std::make_shared<EarthbankService>();
  earthbank::testing::FakeEarthBank& bank = service->bank;
  earthbank::testing::Spy& spy = service->spy;
  Gate& gate = service->gate;
  std::shared_ptr<earthbank::ISecretStore> store{earthbank::make_memory_secret_store()};
  QTemporaryDir dir;

  std::filesystem::path folder() const { return std::filesystem::path(dir.path().toStdString()); }
  std::filesystem::path file() const { return folder() / "profiles.toml"; }
  EarthbankSetup setup() const {
    EarthbankSetup s;
    s.http = [service = service] { return std::make_unique<Through>(service); };
    s.secrets = [store = store] { return store; };
    s.profiles_file = file();
    return s;
  }
  // The fixture account's profile, as a user would have typed it.
  earthbank::Profile profile(const std::string& name = "mylab") const {
    earthbank::Profile p;
    p.name = name;
    p.base_url = bank.base_url();
    p.username = bank.login();
    p.institution_id = 9301;
    return p;
  }
  void write(const earthbank::Profile& p) const {
    earthbank::Profiles all;
    all.active = p.name;
    all.profiles = {p};
    QVERIFY(earthbank::save_profiles(all, file()));
  }
  // The profile in the file, writing to the fixture's package, its password kept.
  void signed_up() const {
    earthbank::Profile p = profile();
    p.data_package_id = bank.writable_package();
    write(p);
    QVERIFY(store->set(earthbank::secret_account(p), bank.password()));
  }
  std::optional<std::string> stored(const earthbank::Profile& p) const {
    auto got = store->get(earthbank::secret_account(p));
    return got ? *got : std::optional<std::string>("<the store failed>");
  }
};

}  // namespace pychron::ui::test
