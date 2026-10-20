#pragma once

// RehearsalRunner: one rehearsal on one thread of its own (spec
// docs/superpowers/specs/2026-10-20-script-rehearsal-design.md, section 5.4).
//
// The thread runs host.rehearse(script, env, rehearsal(), token) on a
// SteadyClock of the runner's (whatever env.clock was), then calls
// on_finished with the result from that thread. It is not on a simulated
// path: it waits on the operator, on real time, and is never a
// Clock::Participant. The destructor stops the rehearsal and joins.

#include <functional>
#include <thread>

#include "pychron/core/clock.hpp"
#include "pychron/core/error.hpp"
#include "pychron/experiment/rehearsal/rehearsal.hpp"
#include "pychron/scripting/cancel_token.hpp"
#include "pychron/scripting/script_host.hpp"

namespace pychron::experiment::rehearsal {

class RehearsalRunner {
 public:
  // `host` outlives the runner. on_step is Rehearsal::on_step; both
  // callbacks are called on the runner's thread.
  RehearsalRunner(scripting::IScriptHost& host, scripting::Script script, scripting::ScriptEnvironment env,
                  LineSnapshot snapshot, std::function<void(const StepRecord&)> on_step,
                  std::function<void(const Result<scripting::ScriptResult>&)> on_finished);
  // stop(), then join.
  ~RehearsalRunner();
  RehearsalRunner(const RehearsalRunner&) = delete;
  RehearsalRunner& operator=(const RehearsalRunner&) = delete;
  RehearsalRunner(RehearsalRunner&&) = delete;
  RehearsalRunner& operator=(RehearsalRunner&&) = delete;

  Rehearsal& rehearsal() { return rehearsal_; }

 private:
  // Declared in the order the thread needs them: all are built before the
  // thread starts and outlive its join.
  SteadyClock clock_;
  scripting::CancelToken token_;
  Rehearsal rehearsal_;
  std::thread thread_;
};

}  // namespace pychron::experiment::rehearsal
