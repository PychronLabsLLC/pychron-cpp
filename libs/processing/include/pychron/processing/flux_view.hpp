#pragma once

// The text of a flux fit, shared by `elctl flux fit` and the flux window: the
// model line, the summary, the warnings and the CSV. Qt-free; the output is
// what elctl has always printed.

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pychron/processing/flux_fit.hpp"

namespace pychron::processing {

// `%.4e`; "-" for an absent or non-finite value.
std::string flux_j_text(double v);
std::string flux_j_text(const std::optional<double>& v);
// `%.2f`; "-" for an absent or non-finite value.
std::string flux_pct_text(double v);
std::string flux_pct_text(const std::optional<double>& v);
// The share of `err` in `value` in percent; "-" when either is absent or the value is 0.
std::string flux_percent_of(double err, double value);
std::string flux_percent_of(const std::optional<double>& err, const std::optional<double>& value);

// "plane, weighted; mean arithmetic (msem); fit error msem" (no "model " prefix).
std::string flux_model_line(const FluxOptions& options);
// "fit MSWD 1.12 (5 dof)   J min 1.0012e-03  max 1.0241e-03  delta 2.24 %"; no line break.
std::string flux_summary(const LevelFit& fit);
// "plane, weighted · fit MSWD 1.12 (5 dof) · J 1.0012e-03 – 1.0241e-03 (2.24 %)": the model's first
// clause, the fit MSWD (left out for a model that has none: dof and MSWD both 0) and the J range.
std::string flux_status_line(const LevelFit& fit);

// What was given on the command line decides two warnings: a monitor set named
// replaces the saved fit's (no "missing" line), and a fit error named replaces
// the saved SD (no "SD" line).
struct FluxWarningContext {
  bool monitor_set_given = false;
  bool fit_error_given = false;
};
// One line each, no "warning: " prefix: positions without a usable analysis,
// left out of the fit, with a mean MSWD outside its limits, extrapolated; the
// fit MSWD outside its limits; the analyses out and why; the saved fit's SD
// replaced; the saved monitor set missing from the store.
std::vector<std::string> flux_warnings(const LevelInputs& inputs, const LevelFit& fit,
                                       const FluxWarningContext& context = {});

// RFC 4180: the field quoted when it holds a comma, a quote or a line break.
std::string csv_field(std::string_view text);
// The head line, and a row for every position of the fit; lines end in CRLF.
std::string flux_csv_header();
std::string flux_csv_rows(const LevelFit& fit);

}  // namespace pychron::processing
