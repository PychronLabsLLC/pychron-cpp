#include "pychron/experiment/plan/plan.hpp"

#include <algorithm>
#include <array>
#include <utility>

namespace pychron::experiment::plan {

namespace {

constexpr std::array<std::pair<When, std::string_view>, 6> kWhenNames{{
    {When::DuringExtraction, "during_extraction"},
    {When::BeforeInlet, "before_inlet"},
    {When::DuringEquilibration, "during_equilibration"},
    {When::AfterInlet, "after_inlet"},
    {When::AfterMain, "after_main"},
    {When::AfterRun, "after_run"},
}};

constexpr std::array<std::pair<SegmentKind, std::string_view>, 10> kSegmentKindNames{{
    {SegmentKind::Signal, "signal"},
    {SegmentKind::Baseline, "baseline"},
    {SegmentKind::Sniff, "sniff"},
    {SegmentKind::Whiff, "whiff"},
    {SegmentKind::PeakCenter, "peak_center"},
    {SegmentKind::Set, "set"},
    {SegmentKind::BlankBeam, "blank_beam"},
    {SegmentKind::Park, "park"},
    {SegmentKind::Delay, "delay"},
    {SegmentKind::TimeZero, "time_zero"},
}};

template <typename E, std::size_t N>
std::string_view name_of(const std::array<std::pair<E, std::string_view>, N>& names, E value) {
  const auto it = std::find_if(names.begin(), names.end(), [&](const auto& e) { return e.first == value; });
  return it == names.end() ? std::string_view{} : it->second;
}

template <typename E, std::size_t N>
std::optional<E> value_of(const std::array<std::pair<E, std::string_view>, N>& names, std::string_view s) {
  const auto it = std::find_if(names.begin(), names.end(), [&](const auto& e) { return e.second == s; });
  if (it == names.end()) return std::nullopt;
  return it->first;
}

}  // namespace

std::string_view to_string(When w) { return name_of(kWhenNames, w); }
std::optional<When> parse_when(std::string_view s) { return value_of(kWhenNames, s); }

std::string_view to_string(SegmentKind k) { return name_of(kSegmentKindNames, k); }
std::optional<SegmentKind> parse_segment_kind(std::string_view s) { return value_of(kSegmentKindNames, s); }

bool is_step(SegmentKind k) {
  switch (k) {
    case SegmentKind::Signal:
    case SegmentKind::Baseline:
    case SegmentKind::Sniff:
    case SegmentKind::Whiff:
      return false;
    case SegmentKind::PeakCenter:
    case SegmentKind::Set:
    case SegmentKind::BlankBeam:
    case SegmentKind::Park:
    case SegmentKind::Delay:
    case SegmentKind::TimeZero:
      return true;
  }
  return false;
}

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
