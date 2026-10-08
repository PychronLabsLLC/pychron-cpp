#pragma once

// The text of a flux fit, shared by `elctl flux fit` and the flux window: the
// model line, the summary, the warnings and the CSV. Qt-free; the output is
// what elctl has always printed.

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pychron/processing/flux_fit.hpp"
#include "pychron/processing/options.hpp"
#include "pychron/processing/scene.hpp"

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

// ---- The options as a schema ------------------------------------------------
// So the schema-driven OptionsEditor and PresetBar can edit a flux fit's options.
// The keys are model.kind, model.weighted, model.neighbors, model.interpolation,
// model.axis, model.degree, mean.kind, mean.error and fit.error, with the
// spellings `elctl flux fit` accepts. One shared instance; kind "flux", version 1.
SchemaPtr flux_options_schema();
Options to_options(const FluxOptions& options);
// A field the chosen model does not use keeps its value and is converted too.
// Error (Config): sd as the error of a fitted surface, the math layer's text.
Result<FluxOptions> flux_options_from(const Options& options);

// ---- The scene --------------------------------------------------------------
// J against the hole angle (or against X or Y for a one-dimensional model): every
// monitor analysis a clickable point (refs are the analysis uuids), the monitor
// means, the unknowns' predicted J, and the fitted curve with its error band.
// kind "flux", one graph, one panel "p0", quantity "J". Layers in order: band,
// line "Fit", points "Analyses", "Monitor means", "Unknowns", then highlight
// layers (no label). Qt-free; the window only draws it.
enum class FluxAbscissa { Angle, X, Y };
FluxAbscissa flux_abscissa(const FluxOptions& options);
// Angle: degrees(atan2(x, y)), from the +y axis, in (-180, 180]; X and Y: that coordinate.
double flux_hole_abscissa(FluxAbscissa kind, double x, double y);

struct FluxSceneOptions {
  std::optional<int> highlight_hole;  // the hole whose analyses, mean or J are drawn on top
};
// The curve exists for the least-squares, weighted-mean and mean1d models, from
// the monitors used in the fit; none (no band, no line) for matching, nearest,
// bracketing and bracketing1d, or when the model cannot be evaluated along it.
ScenePtr flux_scene(const LevelInputs& inputs, const LevelFit& fit, const FluxSceneOptions& options = {});
// When the level could not be fitted: the analyses and the monitor means only,
// worked out under the same omission rules as fit_level.
ScenePtr flux_scene(const LevelInputs& inputs, const FluxOptions& options, const Edits& edits);

}  // namespace pychron::processing
