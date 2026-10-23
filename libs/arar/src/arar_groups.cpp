#include "pychron/arar/arar_groups.hpp"

#include <cmath>

#include "pychron/arar/arar_reduction.hpp"
#include "pychron/arar/reduce.hpp"

namespace pychron::processing {

namespace r = pychron::reduction;

namespace {

Unexpected<Error> group_fail(std::string what) { return fail(ErrorKind::Config, std::move(what)); }

// The Ar-Ar part of the first analysis with a J: its J, constants and
// lambda_k_total run the age equation of the group.
const arar::ArArReduced* first_with_j(const GroupItems& items) {
  for (const auto* it : items) {
    const arar::ArArReduced* reduced = it && it->analysis ? arar::reduced_of(*it->analysis) : nullptr;
    if (reduced && reduced->j) return reduced;
  }
  return nullptr;
}

}  // namespace

double with_external_error(double value, double error, double j_relative) {
  if (value == 0) return error;
  const double rel = error / std::abs(value);
  return std::sqrt(rel * rel + j_relative * j_relative) * std::abs(value);
}

double j_relative_error(const GroupItems& items) {
  const arar::ArArReduced* ref = first_with_j(items);
  if (!ref || ref->j->nominal() == 0) return 0.0;
  return ref->j->std_dev() / std::abs(ref->j->nominal());
}

Result<r::UFloat> integrated_age(const GroupItems& items, bool include_j_error) {
  r::UFloat rad40, k39;
  std::size_t n = 0;
  for (const auto* it : items) {
    // A reduction_error with a result present: corrections are missing.
    const arar::ArArReduced* reduced = it && it->analysis ? arar::reduced_of(*it->analysis) : nullptr;
    if (!reduced || !it->analysis->reduction_error.empty()) continue;
    rad40 = rad40 + reduced->arar.f.rad40;
    k39 = k39 + reduced->arar.f.interference_corrected[r::index(r::ArgonIsotope::Ar39)];
    ++n;
  }
  if (n == 0) return group_fail("integrated age: no reduced analysis");
  if (k39.nominal() == 0) return group_fail("integrated age: total 39ArK is 0");
  const arar::ArArReduced* ref = first_with_j(items);
  if (!ref) return group_fail("integrated age: no J");
  const r::UFloat f = rad40 / k39;
  const r::UFloat j = include_j_error ? *ref->j : r::UFloat(ref->j->nominal());
  auto age = r::age_equation(j, f, ref->constants, ref->lambda_k_total);
  if (!age) return group_fail("integrated age: " + age.error().what);
  return *age;
}

std::vector<IsochronPoint> isochron_points(const GroupItems& items) {
  std::vector<IsochronPoint> out;
  for (const auto* it : items) {
    if (!it || !it->analysis || !it->analysis->reduction_error.empty()) continue;
    const arar::ArArReduced* reduced = arar::reduced_of(*it->analysis);
    if (!reduced) continue;
    const auto a40 = reduced->stage("Ar40", "interference_corrected");
    const auto a39 = reduced->stage("Ar39", "interference_corrected");
    const auto a36 = reduced->stage("Ar36", "interference_corrected");
    if (!a40 || !a39 || !a36 || a40->nominal() == 0) continue;
    const r::UFloat x = *a39 / *a40, y = *a36 / *a40;
    if (!std::isfinite(x.nominal()) || !std::isfinite(y.nominal())) continue;
    IsochronPoint p;
    p.item = it;
    p.x = x.nominal();
    p.sx = x.std_dev();
    p.y = y.nominal();
    p.sy = y.std_dev();
    p.rho = r::correlation(x, y);
    if (!std::isfinite(p.rho)) p.rho = 0;
    out.push_back(p);
  }
  return out;
}

Result<IsochronAge> isochron_age(const std::vector<IsochronPoint>& points, r::YorkMethod method, bool mse,
                                 bool include_j_error) {
  if (points.size() < 3) return group_fail("isochron: needs at least 3 points");
  std::vector<r::XyPoint> xy, yx;
  for (const auto& p : points) {
    xy.push_back({p.x, p.sx, p.y, p.sy, p.rho});
    yx.push_back({p.y, p.sy, p.x, p.sx, p.rho});
  }
  auto fit = r::york_fit(xy, method);
  if (!fit) return group_fail("isochron: " + fit.error().what);
  auto fit_x = r::york_fit(yx, method);
  if (!fit_x) return group_fail("isochron: " + fit_x.error().what);
  IsochronAge out;
  out.fit = *fit;
  out.fit_x = *fit_x;
  const double scale = mse && fit->mswd > 1 ? std::sqrt(fit->mswd) : 1.0;
  const double scale_x = mse && fit_x->mswd > 1 ? std::sqrt(fit_x->mswd) : 1.0;
  if (fit->intercept != 0) {
    const double a = fit->intercept;
    out.trapped = {1.0 / a, fit->intercept_err * scale / (a * a)};
  }
  const double ax = fit_x->intercept;
  if (ax != 0) {
    out.f = r::UFloat::variable(1.0 / ax, fit_x->intercept_err * scale_x / (ax * ax), "isochron F");
    GroupItems items;
    for (const auto& p : points) items.push_back(p.item);
    const arar::ArArReduced* ref = first_with_j(items);
    if (ref && out.f->nominal() > 0) {
      const r::UFloat j = include_j_error ? *ref->j : r::UFloat(ref->j->nominal());
      if (auto age = r::age_equation(j, *out.f, ref->constants, ref->lambda_k_total)) out.age = *age;
    }
  }
  return out;
}

}  // namespace pychron::processing
