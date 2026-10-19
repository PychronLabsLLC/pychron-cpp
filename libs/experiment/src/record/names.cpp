#include "pychron/experiment/record/names.hpp"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <map>
#include <set>
#include <tuple>
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

// Puts a trace's points in time order (stable: points of one time keep
// their order).
void sort_by_time(Trace& tr) {
  if (std::is_sorted(tr.t.begin(), tr.t.end())) return;
  std::vector<std::size_t> idx(tr.t.size());
  for (std::size_t i = 0; i < idx.size(); ++i) idx[i] = i;
  std::stable_sort(idx.begin(), idx.end(), [&](std::size_t a, std::size_t b) { return tr.t[a] < tr.t[b]; });
  Trace out;
  for (auto i : idx) {
    out.t.push_back(tr.t[i]);
    out.v.push_back(tr.v[i]);
    if (!tr.sigma.empty()) out.sigma.push_back(tr.sigma[i]);
  }
  tr = std::move(out);
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

std::string qualified_key(std::string_view label, std::string_view detector) {
  std::string key(label);
  key += '@';
  key += detector;
  return key;
}

std::string v3_intercept_key(const Data& data, std::string_view v2_key) {
  const auto named = intercept_name(data, v2_key);
  if (!named) return std::string(v2_key);
  return qualified_key(named->isotope, named->detector);
}

std::string v2_intercept_key(const Data& data, std::string_view v3_key) {
  const auto at = v3_key.find('@');
  if (at == std::string_view::npos) return std::string(v3_key);
  const auto label = v3_key.substr(0, at);
  const auto detector = v3_key.substr(at + 1);
  if (on_several(detectors_of(data), kSignal, label)) return acquisition_isotope_key(label, detector);
  return std::string(label);
}

std::string analysis_name(const Data& data, std::string_view result_key) {
  const auto named = intercept_name(data, result_key);
  return named ? named->key : std::string(result_key);
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
  // (kind, key, detector) -> its series in `out`: the series of one isotope
  // on one detector read at several places in the plan (v3) are one.
  std::map<std::tuple<std::string, std::string, std::string>, std::size_t> at_of;
  for (const auto& s : data.series) {
    std::string key = s.det;
    if (s.kind != kBaseline) {
      const bool qualified = on_several(detectors, kSignal, s.iso) || on_several(detectors, s.kind, s.iso);
      key = qualified ? detector_isotope_key(s.det, s.iso) : s.iso;
    }
    const auto [at, first] = at_of.try_emplace({s.kind, key, s.det}, out.size());
    if (first) {
      out.push_back({s.kind, std::move(key), s.det, s.trace});
    } else {
      append(out[at->second].trace, s.trace);
    }
  }
  // A detector's baseline series stay in the record's order (the order they
  // were fitted in, before v3); the others are put in time order.
  for (auto& n : out)
    if (n.kind != kBaseline) sort_by_time(n.trace);
  return out;
}

std::optional<InterceptName> intercept_name(const Data& data, std::string_view result_key) {
  const Detectors detectors = detectors_of(data);
  if (const auto at = result_key.find('@'); at != std::string_view::npos) {
    const auto label = result_key.substr(0, at);
    const auto detector = result_key.substr(at + 1);
    const bool measured = std::any_of(data.series.begin(), data.series.end(), [&](const DataSeries& s) {
      return s.kind != kBaseline && s.iso == label && s.det == detector;
    });
    if (!measured) return std::nullopt;
    const bool qualified = on_several(detectors, kSignal, label);
    return InterceptName{qualified ? detector_isotope_key(detector, label) : std::string(label), std::string(label),
                         std::string(detector)};
  }
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
