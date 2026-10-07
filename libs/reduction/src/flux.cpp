#include "pychron/reduction/flux.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <numeric>

#include "least_squares.hpp"

namespace pychron::reduction {

Result<UFloat> j_of(const UFloat& f, const MonitorConstants& monitor) {
  const double f0 = f.nominal();
  if (!std::isfinite(f0) || f0 <= 0.0) return fail(ErrorKind::Config, "flux: F must be positive and finite");
  const double numerator = std::exp(monitor.lambda_k * monitor.age_a) - 1.0;
  return numerator / f;
}

std::string_view to_string(MeanKind kind) noexcept {
  switch (kind) {
    case MeanKind::Arithmetic:
      return "arithmetic";
    case MeanKind::Weighted:
      return "weighted";
  }
  return "arithmetic";
}

std::optional<MeanKind> parse_mean_kind(std::string_view text) noexcept {
  std::string s(text);
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (s == "arithmetic") return MeanKind::Arithmetic;
  if (s == "weighted") return MeanKind::Weighted;
  return std::nullopt;
}

Result<PositionMean> mean_j(std::span<const MonitorAnalysis> analyses, const MonitorConstants& monitor,
                            MeanKind kind, MeanErrorKind error) {
  std::vector<double> values, errors;
  PositionMean out;
  for (const MonitorAnalysis& a : analyses) {
    if (a.omitted) continue;
    auto j = j_of(a.f, monitor);
    const bool bad_error =
        j && kind == MeanKind::Weighted && (!std::isfinite(j->std_dev()) || j->std_dev() == 0.0);
    if (!j || bad_error) {
      out.rejected.push_back(a.record_id);
      continue;
    }
    values.push_back(j->nominal());
    errors.push_back(j->std_dev());
  }
  if (values.empty()) return fail(ErrorKind::Config, "flux: no analysis with a usable J is left");
  auto mean = kind == MeanKind::Weighted ? weighted_mean(values, errors, error)
                                         : arithmetic_mean(values, errors, error);
  if (!mean) return fail(mean.error());
  out.j = mean->value;
  out.j_err = mean->error;
  out.mswd = mean->mswd;
  out.mswd_acceptable = mean->mswd_acceptable;
  out.n = static_cast<int>(mean->n);
  return out;
}

// ---- Flux models ------------------------------------------------------------

namespace {

const char* model_name(ModelKind kind) {
  switch (kind) {
    case ModelKind::Plane: return "plane";
    case ModelKind::Bowl: return "bowl";
    case ModelKind::WeightedMean: return "weighted mean";
    case ModelKind::Matching: return "matching";
    case ModelKind::NearestNeighbors: return "nearest neighbors";
    case ModelKind::Bracketing: return "bracketing";
    case ModelKind::LeastSquares1D: return "least squares 1D";
    case ModelKind::WeightedMean1D: return "weighted mean 1D";
    case ModelKind::Bracketing1D: return "bracketing 1D";
  }
  return "model";
}

// Models whose result uses the monitors' errors as weights.
bool uses_errors(const FitOptions& o) {
  switch (o.kind) {
    case ModelKind::WeightedMean:
    case ModelKind::WeightedMean1D:
    case ModelKind::NearestNeighbors:
      return true;
    case ModelKind::Bracketing:
      return o.interpolation != Interpolation::Average;
    case ModelKind::Bracketing1D:  // always linear
      return true;
    default:
      return false;
  }
}

double coord(const Point& p, Axis axis) { return axis == Axis::X ? p.x : p.y; }

// Interpolates between m0 and m1 at fraction f of the way from m0 to m1.
Predicted interpolate(const Monitor& m0, const Monitor& m1, double f, Interpolation how) {
  switch (how) {
    case Interpolation::Linear: {
      const double a = (1.0 - f) * m0.j_err, b = f * m1.j_err;
      return {m0.j + f * (m1.j - m0.j), std::sqrt(a * a + b * b)};
    }
    case Interpolation::Average: {
      const double mean = 0.5 * (m0.j + m1.j);
      const double d0 = m0.j - mean, d1 = m1.j - mean;
      return {mean, std::sqrt(d0 * d0 + d1 * d1)};  // sample sd, n - 1 == 1
    }
    case Interpolation::WeightedMean: {
      const double w0 = 1.0 / (m0.j_err * m0.j_err), w1 = 1.0 / (m1.j_err * m1.j_err);
      return {(m0.j * w0 + m1.j * w1) / (w0 + w1), 1.0 / std::sqrt(w0 + w1)};
    }
  }
  return {};
}

// ---- Least-squares surfaces (design section 5.3) ----------------------------
//
// J = X beta over the monitors, with X the plane [x, y, 1], the bowl
// [x^2, y^2, x, y, 1] or the powers of one coordinate, highest first. Weighted:
// rows scaled by 1 / j_err, so the solver's covariance is C = (X'WX)^-1.
// With s2 = r'Wr / (n - q) and g the design row of a position:
//   weighted   Sem  var = g C g'       Msem  var = g C g' max(s2, 1)
//   unweighted both var = s2 g C g'    (W = identity)
// The reported mswd is always sum((r / j_err)^2) / (n - q).

std::vector<double> design_row(const FitOptions& o, const Point& p) {
  switch (o.kind) {
    case ModelKind::Plane: return {p.x, p.y, 1.0};
    case ModelKind::Bowl: return {p.x * p.x, p.y * p.y, p.x, p.y, 1.0};
    default: {
      const double t = coord(p, o.axis);
      std::vector<double> row(static_cast<std::size_t>(o.degree) + 1, 1.0);
      for (std::size_t k = row.size() - 1; k-- > 0;) row[k] = row[k + 1] * t;
      return row;
    }
  }
}

Result<FluxFit> fit_surface(std::span<const Monitor> monitors, std::span<const Point> predict_at,
                            const FitOptions& options, const std::string& name) {
  const std::size_t n = monitors.size();
  detail::Matrix a;
  std::vector<double> y;
  for (const Monitor& m : monitors) {
    auto row = design_row(options, m.at);
    double yi = m.j;
    if (options.weighted) {
      for (double& v : row) v /= m.j_err;
      yi /= m.j_err;
    }
    a.push_back(std::move(row));
    y.push_back(yi);
  }
  const std::size_t q = a.front().size();
  auto undetermined = [&] { return fail(ErrorKind::Config, "flux: monitor positions do not determine a " + name); };
  auto solved = detail::least_squares(a, y);
  if (!solved) return undetermined();
  const auto& beta = solved->beta;
  for (double b : beta)
    if (!std::isfinite(b)) return undetermined();

  // Residuals against the unscaled model.
  double ssr = 0.0;  // r'Wr
  double sum_z2 = 0.0;
  std::size_t used = 0;
  for (std::size_t i = 0; i < n; ++i) {
    const auto row = design_row(options, monitors[i].at);
    double fit = 0.0;
    for (std::size_t k = 0; k < q; ++k) fit += row[k] * beta[k];
    const double r = monitors[i].j - fit;
    const double e = monitors[i].j_err;
    ssr += options.weighted ? (r / e) * (r / e) : r * r;
    if (e > 0.0) {  // an unweighted fit may carry a monitor with no error; it is not in the mswd
      sum_z2 += (r / e) * (r / e);
      ++used;
    }
  }
  const double dof = static_cast<double>(n - q);
  const double s2 = ssr / dof;

  FluxFit out;
  out.parameters = beta;
  out.dof = static_cast<int>(n - q);
  out.mswd = used > q ? sum_z2 / static_cast<double>(used - q) : 0.0;
  for (const Point& p : predict_at) {
    const auto g = design_row(options, p);
    double var = 0.0;
    double value = 0.0;
    for (std::size_t i = 0; i < q; ++i) {
      value += g[i] * beta[i];
      for (std::size_t k = 0; k < q; ++k) var += g[i] * solved->cov_unscaled[i][k] * g[k];
    }
    if (options.weighted) {
      if (options.error == MeanErrorKind::Msem) var *= std::max(s2, 1.0);
    } else {
      var *= s2;
    }
    if (!std::isfinite(value) || !std::isfinite(var) || var < 0.0) return undetermined();
    out.at.push_back({value, std::sqrt(var)});
  }
  if (!predict_at.empty() && used > q && !mswd_acceptable(out.mswd, used, static_cast<int>(q)))
    out.notes.push_back({0, FitNote::MswdOutsideLimits});
  return out;
}

}  // namespace

bool is_least_squares(ModelKind kind) noexcept {
  return kind == ModelKind::Plane || kind == ModelKind::Bowl || kind == ModelKind::LeastSquares1D;
}

std::size_t minimum_monitors(const FitOptions& options) {
  switch (options.kind) {
    case ModelKind::Plane: return 4;
    case ModelKind::Bowl: return 6;
    case ModelKind::LeastSquares1D: return static_cast<std::size_t>(std::max(options.degree, 1)) + 2;
    case ModelKind::WeightedMean:
    case ModelKind::WeightedMean1D:
    case ModelKind::Matching: return 1;
    case ModelKind::NearestNeighbors: return static_cast<std::size_t>(std::max(options.n_neighbors, 1));
    case ModelKind::Bracketing:
    case ModelKind::Bracketing1D: return 2;
  }
  return 1;
}

Result<FluxFit> fit_flux(std::span<const Monitor> monitors, std::span<const Point> predict_at,
                         const FitOptions& options) {
  const std::string name = model_name(options.kind);
  if (options.kind == ModelKind::NearestNeighbors && options.n_neighbors < 1)
    return fail(ErrorKind::Config, "flux: nearest neighbors needs at least 1 neighbor");
  if (options.kind == ModelKind::LeastSquares1D && (options.degree < 1 || options.degree > 4))
    return fail(ErrorKind::Config, "flux: least squares 1D degree must be 1 to 4");

  if (is_least_squares(options.kind) && options.error == MeanErrorKind::Sd)
    return fail(ErrorKind::Config, "flux: sd is not an error kind of a fitted surface");

  const std::size_t need = minimum_monitors(options);
  if (monitors.size() < need)
    return fail(ErrorKind::Config, "flux: " + name + " needs " + std::to_string(need) + " monitor positions, " +
                                       std::to_string(monitors.size()) + " used");

  const bool weighted = uses_errors(options) || (is_least_squares(options.kind) && options.weighted);
  for (const Monitor& m : monitors) {
    if (!std::isfinite(m.at.x) || !std::isfinite(m.at.y) || !std::isfinite(m.j) || !std::isfinite(m.j_err))
      return fail(ErrorKind::Config, "flux: monitor " + m.label + " has a non-finite position, J or error");
    if (m.j_err < 0.0) return fail(ErrorKind::Config, "flux: monitor " + m.label + " has a negative error");
    if (weighted && m.j_err == 0.0)
      return fail(ErrorKind::Config, "flux: monitor " + m.label + " has a zero J error, which a weighted model cannot use");
  }
  for (std::size_t i = 0; i < predict_at.size(); ++i)
    if (!std::isfinite(predict_at[i].x) || !std::isfinite(predict_at[i].y))
      return fail(ErrorKind::Config, "flux: position " + std::to_string(i) + " is not finite");

  FluxFit out;
  out.at.reserve(predict_at.size());

  // Monitor indices nearest first; a stable sort keeps monitor order on ties.
  auto by_distance = [&](const Point& p) {
    std::vector<std::size_t> idx(monitors.size());
    std::iota(idx.begin(), idx.end(), std::size_t{0});
    auto d2 = [&](std::size_t i) {
      const double dx = monitors[i].at.x - p.x, dy = monitors[i].at.y - p.y;
      return dx * dx + dy * dy;
    };
    std::stable_sort(idx.begin(), idx.end(), [&](std::size_t a, std::size_t b) { return d2(a) < d2(b); });
    return idx;
  };

  switch (options.kind) {
    case ModelKind::WeightedMean:
    case ModelKind::WeightedMean1D: {
      std::vector<double> js, es;
      for (const Monitor& m : monitors) {
        js.push_back(m.j);
        es.push_back(m.j_err);
      }
      auto mean = weighted_mean(js, es, options.error);
      if (!mean) return fail(mean.error());
      out.at.assign(predict_at.size(), Predicted{mean->value, mean->error});
      out.parameters = {mean->value};
      out.mswd = mean->mswd;
      out.dof = static_cast<int>(mean->n) - 1;
      if (!predict_at.empty() && mean->n > 1 && !mean->mswd_acceptable)
        out.notes.push_back({0, FitNote::MswdOutsideLimits});
      return out;
    }
    case ModelKind::Matching:
      for (const Point& p : predict_at) {
        const Monitor& m = monitors[by_distance(p).front()];
        out.at.push_back({m.j, m.j_err});
      }
      return out;
    case ModelKind::NearestNeighbors:
      for (const Point& p : predict_at) {
        const auto idx = by_distance(p);
        double sw = 0, swj = 0;
        for (int k = 0; k < options.n_neighbors; ++k) {
          const Monitor& m = monitors[idx[static_cast<std::size_t>(k)]];
          const double w = 1.0 / (m.j_err * m.j_err);
          sw += w;
          swj += w * m.j;
        }
        out.at.push_back({swj / sw, 1.0 / std::sqrt(sw)});
      }
      return out;
    case ModelKind::Bracketing:
      for (std::size_t i = 0; i < predict_at.size(); ++i) {
        const Point& p = predict_at[i];
        const auto idx = by_distance(p);
        const Monitor& m0 = monitors[idx[0]];
        const Monitor& m1 = monitors[idx[1]];
        const double dx = m1.at.x - m0.at.x, dy = m1.at.y - m0.at.y;
        const double len2 = dx * dx + dy * dy;
        const double f = len2 == 0.0 ? 0.0 : ((p.x - m0.at.x) * dx + (p.y - m0.at.y) * dy) / len2;
        out.at.push_back(interpolate(m0, m1, f, options.interpolation));
        if (options.interpolation == Interpolation::Linear && (f < 0.0 || f > 1.0))
          out.notes.push_back({i, FitNote::Extrapolated});
      }
      return out;
    case ModelKind::Bracketing1D: {
      std::vector<std::size_t> idx(monitors.size());
      std::iota(idx.begin(), idx.end(), std::size_t{0});
      std::stable_sort(idx.begin(), idx.end(), [&](std::size_t a, std::size_t b) {
        return coord(monitors[a].at, options.axis) < coord(monitors[b].at, options.axis);
      });
      for (std::size_t i = 0; i < predict_at.size(); ++i) {
        const double p = coord(predict_at[i], options.axis);
        std::size_t hi = 1;
        while (hi < idx.size() - 1 && coord(monitors[idx[hi]].at, options.axis) <= p) ++hi;
        const Monitor& m0 = monitors[idx[hi - 1]];
        const Monitor& m1 = monitors[idx[hi]];
        const double c0 = coord(m0.at, options.axis), span = coord(m1.at, options.axis) - c0;
        const double f = span == 0.0 ? 0.0 : (p - c0) / span;
        out.at.push_back(interpolate(m0, m1, f, Interpolation::Linear));  // always linear
        if (f < 0.0 || f > 1.0) out.notes.push_back({i, FitNote::Extrapolated});
      }
      return out;
    }
    case ModelKind::Plane:
    case ModelKind::Bowl:
    case ModelKind::LeastSquares1D:
      return fit_surface(monitors, predict_at, options, name);
  }
  return fail(ErrorKind::Config, "flux: model not implemented");
}

}  // namespace pychron::reduction
