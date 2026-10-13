#pragma once

// elctl db: the DVC store's schema.
//
//   elctl db status  --db <url>   is the store's schema the one this build needs?
//   elctl db migrate --db <url>   apply the migrations it lacks
//
// pychron-ui, export, flux and entry open a store as it is and refuse one
// whose schema is behind (they never change a store's schema in passing).
// After an update of pychron that adds a migration, `db migrate` is what
// brings an existing store up to date. Each migration is applied in one
// transaction: whole, or not at all.
//
// Exit codes: status: 0 up to date, 1 behind; migrate: 0 done (or nothing to
// do); 2 usage or fatal error, both. Neither makes a database: a SQLite path
// that is not there is an error. Built without persistence, says so and
// exits 2.

#include <string>
#include <vector>

#include "cli.hpp"

namespace elctl {

int db_command(const std::vector<std::string>& args, Io io);

}  // namespace elctl
