#pragma once

// elctl export: a publication data report after Schaen et al. (2021),
// "Interpreting and reporting 40Ar/39Ar geochronologic data", from the DVC
// store in one command (processing/report.hpp writes it).
//
//   elctl export --db <url> --out <file.csv|file.json> [selection] [options]
//
// Selection, any of (repeat a flag for several values; every flag narrows):
//   --sample <name>  --identifier <labnumber>  --project <name>
//   --irradiation <name>  --type <analysis type>  --uuid <analysis uuid>
//   --from <YYYY-MM-DD>  --to <YYYY-MM-DD>     UTC, inclusive
//   --include-invalid                           keep analyses tagged invalid
//   Without --type the unknowns are exported.
//
// Options:
//   --group-by auto|aliquot|identifier|sample|none
//                   one summary row per group; auto is aliquot when any
//                   analysis is a heating step, else identifier
//   --sigma 1|2     level of every ± column (default 2)
//   --constants default|legacy|legacy_preferences
//   --decay-error   propagate the decay-constant uncertainty into the ages
//   --plateau fleck|mahon  --plateau-steps <n>  --plateau-gas <percent>
//   --lab <name>    the laboratory, in the metadata
//   --note <text>   a metadata row (repeatable)
//   --limit <n>     at most n analyses (default 5000)
//
// The format follows the extension of --out: .json, else CSV.
//
// Exit codes: 0 wrote the file; 1 no analysis matched; 2 usage or fatal
// error. Built without persistence, says so and exits 2.

#include <string>
#include <vector>

#include "cli.hpp"

namespace elctl {

int export_command(const std::vector<std::string>& args, Io io);

}  // namespace elctl
