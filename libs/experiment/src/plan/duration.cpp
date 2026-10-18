#include "pychron/experiment/plan/duration.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace pychron::experiment::plan {
namespace {

// Where a position puts the magnet. A position without `at` whose reference
// detector is not named by the plan is identified by what it collects: two
// such positions with the same collect list are taken to be one place.
using Target = std::variant<AtLabel, AtMass, AtDac, AtHv, std::vector<CollectEntry>>;

bool is_reference(const std::string& detector, const std::string& reference) {
  // An entry on one of the reference's modes ("H2(CDD)" for "H2") counts as the reference.
  return detector == reference || (detector.size() > reference.size() + 1 && detector.starts_with(reference) &&
                                   detector[reference.size()] == '(');
}

Target target_of(const MeasurementPlan& plan, const Position& pos) {
  if (pos.at) return std::visit([](const auto& at) -> Target { return at; }, *pos.at);
  const auto& ref = plan.detectors.reference;
  if (!ref.empty()) {
    const auto exact = std::find_if(pos.collect.begin(), pos.collect.end(),
                                    [&](const CollectEntry& e) { return e.detector == ref; });
    if (exact != pos.collect.end()) return AtLabel{exact->label, exact->detector};
    const auto mode = std::find_if(pos.collect.begin(), pos.collect.end(),
                                   [&](const CollectEntry& e) { return is_reference(e.detector, ref); });
    if (mode != pos.collect.end()) return AtLabel{mode->label, mode->detector};
  }
  return pos.collect;
}

// Tracks the magnet position so a collection at the current position skips its settle.
class Tally {
 public:
  explicit Tally(const MeasurementPlan& plan) : plan_(plan) {}

  void collect(const Segment& seg) {
    for (int c = 0; c < seg.cycles; ++c)
      for (const auto& pos : seg.positions) {
        auto target = target_of(plan_, pos);
        if (target != here_) {
          seconds_ += pos.settle_s;
          here_ = std::move(target);
        }
        seconds_ += pos.counts * pos.integration_s.value_or(seg.integration_s);
      }
  }
  // Readings taken where the magnet is (a sniff, a whiff): no move, no settle.
  static double in_place(const Segment& seg) {
    double s = 0;
    for (int c = 0; c < seg.cycles; ++c)
      for (const auto& pos : seg.positions) s += pos.counts * pos.integration_s.value_or(seg.integration_s);
    return s;
  }
  void wait(double s) { seconds_ += s; }
  void moved_elsewhere() { here_.reset(); }
  double seconds() const { return seconds_; }

 private:
  const MeasurementPlan& plan_;
  std::optional<Target> here_;
  double seconds_ = 0;
};

bool is_main(const Segment& s) { return s.kind == SegmentKind::Signal && s.when == When::AfterInlet; }

// Inside the equilibration window: what collects while the inlet is open
// (sniff during equilibration, and the whiff the engine takes right after the
// inlet opens).
bool in_window(const Segment& s) {
  return s.when == When::DuringEquilibration || (s.kind == SegmentKind::Whiff && s.when == When::AfterInlet);
}

void step(const Segment& s, const DurationOptions& options, Tally& tally) {
  switch (s.kind) {
    case SegmentKind::PeakCenter: {
      const auto detectors = s.peak_center ? s.peak_center->detectors.size() : 0;
      tally.wait(options.peak_center.count() * static_cast<double>(std::max<std::size_t>(detectors, 1)));
      tally.moved_elsewhere();
      break;
    }
    case SegmentKind::Delay: tally.wait(s.delay_s.value_or(0)); break;
    case SegmentKind::Park: tally.moved_elsewhere(); break;
    default: break;  // set, blank_beam, time_zero: no time of their own
  }
}

void run_segment(const Segment& s, const DurationOptions& options, Tally& tally) {
  if (is_step(s.kind)) step(s, options, tally);
  else tally.collect(s);
}

void run_gas(const Gas& gas, const DurationOptions& options, Tally& tally) {
  // during_extraction segments run while the sample is extracted, which this
  // estimate (the measurement) does not include.
  const auto phase = [&](When when) {
    for (const auto& s : gas.segments)
      if (s.when == when && !in_window(s)) run_segment(s, options, tally);
  };
  phase(When::BeforeInlet);

  const auto& eq = gas.equilibration;
  double window = 0;
  for (const auto& s : gas.segments)
    if (in_window(s)) window += Tally::in_place(s);
  tally.wait(std::max(eq.inlet_delay_s + eq.time_s, window));

  phase(When::AfterInlet);
  phase(When::AfterMain);
}

// after_run segments run once every gas is measured, in gas order.
void run_after(const Gas& gas, const DurationOptions& options, Tally& tally) {
  for (const auto& s : gas.segments)
    if (s.when == When::AfterRun) run_segment(s, options, tally);
}

}  // namespace

Duration main_duration(const MeasurementPlan& plan) {
  Tally tally(plan);
  for (const auto& gas : plan.gases)
    for (const auto& s : gas.segments)
      if (is_main(s)) tally.collect(s);
  return Duration(tally.seconds());
}

Duration estimate_duration(const MeasurementPlan& plan, const DurationOptions& options) {
  Tally tally(plan);
  for (const auto& gas : plan.gases) run_gas(gas, options, tally);
  for (const auto& gas : plan.gases) run_after(gas, options, tally);
  return Duration(tally.seconds());
}

}  // namespace pychron::experiment::plan
