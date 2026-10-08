#pragma once

// elctl earthbank: publishing 40Ar/39Ar results to EarthBank (AuScope's
// AusGeochem data platform) from the DVC store. pychron::earthbank builds one
// submission per sample and fills the service's own import workbooks; the
// user imports them by hand on the service's website. Nothing here talks to
// the service.
//
//   elctl earthbank export-xlsx --db <url> [selection] --out <dir> [options]
//
// Selection and ages: the flags of `elctl export` (selection.hpp), parsed by
// the same code, so a workbook holds the ages `export` reports for the same
// words.
//
// Options:
//   --per-sample         one Ar-Ar workbook per sample, not one for all
//   --no-samples         no Sample workbook
//   --check              say what the export would say and write nothing
//                        (--out is then not needed)
//   --templates <dir>    the service's two templates (default: the installed ones)
//   --overrides <file>   what the analyses cannot say, in TOML:
//
//       setup = "..."                 # the lab's analytical set-up, by its name on EarthBank
//       decay_constant = "..."        # the service's name, or a citation
//       air_ratio = "..."
//       age_includes_j_error = true   # the J uncertainty in the reported age's
//
//       [samples."<sample name>"]
//       interpretation = "..."
//       preferred_age = "plateau"     # plateau | weighted_mean | isochron | integrated
//       flux_monitor = "..."
//       datapoint_name = "..."
//       irradiation = "..."
//
//     Every key may be left out. A key not listed here, or a sample the
//     selection does not hold, is an error naming it.
//
// Prints one line per sample (its analyses and the age reported) with the
// submission's notes under it, then the files written, then the notes of the
// export. A sample the mapping refuses stops the export: nothing is written
// for any sample.
//
// Exit codes: 0 wrote the files (or, with --check, there was something to
// write); 1 no analysis matched; 2 usage or fatal error. Built without
// persistence, says so and exits 2.

#include <string>
#include <vector>

#include "cli.hpp"

namespace elctl {

int earthbank_command(const std::vector<std::string>& args, Io io);

}  // namespace elctl
