#include "pychron/experiment/model/run_spec.hpp"

namespace pychron::experiment {

Duration estimate_run(const RunSpec& run, Duration measurement) {
  const auto& e = run.extraction;
  return e.pre_cleanup + e.ramp + e.duration + e.cleanup + e.post_cleanup + measurement + run.delay_after;
}

}  // namespace pychron::experiment
