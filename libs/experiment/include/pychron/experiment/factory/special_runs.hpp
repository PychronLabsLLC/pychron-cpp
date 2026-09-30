#pragma once

#include <cstddef>
#include <limits>
#include <vector>

#include "pychron/experiment/model/run_spec.hpp"

namespace pychron::experiment {

// Frequency runs (blanks or airs every N unknowns) are expanded at edit time into
// explicit runs; the executor never generates runs.
struct Frequency {
  int every = 0;           // insert after every Nth counted run; 0 = only before/after
  bool before = false;     // insert before the first counted run
  bool after = false;      // insert after the last counted run
  bool count_all = false;  // count every non-skipped run, not only unknowns
  std::size_t first = 0;   // only runs in [first, last) are counted
  std::size_t last = std::numeric_limits<std::size_t>::max();
};

// Copy of `runs` with `special` inserted per `freq`. Skipped runs never count.
// Nothing is inserted if no run counts.
std::vector<RunSpec> insert_frequency(const std::vector<RunSpec>& runs, const RunSpec& special, const Frequency& freq);

}  // namespace pychron::experiment
