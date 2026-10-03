#pragma once

// Checking an install (installation wizard spec section 3.4): each check is
// OK, WARN or FAIL with a hint saying what to do. Runs the same loaders the
// apps run, so "doctor passes" means the apps will start.

#include <functional>
#include <string>
#include <vector>

#include "pychron/core/error.hpp"
#include "pychron/setup/install.hpp"
#include "pychron/setup/site.hpp"

namespace pychron::setup {

struct Check {
  enum class Status { Ok, Warn, Fail };
  Status status = Status::Ok;
  std::string name, detail, hint;
};
std::string_view to_string(Check::Status s) noexcept;

struct DoctorOptions {
  const ProfileLibrary* library = nullptr;  // to report profiles newer than the install
  bool probe = false;                       // connect to each TCP transport, then run the drivers' connect step
  // Opens the install's database (data reduction); empty: reported as not checked.
  std::function<Result<std::string>(const std::string& url)> open_database;
};

std::vector<Check> doctor(const SiteInstall& install, const DoctorOptions& options = {});
bool any_fail(const std::vector<Check>& checks);
bool any_warn(const std::vector<Check>& checks);

// The site entry for what `plan` installs: conventional file names for an
// instrument, the database URL (no password) for data reduction.
SiteInstall site_install(const InstallPlan& plan, const std::string& name);

// The install's database URL with its password (from
// <root>/.pychron/credentials.toml) for a server, as the store opens it.
Result<std::string> database_url(const SiteInstall& install);

}  // namespace pychron::setup
