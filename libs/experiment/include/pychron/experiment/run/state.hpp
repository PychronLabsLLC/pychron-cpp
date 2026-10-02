#pragma once

// Run state machine (experiment spec 3.1).
//
//   Pending -> Preparing -> Extracting -> Equilibrating -> Measuring -> PostMeasuring -> Saving -> Success
//                                                    \--> Truncated -/          \-> Failed (save_error)
//   any non-terminal state -> Failed | Cancelled | Aborted
//
// Equilibrating may go straight to PostMeasuring (a whiff that pumps the gas,
// or a cancel during equilibration that still runs post-measurement).
// Transitions happen only through advance(); each publishes RunStateChanged.

#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pychron/core/clock.hpp"
#include "pychron/core/error.hpp"
#include "pychron/core/signal_bus.hpp"

namespace pychron::experiment::run {

enum class RunState {
  Pending, Preparing, Extracting, Equilibrating, Measuring, Truncated, PostMeasuring, Saving,
  Success, Failed, Cancelled, Aborted,
};

enum class RunEvent {
  Start,         // Pending -> Preparing
  Prepared,      // Preparing -> Extracting
  Extracted,     // Extracting -> Equilibrating
  Equilibrated,  // Equilibrating -> Measuring
  Truncate,      // Measuring -> Truncated
  Measured,      // Equilibrating | Measuring | Truncated -> PostMeasuring
  PostMeasured,  // PostMeasuring -> Saving
  Saved,         // Saving -> Success
  Fail,          // any non-terminal -> Failed
  Cancel,        // any non-terminal -> Cancelled
  Abort,         // any non-terminal -> Aborted
};

std::string_view to_string(RunState s) noexcept;
std::string_view to_string(RunEvent e) noexcept;
bool is_terminal(RunState s) noexcept;

// The state `e` leads to from `from`; nullopt when illegal.
std::optional<RunState> transition(RunState from, RunEvent e) noexcept;

struct RunStateChanged {
  std::string run_id;
  RunState from = RunState::Pending;
  RunState to = RunState::Pending;
  std::string reason;
  TimePoint ts{};
};

class RunStateMachine {
 public:
  RunStateMachine(std::string run_id, const Clock& clock, SignalBus* bus = nullptr)
      : run_id_(std::move(run_id)), clock_(clock), bus_(bus) {}

  // Config error for an illegal transition (the state is unchanged).
  Result<RunState> advance(RunEvent e, std::string reason = {});
  RunState state() const;
  // Every transition so far.
  std::vector<RunStateChanged> history() const;

 private:
  std::string run_id_;
  const Clock& clock_;
  SignalBus* bus_;
  mutable std::mutex mutex_;
  RunState state_ = RunState::Pending;
  std::vector<RunStateChanged> history_;
};

}  // namespace pychron::experiment::run
