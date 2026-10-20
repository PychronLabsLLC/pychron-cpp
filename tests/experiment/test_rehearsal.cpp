// Rehearsal: a script stepped against a copy of the line, judged with the
// switch manager's rule, held between steps by the operator. Not on a
// simulated path: the threads here are plain std::threads, and every bound is
// a real-time upper bound of 5 s, there only to tell a stall.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include "pychron/devices/extraction/capability.hpp"
#include "pychron/experiment/rehearsal/rehearsal.hpp"
#include "pychron/experiment/rehearsal/runner.hpp"
#include "pychron/scripting/script_host.hpp"

namespace rh = pychron::experiment::rehearsal;
using pychron::ErrorKind;
using pychron::Result;
using pychron::ValveState;
using pychron::scripting::CancelMode;
using pychron::scripting::CancelToken;
using pychron::scripting::StepCall;
using pychron::scripting::Value;

namespace {

constexpr auto kLimit = std::chrono::seconds(5);

rh::LineSnapshot line() {
  rh::LineSnapshot s;
  for (const char* name : {"A", "B", "C", "P1", "P2"}) {
    s.switches[name] = pychron::systems::SwitchView{name, ValveState::Closed, false, {}};
    s.interlocks[name] = {};
  }
  return s;
}

// P1 and P2 may not be open together.
rh::LineSnapshot pipette_line() {
  auto s = line();
  s.interlocks["P1"].exclusive = {"P2"};
  s.interlocks["P2"].exclusive = {"P1"};
  return s;
}

StepCall call(std::string command, std::vector<Value> args = {}) {
  StepCall c;
  c.command = std::move(command);
  c.args = std::move(args);
  c.script = "inline.py";
  c.line = 3;
  return c;
}

// Every record on_step was given, and a wait for one in real time.
class Records {
 public:
  std::function<void(const rh::StepRecord&)> sink() {
    return [this](const rh::StepRecord& r) {
      {
        std::lock_guard lock(mutex_);
        records_.push_back(r);
      }
      cv_.notify_all();
    };
  }
  bool await(const std::function<bool(const std::vector<rh::StepRecord>&)>& done) {
    std::unique_lock lock(mutex_);
    return cv_.wait_for(lock, kLimit, [&] { return done(records_); });
  }
  bool await_waiting(rh::Waiting w, std::size_t index = 0) {
    return await([&](const std::vector<rh::StepRecord>& rs) {
      return std::any_of(rs.begin(), rs.end(), [&](const rh::StepRecord& r) {
        return r.waiting == w && (index == 0 || r.index == index);
      });
    });
  }
  std::vector<rh::StepRecord> all() const {
    std::lock_guard lock(mutex_);
    return records_;
  }
  rh::StepRecord last() const {
    std::lock_guard lock(mutex_);
    return records_.empty() ? rh::StepRecord{} : records_.back();
  }

 private:
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::vector<rh::StepRecord> records_;
};

Result<Value> finish(std::future<Result<Value>>& f) {
  if (f.wait_for(kLimit) != std::future_status::ready) {
    ADD_FAILURE() << "step did not return within 5 s";
    return pychron::fail(ErrorKind::Timeout, "stalled");
  }
  return f.get();
}

class Rehearsal : public ::testing::Test {
 protected:
  void make(rh::LineSnapshot s) {
    rehearsal = std::make_unique<rh::Rehearsal>(std::move(s), token);
    rehearsal->on_step = records.sink();
  }
  // One step on a thread of its own.
  std::future<Result<Value>> step_async(StepCall c) {
    return std::async(std::launch::async, [r = rehearsal.get(), c = std::move(c)] { return r->step(c); });
  }
  // Steps one after the other on a thread of its own.
  std::future<Result<Value>> steps_async(std::vector<StepCall> calls) {
    return std::async(std::launch::async, [r = rehearsal.get(), calls = std::move(calls)]() -> Result<Value> {
      Result<Value> last = Value{};
      for (const auto& c : calls) {
        last = r->step(c);
        if (!last) return last;
      }
      return last;
    });
  }
  void TearDown() override {
    // A step left blocked by a failed assertion is woken before the
    // rehearsal goes.
    if (rehearsal) rehearsal->stop();
  }
  CancelToken token;
  Records records;
  std::unique_ptr<rh::Rehearsal> rehearsal;
};

// As editor_environment sets it: every capability, the line's valve names,
// no live service.
pychron::scripting::ScriptEnvironment rehearse_environment() {
  pychron::scripting::ScriptEnvironment env;
  pychron::extraction::CapabilitySet all;
  for (std::size_t i = 0; i < pychron::extraction::kCapabilityCount; ++i)
    all.add(static_cast<pychron::extraction::Capability>(i));
  env.capabilities = all;
  env.valve_names = std::vector<std::string>{"A", "B", "C", "P1", "P2"};
  env.context = pychron::scripting::make_context({{"analysis_type", std::string("unknown")}, {"duration", 5.0}});
  env.log = [](std::string_view) {};
  return env;
}

pychron::scripting::Script example(const std::string& rel, pychron::scripting::ScriptKind kind) {
  std::ifstream in(std::string(PYCHRON_EXAMPLE_CONFIGS_DIR) + "/scripts/" + rel);
  std::ostringstream text;
  text << in.rdbuf();
  return pychron::scripting::Script{rel, text.str(), kind};
}

// What a runner's on_finished was called with, and how often.
class Finished {
 public:
  std::function<void(const Result<pychron::scripting::ScriptResult>&)> sink() {
    return [this](const Result<pychron::scripting::ScriptResult>& r) {
      {
        std::lock_guard lock(mutex_);
        ++calls_;
        ok_ = r.has_value();
        if (!r) kind_ = r.error().kind, what_ = r.error().what;
      }
      cv_.notify_all();
    };
  }
  bool await() {
    std::unique_lock lock(mutex_);
    return cv_.wait_for(lock, kLimit, [&] { return calls_ > 0; });
  }
  int calls() const {
    std::lock_guard lock(mutex_);
    return calls_;
  }
  bool ok() const {
    std::lock_guard lock(mutex_);
    return ok_;
  }
  ErrorKind kind() const {
    std::lock_guard lock(mutex_);
    return kind_;
  }
  std::string what() const {
    std::lock_guard lock(mutex_);
    return what_;
  }

 private:
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  int calls_ = 0;
  bool ok_ = false;
  ErrorKind kind_ = ErrorKind::Io;
  std::string what_;
};

}  // namespace

TEST_F(Rehearsal, OpenChangesRehearsalStateOnly) {
  make(line());
  rehearsal->advance(rh::Advance::Step);
  auto f = step_async(call("open", {Value(std::string("P2"))}));
  auto r = finish(f);
  ASSERT_TRUE(r) << r.error().what;
  const auto s = rehearsal->state();
  EXPECT_EQ(s.switches.at("P2").state, ValveState::Open);
  for (const char* other : {"A", "B", "C", "P1"}) EXPECT_EQ(s.switches.at(other).state, ValveState::Closed) << other;
  const auto rs = records.all();
  ASSERT_EQ(rs.size(), 2U);
  EXPECT_EQ(rs[0].waiting, rh::Waiting::Step);
  EXPECT_EQ(rs[0].index, 1U);
  EXPECT_EQ(rs[1].waiting, rh::Waiting::None);
  EXPECT_EQ(rs[1].index, 1U);
  EXPECT_EQ(rs[1].outcome, "would open");
  EXPECT_EQ(rs[1].changed, std::vector<std::string>{"P2"});
  EXPECT_FALSE(rs[1].refused);
  EXPECT_EQ(rs[1].call.command, "open");
  EXPECT_EQ(rs[1].call.line, 3);
}

TEST_F(Rehearsal, InterlockedOpenRefused) {
  auto s = pipette_line();
  s.switches["P1"].state = ValveState::Open;
  make(std::move(s));
  rehearsal->advance(rh::Advance::Step);
  auto f = step_async(call("open", {Value(std::string("P2"))}));
  auto r = finish(f);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Interlock);
  EXPECT_EQ(r.error().what, "cannot open 'P2': interlocked with 'P1' which is open");
  EXPECT_EQ(rehearsal->state().switches.at("P2").state, ValveState::Closed);
  const auto last = records.last();
  EXPECT_TRUE(last.refused);
  EXPECT_EQ(last.outcome, "would be refused: cannot open 'P2': interlocked with 'P1' which is open");
  EXPECT_TRUE(last.changed.empty());
}

TEST_F(Rehearsal, UnknownPartnerRefusesOpen) {
  auto s = pipette_line();
  s.switches["P1"].state = ValveState::Unknown;
  make(std::move(s));
  rehearsal->advance(rh::Advance::Step);
  auto f = step_async(call("open", {Value(std::string("P2"))}));
  auto r = finish(f);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Interlock);
  EXPECT_TRUE(records.last().refused);
  EXPECT_EQ(rehearsal->state().switches.at("P2").state, ValveState::Closed);
}

TEST_F(Rehearsal, LockedValveRefusedForScriptActor) {
  auto s = line();
  s.switches["P2"].locked = true;
  make(std::move(s));
  rehearsal->advance(rh::Advance::Step);
  auto f = step_async(call("open", {Value(std::string("P2"))}));
  auto r = finish(f);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Interlock);
  EXPECT_EQ(r.error().what, "'P2' is software locked");
  EXPECT_TRUE(records.last().refused);
}

TEST_F(Rehearsal, IsOpenFollowsEarlierStep) {
  make(line());
  rehearsal->advance(rh::Advance::RunToEnd);
  const Value p2{std::string("P2")};
  auto before = step_async(call("is_open", {p2}));
  auto b = finish(before);
  ASSERT_TRUE(b) << b.error().what;
  EXPECT_EQ(std::get<bool>(*b), false);
  auto opened = steps_async({call("open", {p2})});
  ASSERT_TRUE(finish(opened));
  auto is_open = step_async(call("is_open", {p2}));
  auto o = finish(is_open);
  ASSERT_TRUE(o);
  EXPECT_EQ(std::get<bool>(*o), true);
  EXPECT_EQ(records.last().outcome, "= true");
  auto is_closed = step_async(call("is_closed", {p2}));
  auto c = finish(is_closed);
  ASSERT_TRUE(c);
  EXPECT_EQ(std::get<bool>(*c), false);
}

TEST_F(Rehearsal, PressureFromSnapshot) {
  auto s = line();
  s.pressures["ion"] = 2e-9;
  make(std::move(s));
  rehearsal->advance(rh::Advance::Step);
  auto f = step_async(call("get_pressure", {Value(std::string("ig")), Value(std::string("ion"))}));
  auto r = finish(f);
  ASSERT_TRUE(r) << r.error().what;
  EXPECT_DOUBLE_EQ(std::get<double>(*r), 2e-9);
  for (const auto& rec : records.all()) EXPECT_NE(rec.waiting, rh::Waiting::Value);
  EXPECT_EQ(records.last().outcome, "= 2e-09");
}

TEST_F(Rehearsal, PressureWithoutReadingWaitsForValue) {
  make(line());
  auto f = step_async(call("get_pressure", {Value(std::string("ig")), Value(std::string("ion"))}));
  ASSERT_TRUE(records.await_waiting(rh::Waiting::Value));
  EXPECT_EQ(f.wait_for(std::chrono::milliseconds(0)), std::future_status::timeout);
  rehearsal->provide_value(1.5);
  auto r = finish(f);
  ASSERT_TRUE(r) << r.error().what;
  EXPECT_DOUBLE_EQ(std::get<double>(*r), 1.5);
  const auto rs = records.all();
  ASSERT_EQ(rs.size(), 2U);
  EXPECT_EQ(rs[0].waiting, rh::Waiting::Value);
  EXPECT_EQ(rs[1].outcome, "= 1.5");
}

TEST_F(Rehearsal, ABoolReadAnswersNonZero) {
  make(line());
  auto f = step_async(call("is_enabled"));
  ASSERT_TRUE(records.await_waiting(rh::Waiting::Value));
  rehearsal->provide_value(1.0);
  auto r = finish(f);
  ASSERT_TRUE(r) << r.error().what;
  EXPECT_EQ(std::get<bool>(*r), true);
}

TEST_F(Rehearsal, UnknownValveIsConfig) {
  make(line());
  rehearsal->advance(rh::Advance::Step);
  auto f = step_async(call("open", {Value(std::string("Z"))}));
  auto r = finish(f);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Config);
  EXPECT_EQ(r.error().what, "unknown switch 'Z'");
}

TEST_F(Rehearsal, WaitsAreRecordedWithTheirDuration) {
  make(line());
  rehearsal->advance(rh::Advance::RunToEnd);
  auto f = steps_async({call("sleep", {Value(2.0)})});
  ASSERT_TRUE(finish(f));
  EXPECT_EQ(records.last().outcome, "sleep 2 s");
  auto g = steps_async({call("extract", {Value(5.0), Value(std::string("percent"))})});
  ASSERT_TRUE(finish(g));
  EXPECT_EQ(records.last().outcome, "recorded");
}

TEST_F(Rehearsal, StopWakesABlockedStep) {
  make(line());
  auto f = step_async(call("sleep", {Value(2.0)}));
  ASSERT_TRUE(records.await_waiting(rh::Waiting::Step));
  rehearsal->stop();
  auto r = finish(f);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Cancelled);
  EXPECT_EQ(token.mode(), CancelMode::Abort);
  // and every step after it
  auto g = step_async(call("sleep", {Value(2.0)}));
  auto again = finish(g);
  ASSERT_FALSE(again);
  EXPECT_EQ(again.error().kind, ErrorKind::Cancelled);
}

TEST_F(Rehearsal, RunToEndDoesNotBlock) {
  make(line());
  rehearsal->advance(rh::Advance::RunToEnd);
  std::vector<StepCall> calls(5, call("sleep", {Value(2.0)}));
  auto f = steps_async(std::move(calls));
  ASSERT_TRUE(finish(f));
  const auto rs = records.all();
  ASSERT_EQ(rs.size(), 5U);
  for (const auto& r : rs) {
    EXPECT_EQ(r.waiting, rh::Waiting::None);
    EXPECT_EQ(r.outcome, "sleep 2 s");
  }
}

TEST_F(Rehearsal, RunToEndReleasesABlockedStep) {
  make(line());
  auto f = step_async(call("sleep", {Value(2.0)}));
  ASSERT_TRUE(records.await_waiting(rh::Waiting::Step));
  rehearsal->advance(rh::Advance::RunToEnd);
  ASSERT_TRUE(finish(f));
}

TEST_F(Rehearsal, RunToEndFallsBackToStepAfterLimit) {
  make(line());
  rehearsal->advance(rh::Advance::RunToEnd);
  std::vector<StepCall> calls(rh::Rehearsal::kRunToEndLimit + 1, call("sleep", {Value(1.0)}));
  auto f = steps_async(std::move(calls));
  ASSERT_TRUE(records.await_waiting(rh::Waiting::Step, rh::Rehearsal::kRunToEndLimit + 1));
  EXPECT_EQ(f.wait_for(std::chrono::milliseconds(0)), std::future_status::timeout);
  const auto waiting = records.last();
  EXPECT_EQ(waiting.index, rh::Rehearsal::kRunToEndLimit + 1);
  EXPECT_EQ(waiting.outcome.rfind("run to end paused after 1000 steps", 0), 0U) << waiting.outcome;
  rehearsal->advance(rh::Advance::Step);
  ASSERT_TRUE(finish(f));
  const auto done = records.last();
  EXPECT_EQ(done.outcome.rfind("run to end paused after 1000 steps", 0), 0U) << done.outcome;
  EXPECT_EQ(records.all().size(), rh::Rehearsal::kRunToEndLimit + 2);
}

TEST_F(Rehearsal, DestroyWhileWaitingForValueJoins) {
  if (!pychron::scripting::scripting_enabled()) GTEST_SKIP() << "built without PYCHRON_SCRIPTING";
  auto host = pychron::scripting::make_script_host();
  Records steps;
  Finished finished;
  const pychron::scripting::Script script{"post_measurement/inline.py",
                                           "def main():\n"
                                           "    signal_pump_time_start()\n"
                                           "    x = get_intensity('Ar40')\n"
                                           "    open('C')\n",
                                           pychron::scripting::ScriptKind::PostMeasurement};
  auto runner = std::make_unique<rh::RehearsalRunner>(*host, script, rehearse_environment(), line(), steps.sink(),
                                                      finished.sink());
  runner->rehearsal().advance(rh::Advance::RunToEnd);
  ASSERT_TRUE(steps.await_waiting(rh::Waiting::Value));
  EXPECT_EQ(steps.last().call.command, "get_intensity");
  const auto t0 = std::chrono::steady_clock::now();
  runner.reset();
  EXPECT_LT(std::chrono::steady_clock::now() - t0, kLimit);
  EXPECT_EQ(finished.calls(), 1);
  EXPECT_FALSE(finished.ok());
  EXPECT_EQ(finished.kind(), ErrorKind::Cancelled) << finished.what();
}

TEST_F(Rehearsal, RunnerRehearsesSimExtract) {
  if (!pychron::scripting::scripting_enabled()) GTEST_SKIP() << "built without PYCHRON_SCRIPTING";
  auto host = pychron::scripting::make_script_host();
  Records steps;
  Finished finished;
  auto s = pipette_line();
  s.switches["C"].state = ValveState::Open;
  rh::RehearsalRunner runner(*host, example("extraction/sim_extract.py", pychron::scripting::ScriptKind::Extraction),
                             rehearse_environment(), std::move(s), steps.sink(), finished.sink());
  runner.rehearsal().advance(rh::Advance::RunToEnd);
  ASSERT_TRUE(finished.await());
  ASSERT_TRUE(finished.ok()) << finished.what();
  const auto state = runner.rehearsal().state();
  for (const char* name : {"C", "P1", "P2"}) EXPECT_EQ(state.switches.at(name).state, ValveState::Closed) << name;
  const auto rs = steps.all();
  ASSERT_FALSE(rs.empty());
  EXPECT_EQ(rs.front().call.command, "close");
  EXPECT_EQ(rs.front().changed, std::vector<std::string>{"C"});
}
