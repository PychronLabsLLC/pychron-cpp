// Rehearse mode: every hardware call and wait of a script goes through one
// IRehearsal::step, and nothing touches hardware.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <set>
#include <thread>
#include <sstream>

#include "fakes.hpp"
#include "pychron/devices/extraction/capability.hpp"
#include "pychron/scripting/rehearsal.hpp"
#include "pychron/scripting/script_host.hpp"
#include "pychron/scripting/vocabulary.hpp"
#include "rehearse_fakes.hpp"

using namespace pychron;
using namespace pychron::scripting;
using namespace pychron::scripting::testing;

namespace {

[[maybe_unused]] Script example(const std::string& rel, ScriptKind kind) {
  std::ifstream in(std::string(PYCHRON_EXAMPLE_CONFIGS_DIR) + "/scripts/" + rel);
  std::ostringstream text;
  text << in.rdbuf();
  return Script{rel, text.str(), kind};
}

[[maybe_unused]] Script inline_script(std::string text, ScriptKind kind = ScriptKind::Extraction) {
  return Script{"inline.py", std::move(text), kind};
}

// As editor_environment sets it: every capability, the line's valve names,
// no live service.
[[maybe_unused]] ScriptEnvironment rehearse_environment(const std::string& analysis_type = "unknown") {
  ScriptEnvironment env;
  extraction::CapabilitySet all;
  for (std::size_t i = 0; i < extraction::kCapabilityCount; ++i) all.add(static_cast<extraction::Capability>(i));
  env.capabilities = all;
  env.valve_names = std::vector<std::string>{"A", "B", "C", "P1", "P2"};
  env.context = make_context({{"analysis_type", analysis_type}, {"duration", 5.0}});
  env.log = [](std::string_view) {};
  return env;
}

class FakeCryo final : public extraction::ICryo {
 public:
  Result<void> set_cryo(double) override { return {}; }
  Result<double> get_cryo_temp(int) override { return 0.0; }
};

class Rehearse : public ::testing::Test {
 protected:
  void SetUp() override {
#if PYCHRON_SCRIPTING_ENABLED
    if (!scripting_enabled()) GTEST_SKIP() << "built without PYCHRON_SCRIPTING";
#endif
    host = make_script_host();
  }
  RecordingRehearsal rehearsal;
  CancelToken token;
  std::unique_ptr<IScriptHost> host;
};

}  // namespace

#if PYCHRON_SCRIPTING_ENABLED

TEST_F(Rehearse, SimExtractStepsInOrder) {
  auto r = host->rehearse(example("extraction/sim_extract.py", ScriptKind::Extraction), rehearse_environment(),
                          rehearsal, token);
  ASSERT_TRUE(r) << to_string(r.error());
  const std::vector<std::string> expected = {"close C", "open P2",  "sleep 2", "close P2", "open P1",
                                             "sleep 2", "close P1", "sleep 5", "sleep 2"};
  EXPECT_EQ(rehearsal.lines(), expected);
  ASSERT_GE(rehearsal.calls.size(), 2U);
  EXPECT_EQ(rehearsal.calls[0].line, 12);
  EXPECT_EQ(rehearsal.calls[1].line, 14);
  for (const auto& c : rehearsal.calls) {
    EXPECT_EQ(c.script, "extraction/sim_extract.py");
    EXPECT_TRUE(c.gosubs.empty());
  }
}

TEST_F(Rehearse, BlankSkipsThePipette) {
  auto r = host->rehearse(example("extraction/sim_extract.py", ScriptKind::Extraction),
                          rehearse_environment("blank_unknown"), rehearsal, token);
  ASSERT_TRUE(r) << to_string(r.error());
  const std::vector<std::string> expected = {"close C", "sleep 5", "sleep 2"};
  EXPECT_EQ(rehearsal.lines(), expected);
}

TEST_F(Rehearse, InfoDoesNotStep) {
  auto r = host->rehearse(example("extraction/sim_extract.py", ScriptKind::Extraction), rehearse_environment(),
                          rehearsal, token);
  ASSERT_TRUE(r) << to_string(r.error());
  EXPECT_EQ(r->messages.size(), 1U);
  for (const auto& c : rehearsal.calls) EXPECT_NE(c.command, "info");
}

TEST_F(Rehearse, RefusesEveryLiveService) {
  CallLog log;
  FakeValves valves{log};
  FakePressure pressure;
  FakeLaser laser{log};
  FakeCryo cryo;
  FakeResources resources;
  FakeIntensity intensity;
  FakeMeasurementApi api{log};
  const std::vector<std::pair<std::string, std::function<void(ScriptEnvironment&)>>> fields = {
      {"line.device", [&](ScriptEnvironment& e) { e.line.device = &laser; }},
      {"line.valves", [&](ScriptEnvironment& e) { e.line.valves = &valves; }},
      {"line.pressure", [&](ScriptEnvironment& e) { e.line.pressure = &pressure; }},
      {"line.cryo", [&](ScriptEnvironment& e) { e.line.cryo = &cryo; }},
      {"intensity", [&](ScriptEnvironment& e) { e.intensity = &intensity; }},
      {"measurement", [&](ScriptEnvironment& e) { e.measurement = &api; }},
      {"resources", [&](ScriptEnvironment& e) { e.resources = &resources; }},
      {"on_pump_time_start", [&](ScriptEnvironment& e) { e.on_pump_time_start = [] {}; }},
  };
  for (const auto& [name, set] : fields) {
    SCOPED_TRACE(name);
    auto env = rehearse_environment();
    set(env);
    auto r = host->rehearse(example("extraction/sim_extract.py", ScriptKind::Extraction), env, rehearsal, token);
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().kind, ErrorKind::Config);
    EXPECT_NE(r.error().what.find(name), std::string::npos) << r.error().what;
    EXPECT_TRUE(rehearsal.calls.empty());
    EXPECT_TRUE(log.snapshot().empty());
  }
}

TEST_F(Rehearse, CheckFailureStepsNothing) {
  auto r = host->rehearse(inline_script("def main():\n    open('Z')\n"), rehearse_environment(), rehearsal, token);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Config);
  EXPECT_TRUE(rehearsal.calls.empty());
}

TEST_F(Rehearse, StepErrorRaisesAsTheRealCallWould) {
  rehearsal.respond = [](const StepCall& c) -> Result<Value> {
    if (c.command == "open" && !c.args.empty() && c.args[0] == Value(std::string("P2")))
      return fail(ErrorKind::Interlock, "interlocked", "P2");
    return Value{};
  };
  auto r = host->rehearse(example("extraction/sim_extract.py", ScriptKind::Extraction), rehearse_environment(),
                          rehearsal, token);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Interlock);
  EXPECT_EQ(r.error().what, "interlocked");
  ASSERT_FALSE(rehearsal.lines().empty());
  EXPECT_EQ(rehearsal.lines().back(), "open P2");
}

// Scripts reach the host's now() through begin_interval/complete_interval
// (`now` itself is not in the vocabulary): a 5 s interval with a stepped 3 s
// sleep in it has 2 s left.
TEST_F(Rehearse, NowAdvancesBySteppedWaits) {
  auto r = host->rehearse(
      inline_script("def main():\n    begin_interval(5)\n    sleep(3)\n    complete_interval()\n"),
      rehearse_environment(), rehearsal, token);
  ASSERT_TRUE(r) << to_string(r.error());
  const std::vector<std::string> expected = {"sleep 3", "complete_interval 2"};
  EXPECT_EQ(rehearsal.lines(), expected);
}

TEST_F(Rehearse, IsOpenAnswersFromTheStep) {
  rehearsal.answers["is_open"] = true;
  auto r = host->rehearse(inline_script("def main():\n    info(str(is_open('A')))\n"), rehearse_environment(),
                          rehearsal, token);
  ASSERT_TRUE(r) << to_string(r.error());
  ASSERT_EQ(r->messages.size(), 1U);
  EXPECT_EQ(r->messages[0], "True");
}

TEST_F(Rehearse, AReadAnsweredWithTheWrongTypeIsAConfigError) {
  rehearsal.answers["is_closed"] = 1.5;
  auto r = host->rehearse(inline_script("def main():\n    is_closed('A')\n"), rehearse_environment(), rehearsal,
                          token);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Config);
  EXPECT_NE(r.error().what.find("rehearse: is_closed answered with a"), std::string::npos) << r.error().what;
}

namespace {

// Python that is busy for `seconds` of real time, whatever the machine's
// speed (datetime is on the import allowlist; time is not).
std::string spin(double seconds) {
  return "    t0 = datetime.datetime.now()\n    while (datetime.datetime.now() - t0).total_seconds() < " +
         std::to_string(seconds) + ":\n        pass\n";
}

struct HardwareScript {
  std::string name;                   // the vocabulary command
  std::string body;                   // the line(s) of main()
  std::vector<std::string> expected;  // steps it makes (the command itself by default)
};

// One call of each command with arguments that pass the static check.
std::vector<HardwareScript> hardware_scripts() {
  return {
      {"open", "open('A')", {}},
      {"close", "close('A')", {}},
      {"lock", "lock('A')", {}},
      {"unlock", "unlock('A')", {}},
      {"is_open", "is_open('A')", {}},
      {"is_closed", "is_closed('A')", {}},
      {"extract", "extract(5, 'percent')", {}},
      {"end_extract", "end_extract()", {}},
      {"ramp", "ramp(0, 10, 1, 10, 1)", {}},
      {"enable", "enable()", {}},
      {"disable", "disable()", {}},
      {"prepare", "prepare()", {}},
      {"fire_laser", "fire_laser()", {}},
      {"warmup", "warmup()", {}},
      {"move_to_position", "move_to_position('1')", {}},
      {"set_x", "set_x(1.0)", {}},
      {"set_y", "set_y(1.0)", {}},
      {"set_z", "set_z(1.0)", {}},
      {"set_xy", "set_xy(1.0, 2.0)", {}},
      {"set_tray", "set_tray('t')", {}},
      {"execute_pattern", "execute_pattern('circle')", {}},
      {"dump_sample", "dump_sample()", {}},
      {"drop_sample", "drop_sample('1')", {}},
      {"set_pid_parameters", "set_pid_parameters(1.0)", {}},
      {"begin_heating_interval", "begin_heating_interval(10)", {}},
      {"load_pipette", "load_pipette('p')", {}},
      {"extract_pipette", "extract_pipette('p')", {}},
      {"set_motor", "set_motor('m', 1.0)", {}},
      {"get_value", "get_value('m')", {}},
      {"set_cryo", "set_cryo(10.0)", {}},
      {"get_cryo_temp", "get_cryo_temp(1)", {}},
      {"snapshot", "snapshot('s')", {}},
      {"video_start", "video_start('v')", {}},
      {"video_stop", "video_stop()", {}},
      // A context manager: a start and a stop.
      {"video_recording", "with video_recording('v'):\n        pass", {"video_start", "video_stop"}},
      {"get_pressure", "get_pressure('ig', 'ion')", {}},
      {"get_manometer_pressure", "get_manometer_pressure('m')", {}},
      {"waitfor", "waitfor(lambda: False, 5)", {}},
      {"pause", "pause(3)", {}},
      {"sleep", "sleep(1)", {}},
      {"delay", "delay(1)", {}},
      {"acquire", "acquire('r')", {}},
      {"wait", "wait('r', 1)", {}},
      {"release", "release('r')", {}},
      {"set_resource", "set_resource('r', 1)", {}},
      {"get_resource_value", "get_resource_value('r')", {}},
      {"get_intensity", "get_intensity('Ar40')", {}},
      {"signal_pump_time_start", "signal_pump_time_start()", {}},
  };
}

// Commands that need a service or a state of their own and make no step.
const std::set<std::string> kNoStep = {"get_device", "wake", "begin_interval", "complete_interval",
                                       "info", "gosub"};

}  // namespace

TEST_F(Rehearse, EveryHardwareCommandSteps) {
  const auto scripts = hardware_scripts();
  std::vector<std::string> failures;
  for (const auto& cmd : vocabulary()) {
    const std::string name(cmd.name);
    if (kNoStep.contains(name)) continue;
    auto it = std::find_if(scripts.begin(), scripts.end(), [&](const auto& s) { return s.name == name; });
    if (it == scripts.end()) {
      failures.push_back(name + " (no script in the test)");
      continue;
    }
    RecordingRehearsal rec;
    rec.answers = {{"is_open", true}, {"is_closed", true}, {"get_pressure", 1.0}, {"get_manometer_pressure", 1.0},
                   {"get_intensity", 1.0}, {"get_value", 1.0}, {"get_cryo_temp", 1.0}, {"get_resource_value", 1.0},
                   {"snapshot", std::string("x")}};
    const bool post = cmd.post_measurement_only;
    auto r = host->rehearse(
        inline_script("def main():\n    " + it->body + "\n", post ? ScriptKind::PostMeasurement : ScriptKind::Extraction),
        rehearse_environment(), rec, token);
    std::vector<std::string> got;
    got.reserve(rec.calls.size());
    for (const auto& c : rec.calls) got.push_back(c.command);
    const std::vector<std::string> want = it->expected.empty() ? std::vector<std::string>{name} : it->expected;
    if (got != want || !r) failures.push_back(name + (r ? "" : " [" + to_string(r.error()) + "]"));
  }
  EXPECT_TRUE(failures.empty()) << "did not step as expected: " << [&] {
    std::string out;
    for (const auto& f : failures) out += "\n  " + f;
    return out;
  }();
}

TEST_F(Rehearse, SimPumpSteps) {
  auto env = rehearse_environment();
  env.context = make_context({{"run_identifier", std::string("12345-01")}});
  auto r = host->rehearse(example("post_measurement/sim_pump.py", ScriptKind::PostMeasurement), env, rehearsal, token);
  ASSERT_TRUE(r) << to_string(r.error());
  const std::vector<std::string> expected = {"signal_pump_time_start", "open C", "open B", "sleep 50", "close B"};
  EXPECT_EQ(rehearsal.lines(), expected);
}

TEST_F(Rehearse, PressureReadAnswersTheScript) {
  rehearsal.answers["get_pressure"] = 2.5e-9;
  auto r = host->rehearse(inline_script("def main():\n    info(str(get_pressure('ig','ion')))\n"),
                          rehearse_environment(), rehearsal, token);
  ASSERT_TRUE(r) << to_string(r.error());
  ASSERT_EQ(r->messages.size(), 1U);
  EXPECT_EQ(r->messages[0], "2.5e-09");
  ASSERT_EQ(rehearsal.calls.size(), 1U);
  EXPECT_EQ(rehearsal.calls[0].args, (std::vector<Value>{Value(std::string("ig")), Value(std::string("ion"))}));
}

TEST_F(Rehearse, AnIntegerAnswersADoubleRead) {
  rehearsal.answers["get_value"] = std::int64_t{3};
  auto r = host->rehearse(inline_script("def main():\n    info(str(get_value('m')))\n"), rehearse_environment(),
                          rehearsal, token);
  ASSERT_TRUE(r) << to_string(r.error());
  EXPECT_EQ(r->messages.at(0), "3.0");
}

TEST_F(Rehearse, WrongAnswerTypeIsConfig) {
  rehearsal.answers["is_open"] = 1.0;
  auto r = host->rehearse(inline_script("def main():\n    is_open('A')\n"), rehearse_environment(), rehearsal, token);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Config);
  EXPECT_NE(r.error().what.find("is_open"), std::string::npos) << r.error().what;
}

TEST_F(Rehearse, ADoubleReadAnsweredWithAStringIsConfig) {
  rehearsal.answers["get_pressure"] = std::string("high");
  auto r = host->rehearse(inline_script("def main():\n    get_pressure('ig', 'ion')\n"), rehearse_environment(),
                          rehearsal, token);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Config);
  EXPECT_NE(r.error().what.find("get_pressure"), std::string::npos) << r.error().what;
}

TEST_F(Rehearse, KeywordArgumentsAreCarried) {
  auto r = host->rehearse(inline_script("def main():\n    move_to_position('3', autocenter=False, block=False)\n"),
                          rehearse_environment(), rehearsal, token);
  ASSERT_TRUE(r) << to_string(r.error());
  ASSERT_EQ(rehearsal.calls.size(), 1U);
  EXPECT_EQ(rehearsal.calls[0].args, (std::vector<Value>{Value(std::string("3"))}));
  EXPECT_EQ(rehearsal.calls[0].kwargs.at("autocenter"), Value(false));
  EXPECT_EQ(rehearsal.calls[0].kwargs.at("block"), Value(false));
}

TEST_F(Rehearse, GosubCarriesStack) {
  class Resolver final : public IScriptResolver {
   public:
    Result<Script> resolve(std::string_view name, ScriptKind kind) const override {
      if (name != "lib/sub.py") return fail(ErrorKind::Config, "no such script");
      return Script{"lib/sub.py", "def main():\n    open('A')\n", kind};
    }
  } resolver;
  auto env = rehearse_environment();
  env.resolver = &resolver;
  auto r = host->rehearse(Script{"extraction/top.py", "def main():\n    gosub('lib/sub.py')\n", ScriptKind::Extraction},
                          env, rehearsal, token);
  ASSERT_TRUE(r) << to_string(r.error());
  ASSERT_EQ(rehearsal.calls.size(), 1U);
  EXPECT_EQ(rehearsal.calls[0].script, "lib/sub.py");
  EXPECT_EQ(rehearsal.calls[0].gosubs, (std::vector<std::string>{"extraction/top.py"}));
}

TEST_F(Rehearse, PauseStepsOnceWithOrWithoutADuration) {
  for (const char* call : {"pause(3)", "pause()"}) {
    SCOPED_TRACE(call);
    RecordingRehearsal rec;
    auto r = host->rehearse(inline_script(std::string("def main():\n    ") + call + "\n"), rehearse_environment(), rec,
                            token);
    ASSERT_TRUE(r) << to_string(r.error());
    ASSERT_EQ(rec.calls.size(), 1U);
    EXPECT_EQ(rec.calls[0].command, "pause");
  }
}

TEST_F(Rehearse, LockAndUnlockStep) {
  auto r = host->rehearse(inline_script("def main():\n    lock('A')\n    unlock('A')\n"), rehearse_environment(),
                          rehearsal, token);
  ASSERT_TRUE(r) << to_string(r.error());
  const std::vector<std::string> expected = {"lock A", "unlock A"};
  EXPECT_EQ(rehearsal.lines(), expected);
}

TEST_F(Rehearse, ACancelledStepEndsTheScript) {
  // (the recorder copies `respond` for each step: the count lives outside it)
  int steps = 0;
  rehearsal.respond = [&steps](const StepCall&) -> Result<Value> {
    if (++steps == 2) return fail(ErrorKind::Cancelled, "cancelled");
    return Value{};
  };
  auto r = host->rehearse(inline_script("def main():\n    open('A')\n    open('B')\n    open('C')\n"),
                          rehearse_environment(), rehearsal, token);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Cancelled);
  EXPECT_EQ(rehearsal.calls.size(), 2U);
}

// max_wall_time is Python's time: time held in a step does not count, and
// Python time before the step still does. Limit 300 ms; 200 ms of Python
// before the step, a 400 ms hold, 200 ms after: the Python time is 400 ms and
// trips it, where a limit that forgot the earlier Python time (200 ms) or
// counted the hold (600 ms) would not / always. Busy times are real-time
// minimums, so a faster or slower machine changes nothing.
TEST_F(Rehearse, PythonTimeBeforeAStepStillCountsAgainstWallTime) {
  rehearsal.respond = [](const StepCall&) -> Result<Value> {
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    return Value{};
  };
  auto env = rehearse_environment();
  env.limits.max_wall_time = std::chrono::milliseconds(300);
  auto r = host->rehearse(
      inline_script("import datetime\n\ndef main():\n" + spin(0.2) + "    open('A')\n" + spin(0.2)), env, rehearsal,
      token);
  ASSERT_FALSE(r);
  EXPECT_NE(r.error().what.find("wall-time limit"), std::string::npos) << r.error().what;
}

// The hold alone (400 ms) is over the limit; the Python around it is not.
TEST_F(Rehearse, TimeHeldInAStepIsNotWallTime) {
  rehearsal.respond = [](const StepCall&) -> Result<Value> {
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    return Value{};
  };
  auto env = rehearse_environment();
  env.limits.max_wall_time = std::chrono::milliseconds(300);
  auto r = host->rehearse(inline_script("import datetime\n\ndef main():\n    open('A')\n" + spin(0.05)), env,
                          rehearsal, token);
  EXPECT_TRUE(r) << (r ? "" : to_string(r.error()));
}

TEST_F(Rehearse, ANegativeTimeNeverMovesRehearsedTimeBack) {
  auto r = host->rehearse(
      inline_script("def main():\n    begin_interval(5)\n    ramp(0, 10, 1, -5, 1)\n    complete_interval()\n"),
      rehearse_environment(), rehearsal, token);
  ASSERT_TRUE(r) << to_string(r.error());
  ASSERT_FALSE(rehearsal.calls.empty());
  const auto& last = rehearsal.calls.back();
  EXPECT_EQ(last.command, "complete_interval");
  EXPECT_EQ(last.args, (std::vector<Value>{Value(5.0)}));
}

TEST_F(Rehearse, DeviceHandleReadsStepAndAnswer) {
  rehearsal.answers["is_enabled"] = true;
  rehearsal.answers["output"] = 4.5;
  auto r = host->rehearse(
      inline_script("def main():\n    d = get_device('x')\n    info(str(d.is_enabled()))\n    info(str(d.output()))\n"),
      rehearse_environment(), rehearsal, token);
  ASSERT_TRUE(r) << to_string(r.error());
  EXPECT_EQ(r->messages, (std::vector<std::string>{"True", "4.5"}));
  ASSERT_EQ(rehearsal.calls.size(), 2U);
  EXPECT_EQ(rehearsal.calls[0].command, "is_enabled");
  EXPECT_EQ(rehearsal.calls[1].command, "output");
}

TEST_F(Rehearse, MeasurementHookRefused) {
  auto r = host->rehearse(inline_script("def before_main(api):\n    pass\n", ScriptKind::MeasurementHook),
                          rehearse_environment(), rehearsal, token);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Config);
  EXPECT_TRUE(rehearsal.calls.empty());
}

#else

TEST_F(Rehearse, StubHostRefuses) {
  auto r = host->rehearse(inline_script("def main():\n    pass\n"), rehearse_environment(), rehearsal, token);
  ASSERT_FALSE(r);
  EXPECT_TRUE(extraction::is_not_supported(r.error()));
}

#endif  // PYCHRON_SCRIPTING_ENABLED
