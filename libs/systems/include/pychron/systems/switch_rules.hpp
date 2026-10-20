#pragma once

// The rule SwitchManager applies before it opens a switch, as functions of
// plain values: who may command it, and which partners must be where. No
// locking and no I/O, so a model of the line can judge an open against a copy
// of its state with exactly the manager's rule. Closing is never interlocked.

#include <string>
#include <string_view>
#include <vector>

#include "pychron/core/error.hpp"
#include "pychron/devices/types.hpp"

namespace pychron::systems {

// What the rule needs to know of one switch.
struct SwitchView {
  std::string name;
  ValveState state = ValveState::Unknown;
  bool locked = false;
  std::string owner;  // empty: nobody's
};

// Interlock partners of one switch, by name.
struct InterlockSet {
  std::vector<std::string> exclusive;  // negative interlocks, both directions, each once
  std::vector<std::string> positive;
};

// Interlock error if `self` is software locked or owned by someone but `actor`.
[[nodiscard]] Result<void> check_switch_access(const SwitchView& self, std::string_view actor);

// Interlock error naming the first partner in the way: an exclusive partner
// that is not closed (unknown counts), then a positive one that is not open.
[[nodiscard]] Result<void> check_switch_interlocks(const SwitchView& self,
                                                   const std::vector<const SwitchView*>& exclusive,
                                                   const std::vector<const SwitchView*>& positive);

}  // namespace pychron::systems
