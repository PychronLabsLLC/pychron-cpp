#pragma once

// elctl data-source (data source editing spec,
// docs/superpowers/specs/2026-10-18-data-source-editing-design.md, 3.4):
// the database an install reads and writes, and its data folder.
//
//   elctl [--install NAME] data-source
//   elctl [--install NAME] data-source set [--local [FILE]] [--data DIR] [--yes]
//
// Without `set`: the database URL (never a password), the data folder and,
// for a server, whether a password is kept. `set` changes only what it is
// given, prints before -> after and what is not moved, asks unless --yes,
// checks (a new local file is made and migrated, an existing one only
// opened), writes, seeds an instrument's new store and runs doctor.
// Exit 0 written (or nothing to change), 1 refused or check failed, 2 usage.

#include <optional>
#include <string>
#include <vector>

#include "cli.hpp"

namespace elctl {

int data_source_command(const std::vector<std::string>& args, const std::optional<std::string>& install, Io io);

}  // namespace elctl
