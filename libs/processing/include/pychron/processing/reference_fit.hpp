#pragma once

// Blank and IC factor fits from reference analyses (data browsing and
// visualization design, section 7.5, V3 fit units), after legacy
// FitBlanksNode / FitICFactorNode and references_series.py:
//
// - Blanks: per isotope, the references' baseline-corrected intercepts
//   against run time; the fit evaluated at each unknown's time is its blank.
// - IC factors: per numerator / denominator detector pair, the references'
//   (N / D) / standard ratio of blank-corrected signals; the fit at each
//   unknown's time is the IC factor of the denominator detector.
//
// The fit kinds are legacy's: preceding, succeeding, bracketing average,
// bracketing interpolate (interpolations of the included references),
// average, weighted mean, and linear, parabolic, cubic, exponential
// regressions on hours. Polynomial regressions are weighted by 1/sigma^2
// when every included reference has an error (legacy
// WeightedPolynomialRegressor), else ordinary least squares; exponential
// fits are unweighted.

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pychron/core/error.hpp"
#include "pychron/processing/dataset.hpp"
#include "pychron/processing/model.hpp"
#include "pychron/processing/options.hpp"
#include "pychron/processing/scene.hpp"
#include "pychron/processing/source.hpp"

namespace pychron::processing {

enum class ReferenceFitKind {
  Preceding,
  Succeeding,
  BracketingAverage,
  BracketingInterpolate,
  Average,
  WeightedMean,
  Linear,
  Parabolic,
  Cubic,
  Exponential,
};

// "preceding", "succeeding", "bracketing_average", "bracketing_interpolate",
// "average", "weighted_mean", "linear", "parabolic", "cubic", "exponential".
std::string_view to_string(ReferenceFitKind kind) noexcept;
std::optional<ReferenceFitKind> parse_reference_fit(std::string_view text) noexcept;
bool is_interpolation(ReferenceFitKind kind) noexcept;

// Error of a mean or regression at a time (interpolations carry the
// references' own errors whatever the kind):
//   SEM   standard error: means as reduction::MeanErrorKind::Sem; weighted
//         regressions propagate the references' errors, sqrt(x' (X'WX)^-1 x);
//         unweighted ones as reduction::fit (ErrorType::Sem)
//   SD    means: the references' standard deviation; regressions: SEM and the
//         references' scatter about the fit (a prediction error)
//   MSEM  SEM x sqrt(MSWD) when MSWD > 1
//   CI    the 95% confidence half-width, t(0.975, n - p) x MSEM
//   MC    the standard deviation of the fit at that time over 500 refits of
//         references perturbed by their errors (seeded, reproducible)
enum class ReferenceErrorKind { Sem, Sd, Msem, Ci, MonteCarlo };
std::string_view to_string(ReferenceErrorKind kind) noexcept;  // "SEM", "SD", "MSEM", "CI", "MC"
std::optional<ReferenceErrorKind> parse_reference_error(std::string_view text) noexcept;

struct ReferencePoint {
  double t = 0.0;  // UTC epoch seconds
  Value value;
  std::string uuid, runid;
  bool excluded = false;  // left out of the fit
};

// A fit through the included reference points, evaluated at any time.
class ReferenceModel {
 public:
  // Fails (Config) without included points, or with fewer than the fit
  // needs (a regression: its parameter count; a weighted mean: one point
  // with a non-zero error).
  static Result<ReferenceModel> make(std::vector<ReferencePoint> points, ReferenceFitKind kind,
                                     ReferenceErrorKind error);

  Result<Value> at(double t) const;
  ReferenceFitKind kind() const noexcept { return kind_; }
  ReferenceErrorKind error_kind() const noexcept { return error_; }
  const std::vector<ReferencePoint>& included() const noexcept { return included_; }  // by time
  std::optional<double> mswd() const noexcept { return mswd_; }
  bool weighted() const noexcept { return weighted_; }  // a weighted polynomial regression

  static constexpr int kMonteCarloTrials = 500;

 private:
  Result<Value> regression_at(double t) const;

  ReferenceFitKind kind_ = ReferenceFitKind::Average;
  ReferenceErrorKind error_ = ReferenceErrorKind::Sem;
  std::vector<ReferencePoint> included_;
  double t0_ = 0.0;                // regressions: hours are relative to this
  std::optional<Value> constant_;  // means
  std::optional<double> mswd_;
  bool weighted_ = false;
  // Weighted regressions: coefficients c0.. in hours from t0_, (X'WX)^-1,
  // and the references' unweighted residual variance.
  std::vector<double> beta_, cov_;
  double residual_variance_ = 0.0;
  // MC: the fitted curve of every perturbed trial (polynomial coefficients,
  // or exponential a, b, c), in hours from t0_.
  std::vector<std::vector<double>> trials_;
};

// ---------------------------------------------------------------- fit sets

struct ReferenceUse {
  std::string uuid, runid;
  bool excluded = false;
};

// One stored row: a blank (key: isotope) or an IC factor (key: detector).
struct ReferenceRowFit {
  std::string key;
  Value value;
  ReferenceFitKind fit = ReferenceFitKind::Average;
  ReferenceErrorKind error = ReferenceErrorKind::Sem;
  std::string reference_detector;      // IC factors: the numerator detector
  std::optional<double> standard_ratio;  // IC factors
  bool source_correction = false;        // IC factors from a source mass-discrimination fit
  std::vector<ReferenceUse> references;  // every reference shown, with its exclusion
};

struct AnalysisReferenceFits {
  std::string uuid, runid;
  std::map<std::string, std::string> heads;  // Analysis::heads when fitted
  std::vector<ReferenceRowFit> rows;
};

enum class ReferenceFitTarget { Blanks, IcFactors };
std::string_view to_string(ReferenceFitTarget target) noexcept;  // "blanks", "icfactors"

struct ReferenceFitSet {
  ReferenceFitTarget target = ReferenceFitTarget::Blanks;
  std::vector<AnalysisReferenceFits> analyses;  // included unknowns with at least one row
  std::vector<std::string> warnings;            // rows that could not be fitted
  // "<BLANKS> fits=Ar40(linear),Ar36(preceding)" / "<ICFactor> fits=CDD(average)".
  std::string message() const;
};

using ReferenceFitSetPtr = std::shared_ptr<const ReferenceFitSet>;

// ---------------------------------------------------------------- figures

// Options: blanks list "isotopes" (isotope, fit, error); IC factors list
// "ratios" (numerator, denominator, standard_ratio, fit, error, mode); both
// nsigma, show_current and skip_reviewed (rows whose stored value is
// marked reviewed are shown but not refitted).
//
// IC mode "source_correction" (legacy set_beta, WiscAr): the fit of the
// Ar40 / Ar36 detector pair, v = measured / standard, is a source mass
// discrimination beta = ln(1/v) / ln(m40/m36); each of Ar36..Ar39 gets the
// IC factor (m/m40)^beta = v^k, k = ln(m40/m) / ln(m40/m36), on its detector.
const SchemaPtr& blank_fit_schema();
const SchemaPtr& icfactor_fit_schema();

struct ReferenceFigure {
  Scene scene;
  ReferenceFitSet fits;
};

// One panel per row: references (clickable: PointRef is the reference uuid,
// excluded ones hollow), the fit and its envelope, the unknowns' current
// values and the predicted ones, against hours relative to the newest run.
// Excluded unknowns are not fitted. Rows that cannot be fitted become
// scene warnings and fit-set warnings.
Result<ReferenceFigure> build_reference_figure(ReferenceFitTarget target, const Dataset& unknowns,
                                               const Dataset& references, const Options& options);

// ---------------------------------------------------------------- finding references

struct ReferenceQuery {
  std::vector<std::string> analysis_types;  // e.g. blank_unknown, blank_air, blank_cocktail, blank
  double hours = 10.0;                      // window either side of each unknown
  bool same_mass_spectrometer = true;
  bool same_extract_device = false;
  int limit = 2000;
};

// Default analysis types: the blank types for blanks, air for IC factors.
std::vector<std::string> default_reference_types(ReferenceFitTarget target);

// References for `unknowns`: analyses of the query's types run within
// `hours` of any unknown (windows merged as legacy bin_datetimes), on the
// unknowns' spectrometers (and extract devices) when asked, without invalid
// ones and without the unknowns themselves; uuids, newest first.
Result<std::vector<std::string>> find_references(IAnalysisSource& source, const std::vector<AnalysisPtr>& unknowns,
                                                 const ReferenceQuery& query);

class Unit;
// The blank_fit / icfactor_fit units: (unknowns, references) -> (figure, fits).
std::unique_ptr<Unit> make_reference_fit_unit(ReferenceFitTarget target);

}  // namespace pychron::processing
