#include "pychron/reduction/fits.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <set>
#include <string>

// Port of pychron.core.regression. Error model shared by every kind:
//   sef     = sqrt(SSR / (n - q))                 (BaseRegressor.calculate_standard_error_fit)
//   var_hat = g' (J'J)^-1 g, g = d f(0) / d params (OLSRegressor.predict_error_matrix)
//   SEM     = sef * sqrt(var_hat)
//   SD      = sef * sqrt(1 + var_hat)             (Average: SD = sef, MeanRegressor.std)
// Deviation: pychron's ExponentialRegressor.predict_error multiplies the
// covariance by Xk = [x, x, x], which is identically 0 at x = 0; here the
// exponential uses the delta method on a + c (g = [1, 0, 1]) like the OLS path.

namespace pychron::reduction {

namespace {

using Matrix = std::vector<std::vector<double>>;  // row-major, rows = observations

struct LeastSquares {
  std::vector<double> beta;
  Matrix cov_unscaled;  // (A'A)^-1
};

// Householder QR least squares with column equilibration. Fails when the
// design is rank deficient relative to double precision.
Result<LeastSquares> least_squares(Matrix a, std::vector<double> y) {
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

// A solved model on a subset of points.
struct Model {
  FitKind kind = FitKind::Linear;
  std::vector<double> params;  // poly: c0..cd; exponential: a, b, c
  double value = 0.0;          // f(0)
  double var_hat = 0.0;        // g'(J'J)^-1 g, unscaled
  double sef = 0.0;

  double predict(double x) const {
    if (kind == FitKind::Exponential) return params[0] * std::exp(-params[1] * x) + params[2];
    double r = 0.0;
    for (std::size_t i = params.size(); i-- > 0;) r = r * x + params[i];
    return r;
  }
};

double standard_error_fit(const Model& m, const std::vector<double>& x,
                          const std::vector<double>& y) {
  const std::size_t q = m.params.size();
  if (x.size() <= q) return 0.0;
  double ssr = 0.0;
  for (std::size_t i = 0; i < x.size(); ++i) {
    const double r = y[i] - m.predict(x[i]);
    ssr += r * r;
  }
  return std::sqrt(ssr / static_cast<double>(x.size() - q));
}

Result<Model> fit_poly(FitKind kind, int degree, const std::vector<double>& x,
                       const std::vector<double>& y) {
  const auto p = static_cast<std::size_t>(degree) + 1;
  Matrix a(x.size(), std::vector<double>(p));
  for (std::size_t i = 0; i < x.size(); ++i) {
    double xp = 1.0;
    for (std::size_t j = 0; j < p; ++j, xp *= x[i]) a[i][j] = xp;
  }
  auto ls = least_squares(std::move(a), y);
  if (!ls) return fail(ls.error());
  Model m;
  m.kind = kind;
  m.params = ls->beta;
  m.value = m.params[0];
  m.var_hat = ls->cov_unscaled[0][0];
  m.sef = standard_error_fit(m, x, y);
  return m;
}

// --- exponential: y = a exp(-b x) + c --------------------------------------

// For fixed b, (a, c) is linear. Returns SSR and fills a, c; +inf if singular.
double projected_ssr(double b, const std::vector<double>& x, const std::vector<double>& y,
                     double& a, double& c) {
  Matrix d(x.size(), std::vector<double>(2, 1.0));
  for (std::size_t i = 0; i < x.size(); ++i) d[i][0] = std::exp(-b * x[i]);
  auto ls = least_squares(std::move(d), y);
  if (!ls) return std::numeric_limits<double>::infinity();
  a = ls->beta[0];
  c = ls->beta[1];
  double ssr = 0.0;
  for (std::size_t i = 0; i < x.size(); ++i) {
    const double r = y[i] - (a * std::exp(-b * x[i]) + c);
    ssr += r * r;
  }
  return std::isfinite(ssr) ? ssr : std::numeric_limits<double>::infinity();
}

double ssr_of(const std::array<double, 3>& p, const std::vector<double>& x,
              const std::vector<double>& y) {
  double s = 0.0;
  for (std::size_t i = 0; i < x.size(); ++i) {
    const double r = y[i] - (p[0] * std::exp(-p[1] * x[i]) + p[2]);
    s += r * r;
  }
  return std::isfinite(s) ? s : std::numeric_limits<double>::infinity();
}

Matrix jacobian(const std::array<double, 3>& p, const std::vector<double>& x) {
  Matrix j(x.size(), std::vector<double>(3));
  for (std::size_t i = 0; i < x.size(); ++i) {
    const double e = std::exp(-p[1] * x[i]);
    j[i] = {e, -p[0] * x[i] * e, 1.0};
  }
  return j;
}

// pychron uses scipy curve_fit (Levenberg-Marquardt) from a data-driven guess.
// That guess is fragile, so this finds the same least-squares minimum more
// robustly: scan b by variable projection over |b|*max|x| <= 40, refine with
// golden section, then polish all three parameters with damped Gauss-Newton.
Result<Model> fit_exponential(const std::vector<double>& x, const std::vector<double>& y) {
  double xmax = 0.0;
  for (double xi : x) xmax = std::max(xmax, std::abs(xi));
  if (xmax == 0.0) return fail(ErrorKind::Config, "exponential fit needs non-zero x");

  constexpr int kSteps = 400;
  constexpr double kTMax = 40.0;
  const double dt = kTMax / kSteps;
  double best_t = 0.0, best = std::numeric_limits<double>::infinity(), a = 0.0, c = 0.0;
  for (int k = -kSteps; k <= kSteps; ++k) {
    if (k == 0) continue;  // b = 0 makes exp(-bx) collinear with the constant
    const double t = k * dt;
    const double s = projected_ssr(t / xmax, x, y, a, c);
    if (s < best) {
      best = s;
      best_t = t;
    }
  }
  if (!std::isfinite(best)) return fail(ErrorKind::Config, "exponential fit did not converge");

  // Golden section on the bracketing grid cells.
  const double inv_phi = (std::sqrt(5.0) - 1.0) / 2.0;
  double lo = best_t - dt, hi = best_t + dt;
  double t1 = hi - inv_phi * (hi - lo), t2 = lo + inv_phi * (hi - lo);
  double f1 = projected_ssr(t1 / xmax, x, y, a, c), f2 = projected_ssr(t2 / xmax, x, y, a, c);
  for (int it = 0; it < 200 && hi - lo > 1e-14 * std::max(1.0, std::abs(best_t)); ++it) {
    if (f1 < f2) {
      hi = t2;
      t2 = t1;
      f2 = f1;
      t1 = hi - inv_phi * (hi - lo);
      f1 = projected_ssr(t1 / xmax, x, y, a, c);
    } else {
      lo = t1;
      t1 = t2;
      f1 = f2;
      t2 = lo + inv_phi * (hi - lo);
      f2 = projected_ssr(t2 / xmax, x, y, a, c);
    }
  }
  const double t_opt = f1 < f2 ? t1 : t2;
  double cur = projected_ssr(t_opt / xmax, x, y, a, c);
  if (!std::isfinite(cur)) cur = projected_ssr(best_t / xmax, x, y, a, c);
  std::array<double, 3> p{a, std::isfinite(cur) ? t_opt / xmax : best_t / xmax, c};
  cur = ssr_of(p, x, y);

  for (int it = 0; it < 50; ++it) {
    std::vector<double> r(x.size());
    for (std::size_t i = 0; i < x.size(); ++i) r[i] = y[i] - (p[0] * std::exp(-p[1] * x[i]) + p[2]);
    auto step = least_squares(jacobian(p, x), r);
    if (!step) break;
    double lambda = 1.0;
    bool improved = false;
    for (int h = 0; h < 30; ++h, lambda *= 0.5) {
      std::array<double, 3> trial{p[0] + lambda * step->beta[0], p[1] + lambda * step->beta[1],
                                  p[2] + lambda * step->beta[2]};
      const double s = ssr_of(trial, x, y);
      if (s <= cur) {
        improved = s < cur;
        p = trial;
        cur = s;
        break;
      }
    }
    if (!improved) break;
  }

  auto ls = least_squares(jacobian(p, x), std::vector<double>(x.size(), 0.0));
  if (!ls) return fail(ErrorKind::Config, "exponential fit did not converge");
  const auto& cv = ls->cov_unscaled;  // g = [1, 0, 1]
  Model m;
  m.kind = FitKind::Exponential;
  m.params = {p[0], p[1], p[2]};
  m.value = p[0] + p[2];
  m.var_hat = cv[0][0] + cv[2][2] + 2.0 * cv[0][2];
  m.sef = standard_error_fit(m, x, y);
  return m;
}

int poly_degree(const FitSpec& spec) {
  switch (spec.kind) {
    case FitKind::Average: return 0;
    case FitKind::Linear: return 1;
    case FitKind::Parabolic: return 2;
    case FitKind::Cubic: return 3;
    case FitKind::CustomPoly: return spec.degree;
    case FitKind::Exponential: break;
  }
  return -1;
}

Result<Model> solve(const FitSpec& spec, const std::vector<double>& x,
                    const std::vector<double>& y) {
  if (spec.kind == FitKind::Exponential) return fit_exponential(x, y);
  return fit_poly(spec.kind, poly_degree(spec), x, y);
}

void subset(const Series& s, const std::set<std::size_t>& excluded, std::vector<double>& x,
            std::vector<double>& y) {
  x.clear();
  y.clear();
  for (std::size_t i = 0; i < s.x.size(); ++i) {
    if (excluded.count(i)) continue;
    x.push_back(s.x[i]);
    y.push_back(s.y[i]);
  }
}

}  // namespace

std::size_t parameter_count(const FitSpec& spec) noexcept {
  if (spec.kind == FitKind::Exponential) return 3;
  const int d = poly_degree(spec);
  return d < 0 ? 0 : static_cast<std::size_t>(d) + 1;
}

Result<Intercept> fit(const Series& series, const FitSpec& spec) {
  if (series.x.size() != series.y.size())
    return fail(ErrorKind::Config, "x and y have different lengths");
  if (spec.kind == FitKind::CustomPoly && spec.degree < 0)
    return fail(ErrorKind::Config, "custom_poly degree must be >= 0");
  for (std::size_t i = 0; i < series.x.size(); ++i)
    if (!std::isfinite(series.x[i]) || !std::isfinite(series.y[i]))
      return fail(ErrorKind::Config, "series contains a non-finite value");
  const std::size_t q = parameter_count(spec);
  if (series.x.size() < q)
    return fail(ErrorKind::Config, "need at least " + std::to_string(q) + " points for " +
                                       std::string(to_string(spec.kind)) + " fit");

  // BaseRegressor.calculate_filtered_data: each iteration fits the kept points
  // and flags every point with |y - f(x)| >= std_devs * sef. Deviations: a zero
  // bound flags nothing, and an iteration that would leave < q points is dropped.
  std::set<std::size_t> excluded;
  std::vector<double> x, y;
  if (spec.outliers.enabled) {
    for (int it = 0; it < spec.outliers.iterations; ++it) {
      subset(series, excluded, x, y);
      auto m = solve(spec, x, y);
      if (!m) return fail(m.error());
      const double bound = m->sef * spec.outliers.std_devs;
      if (!(bound > 0.0)) break;
      std::set<std::size_t> next = excluded;
      for (std::size_t i = 0; i < series.x.size(); ++i)
        if (std::abs(series.y[i] - m->predict(series.x[i])) >= bound) next.insert(i);
      if (series.x.size() - next.size() < q || next == excluded) break;
      excluded = std::move(next);
    }
  }

  subset(series, excluded, x, y);
  auto m = solve(spec, x, y);
  if (!m) return fail(m.error());

  Intercept out;
  out.value = m->value;
  out.residual_sd = m->sef;
  out.n_used = x.size();
  out.filtered_idx.assign(excluded.begin(), excluded.end());
  if (spec.error == ErrorType::Sem)
    out.error = m->sef * std::sqrt(m->var_hat);
  else if (spec.kind == FitKind::Average)
    out.error = m->sef;
  else
    out.error = m->sef * std::sqrt(1.0 + m->var_hat);
  return out;
}

std::string_view to_string(FitKind kind) noexcept {
  switch (kind) {
    case FitKind::Average: return "average";
    case FitKind::Linear: return "linear";
    case FitKind::Parabolic: return "parabolic";
    case FitKind::Cubic: return "cubic";
    case FitKind::Exponential: return "exponential";
    case FitKind::CustomPoly: return "custom_poly";
  }
  return "unknown";
}

std::optional<FitKind> parse_fit_kind(std::string_view name) noexcept {
  for (auto k : {FitKind::Average, FitKind::Linear, FitKind::Parabolic, FitKind::Cubic,
                 FitKind::Exponential, FitKind::CustomPoly})
    if (to_string(k) == name) return k;
  return std::nullopt;
}

}  // namespace pychron::reduction
