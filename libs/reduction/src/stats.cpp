#include "pychron/reduction/stats.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <numbers>
#include <string>

namespace pychron::reduction {

namespace {

Unexpected<Error> stats_fail(std::string what) { return fail(ErrorKind::Config, "stats: " + std::move(what)); }

bool usable_error(double e) noexcept { return std::isfinite(e) && e != 0.0; }

std::string lower(std::string_view s) {
  std::string out(s);
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

double apply_kind(double sem, double sd, double mswd_value, MeanErrorKind kind) noexcept {
  switch (kind) {
    case MeanErrorKind::Sd:
      return sd;
    case MeanErrorKind::Sem:
      return sem;
    case MeanErrorKind::Msem:
      return mswd_value > 1.0 ? sem * std::sqrt(mswd_value) : sem;
  }
  return sem;
}

// Regularized lower incomplete gamma P(a, x) (Numerical Recipes gammp).
double gamma_p(double a, double x) {
  if (x <= 0.0 || a <= 0.0) return 0.0;
  const double gln = std::lgamma(a);
  if (x < a + 1.0) {
    double ap = a, sum = 1.0 / a, del = sum;
    for (int i = 0; i < 1000; ++i) {
      ap += 1.0;
      del *= x / ap;
      sum += del;
      if (std::abs(del) < std::abs(sum) * 1e-16) break;
    }
    return sum * std::exp(-x + a * std::log(x) - gln);
  }
  // Continued fraction for Q, modified Lentz.
  constexpr double kTiny = 1e-300;
  double b = x + 1.0 - a, c = 1.0 / kTiny, d = 1.0 / b, h = d;
  for (int i = 1; i < 1000; ++i) {
    const double an = -i * (i - a);
    b += 2.0;
    d = an * d + b;
    if (std::abs(d) < kTiny) d = kTiny;
    c = b + an / c;
    if (std::abs(c) < kTiny) c = kTiny;
    d = 1.0 / d;
    const double del = d * c;
    h *= del;
    if (std::abs(del - 1.0) < 1e-16) break;
  }
  return 1.0 - std::exp(-x + a * std::log(x) - gln) * h;
}

// Continued fraction of the incomplete beta (Numerical Recipes betacf).
double beta_cf(double a, double b, double x) {
  constexpr double kTiny = 1e-300;
  const double qab = a + b, qap = a + 1.0, qam = a - 1.0;
  double c = 1.0, d = 1.0 - qab * x / qap;
  if (std::abs(d) < kTiny) d = kTiny;
  d = 1.0 / d;
  double h = d;
  for (int m = 1; m <= 1000; ++m) {
    const double m2 = 2.0 * m;
    double aa = m * (b - m) * x / ((qam + m2) * (a + m2));
    d = 1.0 + aa * d;
    if (std::abs(d) < kTiny) d = kTiny;
    c = 1.0 + aa / c;
    if (std::abs(c) < kTiny) c = kTiny;
    d = 1.0 / d;
    h *= d * c;
    aa = -(a + m) * (qab + m) * x / ((a + m2) * (qap + m2));
    d = 1.0 + aa * d;
    if (std::abs(d) < kTiny) d = kTiny;
    c = 1.0 + aa / c;
    if (std::abs(c) < kTiny) c = kTiny;
    d = 1.0 / d;
    const double del = d * c;
    h *= del;
    if (std::abs(del - 1.0) < 1e-16) break;
  }
  return h;
}

// Regularized incomplete beta I_x(a, b).
double beta_i(double a, double b, double x) {
  if (x <= 0.0) return 0.0;
  if (x >= 1.0) return 1.0;
  const double front =
      std::exp(std::lgamma(a + b) - std::lgamma(a) - std::lgamma(b) + a * std::log(x) + b * std::log1p(-x));
  if (x < (a + 1.0) / (a + b + 2.0)) return front * beta_cf(a, b, x) / a;
  return 1.0 - front * beta_cf(b, a, 1.0 - x) / b;
}

}  // namespace

// ---------------------------------------------------------------- means

std::string_view to_string(MeanErrorKind kind) noexcept {
  switch (kind) {
    case MeanErrorKind::Sd:
      return "sd";
    case MeanErrorKind::Sem:
      return "sem";
    case MeanErrorKind::Msem:
      return "msem";
  }
  return "sem";
}

std::optional<MeanErrorKind> parse_mean_error_kind(std::string_view text) noexcept {
  const std::string s = lower(text);
  if (s == "sd") return MeanErrorKind::Sd;
  if (s == "sem" || s == "se") return MeanErrorKind::Sem;
  if (s == "msem" || s == "mse") return MeanErrorKind::Msem;
  return std::nullopt;
}

double mswd(std::span<const double> values, std::span<const double> errors, double mean, int k) {
  double ssw = 0.0;
  std::size_t n = 0;
  for (std::size_t i = 0; i < values.size() && i < errors.size(); ++i) {
    if (!usable_error(errors[i])) continue;
    const double r = (values[i] - mean) / errors[i];
    ssw += r * r;
    ++n;
  }
  if (static_cast<long>(n) <= k) return 0.0;
  return ssw / static_cast<double>(static_cast<long>(n) - k);
}

Result<Mean> weighted_mean(std::span<const double> values, std::span<const double> errors, MeanErrorKind kind) {
  if (values.size() != errors.size()) return stats_fail("weighted mean: values and errors differ in length");
  double sw = 0.0, swx = 0.0;
  std::vector<double> v, e;
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (!usable_error(errors[i]) || !std::isfinite(values[i])) continue;
    const double w = 1.0 / (errors[i] * errors[i]);
    sw += w;
    swx += w * values[i];
    v.push_back(values[i]);
    e.push_back(errors[i]);
  }
  if (v.empty()) return stats_fail("weighted mean: no value with a non-zero error");
  Mean m;
  m.n = v.size();
  m.value = swx / sw;
  m.sem = 1.0 / std::sqrt(sw);
  if (m.n == 1) {
    m.sd = e[0];
  } else {
    double ss = 0.0;
    for (double x : v) ss += (m.value - x) * (m.value - x);
    m.sd = std::sqrt(ss / static_cast<double>(m.n - 1));
  }
  m.mswd = mswd(v, e, m.value);
  m.mswd_acceptable = mswd_acceptable(m.mswd, m.n);
  m.error = apply_kind(m.sem, m.sd, m.mswd, kind);
  return m;
}

Result<Mean> arithmetic_mean(std::span<const double> values, std::span<const double> errors, MeanErrorKind kind) {
  if (!errors.empty() && values.size() != errors.size())
    return stats_fail("arithmetic mean: values and errors differ in length");
  std::vector<double> v, e;
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (!std::isfinite(values[i])) continue;
    v.push_back(values[i]);
    if (!errors.empty()) e.push_back(errors[i]);
  }
  if (v.empty()) return stats_fail("arithmetic mean: no finite value");
  Mean m;
  m.n = v.size();
  double s = 0.0;
  for (double x : v) s += x;
  m.value = s / static_cast<double>(m.n);
  if (m.n > 1) {
    double ss = 0.0;
    for (double x : v) ss += (x - m.value) * (x - m.value);
    m.sd = std::sqrt(ss / static_cast<double>(m.n - 1));
  } else if (!e.empty()) {
    m.sd = e[0];
  }
  m.sem = m.sd / std::sqrt(static_cast<double>(m.n));
  if (!e.empty()) {
    m.mswd = mswd(v, e, m.value);
    m.mswd_acceptable = mswd_acceptable(m.mswd, m.n);
  }
  m.error = apply_kind(m.sem, m.sd, m.mswd, kind);
  return m;
}

// ---------------------------------------------------------------- chi-squared

double chi2_cdf(double x, double dof) { return gamma_p(dof / 2.0, x / 2.0); }

double chi2_sf(double x, double dof) {
  if (x <= 0.0) return 1.0;
  return std::clamp(1.0 - chi2_cdf(x, dof), 0.0, 1.0);
}

double chi2_quantile(double p, double dof) {
  if (!(p > 0.0) || !(p < 1.0) || !(dof > 0.0)) return std::numeric_limits<double>::quiet_NaN();
  double lo = 0.0, hi = std::max(1.0, dof);
  while (chi2_cdf(hi, dof) < p) hi *= 2.0;
  for (int i = 0; i < 200; ++i) {
    const double mid = 0.5 * (lo + hi);
    if (chi2_cdf(mid, dof) < p)
      lo = mid;
    else
      hi = mid;
    if (hi - lo <= 1e-14 * hi) break;
  }
  return 0.5 * (lo + hi);
}

double student_t_cdf(double t, double dof) {
  if (!(dof > 0.0) || std::isnan(t)) return std::numeric_limits<double>::quiet_NaN();
  const double tail = 0.5 * beta_i(dof / 2.0, 0.5, dof / (dof + t * t));
  return t >= 0.0 ? 1.0 - tail : tail;
}

double student_t_quantile(double p, double dof) {
  if (!(p > 0.0) || !(p < 1.0) || !(dof > 0.0)) return std::numeric_limits<double>::quiet_NaN();
  if (p == 0.5) return 0.0;
  // Symmetric: solve for the upper tail, bisecting on [0, hi].
  const double q = p > 0.5 ? p : 1.0 - p;
  double lo = 0.0, hi = 1.0;
  while (student_t_cdf(hi, dof) < q) hi *= 2.0;
  for (int i = 0; i < 300; ++i) {
    const double mid = 0.5 * (lo + hi);
    if (student_t_cdf(mid, dof) < q)
      lo = mid;
    else
      hi = mid;
    if (hi - lo <= 1e-14 * hi) break;
  }
  const double t = 0.5 * (lo + hi);
  return p > 0.5 ? t : -t;
}

std::pair<double, double> mswd_limits(std::size_t n, int k) {
  const long dof = static_cast<long>(n) - k;
  if (dof <= 0) return {0.0, 0.0};
  const double d = static_cast<double>(dof);
  return {chi2_quantile(0.025, d) / d, chi2_quantile(0.975, d) / d};
}

bool mswd_acceptable(double mswd_value, std::size_t n, int k) {
  if (static_cast<long>(n) <= k) return false;
  const auto [low, high] = mswd_limits(n, k);
  return low <= mswd_value && mswd_value <= high;
}

double mswd_probability(double mswd_value, int dof) {
  if (dof <= 0) return 0.0;
  return chi2_sf(mswd_value * dof, dof);
}

// ---------------------------------------------------------------- curves

Curve cumulative_probability(std::span<const double> values, std::span<const double> errors, double xmin,
                             double xmax, std::size_t n) {
  Curve c;
  if (n == 0) return c;
  c.x.resize(n);
  c.y.assign(n, 0.0);
  for (std::size_t i = 0; i < n; ++i)
    c.x[i] = n == 1 ? xmin : xmin + (xmax - xmin) * static_cast<double>(i) / static_cast<double>(n - 1);
  for (std::size_t j = 0; j < values.size() && j < errors.size(); ++j) {
    const double a = values[j], e = errors[j];
    if (std::abs(a) < 1e-10 || std::abs(e) < 1e-10 || !std::isfinite(a) || !std::isfinite(e)) continue;
    const double es2 = 2.0 * e * e;
    const double norm = 1.0 / std::sqrt(es2 * std::numbers::pi);
    for (std::size_t i = 0; i < n; ++i) {
      const double d = c.x[i] - a;
      c.y[i] += norm * std::exp(-d * d / es2);
    }
  }
  return c;
}

Curve kernel_density(std::span<const double> values, double xmin, double xmax, std::size_t n) {
  Curve c;
  std::vector<double> v;
  for (double x : values)
    if (std::isfinite(x)) v.push_back(x);
  if (v.size() < 2 || n == 0) return c;
  double mean = 0;
  for (double x : v) mean += x;
  mean /= static_cast<double>(v.size());
  double ss = 0;
  for (double x : v) ss += (x - mean) * (x - mean);
  const double sd = std::sqrt(ss / static_cast<double>(v.size() - 1));
  if (!(sd > 0)) return c;
  const double bw = std::pow(static_cast<double>(v.size()), -0.2) * sd;
  const double norm = 1.0 / (static_cast<double>(v.size()) * bw * std::sqrt(2.0 * std::numbers::pi));
  c.x.resize(n);
  c.y.assign(n, 0.0);
  for (std::size_t i = 0; i < n; ++i) {
    c.x[i] = n == 1 ? xmin : xmin + (xmax - xmin) * static_cast<double>(i) / static_cast<double>(n - 1);
    for (double x : v) {
      const double z = (c.x[i] - x) / bw;
      c.y[i] += norm * std::exp(-0.5 * z * z);
    }
  }
  return c;
}

// ---------------------------------------------------------------- plateaus

namespace {

bool is_excluded(std::span<const bool> excluded, std::size_t i) { return i < excluded.size() && excluded[i]; }

}  // namespace

std::optional<StepRange> find_plateau(std::span<const double> ages, std::span<const double> errors,
                                      std::span<const double> gas, std::span<const bool> excluded,
                                      const PlateauCriteria& c) {
  const std::size_t n = std::min({ages.size(), errors.size(), gas.size()});
  double total = 0.0;
  for (std::size_t i = 0; i < n; ++i)
    if (!is_excluded(excluded, i)) total += gas[i];
  if (n == 0 || total == 0.0) return std::nullopt;

  auto overlap = [&](std::size_t i, std::size_t j) {
    const double e1 = errors[i] * c.overlap_sigma, e2 = errors[j] * c.overlap_sigma;
    return ages[i] - e1 < ages[j] + e2 && ages[i] + e1 > ages[j] - e2;
  };

  std::optional<StepRange> best;
  for (std::size_t start = 0; start < n; ++start) {
    if (is_excluded(excluded, start)) continue;
    std::optional<std::size_t> end;
    std::vector<std::size_t> included;  // included steps in [start, i]
    double signal = 0.0;
    for (std::size_t i = start; i < n; ++i) {
      if (is_excluded(excluded, i)) continue;
      // The new step must overlap every included step before it.
      if (c.method == PlateauMethod::Fleck) {
        bool ok = true;
        for (std::size_t j : included)
          if (!overlap(j, i)) ok = false;
        if (!ok) break;
      }
      included.push_back(i);
      signal += gas[i];
      if (static_cast<int>(included.size()) < c.nsteps) continue;
      if (c.method == PlateauMethod::Mahon) {
        std::vector<double> a, e;
        for (std::size_t j : included) {
          a.push_back(ages[j]);
          e.push_back(errors[j]);
        }
        auto wm = weighted_mean(a, e);
        if (!wm || !wm->mswd_acceptable) continue;
      }
      if (signal / total * 100.0 < c.gas_fraction) continue;
      end = i;
    }
    if (end && *end > start && (!best || *end - start > best->last - best->first)) best = StepRange{start, *end};
  }
  return best;
}

Result<PlateauMean> plateau_mean(std::span<const double> ages, std::span<const double> errors,
                                 std::span<const double> gas, std::span<const bool> excluded, StepRange range,
                                 PlateauWeighting weighting, MeanErrorKind kind) {
  const std::size_t n = std::min({ages.size(), errors.size(), gas.size()});
  if (range.first > range.last || range.last >= n) return stats_fail("plateau: step range out of bounds");
  std::vector<double> a, e, w;
  double total = 0.0, in_range = 0.0;
  for (std::size_t i = 0; i < n; ++i) {
    if (is_excluded(excluded, i)) continue;
    total += gas[i];
    if (i < range.first || i > range.last) continue;
    in_range += gas[i];
    a.push_back(ages[i]);
    e.push_back(errors[i]);
    w.push_back(gas[i]);
  }
  if (a.empty()) return stats_fail("plateau: no included step in range");
  PlateauMean out;
  out.nsteps = a.size();
  out.gas_fraction = total != 0.0 ? in_range / total * 100.0 : 0.0;
  auto wm = weighted_mean(a, e, kind);
  if (!wm) return fail(wm.error());
  out.mean = *wm;
  if (weighting == PlateauWeighting::VolumeFraction) {
    double sw = 0.0, swa = 0.0, sw2e2 = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) {
      sw += w[i];
      swa += w[i] * a[i];
      sw2e2 += w[i] * w[i] * e[i] * e[i];
    }
    if (sw == 0.0) return stats_fail("plateau: zero total signal");
    out.mean.value = swa / sw;
    out.mean.sem = std::sqrt(sw2e2) / sw;
    out.mean.mswd = mswd(a, e, out.mean.value);
    out.mean.mswd_acceptable = mswd_acceptable(out.mean.mswd, out.mean.n);
    out.mean.error = apply_kind(out.mean.sem, out.mean.sd, out.mean.mswd, kind);
  }
  return out;
}

// ---------------------------------------------------------------- York

std::string_view to_string(YorkMethod method) noexcept {
  switch (method) {
    case YorkMethod::York:
      return "york";
    case YorkMethod::NewYork:
      return "new_york";
    case YorkMethod::Reed:
      return "reed";
  }
  return "new_york";
}

std::optional<YorkMethod> parse_york_method(std::string_view text) noexcept {
  const std::string s = lower(text);
  if (s == "york") return YorkMethod::York;
  if (s == "new_york" || s == "newyork") return YorkMethod::NewYork;
  if (s == "reed") return YorkMethod::Reed;
  return std::nullopt;
}

Result<YorkFit> york_fit(std::span<const XyPoint> pts, YorkMethod method, int max_iterations, double tolerance) {
  const std::size_t n = pts.size();
  if (n < 3) return stats_fail("york: needs at least 3 points");
  std::vector<double> x(n), y(n), vx(n), vy(n), sxy(n);
  bool all_same_x = true;
  for (std::size_t i = 0; i < n; ++i) {
    const auto& p = pts[i];
    if (!std::isfinite(p.x) || !std::isfinite(p.y)) return stats_fail("york: non-finite point");
    if (!(p.sx > 0.0) || !(p.sy > 0.0) || !std::isfinite(p.sx) || !std::isfinite(p.sy))
      return stats_fail("york: sigma must be positive and finite");
    if (!(std::abs(p.rho) < 1.0)) return stats_fail("york: |rho| must be < 1");
    const double rho = method == YorkMethod::Reed ? 0.0 : p.rho;
    x[i] = p.x;
    y[i] = p.y;
    vx[i] = p.sx * p.sx;
    vy[i] = p.sy * p.sy;
    sxy[i] = rho * p.sx * p.sy;
    if (p.x != pts[0].x) all_same_x = false;
  }
  if (all_same_x) return stats_fail("york: all x are equal");

  std::vector<double> W(n), U(n), V(n);
  double xbar = 0, ybar = 0;
  auto weights = [&](double b) {
    double sw = 0, swx = 0, swy = 0;
    for (std::size_t i = 0; i < n; ++i) {
      W[i] = 1.0 / (vy[i] + b * b * vx[i] - 2.0 * b * sxy[i]);
      sw += W[i];
      swx += W[i] * x[i];
      swy += W[i] * y[i];
    }
    xbar = swx / sw;
    ybar = swy / sw;
    for (std::size_t i = 0; i < n; ++i) {
      U[i] = x[i] - xbar;
      V[i] = y[i] - ybar;
    }
  };

  YorkFit f;
  f.n = n;
  double b = 0.0, prev = std::numeric_limits<double>::infinity();
  int it = 0;
  while (std::abs(prev - b) >= tolerance && it < max_iterations) {
    weights(b);
    double sum_a = 0, sum_b = 0;
    for (std::size_t i = 0; i < n; ++i) {
      const double common = U[i] * vy[i] + b * V[i] * vx[i];
      sum_a += W[i] * W[i] * V[i] * (common - V[i] * sxy[i]);
      sum_b += W[i] * W[i] * U[i] * (common - b * U[i] * sxy[i]);
    }
    if (sum_b == 0.0) break;
    prev = b;
    b = sum_a / sum_b;
    ++it;
  }
  f.iterations = it;
  f.converged = std::abs(prev - b) < tolerance;
  if (!std::isfinite(b)) return stats_fail("york: slope did not converge");
  weights(b);
  f.slope = b;
  f.intercept = ybar - b * xbar;

  double sw = 0, swu2 = 0, swx2 = 0;
  for (std::size_t i = 0; i < n; ++i) {
    sw += W[i];
    swu2 += W[i] * U[i] * U[i];
    swx2 += W[i] * x[i] * x[i];
  }
  double var_b = 0, var_a = 0;
  switch (method) {
    case YorkMethod::York:
      var_b = 1.0 / swu2;
      var_a = var_b * swx2 / sw;
      break;
    case YorkMethod::NewYork: {
      // Mahon 1996 (legacy NewYorkRegressor.get_slope_variance).
      double dthdb = 0, Sx = 0, Sxx = 0;
      std::vector<double> xt(n), xxt(n);
      for (std::size_t i = 0; i < n; ++i) {
        const double aa = 2 * b * (U[i] * V[i] * vx[i] - U[i] * U[i] * sxy[i]);
        const double bb = U[i] * U[i] * vy[i] - V[i] * V[i] * vx[i];
        const double cc = W[i] * W[i] * W[i] * (sxy[i] - b * vx[i]);
        const double dd = b * b * (U[i] * V[i] * vx[i] - U[i] * U[i] * sxy[i]) +
                          b * (U[i] * U[i] * vy[i] - V[i] * V[i] * vx[i]) -
                          (U[i] * V[i] * vy[i] - V[i] * V[i] * sxy[i]);
        dthdb += W[i] * W[i] * (aa + bb) + 4 * cc * dd;
        xt[i] = b * b * (V[i] * vx[i] - 2 * U[i] * sxy[i]) + 2 * b * U[i] * vy[i] - V[i] * vy[i];
        xxt[i] = b * b * U[i] * vx[i] + 2 * V[i] * sxy[i] - 2 * b * V[i] * vx[i] - U[i] * vy[i];
        Sx += W[i] * W[i] * xt[i];
        Sxx += W[i] * W[i] * xxt[i];
      }
      for (std::size_t i = 0; i < n; ++i) {
        const double ww = W[i] / sw;
        const double dthdx = W[i] * W[i] * xt[i] - ww * Sx;
        const double dthdy = W[i] * W[i] * xxt[i] - ww * Sxx;
        const double dadx = -b * ww - xbar * dthdx / dthdb;
        const double dady = ww - xbar * dthdy / dthdb;
        var_b += dthdx * dthdx * vx[i] + dthdy * dthdy * vy[i] + 2 * sxy[i] * dthdx * dthdy;
        var_a += dadx * dadx * vx[i] + dady * dady * vy[i] + 2 * sxy[i] * dadx * dady;
      }
      var_b /= dthdb * dthdb;
      break;
    }
    case YorkMethod::Reed: {
      double sum_a = 0;
      for (std::size_t i = 0; i < n; ++i) sum_a += W[i] * (b * U[i] - V[i]) * (b * U[i] - V[i]);
      var_b = swu2 == 0 ? 0 : sum_a / (swu2 * static_cast<double>(n - 2));
      var_a = var_b * swx2 / sw;
      break;
    }
  }
  f.slope_err = std::sqrt(std::max(0.0, var_b));
  f.intercept_err = std::sqrt(std::max(0.0, var_a));
  f.covariance = -xbar * var_b;

  if (b != 0.0) {
    f.x_intercept = -f.intercept / b;
    const double da = -1.0 / b, db = f.intercept / (b * b);
    const double v = da * da * var_a + db * db * var_b + 2 * da * db * f.covariance;
    f.x_intercept_err = std::sqrt(std::max(0.0, v));
  }

  double chi2 = 0;
  for (std::size_t i = 0; i < n; ++i) {
    const double r = y[i] - (f.intercept + b * x[i]);
    chi2 += r * r / (vy[i] + b * b * vx[i] - 2 * b * sxy[i]);
  }
  f.mswd = chi2 / static_cast<double>(n - 2);
  f.mswd_acceptable = mswd_acceptable(f.mswd, n, 2);
  f.probability = mswd_probability(f.mswd, static_cast<int>(n) - 2);
  return f;
}

}  // namespace pychron::reduction
