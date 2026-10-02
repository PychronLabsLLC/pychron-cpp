#include "pychron/experiment/run/state.hpp"

namespace pychron::experiment::run {

std::string_view to_string(RunState s) noexcept {
  switch (s) {
    case RunState::Pending: return "pending";
    case RunState::Preparing: return "preparing";
    case RunState::Extracting: return "extracting";
    case RunState::Equilibrating: return "equilibrating";
    case RunState::Measuring: return "measuring";
    case RunState::Truncated: return "truncated";
    case RunState::PostMeasuring: return "post_measuring";
    case RunState::Saving: return "saving";
    case RunState::Success: return "success";
    case RunState::Failed: return "failed";
    case RunState::Cancelled: return "cancelled";
    case RunState::Aborted: return "aborted";
  }
  return "?";
}

std::string_view to_string(RunEvent e) noexcept {
  switch (e) {
    case RunEvent::Start: return "start";
    case RunEvent::Prepared: return "prepared";
    case RunEvent::Extracted: return "extracted";
    case RunEvent::Equilibrated: return "equilibrated";
    case RunEvent::Truncate: return "truncate";
    case RunEvent::Measured: return "measured";
    case RunEvent::PostMeasured: return "post_measured";
    case RunEvent::Saved: return "saved";
    case RunEvent::Fail: return "fail";
    case RunEvent::Cancel: return "cancel";
    case RunEvent::Abort: return "abort";
  }
  return "?";
}

bool is_terminal(RunState s) noexcept {
  return s == RunState::Success || s == RunState::Failed || s == RunState::Cancelled || s == RunState::Aborted;
}

std::optional<RunState> transition(RunState from, RunEvent e) noexcept {
  using S = RunState;
  using E = RunEvent;
  if (is_terminal(from)) return std::nullopt;
  switch (e) {
    case E::Fail: return S::Failed;
    case E::Cancel: return S::Cancelled;
    case E::Abort: return S::Aborted;
    case E::Start: return from == S::Pending ? std::optional(S::Preparing) : std::nullopt;
    case E::Prepared: return from == S::Preparing ? std::optional(S::Extracting) : std::nullopt;
    case E::Extracted: return from == S::Extracting ? std::optional(S::Equilibrating) : std::nullopt;
    case E::Equilibrated: return from == S::Equilibrating ? std::optional(S::Measuring) : std::nullopt;
    case E::Truncate: return from == S::Measuring ? std::optional(S::Truncated) : std::nullopt;
    case E::Measured:
      return from == S::Equilibrating || from == S::Measuring || from == S::Truncated ? std::optional(S::PostMeasuring)
                                                                                      : std::nullopt;
    case E::PostMeasured: return from == S::PostMeasuring ? std::optional(S::Saving) : std::nullopt;
    case E::Saved: return from == S::Saving ? std::optional(S::Success) : std::nullopt;
  }
  return std::nullopt;
}

Result<RunState> RunStateMachine::advance(RunEvent e, std::string reason) {
  RunStateChanged ev;
  {
    std::lock_guard lock(mutex_);
    auto to = transition(state_, e);
    if (!to)
      return fail(ErrorKind::Config, "run " + run_id_ + ": illegal transition " + std::string(to_string(e)) +
                                         " from " + std::string(to_string(state_)));
    ev = RunStateChanged{run_id_, state_, *to, std::move(reason), clock_.now()};
    state_ = *to;
    history_.push_back(ev);
  }
  if (bus_ != nullptr) bus_->publish(ev);
  return ev.to;
}

RunState RunStateMachine::state() const {
  std::lock_guard lock(mutex_);
  return state_;
}

std::vector<RunStateChanged> RunStateMachine::history() const {
  std::lock_guard lock(mutex_);
  return history_;
}

}  // namespace pychron::experiment::run
