#include "pychron/experiment/record/names.hpp"

#include <cstddef>
#include <limits>
#include <map>
#include <set>
#include <utility>

namespace pychron::experiment::record {

namespace {

constexpr std::string_view kBaseline = "baseline";
constexpr std::string_view kSignal = "signal";

// kind -> isotope -> the detectors its series of that kind are on.
using Detectors = std::map<std::string, std::map<std::string, std::set<std::string>, std::less<>>, std::less<>>;

Detectors detectors_of(const Data& data) {
  Detectors out;
  for (const auto& s : data.series)
    if (s.kind != kBaseline) out[s.kind][s.iso].insert(s.det);
  return out;
}

bool on_several(const Detectors& detectors, std::string_view kind, std::string_view isotope) {
  const auto of_kind = detectors.find(kind);
  if (of_kind == detectors.end()) return false;
  const auto of_isotope = of_kind->second.find(isotope);
  return of_isotope != of_kind->second.end() && of_isotope->second.size() > 1;
}

// Appends `more` to `to`. `to` has a sigma for every point or for none, and
// so it has afterwards: a part without one gets NaN once any part has one.
void append(Trace& to, const Trace& more) {
  constexpr float unknown = std::numeric_limits<float>::quiet_NaN();
  const std::size_t before = to.t.size();
  const bool had = !to.sigma.empty();
  const bool has = !more.sigma.empty() && more.sigma.size() == more.t.size();
  to.t.insert(to.t.end(), more.t.begin(), more.t.end());
  to.v.insert(to.v.end(), more.v.begin(), more.v.end());
  if (!had && !has) return;
  if (!had) to.sigma.assign(before, unknown);
  if (has) {
    to.sigma.insert(to.sigma.end(), more.sigma.begin(), more.sigma.end());
  } else {
    to.sigma.resize(to.t.size(), unknown);
  }
}

}  // namespace

std::string detector_isotope_key(std::string_view detector, std::string_view isotope) {
  std::string key(detector);
  key += ':';
  key += isotope;
  return key;
}

std::string acquisition_isotope_key(std::string_view isotope, std::string_view detector) {
  std::string key(isotope);
  key += ':';
  key += detector;
  return key;
}

std::vector<std::string> intercept_keys(const Data& data) {
  const Detectors detectors = detectors_of(data);
  std::set<std::string> keys;
  const auto signals = detectors.find(kSignal);
  if (signals == detectors.end()) return {};
  for (const auto& [isotope, on] : signals->second) {
    if (on.size() == 1) {
      keys.insert(isotope);
    } else {
      for (const auto& detector : on) keys.insert(acquisition_isotope_key(isotope, detector));
    }
  }
  return {keys.begin(), keys.end()};
}

std::vector<NamedSeries> named_series(const Data& data) {
  const Detectors detectors = detectors_of(data);
  std::vector<NamedSeries> out;
  out.reserve(data.series.size());
  std::map<std::string, std::size_t, std::less<>> baseline_at;  // detector -> its series in `out`
  for (const auto& s : data.series) {
    if (s.kind == kBaseline) {
      const auto [at, first] = baseline_at.try_emplace(s.det, out.size());
      if (first) {
        out.push_back({s.kind, s.det, s.det, s.trace});
      } else {
        append(out[at->second].trace, s.trace);
      }
      continue;
    }
    const bool qualified = on_several(detectors, kSignal, s.iso) || on_several(detectors, s.kind, s.iso);
    out.push_back({s.kind, qualified ? detector_isotope_key(s.det, s.iso) : s.iso, s.det, s.trace});
  }
  return out;
}

std::optional<InterceptName> intercept_name(const Data& data, std::string_view result_key) {
  const Detectors detectors = detectors_of(data);
  // The acquisition's name of one detector's share of an isotope on several.
  for (const auto& s : data.series) {
    if (s.kind != kSignal || !on_several(detectors, kSignal, s.iso)) continue;
    if (result_key == acquisition_isotope_key(s.iso, s.det))
      return InterceptName{detector_isotope_key(s.det, s.iso), s.iso, s.det};
  }
  std::optional<std::string> lowest;
  for (const auto& s : data.series) {
    if (s.kind == kBaseline || s.iso != result_key) continue;
    if (s.kind == kSignal) return InterceptName{std::string(result_key), std::string(result_key), s.det};
    if (!lowest || s.det < *lowest) lowest = s.det;
  }
  if (!lowest) return std::nullopt;
  return InterceptName{std::string(result_key), std::string(result_key), *lowest};
}

}  // namespace pychron::experiment::record
