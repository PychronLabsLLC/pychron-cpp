// Group statistics (data browsing and visualization design, section 7): means
// and MSWD, chi-squared limits, probability curves, plateaus and York
// regression. Pure functions over doubles: no I/O, no clocks, no state.
//
// Ported from legacy pychron:
//   core/stats/core.py                 weighted mean, MSWD, Mahon limits
//   core/stats/probability_curves.py   cumulative probability
//   processing/plateau.py              Fleck / Mahon plateau search
//   processing/argon_calculations.py   plateau mean (calculate_plateau_age)
//   core/regression/new_york_regressor.py  York, NewYork (Mahon 1996), Reed
//   processing/analyses/analysis_group.py  error kinds (_modify_error)
// Documented deviations are marked "Deviation:".
#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "pychron/core/error.hpp"

namespace pychron::reduction {

// ---- Means ------------------------------------------------------------------

// How a mean's reported error is formed (legacy SD, SEM, MSEM; "SE" and "MSE"
// are legacy aliases of SEM and MSEM).
enum class MeanErrorKind {
  Sd,    // sample standard deviation about the mean (n - 1); the single error when n == 1
  Sem,   // weighted: 1 / sqrt(sum w); arithmetic: sd / sqrt(n)
  Msem,  // Sem * sqrt(mswd) when mswd > 1, else Sem
};

std::string_view to_string(MeanErrorKind kind) noexcept;  // "sd", "sem", "msem"
std::optional<MeanErrorKind> parse_mean_error_kind(std::string_view text) noexcept;  // also "SE", "MSE", any case

struct Mean {
  double value = 0.0;
  double error = 0.0;  // per the requested MeanErrorKind
  double sem = 0.0;
  double sd = 0.0;
  double mswd = 0.0;   // about `value`, k = 1; 0 when n <= 1
  bool mswd_acceptable = false;
  std::size_t n = 0;   // points used
};

// Inverse-variance weighted mean. Points with a zero or non-finite error are
// skipped, as legacy (calculate_weighted_mean drops falsy errors). Error
// (Config, "stats: ...") when the spans differ in length or no point is left.
Result<Mean> weighted_mean(std::span<const double> values, std::span<const double> errors,
                           MeanErrorKind kind = MeanErrorKind::Sem);

// Unweighted mean. `errors` (may be empty) only feed the MSWD.
// Deviation: legacy reports the standard deviation as the arithmetic mean's
// error whatever the kind; here Sem is sd / sqrt(n) and Msem scales it.
Result<Mean> arithmetic_mean(std::span<const double> values, std::span<const double> errors = {},
                             MeanErrorKind kind = MeanErrorKind::Sem);

// sum(((x - mean) / e)^2) / (n - k); 0 when n <= k. Zero errors are skipped.
double mswd(std::span<const double> values, std::span<const double> errors, double mean, int k = 1);

// ---- Chi-squared ----------------------------------------------------------

double chi2_cdf(double x, double dof);
double chi2_sf(double x, double dof);
double chi2_quantile(double p, double dof);  // inverse of chi2_cdf, p in (0, 1)

// Student's t distribution with `dof` degrees of freedom (dof > 0).
double student_t_cdf(double t, double dof);
double student_t_quantile(double p, double dof);  // inverse of student_t_cdf, p in (0, 1); NaN otherwise

// The central 95% interval of the reduced chi-squared for n - k degrees of
// freedom (Mahon 1996; legacy get_mswd_limits). {0, 0} when n <= k.
std::pair<double, double> mswd_limits(std::size_t n, int k = 1);
// low <= mswd <= high; false when n <= k (legacy validate_mswd).
bool mswd_acceptable(double mswd, std::size_t n, int k = 1);
// P(chi2 > mswd * dof) (MassSpec ProbMSWD); 0 when dof <= 0.
double mswd_probability(double mswd, int dof);

// ---- Probability curves -----------------------------------------------------

struct Curve {
  std::vector<double> x, y;
};

// Sum of unit-area Gaussians on `n` evenly spaced x in [xmin, xmax]. Points
// with |value| or |error| below 1e-10 are skipped (legacy).
Curve cumulative_probability(std::span<const double> values, std::span<const double> errors, double xmin,
                             double xmax, std::size_t n = 500);

// Gaussian kernel density of the values (errors ignored), bandwidth by Scott's
// rule n^(-1/5) * sd, normalized to unit area like scipy's gaussian_kde
// (legacy probability_curves.kernel_density). Empty when fewer than two finite
// values or zero spread.
Curve kernel_density(std::span<const double> values, double xmin, double xmax, std::size_t n = 500);

// ---- Plateaus ---------------------------------------------------------------

enum class PlateauMethod { Fleck, Mahon };  // Fleck 1977 overlap, Mahon 1996 MSWD
enum class PlateauWeighting { InverseVariance, VolumeFraction };

struct PlateauCriteria {
  PlateauMethod method = PlateauMethod::Fleck;
  int nsteps = 3;              // minimum number of included steps
  double gas_fraction = 50.0;  // minimum percent of the included signal (39ArK)
  double overlap_sigma = 2.0;  // Fleck: every pair overlaps at this many sigma
};

struct StepRange {
  std::size_t first = 0, last = 0;  // inclusive step indices
  friend bool operator==(const StepRange&, const StepRange&) = default;
};

// The longest plateau (by last - first; the earliest on a tie), searched from
// every included start step as legacy Plateau.find_plateaus. `gas` is each step's signal (39ArK); `excluded` may be
// empty (nothing excluded) or one flag per step. A one-step range is never a
// plateau (legacy returns none when pidx[0] == pidx[1]).
// Deviations (legacy bugs): excluded steps take part in neither the overlap
// test nor the step count; the Mahon MSWD uses the steps in range (legacy
// indexes ages[start, end + 1] and raises).
std::optional<StepRange> find_plateau(std::span<const double> ages, std::span<const double> errors,
                                      std::span<const double> gas, std::span<const bool> excluded,
                                      const PlateauCriteria& criteria);

struct PlateauMean {
  Mean mean;
  std::size_t nsteps = 0;  // included steps in range
  double gas_fraction = 0.0;  // percent of the included signal in range
};

// The plateau age over the included steps in `range`. VolumeFraction weights
// by signal (sum(w a) / sum(w), sigma = sqrt(sum(w^2 s^2)) / sum(w)).
// Deviation: legacy averages every step in range, excluded ones included.
Result<PlateauMean> plateau_mean(std::span<const double> ages, std::span<const double> errors,
                                 std::span<const double> gas, std::span<const bool> excluded,
                                 StepRange range, PlateauWeighting weighting = PlateauWeighting::InverseVariance,
                                 MeanErrorKind kind = MeanErrorKind::Msem);

// ---- York regression -------------------------------------------------------

enum class YorkMethod {
  York,     // York 1969 solution and basic errors
  NewYork,  // York 1969 solution, Mahon 1996 errors (legacy default)
  Reed,     // Reed 1992 MSWD-scaled errors; correlations ignored
};

std::string_view to_string(YorkMethod method) noexcept;  // "york", "new_york", "reed"
std::optional<YorkMethod> parse_york_method(std::string_view text) noexcept;

struct XyPoint {
  double x = 0, sx = 0, y = 0, sy = 0;
  double rho = 0;  // correlation of the x and y errors
};

struct YorkFit {
  double intercept = 0, intercept_err = 0;
  double slope = 0, slope_err = 0;
  double covariance = 0;  // cov(intercept, slope) = -xbar var(slope), as legacy
  double x_intercept = 0, x_intercept_err = 0;  // -a / b; 0 when b == 0
  double mswd = 0;        // chi2 / (n - 2), with correlated weights
  bool mswd_acceptable = false;
  double probability = 0;
  std::size_t n = 0;
  int iterations = 0;
  bool converged = false;
};

// Fits y = intercept + slope x. Reed's slope is the York solution with every
// rho taken as 0 (the root of Reed's cubic). Error (Config, "stats: ...") when
// fewer than 3 points, a non-positive or non-finite sigma, |rho| >= 1 or a
// degenerate design (all x equal).
Result<YorkFit> york_fit(std::span<const XyPoint> points, YorkMethod method = YorkMethod::NewYork,
                         int max_iterations = 500, double tolerance = 1e-10);

}  // namespace pychron::reduction
