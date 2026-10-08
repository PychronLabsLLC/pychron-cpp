// What the password stores share (secret_store.hpp): the checks every store
// makes before it asks the system anything, the process's memory store, and
// the one function a system's store gives.
#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "pychron/core/error.hpp"
#include "pychron/earthbank/secret_store.hpp"

namespace pychron::earthbank::detail {

// An account that is not empty, and for `set` a password that is not empty
// and has no NUL in it; a `Config` error naming `backend` otherwise. The
// message never holds the password.
Result<void> check_account(std::string_view backend, std::string_view account);
Result<void> check_password(std::string_view backend, std::string_view password);

// The memory store all of `service` share in this process: what
// make_secret_store gives where the system has no store to use.
std::unique_ptr<ISecretStore> process_memory_secret_store(std::string_view service);

// The system's store for `service`, defined by the one secret_store_<system>.cpp
// that was compiled (none when PYCHRON_EARTHBANK_SECRET_STORE is "memory").
// Null when the store cannot be used now: no keychain, no session, no secret
// service on the bus.
std::unique_ptr<ISecretStore> make_system_secret_store(std::string_view service);

}  // namespace pychron::earthbank::detail
