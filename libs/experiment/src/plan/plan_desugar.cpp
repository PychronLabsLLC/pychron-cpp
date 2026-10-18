// v1 plan sections -> one implicit gas of v2 segments (v2 spec, section 4.1).
//
// Segment order within the gas follows the v1 block order:
//   before_inlet          peak_center.before, baseline.before
//   during_equilibration  sniff
//   after_inlet           whiff, main
//   after_main            baseline.after
//   after_run             peak_center.after

#include "plan_desugar.hpp"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <string>
#include <utility>

namespace pychron::experiment::plan::detail {
namespace {

bool contains(const std::vector<std::string>& v, const std::string& s) {
  return std::find(v.begin(), v.end(), s) != v.end();
}

class Desugar {
 public:
  Desugar(const MeasurementPlan& v1, const ISpectrometerCatalog* catalog) : v1_(v1), catalog_(catalog) {}

  Result<std::vector<Gas>> run() {
    Gas gas;
    gas.implicit = true;
    gas.equilibration = v1_.equilibration;
    gas.fits = v1_.fits;

    if (v1_.main.hops.empty()) return fail(ErrorKind::Config, "main.hops: at least one hop required");
    std::vector<Position> hops;
    hops.reserve(v1_.main.hops.size());
    for (std::size_t i = 0; i < v1_.main.hops.size(); ++i) {
      auto pos = position_of(v1_.main.hops[i]);
      if (std::none_of(pos.collect.begin(), pos.collect.end(), [](const CollectEntry& e) { return e.active; }))
        return fail(ErrorKind::Config, "main.hops[" + std::to_string(i) + "]: every detector excluded");
      hops.push_back(std::move(pos));
    }

    auto& segs = gas.segments;
    if (v1_.peak_center.before) segs.push_back(peak_center(When::BeforeInlet, 0));
    if (v1_.baseline.before) segs.push_back(baseline(When::BeforeInlet, hops));
    // Sniff and whiff collect on the first signal position (the first hop
    // when every hop is a baseline hop, as the v1 engine did).
    const auto first_signal = std::find_if(hops.begin(), hops.end(),
                                           [](const Position& h) { return h.kind == PositionKind::Signal; });
    const Position& first = first_signal != hops.end() ? *first_signal : hops.front();
    if (v1_.sniff.enabled) segs.push_back(sniff(first));
    if (v1_.whiff.enabled) segs.push_back(whiff(first));

    Segment main;
    main.kind = SegmentKind::Signal;
    main.when = When::AfterInlet;
    main.cycles = v1_.main.cycles;
    main.integration_s = v1_.main.integration_s;
    main.time_zero = v1_.main.time_zero;
    main.positions = std::move(hops);
    segs.push_back(std::move(main));
    const auto& signal_positions = segs.back().positions;

    if (v1_.baseline.after) segs.push_back(baseline(When::AfterMain, signal_positions));
    if (v1_.peak_center.after) segs.push_back(peak_center(When::AfterRun, v1_.peak_center.min_signal));

    std::vector<Gas> out;
    out.push_back(std::move(gas));
    return out;
  }

 private:
  bool excluded(const std::string& detector) const { return contains(v1_.detectors.exclude, detector); }

  // The label a protected detector that this hop does not collect is given:
  // what another hop collects on it, else its configured isotope, else (no
  // catalog) the detector's name.
  std::string label_for(const std::string& detector) const {
    for (const auto& hop : v1_.main.hops)
      for (const auto& [label, det] : hop.positions)
        if (det == detector) return label;
    if (catalog_ != nullptr)
      if (auto iso = catalog_->isotope_of(detector)) return *iso;
    return detector;
  }

  Position position_of(const Hop& hop) const {
    Position pos;
    pos.kind = hop.baseline ? PositionKind::Baseline : PositionKind::Signal;
    pos.counts = hop.counts;
    pos.settle_s = hop.settle_s;
    for (const auto& [label, det] : hop.positions) {
      if (excluded(det)) continue;
      CollectEntry e;
      e.label = label;
      e.detector = det;
      e.protect = contains(hop.protect, det);
      pos.collect.push_back(std::move(e));
    }
    for (const auto& det : hop.protect) {
      if (excluded(det)) continue;
      const bool listed = std::any_of(hop.positions.begin(), hop.positions.end(),
                                      [&](const auto& kv) { return kv.second == det; });
      const bool added = std::any_of(pos.collect.begin(), pos.collect.end(),
                                     [&](const CollectEntry& e) { return e.detector == det; });
      if (listed || added) continue;
      CollectEntry e;
      e.label = label_for(det);
      e.detector = det;
      e.active = false;
      e.protect = true;
      pos.collect.push_back(std::move(e));
    }
    // `at` only where v1 had an explicit target or a mass; otherwise the
    // reference detector's label in `collect` is the v2 default as well.
    if (hop.position) {
      if (hop.baseline && hop.mass) pos.at = AtMass{*hop.mass, hop.position->detector};
      else pos.at = AtLabel{hop.position->isotope, hop.position->detector};
    } else if (hop.baseline && hop.mass) {
      pos.at = AtMass{*hop.mass, v1_.detectors.reference};
    }
    return pos;
  }

  // Active entries of a position, as a sniff or whiff collects them (no move).
  static Position collecting(const Position& from, int counts) {
    Position pos;
    pos.counts = counts;
    std::copy_if(from.collect.begin(), from.collect.end(), std::back_inserter(pos.collect),
                 [](const CollectEntry& e) { return e.active; });
    for (auto& e : pos.collect) e.protect = false;
    return pos;
  }

  Segment sniff(const Position& first) const {
    Segment s;
    s.kind = SegmentKind::Sniff;
    s.when = When::DuringEquilibration;
    s.integration_s = v1_.sniff.integration_s;
    s.positions.push_back(collecting(first, v1_.sniff.counts));
    return s;
  }

  Segment whiff(const Position& first) const {
    Segment s;
    s.kind = SegmentKind::Whiff;
    s.when = When::AfterInlet;
    s.integration_s = v1_.whiff.integration_s;
    s.checks = v1_.whiff.checks;
    s.positions.push_back(collecting(first, v1_.whiff.counts));
    return s;
  }

  // One position collecting every detector of any signal position, each under
  // the first label collected on it.
  Segment baseline(When when, const std::vector<Position>& signal) const {
    const auto& b = v1_.baseline;
    Position pos;
    pos.counts = b.counts;
    pos.settle_s = b.settle_s;
    for (const auto& p : signal) {
      if (p.kind != PositionKind::Signal) continue;
      for (const auto& e : p.collect) {
        if (!e.active) continue;
        const bool seen = std::any_of(pos.collect.begin(), pos.collect.end(),
                                      [&](const CollectEntry& c) { return c.detector == e.detector; });
        if (!seen) pos.collect.push_back(CollectEntry{e.label, e.detector, true, std::nullopt, false});
      }
    }
    if (b.mass) pos.at = AtMass{*b.mass, b.detector.empty() ? v1_.detectors.reference : b.detector};
    Segment s;
    s.kind = SegmentKind::Baseline;
    s.when = when;
    s.integration_s = b.integration_s;
    s.positions.push_back(std::move(pos));
    return s;
  }

  Segment peak_center(When when, double min_signal) const {
    const auto& pc = v1_.peak_center;
    PeakCenterStep step;
    step.label = pc.isotope;
    const auto& det = pc.detector.empty() ? v1_.detectors.reference : pc.detector;
    if (!det.empty()) step.detectors.push_back(det);
    step.config = pc.config;
    step.min_signal = min_signal;
    Segment s;
    s.kind = SegmentKind::PeakCenter;
    s.when = when;
    s.peak_center = std::move(step);
    return s;
  }

  const MeasurementPlan& v1_;
  const ISpectrometerCatalog* catalog_;
};

}  // namespace

Result<std::vector<Gas>> desugar_v1(const MeasurementPlan& v1, const ISpectrometerCatalog* catalog) {
  return Desugar(v1, catalog).run();
}

}  // namespace pychron::experiment::plan::detail
