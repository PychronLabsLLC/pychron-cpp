#pragma once

// elctl flux: the J of an irradiation level, fitted from its flux monitors
// (flux fitting design, section 7; processing/flux_fit.hpp fits, the store
// adapter in processing/flux_store.hpp reads and writes).
//
//   elctl flux fit <irradiation> [<level>] --db <url> [options]
//
// Without a level, every level of the irradiation is fitted in turn.
//
// Options (each flag given replaces one field of the options of the level's
// saved fit, else of the defaults):
//   --model plane|bowl|weighted-mean|matching|nearest|bracketing|ls1d|mean1d|bracketing1d
//   --weighted | --unweighted              least-squares models
//   --mean arithmetic|weighted             how a position's mean J is formed
//   --mean-error sem|msem|sd               error of that mean
//   --fit-error sem|msem|sd                error of the predicted J (not sd for a fitted surface)
//   --neighbors N  --interpolation weighted|average|linear  --axis x|y  --degree 1..4
//   --monitors NAME   the monitor set (default: the saved fit's, else the store's)
//   --sample NAME     the monitor sample's name (default: the saved fit's, else the set's)
//   --all-positions   every position that has analyses is a monitor
//   --monitor-positions  the monitor sample's positions (the default, unless
//                        the saved fit used --all-positions)
// A level only (they name analyses and holes of one level):
//   --omit RECORD_ID  --include RECORD_ID  --exclude-position HOLE
//   --no-save-position HOLE
// And:
//   --reset-omits     ignore the omissions and exclusions of the saved fit
//   --csv FILE        every position, one row each (all levels in one file);
//                     the file is replaced only when a level was fitted
//   --save            save the fit (one changeset per level); --user NAME
//
// Prints the monitor and the unknown tables and the warnings. Without
// --save nothing is written to the store.
//
//   elctl flux show <irradiation> <level> --db <url>
//   elctl flux history <irradiation> <level> [<hole>] --db <url>
//   elctl flux monitors [list | show NAME | set FILE | default NAME] --db <url> [--user NAME]
// (flux_admin.cpp: what a level holds, how it came to hold it, and the
// lab's monitor sets.)
//
// Exit codes: 0 done (warnings included); 1 a level could not be fitted or a
// save conflicted; 2 usage or a fatal error (no store, bad flag). Built
// without persistence, says so and exits 2.

#include <string>
#include <vector>

#include "cli.hpp"

#ifdef PYCHRON_ELCTL_HAS_STORE
#include <memory>
#include <optional>
#include <string_view>

#include "pychron/core/error.hpp"
#include "pychron/persistence/store.hpp"
#include "pychron/processing/flux_store.hpp"
#endif

namespace elctl {

int flux_command(const std::vector<std::string>& args, Io io);

#ifdef PYCHRON_ELCTL_HAS_STORE

// The printed form of a fit: heading, model line, the Monitors and Unknowns
// tables, the fit line and one `warning:` line per thing to look at (those
// of `fit`, then `extra_warnings`).
std::string format_flux_fit(const pychron::processing::LevelFit& fit, const std::vector<std::string>& extra_warnings = {});

// What a save says; "not saved: ..." when it conflicted. `saved_by` is the
// author of the head that moved (empty: unknown).
std::string format_flux_save(const pychron::processing::FluxSaveOutcome& outcome, std::string_view saved_by);

// RFC 4180: the field quoted when it holds a comma, a quote or a line break.
std::string csv_field(std::string_view text);
// The head line, and a row for every position of the fit; rows end in CRLF.
std::string flux_csv_header();
std::string flux_csv_rows(const pychron::processing::LevelFit& fit);

// ---- Shared by flux.cpp and flux_admin.cpp ----------------------------------

// The value of the flag at `args[i]`, which it steps over (`i` is then at the
// value). An error when there is none, or when what follows is itself a
// flag: "--csv --save" names no file.
pychron::Result<std::string> flux_flag_value(const std::vector<std::string>& args, std::size_t& i);

// The store behind `url` for a command that reads: a SQLite path that is not
// a store is an error, not a new empty one.
pychron::Result<std::unique_ptr<pychron::persistence::IStore>> open_flux_store(const std::string& url);
// The actor that writes: this machine as a client, `user_name` (else $USER) as the user.
pychron::Result<pychron::persistence::Actor> flux_actor(pychron::persistence::IStore& store, const std::string& user_name);

// `%.4e`, "-" for an absent or non-finite value; the share of `err` in `value` in percent.
std::string flux_j_text(const std::optional<double>& v);
std::string flux_percent_of(const std::optional<double>& err, const std::optional<double>& value);
// Left aligned columns two spaces apart; the head row first; every line ends in a line break.
std::string flux_table(const std::vector<std::string>& head, const std::vector<std::vector<std::string>>& rows);

// show, history and monitors; `rest` is what follows the subcommand.
int flux_admin_command(const std::string& subcommand, const std::vector<std::string>& rest, Io io);

#endif

}  // namespace elctl
