#pragma once

#include <string_view>

#include "pychron/core/error.hpp"
#include "pychron/experiment/model/run_spec.hpp"

namespace pychron::experiment {

// Parses experiment.toml text: a [queue] table plus [[runs]]. Pychron aliases
// (e_value/extract_value, t_o, s_opt, truncate, flat extraction keys) are
// normalized here, once. Unknown keys and wrong types are errors. The run's
// AnalysisType is derived from its identifier through `ids`.
Result<QueueSpec> parse_queue(std::string_view text, const IdentifierRules& ids,
                              std::string_view name = "experiment.toml");

}  // namespace pychron::experiment
