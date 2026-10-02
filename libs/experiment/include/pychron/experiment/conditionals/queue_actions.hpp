#pragma once

// Queue actions of modification and post-run conditionals (conditionals spec
// section 6.2, legacy L20). Applied by the executor to the rows after the
// current run; already-skipped rows are left alone.
//
//   skip_next                 skip the next run
//   skip_n N                  skip the next N runs
//   skip_aliquot              skip every following unknown of the current
//                             identifier and aliquot
//   skip_to_last_in_aliquot   ... all but the last of them
//   set_extract STEPS         successive following runs of the aliquot get
//                             value += step (or value *= 1 + step/100); stops
//                             when the steps run out
//   repeat                    insert a copy of the current run after it
//                             (aliquot cleared so it gets a new one)
//   run_blank                 insert the matching blank after it

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "pychron/core/error.hpp"
#include "pychron/experiment/conditionals/conditional.hpp"
#include "pychron/experiment/model/experiment_queue.hpp"
#include "pychron/experiment/model/identifiers.hpp"

namespace pychron::experiment {

struct QueueChange {
  std::string description;
  std::vector<std::size_t> skipped;   // rows, in the queue after the change
  std::vector<std::size_t> modified;
  std::optional<std::size_t> inserted;
  bool changed() const { return !skipped.empty() || !modified.empty() || inserted.has_value(); }
};

// The blank type that pairs with a run: air -> blank_air, cocktail ->
// blank_cocktail, blank_* stays, anything else -> blank_unknown.
AnalysisType blank_type_for(AnalysisType type) noexcept;

// Builds the blank run inserted by run_blank.
using BlankFactory = std::function<RunSpec(const RunSpec& current, AnalysisType blank_type)>;

// Copies the current run's extraction and measurement, with the blank type's
// special identifier from `rules`, no sample, aliquot and step cleared.
BlankFactory default_blank_factory(IdentifierRules rules);

// Config error for a non-queue action or a row out of range.
Result<QueueChange> apply_queue_action(ExperimentQueue& queue, std::size_t current, const ActionSpec& action,
                                       const BlankFactory& blank = default_blank_factory(IdentifierRules::defaults()));

}  // namespace pychron::experiment
