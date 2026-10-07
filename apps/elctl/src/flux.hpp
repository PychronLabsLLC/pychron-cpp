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
//   --fit-error sem|msem|sd                error of the mean models' prediction
//   --neighbors N  --interpolation weighted|average|linear  --axis x|y  --degree 1..4
//   --monitors NAME   the monitor set (default: the saved fit's, else the store's)
//   --sample NAME     the monitor sample's name, when it is not the set's
//   --all-positions   every position that has analyses is a monitor
// A level only (they name analyses and holes of one level):
//   --omit RECORD_ID  --include RECORD_ID  --exclude-position HOLE
//   --no-save-position HOLE
// And:
//   --reset-omits     ignore the omissions and exclusions of the saved fit
//   --csv FILE        every position, one row each (all levels in one file)
//   --save            save the fit (one changeset per level); --user NAME
//
// Prints the monitor and the unknown tables and the warnings. Without
// --save nothing is written to the store.
//
// Exit codes: 0 done (warnings included); 1 a level could not be fitted or a
// save conflicted; 2 usage or a fatal error (no store, bad flag). Built
// without persistence, says so and exits 2.

#include <string>
#include <vector>

#include "cli.hpp"

#ifdef PYCHRON_ELCTL_HAS_STORE
#include <string_view>

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

#endif

}  // namespace elctl
