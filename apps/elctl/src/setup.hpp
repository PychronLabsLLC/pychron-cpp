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
#include <optional>
#include <string>
#include <vector>

#include "cli.hpp"
#include "pychron/core/error.hpp"
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

}  // namespace elctl
