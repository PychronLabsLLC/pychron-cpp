#include "pychron/experiment/rehearsal/rehearsal.hpp"

#include <algorithm>
#include <array>
#include <sstream>
#include <string_view>
#include <utility>

#include "pychron/experiment/lab/script_actor.hpp"

namespace pychron::experiment::rehearsal {

namespace {

constexpr std::array<std::string_view, 6> kSwitchCommands{"open", "close", "lock", "unlock", "is_open", "is_closed"};

// Reads the copy may have no value for; with none they wait for a typed one.
constexpr std::array<std::string_view, 8> kReads{"get_pressure", "get_manometer_pressure", "get_intensity",
                                                 "output",       "is_enabled",             "get_value",
                                                 "get_cryo_temp", "get_resource_value"};

bool is_one_of(std::string_view command, const auto& names) {
  return std::find(names.begin(), names.end(), command) != names.end();
}

std::string num(double v) {
  std::ostringstream out;
  out << v;
  return out.str();
}

std::optional<double> number(const scripting::Value& v) {
  if (const auto* d = std::get_if<double>(&v)) return *d;
  if (const auto* i = std::get_if<std::int64_t>(&v)) return static_cast<double>(*i);
  return std::nullopt;
}

const std::string* text_arg(const scripting::StepCall& call, std::size_t i) {
  if (call.args.size() <= i) return nullptr;
  return std::get_if<std::string>(&call.args[i]);
}

// The gauge a pressure read names: get_pressure(controller, gauge),
// get_manometer_pressure(name). Null for every other read.
const std::string* gauge_of(const scripting::StepCall& call) {
  if (call.command == "get_pressure") return text_arg(call, 1);
  if (call.command == "get_manometer_pressure") return text_arg(call, 0);
  return nullptr;
}

// Seconds a wait stands for, by where the host puts it; nullopt for a call
// that is not a wait.
std::optional<double> wait_seconds(const scripting::StepCall& call) {
  constexpr std::array<std::string_view, 6> kWaits{"sleep",    "delay",   "pause",
                                                   "complete_interval", "waitfor", "begin_heating_interval"};
  std::size_t at = 0;
  if (call.command == "ramp") {
    at = 3;  // ramp(start, end, rate, duration, period)
  } else if (!is_one_of(call.command, kWaits)) {
    return std::nullopt;
  }
  if (call.args.size() <= at) return 0.0;
  return number(call.args[at]).value_or(0.0);
}

Error cancelled() { return Error{ErrorKind::Cancelled, "aborted: rehearsal stopped", {}}; }

}  // namespace

Rehearsal::Rehearsal(LineSnapshot snapshot, scripting::CancelToken& token)
    : token_(token), state_(std::move(snapshot)) {}

void Rehearsal::report(const StepRecord& record) const {
  if (on_step) on_step(record);
}

bool Rehearsal::needs_value(const scripting::StepCall& call) const {
  if (!is_one_of(call.command, kReads)) return false;
  const std::string* gauge = gauge_of(call);
  return gauge == nullptr || !state_.pressures.contains(*gauge);
}

Result<scripting::Value> Rehearsal::step(const scripting::StepCall& call) {
  std::unique_lock lock(mutex_);
  if (stopped_) return fail(cancelled());
  const std::size_t index = ++steps_;
  const bool wants_value = needs_value(call);
  std::string note;
  bool must_wait = true;
  if (mode_ == Advance::RunToEnd) {
    if (run_left_ > 0) {
      --run_left_;
      must_wait = false;
    } else {
      mode_ = Advance::Step;
      note = "run to end paused after " + std::to_string(kRunToEndLimit) + " steps";
    }
  }
  if (wants_value) must_wait = true;  // a read with no value waits whatever the mode

  if (must_wait) {
    StepRecord waiting;
    waiting.index = index;
    waiting.call = call;
    waiting.outcome = note;
    waiting.waiting = wants_value ? Waiting::Value : Waiting::Step;
    value_waiting_ = wants_value;
    lock.unlock();
    report(waiting);
    lock.lock();
    if (wants_value)
      cv_.wait(lock, [&] { return stopped_ || value_.has_value(); });
    else
      cv_.wait(lock, [&] { return stopped_ || permits_ > 0 || mode_ == Advance::RunToEnd; });
    value_waiting_ = false;
    if (stopped_) {
      StepRecord ended;
      ended.index = index;
      ended.call = call;
      ended.outcome = "stopped";
      lock.unlock();
      report(ended);
      return fail(cancelled());
    }
    // Released by Run to End, a step uses no permit.
    if (!wants_value && mode_ == Advance::Step) --permits_;
  }

  std::optional<double> typed;
  if (wants_value) typed = std::exchange(value_, std::nullopt);
  auto answer = execute(call, typed);
  answer.record.index = index;
  if (!note.empty()) answer.record.outcome = note + "; " + answer.record.outcome;
  lock.unlock();
  report(answer.record);
  return std::move(answer.value);
}

Rehearsal::Answer Rehearsal::execute(const scripting::StepCall& call, std::optional<double> typed) {
  if (is_one_of(call.command, kSwitchCommands)) return switch_command(call);

  Answer a{scripting::Value{}, {}};
  a.record.call = call;
  if (is_one_of(call.command, kReads)) {
    double v = 0.0;
    if (typed) v = *typed;
    else if (const std::string* gauge = gauge_of(call)) v = state_.pressures.find(*gauge)->second;
    if (call.command == "is_enabled") {
      a.value = scripting::Value(v != 0.0);
      a.record.outcome = v != 0.0 ? "= true" : "= false";
    } else {
      a.value = scripting::Value(v);
      a.record.outcome = "= " + num(v);
    }
    return a;
  }
  if (const auto s = wait_seconds(call)) {
    // begin_heating_interval starts an interval; it does not wait out the time.
    const std::string verb = call.command == "begin_heating_interval" ? "begin interval" : call.command;
    a.record.outcome = *s > 0 ? verb + " " + num(*s) + " s" : verb;
    return a;
  }
  a.record.outcome = "recorded";
  return a;
}

Rehearsal::Answer Rehearsal::switch_command(const scripting::StepCall& call) {
  Answer a{scripting::Value{}, {}};
  a.record.call = call;
  const std::string* name = text_arg(call, 0);
  if (name == nullptr) {
    a.value = fail(ErrorKind::Config, "rehearse: " + call.command + " without a valve name");
    a.record.outcome = a.value.error().what;
    return a;
  }
  const auto it = state_.switches.find(*name);
  if (it == state_.switches.end()) {
    // As SwitchManager says it.
    a.value = fail(ErrorKind::Config, "unknown switch '" + *name + "'", *name);
    a.record.outcome = a.value.error().what;
    return a;
  }
  systems::SwitchView& self = it->second;
  const auto& command = call.command;

  if (command == "is_open" || command == "is_closed") {
    const bool yes = self.state == (command == "is_open" ? ValveState::Open : ValveState::Closed);
    a.value = scripting::Value(yes);
    a.record.outcome = yes ? "= true" : "= false";
    return a;
  }
  if (command == "lock" || command == "unlock") {
    const bool locked = command == "lock";
    if (self.locked != locked) a.record.changed.push_back(self.name);
    self.locked = locked;
    a.record.outcome = "would " + command;
    return a;
  }

  // open, close: the manager's rule, against the copy.
  Result<void> allowed = systems::check_switch_access(self, lab::kScriptActor);
  if (allowed && command == "open") {
    // A partner the copy does not hold is not known to be closed or open.
    std::vector<systems::SwitchView> absent;
    std::vector<const systems::SwitchView*> exclusive;
    std::vector<const systems::SwitchView*> positive;
    if (const auto set = state_.interlocks.find(self.name); set != state_.interlocks.end()) {
      absent.reserve(set->second.exclusive.size() + set->second.positive.size());
      const auto view = [&](const std::string& partner) -> const systems::SwitchView* {
        if (const auto p = state_.switches.find(partner); p != state_.switches.end()) return &p->second;
        absent.push_back(systems::SwitchView{partner, ValveState::Unknown, false, {}});
        return &absent.back();
      };
      for (const auto& partner : set->second.exclusive) exclusive.push_back(view(partner));
      for (const auto& partner : set->second.positive) positive.push_back(view(partner));
    }
    allowed = systems::check_switch_interlocks(self, exclusive, positive);
  }
  if (!allowed) {
    a.record.refused = true;
    a.record.outcome = "would be refused: " + allowed.error().what;
    a.value = fail(std::move(allowed).error());
    return a;
  }
  const ValveState target = command == "open" ? ValveState::Open : ValveState::Closed;
  if (self.state != target) a.record.changed.push_back(self.name);
  self.state = target;
  a.record.outcome = "would " + command;
  return a;
}

void Rehearsal::advance(Advance how) {
  {
    std::lock_guard lock(mutex_);
    if (how == Advance::RunToEnd) {
      mode_ = Advance::RunToEnd;
      run_left_ = kRunToEndLimit;
    } else if (!value_waiting_) {
      // The value is what lets a waiting read go on.
      mode_ = Advance::Step;
      ++permits_;
    }
  }
  cv_.notify_all();
}

void Rehearsal::provide_value(double v) {
  {
    std::lock_guard lock(mutex_);
    value_ = v;
  }
  cv_.notify_all();
}

void Rehearsal::stop() {
  // Abort first, so that the host finds the token aborted when step returns.
  token_.abort();
  {
    std::lock_guard lock(mutex_);
    stopped_ = true;
  }
  cv_.notify_all();
}

LineSnapshot Rehearsal::state() const {
  std::lock_guard lock(mutex_);
  return state_;
}

}  // namespace pychron::experiment::rehearsal
