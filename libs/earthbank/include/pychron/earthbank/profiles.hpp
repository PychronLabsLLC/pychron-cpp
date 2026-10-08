// Where a user's connection to EarthBank is written down.
//
// A profile is a name for one account on one server: the server's address,
// the login, and the institution and data package uploads go to. Profiles
// live in one TOML file per user, read by `elctl` and by the UI alike:
//
//   active = "mylab"
//
//   [profiles.mylab]
//   base_url = "https://earthbank.test"   # left out: the service itself
//   username = "a.user"
//   institution_id = 1001
//   data_package_id = 12345
//
// The password is not in it and cannot be put in it: a file with a
// `password`, `token` or `secret` key is refused, and Profile has no field
// to hold one. The password is kept by the operating system
// (secret_store.hpp), under an account name made from the profile.
//
// The file is pychron's own configuration, read back by pychron: it is not
// marked as a user file. It is written with the owner's permissions only (it
// names a login) and replaced whole, so that a reader never sees half of it.
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pychron/core/error.hpp"

namespace pychron::earthbank {

// The service. This is the one place in the code that names it: everything
// else is given a profile's `base_url`, and the tests use other hosts.
inline constexpr std::string_view kDefaultBaseUrl = "https://ausgeochem.auscope.org.au";

struct Profile {
  std::string name;
  std::string base_url{kDefaultBaseUrl};
  std::string username;
  std::optional<std::int64_t> institution_id, data_package_id;

  friend bool operator==(const Profile&, const Profile&) = default;
};

struct Profiles {
  // The profile used when none is named; empty when none was chosen.
  std::string active;
  std::vector<Profile> profiles;
  // What reading it has to tell the user, a sentence each: what an import
  // changed, left out or ignored. Never part of the file: save_profiles does
  // not write them and load_profiles gives none.
  std::vector<std::string> notes;

  // Null when there is no such profile. Names are compared exactly.
  const Profile* find(std::string_view name) const;
  // Null when `active` is empty or names no profile.
  const Profile* active_profile() const;
};

// A profile's name: letters, digits, `-` and `_`, at least one. It is a key
// of the file and part of the name its password is kept under.
bool valid_profile_name(std::string_view name);

// `url` as a profile holds it, or a `Config` error saying what is wrong with
// it. It is `https://` and a host, with a port if need be and nothing after
// it: no user or password, no path, no query, no fragment. One `/` at the
// end is allowed and dropped. `http://` is taken for this machine only
// (`localhost`, `127.0.0.1`, `[::1]`), which is what a test server is.
Result<std::string> checked_base_url(std::string_view url);

// The profiles in `file`, in the order the file has them. A file that does
// not exist is no profiles; one that is there and cannot be read, or a path
// that is not a file (a directory), is an `Io` error naming it, never no
// profiles. A `Config` error, naming the file and the key, for a file that
// is not TOML, a key this does not know (a `password`, `token` or `secret`
// key in any spelling of case among them), a value of the wrong type, a name
// or a `base_url` that is not one, two names that differ only in case, and
// an `active` that names no profile.
Result<Profiles> load_profiles(const std::filesystem::path& file);

// Writes `profiles` to `file`, making its directory if need be. What
// load_profiles would refuse is refused here (`Config`) and nothing is
// written. The file is written beside itself under another name and renamed
// over the old one, so the old one stays whole if anything fails (`Io`), and
// it is readable by its owner only.
Result<void> save_profiles(const Profiles& profiles, const std::filesystem::path& file);

// The file both programs use unless told otherwise:
//   Linux    $XDG_CONFIG_HOME/pychron/earthbank/profiles.toml, or under ~/.config
//   macOS    ~/Library/Application Support/Pychron/earthbank/profiles.toml
//   Windows  %APPDATA%\Pychron\earthbank\profiles.toml
// The directory is the one the site file is in (libs/setup,
// `default_site_path`), hence its capital on macOS and Windows. This is not what Qt calls the application's config location, which has the
// program's name in it: the UI asks here and does not work out a path of its
// own. Empty when the user has no home to put it in; load_profiles and
// save_profiles refuse an empty path.
std::filesystem::path default_profiles_file();

// The profiles of legacy pychron's preferences file (the `profiles_json` and
// `active_profile` keys of its `[pychron.ausgeochem]` section), for writing
// with save_profiles. Passwords are not imported: legacy kept them in its
// own store, and one found in the file is ignored. The user signs in once
// for each profile. `notes` says what was not taken as it stood: a name
// that had to be respelled, an id that is not a number, a profile left out
// for its address, a password that was ignored. No note holds a password.
// An `Io` error when the file cannot be read, `Config` when it has no such
// section or its profiles are not the JSON legacy wrote.
Result<Profiles> import_legacy_ini(const std::filesystem::path& ini);

}  // namespace pychron::earthbank
