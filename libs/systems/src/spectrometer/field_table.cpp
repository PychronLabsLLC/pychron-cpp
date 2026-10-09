#include "pychron/systems/spectrometer/field_table.hpp"

#include <algorithm>
#include <cmath>
#include <ranges>
#include <sstream>
#include <utility>

#include <toml++/toml.hpp>

namespace pychron::spectrometer {

namespace {

struct Sample {
  double mass;
  double value;
};

int degree(FitKind kind) {
  switch (kind) {
    case FitKind::Linear: return 1;
    case FitKind::Quadratic: return 2;
    case FitKind::Cubic: return 3;
    case FitKind::Discrete: break;
  }
  return 0;
}

// Least-squares polynomial in x = (mass - center) / scale; centering keeps the
// normal equations well conditioned for masses far from zero.
struct Poly {
  double center = 0.0;
  double scale = 1.0;
  std::vector<double> c;  // c[0] + c[1] x + ...

  double operator()(double mass) const {
    double x = (mass - center) / scale;
    double y = 0.0;
    for (double it : std::views::reverse(c)) y = y * x + it;
    return y;
  }
};

Result<Poly> fit_poly(const std::vector<Sample>& s, int deg, std::string_view det) {
  const auto n = static_cast<size_t>(deg + 1);
  if (s.size() < n) {
    return fail(ErrorKind::Config, "field table: detector " + std::string(det) + " needs at least " +
                                       std::to_string(n) + " points for a degree-" + std::to_string(deg) + " fit");
  }
  Poly p;
  for (const auto& e : s) p.center += e.mass;
  p.center /= static_cast<double>(s.size());
  double span = 0.0;
  for (const auto& e : s) span = std::max(span, std::abs(e.mass - p.center));
  p.scale = span > 0.0 ? span : 1.0;

  // Normal equations A c = b, A[i][j] = sum x^(i+j), b[i] = sum y x^i.
  std::vector<std::vector<double>> a(n, std::vector<double>(n + 1, 0.0));
  for (const auto& e : s) {
    double x = (e.mass - p.center) / p.scale;
    std::vector<double> pw(2 * n, 1.0);
    for (size_t k = 1; k < pw.size(); ++k) pw[k] = pw[k - 1] * x;
    for (size_t i = 0; i < n; ++i) {
      for (size_t j = 0; j < n; ++j) a[i][j] += pw[i + j];
      a[i][n] += e.value * pw[i];
    }
  }
  for (size_t col = 0; col < n; ++col) {
    size_t piv = col;
    for (size_t r = col + 1; r < n; ++r)
      if (std::abs(a[r][col]) > std::abs(a[piv][col])) piv = r;
    if (std::abs(a[piv][col]) < 1e-12) {
      return fail(ErrorKind::Config,
                  "field table: detector " + std::string(det) + " points do not determine the fit (repeated masses)");
    }
    std::swap(a[col], a[piv]);
    for (size_t r = 0; r < n; ++r) {
      if (r == col) continue;
      double f = a[r][col] / a[col][col];
      for (size_t k = col; k <= n; ++k) a[r][k] -= f * a[col][k];
    }
  }
  p.c.resize(n);
  for (size_t i = 0; i < n; ++i) p.c[i] = a[i][n] / a[i][i];
  return p;
}

// Mass of the root of `f` (strictly monotonic expected) in [lo, hi].
Result<double> bracketed_root(const std::function<double(double)>& f, double lo, double hi, std::string_view det,
                              double target) {
  constexpr int kSamples = 256;
  const double step = (hi - lo) / kSamples;
  std::vector<std::pair<double, double>> brackets;
  double a = lo;
  double fa = f(a);
  for (int i = 1; i <= kSamples; ++i) {
    double b = i == kSamples ? hi : lo + step * i;
    double fb = f(b);
    if (fa == 0.0) {
      brackets.emplace_back(a, a);
    } else if ((fa < 0.0) != (fb < 0.0) && fb != 0.0) {
      brackets.emplace_back(a, b);
    }
    if (i == kSamples && fb == 0.0) brackets.emplace_back(b, b);
    a = b;
    fa = fb;
  }
  if (brackets.empty()) {
    return fail(ErrorKind::Config, "field table: value " + std::to_string(target) + " is outside detector " +
                                       std::string(det) + "'s table range");
  }
  if (brackets.size() > 1) {
    return fail(ErrorKind::Config, "field table: fit for detector " + std::string(det) +
                                       " is not monotonic; value " + std::to_string(target) + " is ambiguous");
  }
  auto [x0, x1] = brackets.front();
  double f0 = f(x0);
  for (int it = 0; it < 200 && x1 - x0 > 1e-12; ++it) {
    double mid = 0.5 * (x0 + x1);
    double fm = f(mid);
    if (fm == 0.0) return mid;
    if ((fm < 0.0) == (f0 < 0.0)) {
      x0 = mid;
      f0 = fm;
    } else {
      x1 = mid;
    }
  }
  return 0.5 * (x0 + x1);
}

}  // namespace

std::string_view to_string(FitKind kind) noexcept {
  switch (kind) {
    case FitKind::Discrete: return "discrete";
    case FitKind::Linear: return "linear";
    case FitKind::Quadratic: return "quadratic";
    case FitKind::Cubic: return "cubic";
  }
  return "?";
}

std::string_view to_string(TableAxis axis) noexcept {
  switch (axis) {
    case TableAxis::Dac: return "dac";
    case TableAxis::Field: return "field";
    case TableAxis::Mass: return "mass";
  }
  return "?";
}

Result<FitKind> parse_fit_kind(std::string_view text) {
  if (text == "discrete") return FitKind::Discrete;
  if (text == "linear") return FitKind::Linear;
  if (text == "quadratic" || text == "parabolic") return FitKind::Quadratic;
  if (text == "cubic") return FitKind::Cubic;
  return fail(ErrorKind::Config, "unknown fit kind '" + std::string(text) + "'");
}

Result<TableAxis> parse_table_axis(std::string_view text) {
  if (text == "dac") return TableAxis::Dac;
  if (text == "field") return TableAxis::Field;
  if (text == "mass") return TableAxis::Mass;
  return fail(ErrorKind::Config, "unknown table axis '" + std::string(text) + "'");
}

FieldTable::FieldTable(FitKind fit, TableAxis axis, std::vector<ControlPoint> points)
    : fit_(fit), axis_(axis), points_(std::move(points)) {}

std::vector<std::string> FieldTable::detectors() const {
  std::vector<std::string> out;
  for (const auto& p : points_)
    for (const auto& [det, _] : p.values)
      if (std::find(out.begin(), out.end(), det) == out.end()) out.push_back(det);
  return out;
}

bool FieldTable::has_detector(std::string_view det) const {
  return std::any_of(points_.begin(), points_.end(),
                     [&](const ControlPoint& p) { return p.values.contains(std::string(det)); });
}

FitKind FieldTable::fit(std::string_view det) const {
  auto it = fits_.find(det);
  return it == fits_.end() ? fit_ : it->second;
}

void FieldTable::set_fit(std::string det, FitKind kind) { fits_[std::move(det)] = kind; }

namespace {

Result<std::vector<Sample>> samples_for(const std::vector<ControlPoint>& points, std::string_view det) {
  std::vector<Sample> out;
  const std::string key(det);
  for (const auto& p : points) {
    auto it = p.values.find(key);
    if (it != p.values.end()) out.push_back({p.mass, it->second});
  }
  if (out.empty()) return fail(ErrorKind::Config, "field table has no column for detector " + key);
  std::sort(out.begin(), out.end(), [](const Sample& a, const Sample& b) { return a.mass < b.mass; });
  return out;
}

}  // namespace

Result<double> FieldTable::value_for(double mass, std::string_view det) const {
  auto s = samples_for(points_, det);
  if (!s) return fail(s.error());
  const FitKind kind = fit(det);
  if (kind == FitKind::Discrete) {
    const Sample* best = nullptr;
    for (const auto& e : *s)
      if (!best || std::abs(e.mass - mass) < std::abs(best->mass - mass)) best = &e;
    if (std::abs(best->mass - mass) > kDiscreteTolerance) {
      return fail(ErrorKind::Config, "field table: no control point for detector " + std::string(det) +
                                         " within " + std::to_string(kDiscreteTolerance) + " amu of " +
                                         std::to_string(mass));
    }
    return best->value;
  }
  auto p = fit_poly(*s, degree(kind), det);
  if (!p) return fail(p.error());
  return (*p)(mass);
}

Result<double> FieldTable::mass_for(double value, std::string_view det) const {
  auto s = samples_for(points_, det);
  if (!s) return fail(s.error());
  const auto& v = *s;
  if (fit(det) == FitKind::Discrete) {
    // Piecewise-linear between neighbouring control points.
    for (const auto& e : v)
      if (e.value == value) return e.mass;
    for (size_t i = 0; i + 1 < v.size(); ++i) {
      const auto& a = v[i];
      const auto& b = v[i + 1];
      if ((value - a.value) * (value - b.value) < 0.0) {
        return a.mass + (b.mass - a.mass) * (value - a.value) / (b.value - a.value);
      }
    }
    return fail(ErrorKind::Config, "field table: value " + std::to_string(value) + " is outside detector " +
                                       std::string(det) + "'s control points");
  }
  auto p = fit_poly(v, degree(fit(det)), det);
  if (!p) return fail(p.error());
  const Poly poly = *p;
  return bracketed_root([&](double m) { return poly(m) - value; }, v.front().mass - kInverseMargin,
                        v.back().mass + kInverseMargin, det, value);
}

Result<void> FieldTable::update(std::string_view det, std::string_view isotope, double new_value, bool propagate) {
  if (!has_detector(det)) return fail(ErrorKind::Config, "field table has no column for detector " + std::string(det));
  auto it = std::find_if(points_.begin(), points_.end(), [&](const ControlPoint& p) { return p.isotope == isotope; });
  if (it == points_.end()) return fail(ErrorKind::Config, "field table has no isotope " + std::string(isotope));

  const std::string key(det);
  auto cur = it->values.find(key);
  const double offset = cur == it->values.end() ? 0.0 : new_value - cur->second;
  it->values[key] = new_value;
  if (propagate) {
    for (auto& [other, value] : it->values)
      if (other != key) value += offset;
  }
  return {};
}

Result<FieldTable> parse_field_table(std::string_view toml_text) {
  auto parsed = toml::parse(toml_text);
  if (!parsed) {
    return fail(ErrorKind::Config, "field table: " + std::string(parsed.error().description()));
  }
  const toml::table& root = parsed.table();

  auto fit_text = root["fit"].value<std::string>();
  if (!fit_text) return fail(ErrorKind::Config, "field table: missing string key 'fit'");
  auto fit = parse_fit_kind(*fit_text);
  if (!fit) return fail(fit.error());
  auto axis_text = root["axis"].value<std::string>();
  if (!axis_text) return fail(ErrorKind::Config, "field table: missing string key 'axis'");
  auto axis = parse_table_axis(*axis_text);
  if (!axis) return fail(axis.error());

  std::vector<ControlPoint> points;
  if (const auto* node = root.get("points")) {
    const auto* arr = node->as_array();
    if (!arr) return fail(ErrorKind::Config, "field table: 'points' must be an array of tables");
    for (size_t i = 0; i < arr->size(); ++i) {
      const std::string where = "field table: points[" + std::to_string(i) + "]";
      const auto* t = arr->get(i)->as_table();
      if (!t) return fail(ErrorKind::Config, where + " is not a table");
      ControlPoint cp;
      auto iso = (*t)["isotope"].value<std::string>();
      if (!iso) return fail(ErrorKind::Config, where + " missing string 'isotope'");
      cp.isotope = *iso;
      auto mass = (*t)["mass"].value<double>();
      if (!mass) return fail(ErrorKind::Config, where + " missing numeric 'mass'");
      cp.mass = *mass;
      for (const auto& [k, v] : *t) {
        if (k == "isotope" || k == "mass") continue;
        auto d = v.value<double>();
        if (!d) return fail(ErrorKind::Config, where + " detector '" + std::string(k.str()) + "' is not a number");
        cp.values.emplace(std::string(k.str()), *d);
      }
      points.push_back(std::move(cp));
    }
  }

  FieldTable table(*fit, *axis, std::move(points));
  if (const auto* node = root.get("fits")) {
    const auto* fits = node->as_table();
    if (!fits) return fail(ErrorKind::Config, "field table: 'fits' must be a table");
    for (const auto& [k, v] : *fits) {
      auto text = v.value<std::string>();
      if (!text) return fail(ErrorKind::Config, "field table: fits." + std::string(k.str()) + " must be a string");
      auto kind = parse_fit_kind(*text);
      if (!kind) return fail(kind.error());
      table.set_fit(std::string(k.str()), *kind);
    }
  }
  return table;
}

std::string to_toml(const FieldTable& table) {
  toml::table root;
  root.insert("fit", std::string(to_string(table.default_fit())));
  root.insert("axis", std::string(to_string(table.axis())));
  if (!table.fit_overrides().empty()) {
    toml::table fits;
    for (const auto& [det, kind] : table.fit_overrides()) fits.insert(det, std::string(to_string(kind)));
    root.insert("fits", std::move(fits));
  }
  toml::array points;
  for (const auto& p : table.points()) {
    toml::table t;
    t.insert("isotope", p.isotope);
    t.insert("mass", p.mass);
    for (const auto& [det, value] : p.values) t.insert(det, value);
    points.push_back(std::move(t));
  }
  root.insert("points", std::move(points));
  std::ostringstream out;
  out << root;
  return out.str();
}

}  // namespace pychron::spectrometer
