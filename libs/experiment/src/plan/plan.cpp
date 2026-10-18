#include "pychron/experiment/plan/plan.hpp"

#include <algorithm>
#include <array>
#include <iterator>
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

std::optional<At> position_target(const MeasurementPlan& plan, const Position& position,
                                  std::string_view spectrometer_reference) {
  if (position.at) return position.at;
  const bool plan_names_one = !plan.detectors.reference.empty();
  const std::string_view ref = plan_names_one ? std::string_view(plan.detectors.reference) : spectrometer_reference;
  if (ref.empty()) return std::nullopt;
  const auto& c = position.collect;
  if (const auto it = std::find_if(c.begin(), c.end(), [&](const CollectEntry& e) { return e.detector == ref; });
      it != c.end())
    return AtLabel{it->label, it->detector};
  // A mode of the reference: collector H2 in CDD mode is "H2(CDD)" (spec 3.6).
  const auto mode = [&](const CollectEntry& e) {
    return e.detector.size() > ref.size() && e.detector.starts_with(ref) && e.detector[ref.size()] == '(';
  };
  if (const auto it = std::find_if(c.begin(), c.end(), mode); it != c.end()) return AtLabel{it->label, it->detector};
  // The plan names no reference and the spectrometer's is not collected
  // here: the first active entry positions the magnet, as a legacy hop's
  // first isotope:detector did. A plan that names a reference needs `at`
  // instead (checked at load).
  if (!plan_names_one) {
    if (const auto it = std::find_if(c.begin(), c.end(), [](const CollectEntry& e) { return e.active; });
        it != c.end())
      return AtLabel{it->label, it->detector};
  }
  return std::nullopt;
}

std::vector<CollectEntry> collecting(const Position& position) {
  std::vector<CollectEntry> out;
  std::copy_if(position.collect.begin(), position.collect.end(), std::back_inserter(out),
               [](const CollectEntry& e) { return e.active; });
  return out;
}

const Segment* main_segment(const Gas& gas) {
  const auto it = std::find_if(gas.segments.begin(), gas.segments.end(), [](const Segment& s) {
    return s.kind == SegmentKind::Signal && s.when == When::AfterInlet;
  });
  return it == gas.segments.end() ? nullptr : &*it;
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
