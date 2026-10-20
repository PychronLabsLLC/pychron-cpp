// Rehearse mode: every hardware call and wait of a script goes through one
// IRehearsal::step, and nothing touches hardware.

#include <gtest/gtest.h>

#include <fstream>
#include <sstream>

#include "fakes.hpp"
#include "pychron/devices/extraction/capability.hpp"
#include "pychron/scripting/rehearsal.hpp"
#include "pychron/scripting/script_host.hpp"
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
