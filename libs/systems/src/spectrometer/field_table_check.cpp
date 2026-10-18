#include "pychron/systems/spectrometer/field_table_check.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <sstream>
#include <utility>

namespace pychron::spectrometer {

namespace {

std::string_view trimmed(std::string_view s) {
  constexpr std::string_view kBlank = " \t\r\n";
  const auto b = s.find_first_not_of(kBlank);
  if (b == std::string_view::npos) return {};
  const auto e = s.find_last_not_of(kBlank);
  return s.substr(b, e - b + 1);
}

std::string q(std::string_view s) { return "'" + std::string(s) + "'"; }

std::string num(double v) {
  std::ostringstream os;
  os << v;
  return os.str();
}

class Collector {
 public:
  void error(std::optional<std::size_t> point, std::string detector, std::string message) {
    out_.push_back({TableSeverity::Error, point, std::move(detector), std::move(message)});
  }
  void warning(std::optional<std::size_t> point, std::string detector, std::string message) {
    out_.push_back({TableSeverity::Warning, point, std::move(detector), std::move(message)});
  }
  TableDiagnostics take() { return std::move(out_); }

 private:
  TableDiagnostics out_;
};

void check_rows(const FieldTable& table, const MolecularWeights& weights, Collector& out) {
  const auto& pts = table.points();
  std::set<std::string, std::less<>> seen;
  for (std::size_t i = 0; i < pts.size(); ++i) {
    const auto& p = pts[i];
    const auto iso = trimmed(p.isotope);
    if (iso.empty()) {
      out.error(i, {}, "isotope is empty");
    } else if (!seen.insert(std::string(iso)).second) {
      out.error(i, {}, "duplicate isotope " + q(iso));
    }
    const bool mass_ok = std::isfinite(p.mass) && p.mass > 0.0;
    if (!mass_ok) out.error(i, {}, "mass " + num(p.mass) + " is not a finite positive number");
    for (const auto& [det, v] : p.values) {
      if (!std::isfinite(v)) out.error(i, det, "value " + num(v) + " is not finite");
    }
    if (iso.empty()) continue;
    if (!weights.contains(iso)) {
      out.warning(i, {}, "isotope " + q(iso) + " is not in molecular weights");
    } else if (mass_ok) {
      const double w = *weights.mass(iso);
      if (std::abs(p.mass - w) > kWeightTolerance) {
        out.warning(i, {}, "mass " + num(p.mass) + " differs from molecular weight " + num(w) + " of " + q(iso) +
                               " by more than " + num(kWeightTolerance) + " amu");
      }
    }
  }
}

// Masses and values of `det` are all finite: the fit-dependent warnings mean
// nothing otherwise, and the row errors already name the bad cells.
bool finite_samples(const FieldTable& table, const std::string& det) {
  return std::all_of(table.points().begin(), table.points().end(), [&](const ControlPoint& p) {
    auto it = p.values.find(det);
    return it == p.values.end() || (std::isfinite(p.mass) && std::isfinite(it->second));
  });
}

void check_monotonic(const FieldTable& table, const std::string& det, double lo, double hi, Collector& out) {
  if (!(hi > lo)) return;
  int last_sign = 0;
  auto prev = table.value_for(lo, det);
  if (!prev) return;
  for (int i = 1; i < kMonotonicSamples; ++i) {
    const double m = i == kMonotonicSamples - 1 ? hi : lo + (hi - lo) * i / (kMonotonicSamples - 1);
    auto cur = table.value_for(m, det);
    if (!cur) return;
    const double diff = *cur - *prev;
    const int sign = diff > 0.0 ? 1 : (diff < 0.0 ? -1 : 0);
    if (sign != 0) {
      if (last_sign != 0 && sign != last_sign) {
        out.warning(std::nullopt, det,
                    "fit is not monotonic between " + num(lo) + " and " + num(hi) +
                        " amu; mass_for may pick the wrong root");
        return;
      }
      last_sign = sign;
    }
    prev = cur;
  }
}

void check_residuals(const FieldTable& table, const std::string& det, Collector& out) {
  const auto& pts = table.points();
  double vmin = 0.0;
  double vmax = 0.0;
  bool any = false;
  for (const auto& p : pts) {
    auto it = p.values.find(det);
    if (it == p.values.end()) continue;
    vmin = any ? std::min(vmin, it->second) : it->second;
    vmax = any ? std::max(vmax, it->second) : it->second;
    any = true;
  }
  const double limit = kResidualSpanFraction * (vmax - vmin);
  if (!(limit > 0.0)) return;
  for (std::size_t i = 0; i < pts.size(); ++i) {
    auto it = pts[i].values.find(det);
    if (it == pts[i].values.end()) continue;
    auto fitted = table.value_for(pts[i].mass, det);
    if (!fitted) continue;
    const double r = std::abs(it->second - *fitted);
    if (r > limit) {
      out.warning(i, det,
                  "residual " + num(r) + " is more than " + num(kResidualSpanFraction * 100.0) +
                      " % of the detector's value span " + num(vmax - vmin));
    }
  }
}

void check_detectors(const FieldTable& table, const cfg::SpectrometerConfig& config, Collector& out) {
  for (const auto& det : table.detectors()) {
    if (config.detector(det) == nullptr) out.warning(std::nullopt, det, "detector is not in config");
    if (auto fit = table.check_fit(det); !fit) {
      out.error(std::nullopt, det, fit.error().what);
      continue;
    }
    if (table.fit(det) == FitKind::Discrete || !finite_samples(table, det)) continue;
    double lo = 0.0;
    double hi = 0.0;
    bool any = false;
    for (const auto& p : table.points()) {
      if (!p.values.contains(det)) continue;
      lo = any ? std::min(lo, p.mass) : p.mass;
      hi = any ? std::max(hi, p.mass) : p.mass;
      any = true;
    }
    check_monotonic(table, det, lo, hi, out);
    check_residuals(table, det, out);
  }
}

}  // namespace

bool has_error(const TableDiagnostics& d) noexcept { return first_error(d) != nullptr; }

const TableDiagnostic* first_error(const TableDiagnostics& d) noexcept {
  auto it = std::find_if(d.begin(), d.end(), [](const TableDiagnostic& e) { return e.severity == TableSeverity::Error; });
  return it == d.end() ? nullptr : &*it;
}

TableDiagnostics check_field_table(const FieldTable& table, const cfg::SpectrometerConfig& config,
                                   const MolecularWeights& weights, std::string_view name) {
  Collector out;
  check_rows(table, weights, out);
  check_detectors(table, config, out);
  const auto& m = config.magnet;
  const bool in_use = (!m.field_table.empty() && name == m.field_table) || (!m.hv_table.empty() && name == m.hv_table);
  if (in_use) {
    for (const auto& d : config.detectors) {
      if (d.active && !table.has_detector(d.name)) {
        out.error(std::nullopt, d.name, "active detector has no value in any point of table " + q(name));
      }
    }
  }
  return out.take();
}

std::string to_string(const TableDiagnostic& d) {
  std::string out = d.severity == TableSeverity::Error ? "error: " : "warning: ";
  std::string where;
  if (d.point) where = "row " + std::to_string(*d.point + 1);
  if (!d.detector.empty()) where += (where.empty() ? "" : "/") + d.detector;
  if (!where.empty()) out += where + ": ";
  return out + d.message;
}

namespace {

// First row of each trimmed isotope name.
std::map<std::string_view, const ControlPoint*, std::less<>> by_isotope(const FieldTable& t) {
  std::map<std::string_view, const ControlPoint*, std::less<>> out;
  for (const auto& p : t.points()) out.try_emplace(trimmed(p.isotope), &p);
  return out;
}

// Exact comparison, except that two NaNs are the same value.
bool same_value(double x, double y) noexcept { return x == y || (std::isnan(x) && std::isnan(y)); }

std::string fit_text(const FieldTable& t, std::string_view det) {
  const auto& o = t.fit_overrides();
  const auto it = o.find(det);
  return it == o.end() ? std::string("default") : std::string(to_string(it->second));
}

// "<label>: <before> -> <after>"
std::string change(std::string_view label, std::string_view before, std::string_view after) {
  std::string out(label);
  out.append(": ").append(before).append(" -> ").append(after);
  return out;
}

}  // namespace

TableDiff diff_field_tables(const FieldTable& a, const FieldTable& b) {
  TableDiff d;

  if (a.axis() != b.axis()) {
    d.fit_changes.push_back(change("axis", to_string(a.axis()), to_string(b.axis())));
  }
  if (a.default_fit() != b.default_fit()) {
    d.fit_changes.push_back(change("fit", to_string(a.default_fit()), to_string(b.default_fit())));
  }
  std::set<std::string_view> override_dets;
  for (const auto& [det, kind] : a.fit_overrides()) override_dets.insert(det);
  for (const auto& [det, kind] : b.fit_overrides()) override_dets.insert(det);
  for (const auto det : override_dets) {
    const auto before = fit_text(a, det);
    const auto after = fit_text(b, det);
    if (before != after) d.fit_changes.push_back(change(std::string("fit ").append(det), before, after));
  }

  const auto old_rows = by_isotope(a);
  const auto new_rows = by_isotope(b);

  std::set<std::string_view> seen;
  for (const auto& p : b.points()) {
    const auto iso = trimmed(p.isotope);
    if (!seen.insert(iso).second) continue;
    if (!old_rows.contains(iso)) d.rows.push_back({std::string(iso), RowChange::Added});
  }
  seen.clear();
  for (const auto& p : a.points()) {
    const auto iso = trimmed(p.isotope);
    if (!seen.insert(iso).second) continue;
    if (!new_rows.contains(iso)) d.rows.push_back({std::string(iso), RowChange::Removed});
  }

  seen.clear();
  for (const auto& p : b.points()) {
    const auto iso = trimmed(p.isotope);
    if (!seen.insert(iso).second) continue;
    const auto it = old_rows.find(iso);
    if (it == old_rows.end()) continue;
    const ControlPoint& was = *it->second;
    const ControlPoint& now = *new_rows.at(iso);
    if (!same_value(was.mass, now.mass)) d.masses.push_back({std::string(iso), was.mass, now.mass});

    std::set<std::string_view> dets;
    for (const auto& [det, v] : was.values) dets.insert(det);
    for (const auto& [det, v] : now.values) dets.insert(det);
    for (const auto det : dets) {
      const auto o = was.values.find(std::string(det));
      const auto n = now.values.find(std::string(det));
      std::optional<double> ov;
      std::optional<double> nv;
      if (o != was.values.end()) ov = o->second;
      if (n != now.values.end()) nv = n->second;
      if (ov && nv && same_value(*ov, *nv)) continue;
      d.cells.push_back({std::string(iso), std::string(det), ov, nv});
    }
  }
  return d;
}

}  // namespace pychron::spectrometer
