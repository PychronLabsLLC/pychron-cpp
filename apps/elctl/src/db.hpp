#pragma once

// elctl db: the DVC store's schema.
//
//   elctl [--install NAME] db status  [--db <url>]   is the store's schema the one this build needs?
//   elctl [--install NAME] db migrate [--db <url>]   apply the migrations it lacks
//   elctl [--install NAME] db derive  [--db <url>] [--all] [--limit N] [--quiet]
//                                 fill the cache of ages (age cache spec 4.5):
//                                 reduce, and cache the age of, every analysis
//                                 whose cached age is not of its inputs now
//
// Without --db: the install's database (setup::database_url, its kept
// password joined in), the one --install names or else the default one; a
// switched install (`elctl data-source set`) is migrated with
// `elctl --install NAME db migrate`. --install naming no install is an
// error (exit 1, as for every command); no --db and no install at all is a
// usage error.
//
// pychron-ui, export, flux and entry open a store as it is and refuse one
// whose schema is behind (they never change a store's schema in passing).
// After an update of pychron that adds a migration, `db migrate` is what
// brings an existing store up to date. Each migration is applied in one
// transaction: whole, or not at all.
//
// Exit codes: status: 0 up to date, 1 behind; migrate: 0 done (or nothing to
// do); derive: 0 done, 1 when an analysis could not be derived (each named on
// stderr); 2 usage or fatal error (a store that refused a write among them),
// all three. derive refuses a store behind its migrations, prints the counts
// so far after each page of 200 unless --quiet, and a summary always; a
// second run finds everything current. None makes a database: a
// SQLite path that is not there is an error. Built without persistence, says
// so and exits 2.

#include <optional>
#include <string>
#include <vector>

#include "cli.hpp"
#include "pychron/processing/domain.hpp"

namespace elctl {

int db_command(const std::vector<std::string>& args, const std::optional<std::string>& install,
               const pychron::processing::Domain& domain, Io io);

}  // namespace elctl
