#pragma once

// Per-group statistics produced by the group_stats unit (design 7.5).

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "pychron/reduction/stats.hpp"

namespace pychron::processing {

struct GroupResult {
  int graph = 0, group = 0;
  std::string name;
  std::string quantity;
  std::optional<reduction::Mean> mean;  // absent when no included analysis has the quantity
  std::size_t n_total = 0;              // analyses in the group
  std::size_t n_included = 0;           // included analyses with a value
  std::string error;                    // why mean is absent
};

struct GroupResults {
  std::vector<GroupResult> rows;
};

using GroupResultsPtr = std::shared_ptr<const GroupResults>;

}  // namespace pychron::processing
