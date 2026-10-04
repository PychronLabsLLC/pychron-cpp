#pragma once

// The identifier planner (sample and package entry spec, section 8): pure,
// so the preview a user approves is exactly what allocate_identifiers writes.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "pychron/entry/settings.hpp"
#include "pychron/persistence/store.hpp"

namespace pychron::entry {

struct PlannedIdentifier {
  persistence::Uuid position;
  std::string level;
  int position_number = 0;
  std::string sample;
  std::optional<std::string> current;  // the identifier it replaces
  std::int64_t number = 0;
  std::optional<persistence::Uuid> replaces;
  friend bool operator==(const PlannedIdentifier&, const PlannedIdentifier&) = default;
};

struct IdentifierPlan {
  std::int64_t expected_last = 0;  // the counter the plan starts from
  std::int64_t last = 0;           // the counter after it
  std::vector<PlannedIdentifier> assignments;  // in (level name, position) order
  persistence::IdentifierAllocation allocation() const;
};

// Every position with a sample, in level-name then position order, gets
// last + 1, last + 2, ... A position that has an identifier is skipped unless
// `overwrite`, and always when it is analyzed or loaded.
IdentifierPlan plan_identifiers(std::vector<persistence::LevelSheet> sheets, std::int64_t last, bool overwrite);

// Warnings before identifiers are generated (legacy labnumber_entry.py:464-534):
// a level with positions but no monitor, and a monitor outside the project
// <irradiation_project_prefix><package>.
std::vector<std::string> human_error_checks(const std::vector<persistence::LevelSheet>& sheets,
                                            const EntrySettings& settings, const std::string& package);

// The counter value a plan starts from: identifier_counter, or the largest
// numeric identifier before the first allocation.
Result<std::int64_t> current_last(persistence::IStore& store);

// Every level sheet of a package, in level-name order.
Result<std::vector<persistence::LevelSheet>> package_sheets(persistence::IStore& store, persistence::Uuid package);

}  // namespace pychron::entry
