#pragma once

// elctl import: bring legacy Python-pychron data into the DVC store (legacy
// ingestion spec, sections 5 to 7 and 10).
//
//   elctl import add  --db <url> --kind legacy_db|meta_repo|project_repo
//                     --source <path|url> --tz <IANA> [--branch <b>]
//                     [--author-map <file.toml>] [--catalog-from-repos] [--reference-runs]
//   elctl import run  --db <url> [--source <id|name> | --all] [--batch <n>] [--limit <n>]
//                     [--replay] [--dry-run]
//   elctl import status    --db <url>
//   elctl import conflicts --db <url> [--source <s>] [--kind <k>] [--all] [--json]
//   elctl import verify    --db <url> [--source <s>] [--tolerance 1e-9] [--json]
//
// Every subcommand takes --cache <dir>: where the settings of each source and
// the mirrors of remote repositories are kept (default: the user's cache
// directory, pychron/import).
//
// Exit codes: 0 ok (a paused run included); 1 verify is not ok, or a run
// finished with blocking conflicts pending; 2 usage or fatal error.
//
// Built without persistence, every subcommand says so and exits 2.

#include <functional>
#include <string>
#include <vector>

#include "cli.hpp"

namespace elctl {

int import_command(const std::vector<std::string>& args, Io io);

// For tests: called after each batch `import run` commits, before the run
// asks whether it was interrupted. Empty: nothing is called.
void set_import_batch_hook(std::function<void()> hook);

}  // namespace elctl
