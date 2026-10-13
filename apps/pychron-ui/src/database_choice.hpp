#pragma once

// Which database pychron-ui browses, enters samples into and saves runs to.

#include <filesystem>
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

#ifdef PYCHRON_UI_HAS_STORE
// Whether the session saves its runs to `database` (the url chosen above).
struct RunSaving {
  bool to_database = true;
  // When a simulated session does not: where its runs go instead, to be said once. Else empty.
  std::string note;
};

// A session that is not simulated saves there. A simulated one does when
// experiment::store::saves_simulated_runs says so (the install's own file on
// this computer, or --sim-save); otherwise it writes record files under
// `data`, and the data window goes on reading the database. `simulated` is
// spectrometer::simulated_session's answer.
RunSaving choose_run_saving(const CommandLine& cli, const std::string& database, bool simulated,
                            const std::filesystem::path& data);
#endif

}  // namespace pychron::ui
