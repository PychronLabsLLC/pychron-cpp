#pragma once

// What the parts of `elctl earthbank` share (earthbank.cpp: the workbooks and
// the pre-flight check; earthbank_account.cpp: login, account, packages,
// import-profiles; earthbank_prompt.cpp: a password asked for at a terminal).
// Internal to elctl; built with persistence only.

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cli.hpp"
#include "earthbank.hpp"
#include "pychron/core/error.hpp"
#include "pychron/earthbank/http.hpp"
#include "pychron/earthbank/profiles.hpp"
#include "pychron/earthbank/secret_store.hpp"
#include "pychron/earthbank/session.hpp"

namespace elctl::earthbank_cli {

// The environment variable a script gives the password in.
inline constexpr const char* kPasswordVariable = "EARTHBANK_PASSWORD";
inline constexpr const char* kNoNetwork = "this build has no network support (Qt6 Network was not found)";

// `elctl earthbank: <message>`, exit 2. The library says `earthbank: ...`;
// under this command's name that is said once.
int fatal(Io io, std::string_view message);
inline int fatal(Io io, const pychron::Error& error) { return fatal(io, error.what); }

// The flags that say which profile and server: --profiles <file>, --profile
// <name>, --base-url <url>.
struct ConnectFlags {
  std::string profiles_file, profile, base_url;
};

// `args[i]` as one of them. True: it was one, and `i` is on its value. False:
// it is not one of these. A flag without its value is an error.
pychron::Result<bool> take_connect_flag(ConnectFlags& flags, const std::vector<std::string>& args, std::size_t& i);

// A session on the server of a profile, signed in when a password is to be had.
struct Online {
  std::unique_ptr<pychron::earthbank::IHttpClient> owned_http;
  std::unique_ptr<pychron::earthbank::ISecretStore> owned_secrets;
  std::unique_ptr<pychron::earthbank::Session> session;
  std::filesystem::path profiles_file;
  pychron::earthbank::Profiles profiles;
  // The profile in use, if there is one (a pre-flight may go without).
  std::optional<pychron::earthbank::Profile> profile;
  // Why the session is not signed in, when it is not, in a sentence.
  std::string not_signed_in;
};

// Opens `online` from the profiles file and the flags. `need_login`: no
// profile, no password, or a sign-in refused is an error (said on io.err; the
// exit code returned, which is kOk when all is well). Without it the session
// may be left signed out and `online.not_signed_in` says why; only a profiles
// file that cannot be read, an address that is none, or no network support is
// an error then. The profile named by --profile must exist, whatever.
int go_online(const ConnectFlags& flags, bool need_login, Io io, const EarthbankDeps& deps, Online& online);

// The four account subcommands. `sub` is the first word; `args` the rest.
int login_command(const std::vector<std::string>& args, Io io, const EarthbankDeps& deps);
int account_command(const std::vector<std::string>& args, Io io, const EarthbankDeps& deps);
int packages_command(const std::vector<std::string>& args, Io io, const EarthbankDeps& deps);
int import_profiles_command(const std::vector<std::string>& args, Io io);

// Whether the program's input is a terminal a password can be typed at
// without echo; and that password, read. Null if it could not be read.
bool at_terminal(Io io);
std::optional<std::string> read_password(Io io, std::string_view prompt);

}  // namespace elctl::earthbank_cli
