#pragma once

// elctl spec: the spectrometer's field tables, read and written as files
// (field table editor design, section 4). No device is opened.
//
//   elctl spec table list                          tables, current version, (active) and (HV)
//   elctl spec table show [<name>] [--version V]   grid as text; default the active table, current
//   elctl spec table versions <name>               newest first, current marked
//   elctl spec table diff <name> <v1> [<v2>]       v2 defaults to current; rows matched by isotope
//   elctl spec table check [<name>]                diagnostics; exit 1 on an error
//
// Every subcommand takes --spectrometer <file>; without it the install's
// spectrometer is used. Tables live under <file's directory>/tables.

#include <string>
#include <vector>

#include "cli.hpp"
#include "exp.hpp"

namespace elctl {

int spec_command(const std::vector<std::string>& args, const ExpGlobals& globals, Io io);

}  // namespace elctl
