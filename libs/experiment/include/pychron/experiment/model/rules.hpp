#pragma once

#include <string>
#include <vector>

#include "pychron/experiment/model/run_spec.hpp"

namespace pychron::experiment {

// Which extraction fields are legal for an analysis type (the pychron factory's
// per-type stripping rules expressed as schema rules).
struct FieldRules {
  bool extraction = true;   // any extraction at all (device, script, durations)
  bool heating = true;      // value, pattern, beam_diameter, ramp, cryo_temp
  bool position = true;
  bool measurement = true;  // needs a measurement plan
};

FieldRules rules_for(AnalysisType type);

struct Issue {
  int run = -1;  // index into QueueSpec::runs, -1 for queue-level
  std::string field;
  std::string message;
  friend bool operator==(const Issue&, const Issue&) = default;
};

std::vector<Issue> validate_run(const RunSpec& run, const IdentifierRules& ids, int index = -1);
std::vector<Issue> validate_queue(const QueueSpec& queue, const IdentifierRules& ids);

// Sum of estimate_run over non-skipped runs, plus queue delays.
// `measurement` gives the measurement duration for a run.
template <class F>
Duration queue_eta(const QueueSpec& q, F&& measurement) {
  Duration total = q.delays.before_analyses;
  bool first = true;
  for (const auto& r : q.runs) {
    if (r.skip) continue;
    if (!first) total += q.delays.between_analyses;
    first = false;
    total += estimate_run(r, measurement(r));
  }
  return total;
}

}  // namespace pychron::experiment
