// elctl earthbank login, account, packages and import-profiles: the account
// the user publishes as. The session and the words for what went wrong are
// pychron::earthbank's (session.hpp); this file reads the flags, finds the
// profile and its password, and says the results.

#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "earthbank_impl.hpp"

namespace elctl::earthbank_cli {

namespace fs = std::filesystem;
namespace eb = pychron::earthbank;
using pychron::Error;
using pychron::ErrorKind;
using pychron::fail;
using pychron::Result;

int fatal(Io io, std::string_view message) {
  constexpr std::string_view kLibrary = "earthbank: ";
  if (message.starts_with(kLibrary)) message.remove_prefix(kLibrary.size());
  io.err << "elctl earthbank: " << message << '\n';
  return kUsage;
}

Result<bool> take_connect_flag(ConnectFlags& flags, const std::vector<std::string>& args, std::size_t& i) {
  const std::string& flag = args[i];
  std::string* into = flag == "--profiles"   ? &flags.profiles_file
                      : flag == "--profile"  ? &flags.profile
                      : flag == "--base-url" ? &flags.base_url
                                             : nullptr;
  if (into == nullptr) return false;
  if (i + 1 >= args.size() || args[i + 1].empty()) return fail(ErrorKind::Config, flag + " needs a value");
  *into = args[++i];
  return true;
}

namespace {

// `text` as a whole number, or an error naming the flag.
Result<std::int64_t> whole_number(const std::string& flag, const std::string& text) {
  std::int64_t value = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (text.empty() || error != std::errc() || end != text.data() + text.size() || value <= 0)
    return fail(ErrorKind::Config, flag + " takes a number (the service's id), not '" + text + "'");
  return value;
}

std::optional<std::string> password_from_environment() {
  const char* value = std::getenv(kPasswordVariable);
  if (value == nullptr || *value == '\0') return std::nullopt;
  return std::string(value);
}

fs::path profiles_path(const ConnectFlags& flags) {
  return flags.profiles_file.empty() ? eb::default_profiles_file() : fs::path(flags.profiles_file);
}

// The client and the store a command uses: the ones it was given, else this
// build's. Null client: this build cannot reach the network.
struct Parts {
  eb::IHttpClient* http = nullptr;
  eb::ISecretStore* secrets = nullptr;
};

bool parts_of(const EarthbankDeps& deps, Online& online, Parts& parts, Io io) {
  parts.http = deps.http;
  if (parts.http == nullptr) {
    online.owned_http = eb::make_http_client();
    parts.http = online.owned_http.get();
  }
  if (parts.http == nullptr) {
    io.err << kNoNetwork << '\n';
    return false;
  }
  parts.secrets = deps.secrets;
  if (parts.secrets == nullptr) {
    online.owned_secrets = eb::make_secret_store();
    parts.secrets = online.owned_secrets.get();
  }
  return true;
}

constexpr const char* kHowToLogIn = "run `elctl earthbank login` first";

// Says an Error from the service and the exit for it.
int failed(Io io, const Error& error) {
  io.err << "elctl earthbank: " << error.what << '\n';
  return kFailed;
}

}  // namespace

int go_online(const ConnectFlags& flags, bool need_login, Io io, const EarthbankDeps& deps, Online& online) {
  Parts parts;
  if (!parts_of(deps, online, parts, io)) return kUsage;

  online.profiles_file = profiles_path(flags);
  auto loaded = eb::load_profiles(online.profiles_file);
  if (!loaded) return fatal(io, loaded.error());
  online.profiles = std::move(*loaded);

  const eb::Profile* chosen = nullptr;
  if (!flags.profile.empty()) {
    chosen = online.profiles.find(flags.profile);
    if (chosen == nullptr)
      return fatal(io, "no profile '" + flags.profile + "' in " + online.profiles_file.string() + " (" + kHowToLogIn + ")");
  } else {
    chosen = online.profiles.active_profile();
  }
  if (chosen != nullptr) online.profile = *chosen;

  std::string base = online.profile ? online.profile->base_url : std::string(eb::kDefaultBaseUrl);
  if (!flags.base_url.empty()) {
    auto checked = eb::checked_base_url(flags.base_url);
    if (!checked) return fatal(io, checked.error());
    base = std::move(*checked);
  } else if (!online.profile && need_login) {
    return fatal(io, "there is no profile (" + std::string(kHowToLogIn) + ")");
  }
  online.session = std::make_unique<eb::Session>(*parts.http, base);

  const auto unsigned_because = [&](std::string why) -> int {
    if (need_login) return fatal(io, why + " (" + kHowToLogIn + ")");
    online.not_signed_in = std::move(why);
    return static_cast<int>(kOk);
  };
  if (!online.profile) return unsigned_because("there is no profile to sign in with");
  // A password goes to the server it was kept for.
  if (base != online.profile->base_url)
    return unsigned_because("--base-url is not the server of profile '" + online.profile->name + "', and its password is not sent elsewhere");
  if (online.profile->username.empty()) return unsigned_because("profile '" + online.profile->name + "' has no user name");

  std::optional<std::string> password;
  auto kept = parts.secrets->get(eb::secret_account(*online.profile));
  if (!kept) {
    if (need_login) return fatal(io, kept.error());
    io.err << "warning: " << kept.error().what << '\n';
  } else {
    password = std::move(*kept);
  }
  if (!password) password = password_from_environment();
  if (!password)
    return unsigned_because("no password is kept for profile '" + online.profile->name + "' and $" + kPasswordVariable +
                            " is not set");

  const auto in = online.session->sign_in(online.profile->username, *password);
  password->assign(password->size(), '\0');
  if (!in) {
    if (need_login) return failed(io, in.error());
    online.not_signed_in = in.error().what;
  }
  return kOk;
}

// ---------------------------------------------------------------- login

int login_command(const std::vector<std::string>& args, Io io, const EarthbankDeps& deps) {
  ConnectFlags connect;
  std::string username, institution;
  for (std::size_t i = 0; i < args.size(); ++i) {
    auto taken = take_connect_flag(connect, args, i);
    if (!taken) return fatal(io, taken.error());
    if (*taken) continue;
    const std::string& flag = args[i];
    if (flag != "--username" && flag != "--institution") return fatal(io, "login: unknown argument '" + flag + "'");
    if (i + 1 >= args.size()) return fatal(io, flag + " needs a value");
    (flag == "--username" ? username : institution) = args[++i];
  }
  std::optional<std::int64_t> institution_id;
  if (!institution.empty()) {
    auto id = whole_number("--institution", institution);
    if (!id) return fatal(io, id.error());
    institution_id = *id;
  }

  Online online;
  Parts parts;
  if (!parts_of(deps, online, parts, io)) return kUsage;
  const fs::path file = profiles_path(connect);
  auto loaded = eb::load_profiles(file);
  if (!loaded) return fatal(io, loaded.error());
  eb::Profiles profiles = std::move(*loaded);

  // The profile this is for: named, else the active one, else a new `default`.
  std::string name = connect.profile.empty() ? profiles.active : connect.profile;
  if (name.empty()) name = "default";
  if (!eb::valid_profile_name(name))
    return fatal(io, "'" + name + "' is not a profile name (letters, digits, - and _)");
  eb::Profile profile;
  if (const eb::Profile* known = profiles.find(name)) profile = *known;
  profile.name = name;
  if (!username.empty()) profile.username = username;
  if (institution_id) profile.institution_id = institution_id;
  if (!connect.base_url.empty()) {
    auto checked = eb::checked_base_url(connect.base_url);
    if (!checked) return fatal(io, checked.error());
    profile.base_url = std::move(*checked);
  }
  if (profile.username.empty()) return fatal(io, "login: --username is required for a profile that has none");

  // The password: the variable, else the person at the terminal. Never
  // waiting on a script's input.
  std::optional<std::string> password = password_from_environment();
  if (!password) {
    if (!at_terminal(io)) {
      return fatal(io, std::string("login: no terminal to ask for the password at, and $") + kPasswordVariable +
                           " is not set");
    }
    password = read_password(io, "Password for " + profile.username + " at " + profile.base_url + ": ");
    if (!password || password->empty()) return fatal(io, "login: no password was given");
  }

  eb::Session session(*parts.http, profile.base_url);
  const auto in = session.sign_in(profile.username, *password);
  if (!in) {
    password->assign(password->size(), '\0');
    return failed(io, in.error());
  }
  const auto account = session.account();
  if (!account) {
    password->assign(password->size(), '\0');
    return failed(io, account.error());
  }
  io.out << "signed in to " << profile.base_url << " as " << account->name << " (" << account->login << ")\n";

  const auto stored = parts.secrets->set(eb::secret_account(profile), *password);
  password->assign(password->size(), '\0');
  if (!stored) {
    io.err << "elctl earthbank: signed in, but the password could not be kept, so nothing was saved: " << stored.error().what
           << '\n';
    return kFailed;
  }
  io.out << "password for profile '" << name << "' kept in the " << parts.secrets->backend() << " store\n";
  if (!parts.secrets->persistent()) {
    io.out << "note: this store lasts as long as the program; later commands need $" << kPasswordVariable << " set\n";
  }

  bool replaced = false;
  for (eb::Profile& known : profiles.profiles) {
    if (known.name != name) continue;
    known = profile;
    replaced = true;
  }
  if (!replaced) profiles.profiles.push_back(profile);
  profiles.active = name;
  if (const auto saved = eb::save_profiles(profiles, file); !saved) {
    io.err << "elctl earthbank: signed in and the password is kept, but the profile could not be saved: "
           << saved.error().what << '\n';
    return kFailed;
  }
  io.out << "profile '" << name << "' saved in " << file.string() << " and made active\n";
  return kOk;
}

// ---------------------------------------------------------------- account

int account_command(const std::vector<std::string>& args, Io io, const EarthbankDeps& deps) {
  ConnectFlags connect;
  for (std::size_t i = 0; i < args.size(); ++i) {
    auto taken = take_connect_flag(connect, args, i);
    if (!taken) return fatal(io, taken.error());
    if (!*taken) return fatal(io, "account: unknown argument '" + args[i] + "'");
  }
  Online online;
  if (const int code = go_online(connect, true, io, deps, online); code != kOk) return code;
  const auto account = online.session->account();
  if (!account) return failed(io, account.error());
  io.out << "profile: " << online.profile->name << " (" << online.session->base_url() << ")\n";
  io.out << "account: " << account->name << " (" << account->login << "), id " << account->id << '\n';
  return kOk;
}

// ---------------------------------------------------------------- packages

int packages_command(const std::vector<std::string>& args, Io io, const EarthbankDeps& deps) {
  ConnectFlags connect;
  std::string create, institution;
  bool creating = false;
  for (std::size_t i = 0; i < args.size(); ++i) {
    auto taken = take_connect_flag(connect, args, i);
    if (!taken) return fatal(io, taken.error());
    if (*taken) continue;
    const std::string& flag = args[i];
    if (flag != "--create" && flag != "--institution") return fatal(io, "packages: unknown argument '" + flag + "'");
    if (i + 1 >= args.size() || args[i + 1].empty()) return fatal(io, flag + " needs a value");
    if (flag == "--create") {
      create = args[++i];
      creating = true;
    } else {
      institution = args[++i];
    }
  }
  std::optional<std::int64_t> institution_id;
  if (!institution.empty()) {
    auto id = whole_number("--institution", institution);
    if (!id) return fatal(io, id.error());
    institution_id = *id;
  }

  Online online;
  if (const int code = go_online(connect, true, io, deps, online); code != kOk) return code;
  if (!institution_id) institution_id = online.profile->institution_id;
  eb::Session& session = *online.session;

  if (creating) {
    const auto made = session.create_package(create, institution_id);
    if (!made) return failed(io, made.error());
    io.out << "created data package " << made->id << " '" << made->name << "'\n";
    eb::Profile* mine = nullptr;
    for (eb::Profile& p : online.profiles.profiles)
      if (p.name == online.profile->name) mine = &p;
    if (mine == nullptr) return fatal(io, "the profile is not in the file any more");
    mine->data_package_id = made->id;
    if (const auto saved = eb::save_profiles(online.profiles, online.profiles_file); !saved) {
      io.err << "elctl earthbank: data package " << made->id << " was made, but the profile could not be saved with it: "
             << saved.error().what << '\n';
      return kFailed;
    }
    io.out << "profile '" << mine->name << "' now uploads to data package " << made->id << '\n';
    return kOk;
  }

  const auto packages = session.packages(institution_id);
  if (!packages) return failed(io, packages.error());
  if (institution_id) io.out << "data packages of institution " << *institution_id << ":\n";
  for (const eb::DataPackage& p : *packages) {
    const bool mine = online.profile->data_package_id && *online.profile->data_package_id == p.id;
    io.out << (mine ? "* " : "  ") << p.id << "  " << p.name;
    if (!p.institution_name.empty()) io.out << "  [" << p.institution_name << ']';
    if (!p.workflow_state.empty()) io.out << "  " << p.workflow_state;
    io.out << '\n';
  }
  io.out << packages->size() << (packages->size() == 1 ? " data package" : " data packages");
  if (online.profile->data_package_id) io.out << " (* is the one profile '" << online.profile->name << "' uploads to)";
  io.out << '\n';
  return kOk;
}

// ---------------------------------------------------------------- import-profiles

int import_profiles_command(const std::vector<std::string>& args, Io io) {
  ConnectFlags connect;
  std::string ini;
  for (std::size_t i = 0; i < args.size(); ++i) {
    auto taken = take_connect_flag(connect, args, i);
    if (!taken) return fatal(io, taken.error());
    if (*taken) continue;
    if (args[i].rfind("--", 0) == 0 || !ini.empty()) return fatal(io, "import-profiles: unexpected argument '" + args[i] + "'");
    ini = args[i];
  }
  if (ini.empty()) return fatal(io, "import-profiles: the legacy preferences file is required");

  auto imported = eb::import_legacy_ini(ini);
  if (!imported) return fatal(io, imported.error());
  const fs::path file = profiles_path(connect);
  auto loaded = eb::load_profiles(file);
  if (!loaded) return fatal(io, loaded.error());
  eb::Profiles profiles = std::move(*loaded);

  // What is there is the user's: a profile of a name that is taken is left as
  // it is, and said.
  std::size_t added = 0;
  for (const eb::Profile& p : imported->profiles) {
    if (profiles.find(p.name) != nullptr) {
      imported->notes.push_back("profile '" + p.name + "' is already in " + file.string() + " and was left as it is");
      continue;
    }
    profiles.profiles.push_back(p);
    ++added;
  }
  if (profiles.active.empty() && !imported->active.empty() && profiles.find(imported->active) != nullptr)
    profiles.active = imported->active;
  if (const auto saved = eb::save_profiles(profiles, file); !saved) return fatal(io, saved.error());

  for (const std::string& note : imported->notes) io.out << "note: " << note << '\n';
  io.out << "imported " << added << (added == 1 ? " profile" : " profiles") << " into " << file.string() << '\n';
  if (added > 0) io.out << "passwords are not imported: sign in once for each profile with `elctl earthbank login --profile <name>`\n";
  return kOk;
}

}  // namespace elctl::earthbank_cli
