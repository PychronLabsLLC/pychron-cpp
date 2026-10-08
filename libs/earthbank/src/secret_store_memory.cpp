// The memory store, the choice between it and the system's, and the name a
// profile's password is kept under.

#include "pychron/earthbank/secret_store.hpp"

#include <cctype>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "pychron/core/error.hpp"
#include "pychron/earthbank/profiles.hpp"
#include "secret_store_detail.hpp"

namespace pychron::earthbank {
namespace {

// The passwords of one service, by account.
struct Shelf {
  std::mutex mutex;
  std::map<std::string, std::string, std::less<>> passwords;
};

class MemorySecretStore final : public ISecretStore {
 public:
  explicit MemorySecretStore(std::shared_ptr<Shelf> shelf) : shelf_(std::move(shelf)) {}

  std::string backend() const override { return "memory"; }
  bool persistent() const override { return false; }

  Result<std::optional<std::string>> get(std::string_view account) override {
    if (auto ok = detail::check_account(backend(), account); !ok) return fail(ok.error());
    const std::lock_guard lock(shelf_->mutex);
    const auto found = shelf_->passwords.find(account);
    if (found == shelf_->passwords.end()) return std::optional<std::string>{};
    return std::optional<std::string>{found->second};
  }

  Result<void> set(std::string_view account, std::string_view password) override {
    if (auto ok = detail::check_account(backend(), account); !ok) return ok;
    if (auto ok = detail::check_password(backend(), password); !ok) return ok;
    const std::lock_guard lock(shelf_->mutex);
    shelf_->passwords.insert_or_assign(std::string(account), std::string(password));
    return {};
  }

  Result<void> erase(std::string_view account) override {
    if (auto ok = detail::check_account(backend(), account); !ok) return ok;
    const std::lock_guard lock(shelf_->mutex);
    if (const auto found = shelf_->passwords.find(account); found != shelf_->passwords.end())
      shelf_->passwords.erase(found);
    return {};
  }

 private:
  std::shared_ptr<Shelf> shelf_;
};

// The scheme's own port, which is not part of an account's name.
std::string_view default_port(std::string_view scheme) {
  if (scheme == "https") return "443";
  if (scheme == "http") return "80";
  return {};
}

// The host of a URL, with its port when that is not the scheme's own, in
// lower case. Of a URL that is not one (checked_base_url says which are),
// what stands where the host would.
std::string host_of(std::string_view url) {
  std::string_view scheme;
  if (const auto at = url.find("://"); at != std::string_view::npos) {
    scheme = url.substr(0, at);
    url.remove_prefix(at + 3);
  }
  url = url.substr(0, url.find_first_of("/?#"));
  if (const auto user = url.rfind('@'); user != std::string_view::npos) url.remove_prefix(user + 1);
  // The last colon is the port's, unless it is inside the brackets of an
  // IPv6 address.
  const auto colon = url.rfind(':');
  if (colon != std::string_view::npos && url.find(']', colon) == std::string_view::npos &&
      url.substr(colon + 1) == default_port(scheme))
    url = url.substr(0, colon);
  std::string host(url);
  for (char& c : host) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return host;
}

}  // namespace

namespace detail {

Result<void> check_account(std::string_view backend, std::string_view account) {
  if (account.empty())
    return fail(ErrorKind::Config, std::string(backend) + ": a password is kept under an account, and none was named");
  if (account.find('\0') != std::string_view::npos)
    return fail(ErrorKind::Config, std::string(backend) + ": an account's name cannot have a NUL in it");
  return {};
}

Result<void> check_password(std::string_view backend, std::string_view password) {
  if (password.empty())
    return fail(ErrorKind::Config,
                std::string(backend) + ": an empty password is not stored (erase the account to forget one)");
  if (password.find('\0') != std::string_view::npos)
    return fail(ErrorKind::Config, std::string(backend) + ": a password with a NUL in it cannot be stored");
  return {};
}

std::unique_ptr<ISecretStore> process_memory_secret_store(std::string_view service) {
  static std::mutex mutex;
  static std::map<std::string, std::shared_ptr<Shelf>, std::less<>> shelves;
  const std::lock_guard lock(mutex);
  auto found = shelves.find(service);
  if (found == shelves.end()) found = shelves.emplace(std::string(service), std::make_shared<Shelf>()).first;
  return std::make_unique<MemorySecretStore>(found->second);
}

}  // namespace detail

std::unique_ptr<ISecretStore> make_memory_secret_store() {
  return std::make_unique<MemorySecretStore>(std::make_shared<Shelf>());
}

std::string_view built_in_secret_store() { return PYCHRON_EARTHBANK_SECRET_STORE; }

std::unique_ptr<ISecretStore> make_secret_store(std::string_view service) {
#if PYCHRON_EARTHBANK_HAS_SYSTEM_SECRET_STORE
  if (auto store = detail::make_system_secret_store(service)) return store;
#endif
  return detail::process_memory_secret_store(service);
}

std::string secret_account(const Profile& profile) { return profile.name + "@" + host_of(profile.base_url); }

}  // namespace pychron::earthbank
