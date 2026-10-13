#include "pychron/experiment/plan/plan.hpp"

#include <algorithm>

namespace pychron::experiment::plan {

std::optional<HopTarget> hop_target(const MeasurementPlan& plan, const Hop& hop) {
  if (hop.position) {
    HopTarget t{hop.position->isotope, std::nullopt, hop.position->detector};
    if (hop.baseline && hop.mass) t = {"", hop.mass, hop.position->detector};
    return t;
  }
  const auto& ref = plan.detectors.reference;
  for (const auto& [iso, det] : hop.positions) {
    if (det != ref) continue;
    if (hop.baseline && hop.mass) return HopTarget{"", hop.mass, det};
    return HopTarget{iso, std::nullopt, det};
  }
  return std::nullopt;
}

std::optional<HopTarget> baseline_target(const MeasurementPlan& plan) {
  if (!plan.baseline.mass) return std::nullopt;
  const auto& det = plan.baseline.detector.empty() ? plan.detectors.reference : plan.baseline.detector;
  return HopTarget{"", plan.baseline.mass, det};
}

std::vector<ActiveDetector> active_detectors(const MeasurementPlan& plan, const Hop& hop) {
  std::vector<ActiveDetector> out;
  const auto& ex = plan.detectors.exclude;
  for (const auto& [iso, det] : hop.positions)
    if (std::find(ex.begin(), ex.end(), det) == ex.end()) out.push_back({iso, det});
  return out;
}

namespace {

reduction::FitSpec fit_for(const std::map<std::string, reduction::FitKind>& kinds, const Fits& fits,
                           const std::string& key) {
  reduction::FitSpec spec;
  spec.error = fits.error;
  spec.outliers = fits.outliers;
  if (auto it = kinds.find(key); it != kinds.end()) spec.kind = it->second;
  else if (auto d = kinds.find("default"); d != kinds.end()) spec.kind = d->second;
  return spec;
}

}  // namespace

reduction::FitSpec signal_fit(const Fits& fits, const std::string& isotope) {
  return fit_for(fits.signal, fits, isotope);
}

reduction::FitSpec baseline_fit(const Fits& fits, const std::string& detector) {
  auto spec = fit_for(fits.baseline, fits, detector);
  if (!fits.baseline.contains(detector) && !fits.baseline.contains("default"))
    spec.kind = reduction::FitKind::Average;
  return spec;
}

}  // namespace pychron::experiment::plan
