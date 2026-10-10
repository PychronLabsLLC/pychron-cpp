#pragma once

// Which database pychron-ui browses, enters samples into and saves runs to.

#include <optional>
#include <string>

#include "command_line.hpp"
#include "pychron/core/error.hpp"
#include "pychron/setup/site.hpp"

namespace pychron::ui {

// --db when given; else the install's database with its password (`install`
// may be null: no install); else nullopt. An install whose password cannot be
// read is an error: it has a database, and it cannot be used.
Result<std::optional<std::string>> choose_database(const CommandLine& cli, const setup::SiteInstall* install);

}  // namespace pychron::ui
