#pragma once

// elctl earthbank: publishing 40Ar/39Ar results to EarthBank (AuScope's
// AusGeochem data platform) from the DVC store. pychron::earthbank builds one
// submission per sample and fills the service's own import workbooks; the
// user imports them by hand on the service's website. That command does not
// talk to the service.
//
//   elctl earthbank export-xlsx --db <url> [selection] --out <dir> [options]
//
// and, for the account the user publishes as (the one network access the
// command has, and only with these):
//
//   elctl earthbank login [--profile <name>] [--username <u>] [--base-url <url>] [--institution <id>]
//   elctl earthbank account
//   elctl earthbank packages [--create <name>] [--institution <id>]
//   elctl earthbank preflight --db <url> [selection] [options]
//   elctl earthbank upload --db <url> [selection] [options] --yes
//       [--package <id>] [--allow-unresolved] [--new-datapoint]
//       [--in-doubt written:<id>|not-written] [--journal <dir>]
//   elctl earthbank import-profiles <legacy preferences file>
//
// Every one takes --profiles <file> (default: the file the UI uses) and the
// ones that sign in --profile <name> (default: the active one). A login asks
// for the password without echo, or takes $EARTHBANK_PASSWORD; with neither
// (no terminal) it exits 2 and says so: it never waits on a script's input.
// The password is kept by the operating system (secret_store.hpp) and the
// profile (profiles.hpp) in a file without it. Their text is `elctl earthbank
// help`; nothing is printed that holds a password or a token.
//
// Selection and ages: the flags of `elctl export` (selection.hpp), parsed by
// the same code. A submission is of a sample and at one sigma, so a workbook
// holds the ages of `export --group-by sample --sigma 1` for the same words.
//
// Options:
//   --per-sample         one Ar-Ar workbook per sample, not one for all
//   --no-samples         no Sample workbook
//   --check              say what the export would say and write nothing
//                        (--out is then not needed)
//   --templates <dir>    the service's two templates (default: the directory
//                        $PYCHRON_EARTHBANK_TEMPLATES names, else the installed ones)
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
// submission's notes under it (the age's description as `description: ...`), then the files written, then the notes of the
// export. A sample the mapping refuses stops the export: nothing is written
// for any sample.
//
// upload sends the submissions themselves (uploader.hpp), and since nothing
// on EarthBank can be changed or removed from here it first prints the
// account, the data package, each sample's records and the pre-flight report,
// and sends only with --yes. It prints a line per record written and each
// sample's outcome; for a sample that is not complete, the command line that
// takes it further (the same words; for a record in doubt, with --in-doubt
// either way; for changed data, with --new-datapoint). The journals are in
// --journal, by default `journal` beside the default profiles file. The
// password is the one login kept, else $EARTHBANK_PASSWORD, else asked for at
// a terminal. Exit codes of upload: 0 every sample is complete; 1 no analysis
// matched, or terms the service does not know (without --allow-unresolved) or
// could not be asked about; 2 anything else: usage, no --yes, not signed in,
// no data package, a sample that is not complete.
//
// Exit codes: 0 wrote the files (or, with --check, there was something to
// write; preflight: nothing the service does not know); 1 no analysis matched
// (preflight: terms the service does not know or could not be asked about;
// login, account, packages: the service refused or did not answer); 2 usage
// or fatal error. Built without persistence, says so and exits 2; built
// without network support (Qt6 Network), login, account, packages, preflight
// and upload say that and exit 2.

#include <string>
#include <vector>

#include "cli.hpp"

namespace pychron::http {
class IHttpClient;
}  // namespace pychron::http

namespace pychron::earthbank {
using http::IHttpClient;
class ISecretStore;
}  // namespace pychron::earthbank

namespace elctl {

// What the command talks to and keeps passwords in. Null: the real ones (the
// HTTPS client of this build, the operating system's store). The tests give
// an in-memory EarthBank and the memory store.
struct EarthbankDeps {
  pychron::earthbank::IHttpClient* http = nullptr;
  pychron::earthbank::ISecretStore* secrets = nullptr;
};

int earthbank_command(const std::vector<std::string>& args, Io io, EarthbankDeps deps = {});

}  // namespace elctl
