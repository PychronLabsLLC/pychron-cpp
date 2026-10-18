#pragma once

// elctl init / doctor (installation wizard spec section 3.4).
//
//   elctl init --list
//   elctl init <profile> [--root DIR] [--name NAME] [--answers FILE] [--set id=value]... [--yes]
//   elctl init --reconfigure [--install NAME] [--set id=value]... [--yes]
//   elctl doctor [--install NAME] [--strict] [--probe]
//
// Profiles come from --profiles DIR, else $PYCHRON_PROFILES_DIR, else the
// copy shipped with elctl; the site config from $PYCHRON_SITE_CONFIG, else
// the platform's per-user config directory.

#include <filesystem>
#include <istream>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

#include "cli.hpp"
#include "pychron/core/error.hpp"
#include "pychron/setup/profile.hpp"
#include "pychron/setup/site.hpp"

namespace elctl {

int init_command(const std::vector<std::string>& args, Io io);
int doctor_command(const std::vector<std::string>& args, std::optional<std::string> install, Io io);
// elctl import-line <setupfiles folder> [--out DIR] [--force]: converts a
// legacy Pychron extraction line and canvas (setup::import_legacy_line).
int import_line_command(const std::vector<std::string>& args, Io io);

// The install `--install NAME` names (or the default one); Config with what
// to do when there is none.
pychron::Result<pychron::setup::SiteInstall> resolve_install(const std::optional<std::string>& name);

// Asks `question [Y/n]` on `out`; an empty line, "y", "Y" or "yes" is yes,
// anything else (or input that ended) no.
bool yes(std::istream& in, std::ostream& out, const std::string& question);

// Doctor's checks of `install`, printed one a line (OK on out, the rest on
// err). kFailed when any fails, or any warns with `strict`; else kOk.
int report_doctor(const pychron::setup::SiteInstall& install, const pychron::setup::ProfileLibrary* library,
                  bool strict, bool probe, Io io);

#ifdef PYCHRON_ELCTL_HAS_STORE
// Opens the store at `url` (`migrate`: made and brought up to date) and says
// its schema ("schema version 12").
pychron::Result<std::string> open_store(const std::string& url, bool migrate);
#endif

}  // namespace elctl
