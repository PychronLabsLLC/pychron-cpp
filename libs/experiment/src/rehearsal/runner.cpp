#include "pychron/experiment/rehearsal/runner.hpp"

#include <utility>

#include "pychron/core/log_context.hpp"

namespace pychron::experiment::rehearsal {

RehearsalRunner::RehearsalRunner(scripting::IScriptHost& host, scripting::Script script,
                                 scripting::ScriptEnvironment env, LineSnapshot snapshot,
                                 std::function<void(const StepRecord&)> on_step,
                                 std::function<void(const Result<scripting::ScriptResult>&)> on_finished)
    : rehearsal_(std::move(snapshot), token_) {
  rehearsal_.on_step = std::move(on_step);
  // Its own steady clock, whatever the line runs on: the thread waits on the
  // operator and is no participant of a simulated clock.
  env.clock = &clock_;
  thread_ = std::thread([this, &host, script = std::move(script), env = std::move(env),
                         on_finished = std::move(on_finished)] {
    const LogContext::Thread log_thread("rehearsal");
    const auto result = host.rehearse(script, env, rehearsal_, token_);
    if (on_finished) on_finished(result);
  });
}

RehearsalRunner::~RehearsalRunner() {
  rehearsal_.stop();
  if (thread_.joinable()) thread_.join();
}

}  // namespace pychron::experiment::rehearsal
