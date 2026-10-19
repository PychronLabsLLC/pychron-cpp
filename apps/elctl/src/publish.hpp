#pragma once

// elctl publish: releases of a lab's reduced data (data publishing and
// hosted sharing design, section 6.2).
//
//   elctl publish create  --db <url> [selection] [--tag <name>]... [--ia <name>|--no-ia]
//                         --name <slug> --title <text> --author "<name>[|orcid|affiliation]"...
//                         [--licence <spdx>] [--description <text>] [--publisher <name>]
//                         [--embargo-until YYYY-MM-DD] [--supersedes <name|uuid>] [reduction options]
//   elctl publish build   --db <url> --release <name|name@vN|uuid> --out <dir> [--with-viewer] [--with-figures]
//                         [--private]
//   elctl publish verify  <dir> [--db <url>]
//   elctl publish publish --db <url> --release <r> --manifest <dir>/manifest.json [--public]
//   elctl publish withdraw --db <url> --release <r> --reason <text>
//   elctl publish list    --db <url> [--state draft|published|withdrawn]
//   elctl publish show    --db <url> --release <r>
//   elctl publish delete  --db <url> --release <r>
//
// Selection and reduction options are `elctl export`'s (selection.hpp).
// Not in this phase: doi, igsn, upload (they say so and exit 2).
//
// Exit codes: 0 done; 1 nothing matched, or verify found a difference; 2
// usage or fatal error. Built without persistence, says so and exits 2.

#include <string>
#include <vector>

#include "cli.hpp"

namespace elctl {

int publish_command(const std::vector<std::string>& args, Io io);

}  // namespace elctl
