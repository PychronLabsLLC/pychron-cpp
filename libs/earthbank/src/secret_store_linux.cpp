// The password store on Linux and the other free systems: the desktop's
// secret service (GNOME Keyring, KWallet, KeePassXC), through libsecret.
// Compiled only when libsecret was found at build time; and a machine that
// has the library may still have no service to talk to (a server, a session
// with no D-Bus), or a service with no default collection to keep an item
// in, in which case there is no store and the caller is given the memory
// one.
//
// An item is found by two attributes, the service and the account, under a
// schema of this program's own, and lives in the user's default collection.
// A locked collection is unlocked by the service, which may ask the user.
// The question whether there is a service at all is asked for no longer than
// kProbeLimit. A call that reads, stores or removes a password is waited for
// kCallLimit, long enough to type a keyring's password into the window the
// service raises to unlock it; a keyring still locked then is an error that
// says so, and the call is made again once it is unlocked.

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <gio/gio.h>
#include <libsecret/secret.h>

#include "pychron/core/error.hpp"
#include "pychron/earthbank/secret_store.hpp"
#include "secret_store_detail.hpp"

namespace pychron::earthbank {
namespace {

constexpr std::string_view kBackend = "libsecret";

// Filled in here rather than with an initializer: the structure has more
// members than the three this uses.
const SecretSchema& schema() {
  static const SecretSchema made = [] {
    SecretSchema s{};
    s.name = "org.pychron.earthbank.Password";
    s.flags = SECRET_SCHEMA_NONE;
    s.attributes[0].name = "service";
    s.attributes[0].type = SECRET_SCHEMA_ATTRIBUTE_STRING;
    s.attributes[1].name = "account";
    s.attributes[1].type = SECRET_SCHEMA_ATTRIBUTE_STRING;
    return s;
  }();
  return made;
}

// How long the question "is there a secret service?" may take. Left to
// itself a call waits as long as D-Bus lets a method call wait, 25 s, and
// longer where the bus first has to start a service that then never comes up:
// the program would stand still for that at its start. A service that is
// there answers on the session's own bus in milliseconds, so 3 s is long for
// one that will answer and short enough to sit through.
constexpr std::chrono::seconds kProbeLimit{3};

// How long a call that reads, stores or removes a password may take. The
// service may have to unlock its keyring first, and raises a window for the
// user to do that: three seconds would cancel the call under their hands.
constexpr std::chrono::seconds kCallLimit{120};

// A GCancellable for one call, or a few made one after the other, cancelled
// when `limit` has passed since it was made. The wait is in real time and on
// a thread of its own: libsecret's `_sync` calls run a main loop of their
// own, which nothing of ours can put a timer on.
class Limited {
 public:
  explicit Limited(std::chrono::seconds limit) : cancellable_(g_cancellable_new()) {
    timer_ = std::thread([this, limit] {
      std::unique_lock lock(mutex_);
      if (!over_.wait_for(lock, limit, [this] { return done_; })) g_cancellable_cancel(cancellable_);
    });
  }
  ~Limited() {
    {
      const std::lock_guard lock(mutex_);
      done_ = true;
    }
    over_.notify_one();
    timer_.join();
    g_object_unref(cancellable_);
  }
  Limited(const Limited&) = delete;
  Limited& operator=(const Limited&) = delete;

  GCancellable* get() const { return cancellable_; }

 private:
  std::mutex mutex_;
  std::condition_variable over_;
  bool done_ = false;
  GCancellable* cancellable_;
  std::thread timer_;
};

// What the secret service said, as an error, and the GError freed: the
// doing, the account, the service's words and its code. Never a password.
Unexpected<Error> refused(std::string_view doing, std::string_view account, GError* error,
                          std::chrono::seconds limit = kCallLimit) {
  std::string what = std::string(kBackend) + ": could not " + std::string(doing) + " the password of '" +
                     std::string(account) + "': ";
  if (error != nullptr && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
    // Ours: the limit passed (Limited).
    what += "the secret service did not answer within " + std::to_string(limit.count()) +
            " s (a keyring that is waiting to be unlocked does not: unlock it and try again)";
    g_error_free(error);
  } else if (error != nullptr) {
    what += error->message != nullptr ? error->message : "the secret service refused";
    what += " (" + std::string(g_quark_to_string(error->domain)) + " " + std::to_string(error->code) + ")";
    g_error_free(error);
  } else {
    what += "the secret service refused";
  }
  return fail(ErrorKind::Io, std::move(what));
}

class LibsecretSecretStore final : public ISecretStore {
 public:
  explicit LibsecretSecretStore(std::string_view service) : service_(service) {}

  std::string backend() const override { return std::string(kBackend); }
  bool persistent() const override { return true; }

  Result<std::optional<std::string>> get(std::string_view account) override {
    if (auto ok = detail::check_account(kBackend, account); !ok) return fail(ok.error());
    const std::string name(account);
    GError* error = nullptr;
    const Limited limit(kCallLimit);
    gchar* found = secret_password_lookup_sync(&schema(), limit.get(), &error, "service", service_.c_str(), "account",
                                               name.c_str(), nullptr);
    if (error != nullptr) {
      if (found != nullptr) secret_password_free(found);
      return refused("read", account, error);
    }
    if (found == nullptr) return std::optional<std::string>{};
    std::string password(found);
    secret_password_free(found);
    return std::optional<std::string>{std::move(password)};
  }

  Result<void> set(std::string_view account, std::string_view password) override {
    if (auto ok = detail::check_account(kBackend, account); !ok) return ok;
    if (auto ok = detail::check_password(kBackend, password); !ok) return ok;
    const std::string name(account);
    // What a keyring's own window shows for the item; no secret in it.
    const std::string label = "Pychron: EarthBank password of " + name;
    const std::string secret(password);
    GError* error = nullptr;
    const Limited limit(kCallLimit);
    const gboolean stored =
        secret_password_store_sync(&schema(), SECRET_COLLECTION_DEFAULT, label.c_str(), secret.c_str(), limit.get(),
                                   &error, "service", service_.c_str(), "account", name.c_str(), nullptr);
    if (!stored || error != nullptr) return refused("store", account, error);
    return {};
  }

  Result<void> erase(std::string_view account) override {
    if (auto ok = detail::check_account(kBackend, account); !ok) return ok;
    const std::string name(account);
    GError* error = nullptr;
    const Limited limit(kCallLimit);
    // False with no error: there was nothing to remove.
    (void)secret_password_clear_sync(&schema(), limit.get(), &error, "service", service_.c_str(), "account",
                                     name.c_str(), nullptr);
    if (error != nullptr) return refused("remove", account, error);
    return {};
  }

 private:
  std::string service_;
};

// Whether a secret service answers on the session's bus and has a default
// collection, which is where `set` puts an item: a service without one (a
// keyring nobody has set up yet) would refuse every password given to it,
// and the memory store at least keeps one for the session. Both questions
// within one kProbeLimit.
bool service_usable() {
  const Limited limit(kProbeLimit);
  GError* error = nullptr;
  SecretService* service = secret_service_get_sync(SECRET_SERVICE_NONE, limit.get(), &error);
  if (error != nullptr) g_error_free(error);
  if (service == nullptr) return false;
  error = nullptr;
  // Null with no error: the service has no collection under that alias.
  SecretCollection* kept_in =
      secret_collection_for_alias_sync(service, SECRET_COLLECTION_DEFAULT, SECRET_COLLECTION_NONE, limit.get(), &error);
  if (error != nullptr) g_error_free(error);
  g_object_unref(service);
  if (kept_in == nullptr) return false;
  g_object_unref(kept_in);
  return true;
}

}  // namespace

std::unique_ptr<ISecretStore> detail::make_system_secret_store(std::string_view service) {
  if (!service_usable()) return nullptr;
  return std::make_unique<LibsecretSecretStore>(service);
}

}  // namespace pychron::earthbank
