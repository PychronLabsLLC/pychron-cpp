// The password store on Windows: the user's Credential Manager, as a generic
// credential that stays on this machine (it does not roam with the profile).
//
// A credential is found by one name, its target: `<service>:<account>`. The
// Credential Manager compares targets without regard to case, which is why
// two profiles may not differ in case alone (profiles.hpp). The password is
// the credential's blob, as the bytes it was given.

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <wincred.h>

#include "pychron/core/error.hpp"
#include "pychron/earthbank/secret_store.hpp"
#include "secret_store_detail.hpp"

namespace pychron::earthbank {
namespace {

constexpr std::string_view kBackend = "credential-manager";

// UTF-8 as the wide text the API takes. Empty when `text` is not UTF-8.
std::wstring wide(std::string_view text) {
  if (text.empty()) return {};
  const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                                       nullptr, 0);
  if (size <= 0) return {};
  std::wstring out(static_cast<std::size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), out.data(), size);
  return out;
}

// What Windows said, as an error: the doing, the account, the system's words
// for the code and its number. Never a password.
Unexpected<Error> refused(std::string_view doing, std::string_view account, DWORD code) {
  return fail(ErrorKind::Io, std::string(kBackend) + ": could not " + std::string(doing) + " the password of '" +
                                 std::string(account) + "': " +
                                 std::system_category().message(static_cast<int>(code)) + " (error " +
                                 std::to_string(code) + ")");
}

class CredentialSecretStore final : public ISecretStore {
 public:
  explicit CredentialSecretStore(std::string_view service) : service_(service) {}

  std::string backend() const override { return std::string(kBackend); }
  bool persistent() const override { return true; }

  Result<std::optional<std::string>> get(std::string_view account) override {
    if (auto ok = detail::check_account(kBackend, account); !ok) return fail(ok.error());
    const std::wstring name = target(account);
    if (name.empty()) return refused("read", account, ERROR_NO_UNICODE_TRANSLATION);
    PCREDENTIALW found = nullptr;
    if (!CredReadW(name.c_str(), CRED_TYPE_GENERIC, 0, &found)) {
      const DWORD code = GetLastError();
      if (code == ERROR_NOT_FOUND) return std::optional<std::string>{};
      return refused("read", account, code);
    }
    std::string password;
    if (found->CredentialBlob != nullptr)
      password.assign(reinterpret_cast<const char*>(found->CredentialBlob), found->CredentialBlobSize);
    CredFree(found);
    return std::optional<std::string>{std::move(password)};
  }

  Result<void> set(std::string_view account, std::string_view password) override {
    if (auto ok = detail::check_account(kBackend, account); !ok) return ok;
    if (auto ok = detail::check_password(kBackend, password); !ok) return ok;
    std::wstring name = target(account);
    std::wstring user = wide(account);
    if (name.empty() || user.empty()) return refused("store", account, ERROR_NO_UNICODE_TRANSLATION);
    // The most a generic credential holds (2560 bytes).
    if (password.size() > CRED_MAX_CREDENTIAL_BLOB_SIZE) return refused("store", account, ERROR_INVALID_PARAMETER);
    // CredWriteW takes the blob as writable bytes and does not write them.
    std::string blob(password);
    CREDENTIALW credential{};
    credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName = name.data();
    credential.UserName = user.data();
    credential.CredentialBlobSize = static_cast<DWORD>(blob.size());
    credential.CredentialBlob = reinterpret_cast<LPBYTE>(blob.data());
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
    const BOOL written = CredWriteW(&credential, 0);
    const DWORD code = written ? ERROR_SUCCESS : GetLastError();
    SecureZeroMemory(blob.data(), blob.size());
    if (!written) return refused("store", account, code);
    return {};
  }

  Result<void> erase(std::string_view account) override {
    if (auto ok = detail::check_account(kBackend, account); !ok) return ok;
    const std::wstring name = target(account);
    if (name.empty()) return refused("remove", account, ERROR_NO_UNICODE_TRANSLATION);
    if (!CredDeleteW(name.c_str(), CRED_TYPE_GENERIC, 0)) {
      const DWORD code = GetLastError();
      if (code != ERROR_NOT_FOUND) return refused("remove", account, code);
    }
    return {};
  }

  // Whether this logon session has a credential set at all: a service
  // account or a network logon may have none.
  bool usable() const {
    const std::wstring name = target("pychron.probe");
    if (name.empty()) return false;
    PCREDENTIALW found = nullptr;
    if (CredReadW(name.c_str(), CRED_TYPE_GENERIC, 0, &found)) {
      CredFree(found);
      return true;
    }
    return GetLastError() == ERROR_NOT_FOUND;
  }

 private:
  std::wstring target(std::string_view account) const { return wide(service_ + ":" + std::string(account)); }

  std::string service_;
};

}  // namespace

std::unique_ptr<ISecretStore> detail::make_system_secret_store(std::string_view service) {
  auto store = std::make_unique<CredentialSecretStore>(service);
  if (!store->usable()) return nullptr;
  return store;
}

}  // namespace pychron::earthbank
