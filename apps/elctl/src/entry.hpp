#pragma once

// elctl entry: sample and package entry from the command line (sample and
// package entry spec, section 10).
//
//   elctl entry samples import <file.csv> --db <url> [--update-existing] [--errors <out.csv>] [--dry-run]
//   elctl entry samples template <out.csv>
//   elctl entry samples list --db <url> [--pi <name>] [--project <name>] [--material <name>] [--text <t>]
//   elctl entry package add <name> --db <url> [--kind irradiation|package] [--chronology <file> --tz <zone>]
//                           [--reactor <name>] [--levels A-C] [--holder <name>] [--z <z>] [--dry-run]
//   elctl entry package show <name> --db <url> [--level <L>] [--csv]
//   elctl entry package set-kind <name> irradiation|package --db <url>
//   elctl entry positions import <package> <file.csv> --db <url> [--dry-run]
//   elctl entry identifiers generate <package> --db <url> [--overwrite] [--dry-run]
//   elctl entry holders import <file.txt> --db <url> [--name <name>]
//   elctl entry settings show|set <key> <value> --db <url>
//   elctl entry seed <seed.toml> --db <url> [--dry-run]
//
// Writing commands take --user (default $USER). Exit codes: 0 ok; 1 nothing
// was written because a row was stale, refused or invalid; 2 usage or fatal
// error. Built without persistence, every subcommand says so and exits 2.

#include <filesystem>
#include <string>
#include <vector>

#include "cli.hpp"
#include "pychron/core/error.hpp"

namespace elctl {

int entry_command(const std::vector<std::string>& args, Io io);

// Puts the seed file (install defaults design: the references project, its
// samples and special identifiers, the reactors) into the store at `url`;
// what is there is kept. Returns what was done, in one line. `migrate`
// creates the schema or brings it up to date first; without it a store whose
// schema is not current is an error. Not in a build without persistence.
pychron::Result<std::string> seed_database(const std::string& url, const std::filesystem::path& seed_file, bool migrate,
                                           bool dry_run, const std::string& user = {});

}  // namespace elctl
