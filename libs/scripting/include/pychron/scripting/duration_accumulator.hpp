#pragma once

// DurationAccumulator: the host behind estimate(). Commands add their nominal
// time instead of touching hardware; loops whose length cannot be known are
// flagged rather than guessed.

#include <string>
#include <vector>

#include "pychron/core/clock.hpp"

namespace pychron::scripting {

struct DurationEntry {
  std::string script;  // the script (or gosub) that spent it
  int line = 0;
  std::string command;
  Duration duration{};
};

class DurationAccumulator {
 public:
  void add(Duration d, std::string command, std::string script = {}, int line = 0);
  // Something the estimate cannot bound (while loops, open-ended waits).
  void flag_unbounded(std::string reason);

  Duration total() const noexcept { return total_; }
  const std::vector<DurationEntry>& entries() const noexcept { return entries_; }
  const std::vector<std::string>& unbounded() const noexcept { return unbounded_; }
  bool bounded() const noexcept { return unbounded_.empty(); }

  // Estimated elapsed time so far (for begin_interval/complete_interval).
  TimePoint now() const noexcept { return TimePoint{} + total_; }

 private:
  Duration total_{};
  std::vector<DurationEntry> entries_;
  std::vector<std::string> unbounded_;
};

}  // namespace pychron::scripting
