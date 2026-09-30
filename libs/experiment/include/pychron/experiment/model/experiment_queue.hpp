#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <vector>

#include "pychron/core/error.hpp"
#include "pychron/experiment/model/run_spec.hpp"

namespace pychron::experiment {

// Frequency runs (e.g. a blank every N unknowns) are expanded into explicit
// runs at edit time; the executor never generates runs.
struct FrequencySpec {
  RunSpec run;                // template inserted at each slot
  int every = 0;              // after every Nth counted run; 0 = only before/after
  bool before = false;        // one before the first counted run
  bool after = false;         // one after the last counted run
  std::set<AnalysisType> counted{AnalysisType::Unknown};
  std::size_t first = 0;      // row range considered, [first, last)
  std::optional<std::size_t> last;
};

// Ordered runs plus edit operations. Row indices always refer to the queue as
// it was before the call; operations that fail leave the queue unchanged.
class ExperimentQueue {
 public:
  ExperimentQueue() = default;
  explicit ExperimentQueue(QueueSpec spec) : spec_(std::move(spec)) {}

  const QueueSpec& spec() const { return spec_; }
  QueueSpec& header() { return spec_; }  // queue-level fields; edit runs through the ops below
  const std::vector<RunSpec>& runs() const { return spec_.runs; }
  std::size_t size() const { return spec_.runs.size(); }

  void append(RunSpec run);
  Result<void> insert(std::size_t at, RunSpec run);
  Result<void> remove(std::vector<std::size_t> rows);
  Result<void> replace(std::size_t row, RunSpec run);

  // Moves the selected rows, in queue order, to sit before row `to` (== size() appends).
  Result<void> move(std::vector<std::size_t> rows, std::size_t to);
  // Inserts copies of the selected rows, in queue order, before row `to`.
  Result<void> copy(std::vector<std::size_t> rows, std::size_t to);
  // Inserts `times` further copies of rows [first, first + count) right after the block.
  Result<void> repeat_block(std::size_t first, std::size_t count, std::size_t times);
  // Deterministic shuffle (seeded Fisher-Yates, portable across standard libraries).
  // With `rows`, only those rows are permuted among their own positions.
  Result<void> randomize(std::uint64_t seed, std::vector<std::size_t> rows = {});
  // Stable grouping: runs sharing (device, value, units) become contiguous,
  // groups ordered by first appearance.
  void group_by_extraction();
  Result<void> toggle_skip(const std::vector<std::size_t>& rows);
  // Sets end_after on `row` (clearing it elsewhere) or clears it if already set.
  Result<void> toggle_end_after(std::size_t row);

  // Returns the number of runs inserted.
  Result<std::size_t> add_frequency_runs(const FrequencySpec& f);

 private:
  Result<std::vector<std::size_t>> check_rows(std::vector<std::size_t> rows) const;
  QueueSpec spec_;
};

}  // namespace pychron::experiment
