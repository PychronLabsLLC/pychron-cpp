#pragma once

// elctl embargo: hide a project, a sample or an analysis from every public
// release (data publishing design, sections 4.2 and 6.2).
//
//   elctl embargo set  --db <url> (--project P | --sample S | --analysis A)... [--until YYYY-MM-DD] [--reason <text>]
//   elctl embargo lift --db <url> (--id <uuid> | --project P | --sample S | --analysis A)...
//   elctl embargo list --db <url> [--live]
//
// A sample is "name" or "name@project"; an analysis a run id ("70001-01A")
// or a uuid. --until is a date, inclusive: the embargo ends at midnight UTC
// after it. `lift` by scope lifts every live embargo on it.
//
// Exit codes: 0 done; 2 usage or fatal error (an ambiguous name among them).
// Built without persistence, says so and exits 2.

#include <string>
#include <vector>

#include "cli.hpp"

namespace elctl {

int embargo_command(const std::vector<std::string>& args, Io io);

}  // namespace elctl
