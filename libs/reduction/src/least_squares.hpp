// Householder QR least squares with column equilibration, shared by the
// regression fits (fits.cpp) and the flux surface models (flux.cpp).
#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

#include "pychron/core/error.hpp"

namespace pychron::reduction::detail {

using Matrix = std::vector<std::vector<double>>;  // row-major, rows = observations

struct LeastSquares {
  std::vector<double> beta;
  Matrix cov_unscaled;  // (A'A)^-1
  double pivot_ratio = 0.0;  // min |R_kk| / max |R_kk| of the column-equilibrated R: 1 is orthogonal, 0 singular
};

// Householder QR least squares with column equilibration. Fails when the
// design is rank deficient relative to double precision.
inline Result<LeastSquares> least_squares(Matrix a, std::vector<double> y) {
  const std::size_t n = a.size();
  const std::size_t p = n ? a[0].size() : 0;
  if (n < p || p == 0) return fail(ErrorKind::Config, "underdetermined least squares");

  std::vector<double> scale(p, 0.0);
  for (std::size_t j = 0; j < p; ++j) {
    for (std::size_t i = 0; i < n; ++i) scale[j] += a[i][j] * a[i][j];
    scale[j] = std::sqrt(scale[j]);
    if (!(scale[j] > 0.0) || !std::isfinite(scale[j]))
      return fail(ErrorKind::Config, "singular design matrix");
    for (std::size_t i = 0; i < n; ++i) a[i][j] /= scale[j];
  }

  for (std::size_t k = 0; k < p; ++k) {
    double norm = 0.0;
    for (std::size_t i = k; i < n; ++i) norm += a[i][k] * a[i][k];
    norm = std::sqrt(norm);
    if (norm == 0.0) return fail(ErrorKind::Config, "singular design matrix");
    const double alpha = a[k][k] > 0 ? -norm : norm;
    std::vector<double> v(n - k);
    for (std::size_t i = k; i < n; ++i) v[i - k] = a[i][k];
    v[0] -= alpha;
    double vnorm2 = 0.0;
    for (double vi : v) vnorm2 += vi * vi;
    if (vnorm2 > 0.0) {
      for (std::size_t j = k; j < p; ++j) {
        double dot = 0.0;
        for (std::size_t i = k; i < n; ++i) dot += v[i - k] * a[i][j];
        const double f = 2.0 * dot / vnorm2;
        for (std::size_t i = k; i < n; ++i) a[i][j] -= f * v[i - k];
      }
      double dot = 0.0;
      for (std::size_t i = k; i < n; ++i) dot += v[i - k] * y[i];
      const double f = 2.0 * dot / vnorm2;
      for (std::size_t i = k; i < n; ++i) y[i] -= f * v[i - k];
    }
  }

  double rmax = 0.0;
  for (std::size_t k = 0; k < p; ++k) rmax = std::max(rmax, std::abs(a[k][k]));
  for (std::size_t k = 0; k < p; ++k)
    if (std::abs(a[k][k]) <= 1e-12 * rmax) return fail(ErrorKind::Config, "singular design matrix");

  // Solve R z = Q'y and invert R (upper triangular).
  std::vector<double> z(p);
  Matrix rinv(p, std::vector<double>(p, 0.0));
  for (std::size_t kk = p; kk-- > 0;) {
    double s = y[kk];
    for (std::size_t j = kk + 1; j < p; ++j) s -= a[kk][j] * z[j];
    z[kk] = s / a[kk][kk];
    rinv[kk][kk] = 1.0 / a[kk][kk];
    for (std::size_t j = kk + 1; j < p; ++j) {
      double t = 0.0;
      for (std::size_t m = kk + 1; m <= j; ++m) t += a[kk][m] * rinv[m][j];
      rinv[kk][j] = -t / a[kk][kk];
    }
  }

  LeastSquares out;
  double rmin = rmax;
  for (std::size_t k = 0; k < p; ++k) rmin = std::min(rmin, std::abs(a[k][k]));
  out.pivot_ratio = rmin / rmax;
  out.beta.resize(p);
  out.cov_unscaled.assign(p, std::vector<double>(p, 0.0));
  for (std::size_t j = 0; j < p; ++j) out.beta[j] = z[j] / scale[j];
  for (std::size_t i = 0; i < p; ++i)
    for (std::size_t j = 0; j < p; ++j) {
      double c = 0.0;
      for (std::size_t m = std::max(i, j); m < p; ++m) c += rinv[i][m] * rinv[j][m];
      out.cov_unscaled[i][j] = c / (scale[i] * scale[j]);
    }
  return out;
}

}  // namespace pychron::reduction::detail
