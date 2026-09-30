#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "pychron/core/error.hpp"
#include "pychron/experiment/factory/defaults.hpp"
#include "pychron/experiment/model/run_spec.hpp"

namespace pychron::experiment {

// A reusable run sequence (block template) stored as TOML:
//   [block]  name = "blank_air_blank"  description = "..."
//   [[runs]] ...                       # same schema as experiment.toml [[runs]]
struct Block {
  std::string name, description;
  std::vector<RunSpec> runs;
};

Result<Block> parse_block(std::string_view text, const IdentifierRules& ids, std::string_view name = "block.toml");
Result<Block> load_block(const std::string& path, const IdentifierRules& ids);

struct BlockContext {
  std::string extract_device;               // fills runs with no device (where extraction is legal)
  const DefaultsTable* defaults = nullptr;  // applied to runs with no measurement plan or hook
};

// Block runs ready to insert into a queue.
std::vector<RunSpec> instantiate_block(const Block& block, const BlockContext& ctx);

// Copy of `runs` with `insert` placed before `index` (clamped to the end).
std::vector<RunSpec> insert_runs(const std::vector<RunSpec>& runs, std::size_t index, const std::vector<RunSpec>& insert);

// `runs` repeated `times` times in order.
std::vector<RunSpec> repeat_block(const std::vector<RunSpec>& runs, int times);

}  // namespace pychron::experiment
