// The password store on Linux and the other free systems: the desktop's
// secret service (GNOME Keyring, KWallet, KeePassXC), through libsecret.
// Compiled only when libsecret was found at build time; and a machine that
// has the library may still have no service to talk to (a server, a session
// with no D-Bus), in which case there is no store and the caller is given
// the memory one.
//
// An item is found by two attributes, the service and the account, under a
// schema of this program's own, and lives in the user's default collection.
// A locked collection is unlocked by the service, which may ask the user.

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

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

// What the secret service said, as an error, and the GError freed: the
// doing, the account, the service's words and its code. Never a password.
Unexpected<Error> refused(std::string_view doing, std::string_view account, GError* error) {
  std::string what = std::string(kBackend) + ": could not " + std::string(doing) + " the password of '" +
                     std::string(account) + "': ";
  if (error != nullptr) {
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
    gchar* found = secret_password_lookup_sync(&schema(), nullptr, &error, "service", service_.c_str(), "account",
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
    const gboolean stored =
        secret_password_store_sync(&schema(), SECRET_COLLECTION_DEFAULT, label.c_str(), secret.c_str(), nullptr, &error,
                                   "service", service_.c_str(), "account", name.c_str(), nullptr);
    if (!stored || error != nullptr) return refused("store", account, error);
    return {};
  }

  Result<void> erase(std::string_view account) override {
    if (auto ok = detail::check_account(kBackend, account); !ok) return ok;
    const std::string name(account);
    GError* error = nullptr;
    // False with no error: there was nothing to remove.
    (void)secret_password_clear_sync(&schema(), nullptr, &error, "service", service_.c_str(), "account", name.c_str(),
                                     nullptr);
    if (error != nullptr) return refused("remove", account, error);
    return {};
  }

 private:
  std::string service_;
};

// Whether a secret service answers on the session's bus.
bool service_reachable() {
  GError* error = nullptr;
  SecretService* service = secret_service_get_sync(SECRET_SERVICE_NONE, nullptr, &error);
  if (error != nullptr) g_error_free(error);
  if (service == nullptr) return false;
  g_object_unref(service);
  return true;
}

}  // namespace

std::unique_ptr<ISecretStore> detail::make_system_secret_store(std::string_view service) {
  if (!service_reachable()) return nullptr;
  return std::make_unique<LibsecretSecretStore>(service);
}

}  // namespace pychron::earthbank
