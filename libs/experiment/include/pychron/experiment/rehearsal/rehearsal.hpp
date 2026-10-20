#pragma once

// A script rehearsal over a copy of the line (spec
// docs/superpowers/specs/2026-10-20-script-rehearsal-design.md, section 5).
//
// Rehearsal is the IRehearsal a rehearse() run steps through. It holds a copy
// of the line's switches, their interlocks and the latest pressures, judges
// every open and close with the manager's own rule (switch_rules.hpp) for the
// actor "script", and changes only that copy. Between steps it blocks the
// script's thread until the operator says go on (advance) or answers a read
// the copy has no value for (provide_value).
//
// The script's thread is not on a simulated path: it waits on the operator,
// on real time, with a plain mutex and condition variable, and is never a
// Clock::Participant.

#include <cstddef>
#include <functional>
#include <map>
#include <mutex>
#include <condition_variable>
#include <optional>
#include <string>
#include <vector>

#include "pychron/core/error.hpp"
#include "pychron/scripting/cancel_token.hpp"
#include "pychron/scripting/rehearsal.hpp"
#include "pychron/systems/switch_rules.hpp"

namespace pychron::experiment::rehearsal {

// Taken once from the live line when a rehearsal starts; never read again.
struct LineSnapshot {
  std::map<std::string, systems::SwitchView, std::less<>> switches;
  std::map<std::string, systems::InterlockSet, std::less<>> interlocks;
  std::map<std::string, double, std::less<>> pressures;  // keyed by gauge name
};

enum class Advance { Step, RunToEnd };

// What a step waits for: nothing, the operator's go-ahead, or a typed value.
enum class Waiting { None, Step, Value };

struct StepRecord {
  std::size_t index = 0;  // from 1
  scripting::StepCall call;
  // "would open", "would be refused: <text>", "sleep 2 s", "= 2.5e-09",
  // "recorded"; empty in a record that is waiting.
  std::string outcome;
  std::vector<std::string> changed;  // switches whose rehearsal state this step changed
  Waiting waiting = Waiting::None;
  bool refused = false;
};

class Rehearsal final : public scripting::IRehearsal {
 public:
  // Run to End lets this many steps through without a stop, then falls back
  // to Step: a script in a loop does not run on for good.
  static constexpr std::size_t kRunToEndLimit = 1000;

  // `token` is the token the rehearse() run is checked against; it outlives
  // the rehearsal.
  explicit Rehearsal(LineSnapshot snapshot, scripting::CancelToken& token);

  // On the script's thread. Calls on_step with the record waiting (not under
  // Run to End while the limit is not reached), blocks, then calls it again
  // with the outcome. A refused open or close returns the Interlock error.
  // Cancelled once stop() has been called.
  Result<scripting::Value> step(const scripting::StepCall& call) override;

  // Any thread. Step: lets the next stop go on (one each call; a call made
  // before the step reaches its stop is kept for it; ignored while a read
  // waits for a value) and ends a Run to End. RunToEnd: no further stops,
  // up to kRunToEndLimit steps; a read with no value still waits for one.
  void advance(Advance how);
  // Any thread. Answers the read waiting for a value, or the next one.
  // A bool read answers v != 0.
  void provide_value(double v);
  // Any thread. Aborts the token and wakes a blocked step.
  void stop();
  // Copy of the rehearsal's state, under the mutex.
  LineSnapshot state() const;

  // Set before the run starts. Called on the script's thread, with the
  // rehearsal's mutex released.
  std::function<void(const StepRecord&)> on_step;

 private:
  struct Answer {
    Result<scripting::Value> value;
    StepRecord record;
  };

  // Caller holds mutex_; the go-ahead (or the value) has been had.
  Answer execute(const scripting::StepCall& call, std::optional<double> typed);
  Answer switch_command(const scripting::StepCall& call);
  // Caller holds mutex_. True when the call is a read the copy cannot
  // answer, so it waits for a typed value.
  bool needs_value(const scripting::StepCall& call) const;
  void report(const StepRecord& record) const;

  scripting::CancelToken& token_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;  // advance, provide_value, stop
  LineSnapshot state_;
  Advance mode_ = Advance::Step;
  std::size_t run_left_ = 0;  // steps left in the Run to End run
  std::size_t permits_ = 0;   // advance(Step) calls not yet used
  std::optional<double> value_;  // provide_value not yet used
  bool value_waiting_ = false;   // a read is waiting for a value
  bool stopped_ = false;
  std::size_t steps_ = 0;
};

}  // namespace pychron::experiment::rehearsal
