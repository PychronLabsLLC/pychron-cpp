#pragma once

// Isotope-evolution fits (spec 8.2). Pure functions: no I/O, no clocks, no
// shared state, so the live intercept in the UI/conditionals and the final
// intercept in the AnalysisRecord are the same computation.
//
// Ported from pychron.core.regression (MeanRegressor, OLSRegressor,
// ExponentialRegressor, BaseRegressor.calculate_filtered_data). Documented
// deviations from pychron are marked "Deviation:" below and in fits.cpp.

#include <cstddef>
#include <optional>
#include <string_view>
#include <vector>

#include "pychron/core/error.hpp"

namespace pychron::reduction {

enum class FitKind {
  Average,      // mean of y
  Linear,       // y = c0 + c1 x
  Parabolic,    // y = c0 + c1 x + c2 x^2
  Cubic,        // y = c0 + ... + c3 x^3
  Exponential,  // y = a exp(-b x) + c
  CustomPoly,   // y = c0 + ... + cn x^n, n = FitSpec::degree
};

enum class ErrorType {
  Sem,  // pychron "SEM": standard error of the fitted value at x = 0
  Sd,   // pychron "SD": prediction error, sef * sqrt(1 + var_hat)
};

struct OutlierSpec {
  bool enabled = false;
  int iterations = 1;
  double std_devs = 2.0;
};

struct FitSpec {
  FitKind kind = FitKind::Linear;
  ErrorType error = ErrorType::Sem;
  OutlierSpec outliers{};
  int degree = 0;  // polynomial order; used only by CustomPoly
};

struct Series {
  std::vector<double> x;  // seconds since measurement start
  std::vector<double> y;  // signal
};

struct Intercept {
  double value = 0.0;                    // fitted y at x = 0
  double error = 0.0;                    // per FitSpec::error
  std::size_t n_used = 0;                // points in the final fit
  std::vector<std::size_t> filtered_idx; // ascending indices removed as outliers
  double residual_sd = 0.0;              // sqrt(SSR / (n_used - n_params)); 0 if n_used <= n_params
};

// Number of free parameters for a spec (Average=1, Linear=2, ..., Exponential=3).
std::size_t parameter_count(const FitSpec& spec) noexcept;

// Fit `series` and evaluate the intercept at x = 0.
// Fails (ErrorKind::Config) on mismatched x/y lengths, negative CustomPoly
// degree, fewer points than parameters, non-finite input, a singular design,
// or an exponential fit that cannot be solved.
Result<Intercept> fit(const Series& series, const FitSpec& spec);

// "average", "linear", "parabolic", "cubic", "exponential", "custom_poly".
std::string_view to_string(FitKind kind) noexcept;
std::optional<FitKind> parse_fit_kind(std::string_view name) noexcept;

}  // namespace pychron::reduction
