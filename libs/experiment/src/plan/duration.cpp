#include "pychron/experiment/plan/duration.hpp"

namespace pychron::experiment::plan {
namespace {

// Tracks the magnet position so a collection at the current position skips its settle.
class Clock {
 public:
  void collect(const std::optional<HopTarget>& target, double settle_s, int counts, double integration_s) {
    if (target && target != here_) {
      seconds_ += settle_s;
      here_ = target;
    }
    seconds_ += counts * integration_s;
  }
  void wait(double s) { seconds_ += s; }
  void moved_elsewhere() { here_.reset(); }
  Duration total() const { return Duration(seconds_); }

 private:
  std::optional<HopTarget> here_;
  double seconds_ = 0;
};

void run_main(const MeasurementPlan& plan, Clock& clock) {
  for (int c = 0; c < plan.main.cycles; ++c)
    for (const auto& hop : plan.main.hops)
      clock.collect(hop_target(plan, hop), hop.settle_s, hop.counts, plan.main.integration_s);
}

}  // namespace

Duration main_duration(const MeasurementPlan& plan) {
  Clock clock;
  run_main(plan, clock);
  return clock.total();
}

Duration estimate_duration(const MeasurementPlan& plan, const DurationOptions& options) {
  Clock clock;
  const auto& b = plan.baseline;
  if (plan.peak_center.before) {
    clock.wait(options.peak_center.count());
    clock.moved_elsewhere();
  }
  if (b.before) clock.collect(baseline_target(plan), b.settle_s, b.counts, b.integration_s);
  // Sniff runs inside the equilibration window.
  clock.wait(plan.equilibration.time_s);
  run_main(plan, clock);
  if (b.after) clock.collect(baseline_target(plan), b.settle_s, b.counts, b.integration_s);
  if (plan.peak_center.after) clock.wait(options.peak_center.count());
  return clock.total();
}

}  // namespace pychron::experiment::plan
