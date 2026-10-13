// Where a profile's password is kept.
//
// The operating system keeps it: the login keychain on macOS, the Credential
// Manager on Windows, the secret service (libsecret) on Linux when this build
// has it and a session offers one. Where there is none the password is held
// in memory until the program ends, and the user is asked again next time.
// There is no store that writes a password to a file, and nothing here reads
// one from the environment.
//
// A password goes through `set` and `get` as a std::string. A caller keeps it
// no longer than the sign-in it is for, and never logs it. An error from a
// store names the store and the system's status; it never holds a password.
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "pychron/core/error.hpp"
#include "pychron/earthbank/profiles.hpp"

namespace pychron::earthbank {

// One password per account, under the service name the store was made with.
// A store may be used from several threads.
//
// An account is not empty; a password is not empty and has no NUL in it
// (`Config` otherwise: nothing is stored). To forget a password, `erase`.
// A failure of the system's store is an `Io` error.
class ISecretStore {
 public:
  virtual ~ISecretStore() = default;
  // "keychain", "credential-manager", "libsecret" or "memory".
  virtual std::string backend() const = 0;
  // Whether what is set is still there when the program runs again. False
  // for "memory": the user should be told the password is for this session.
  virtual bool persistent() const = 0;
  // Nothing when the account has no password.
  virtual Result<std::optional<std::string>> get(std::string_view account) = 0;
  // Replaces the account's password if it has one.
  virtual Result<void> set(std::string_view account, std::string_view password) = 0;
  // An account with no password is not an error.
  virtual Result<void> erase(std::string_view account) = 0;
};

// The system's store when this build has one and it can be used now,
// otherwise the memory store: ask `backend()` or `persistent()` which. The
// memory store given here is the process's: every store made with the same
// `service` sees the same passwords until the program ends.
std::unique_ptr<ISecretStore> make_secret_store(std::string_view service = "pychron.earthbank");

// A memory store of its own, sharing nothing: for tests, and for a program
// told not to keep anything.
std::unique_ptr<ISecretStore> make_memory_secret_store();

// The system's store this build was made with, as `backend()` names it, or
// "memory" when it was made with none. What make_secret_store gives may
// still be "memory" (no session, no secret service running).
std::string_view built_in_secret_store();

// The account a profile's password is kept under: `<profile>@<host>`, the
// host being that of `base_url` in lower case, with its port when that is
// not the scheme's own. `mylab@earthbank.test`, `dev@localhost:8080`.
std::string secret_account(const Profile& profile);

}  // namespace pychron::earthbank
