// The script host against the real embedded interpreter and the example
// scripts under tests/scripting/scripts.

#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <set>
#include <sstream>
#include <thread>

#include "fakes.hpp"
#include "pychron/scripting/script_host.hpp"
#include "pychron/scripting/vocabulary.hpp"

using namespace pychron;
using namespace pychron::scripting;
using namespace pychron::scripting::testing;
using namespace std::chrono_literals;

namespace {

[[maybe_unused]] Script load(const std::string& rel, ScriptKind kind) {
  std::ifstream in(std::string(PYCHRON_SCRIPTING_TEST_SCRIPTS_DIR) + "/" + rel);
  std::ostringstream text;
  text << in.rdbuf();
  return Script{rel, text.str(), kind};
}

[[maybe_unused]] Script inline_script(std::string text, ScriptKind kind = ScriptKind::Extraction) {
  return Script{"inline.py", std::move(text), kind};
}

[[maybe_unused]] std::set<std::string> codes(const CheckReport& r) {
  std::set<std::string> out;
  for (const auto& d : r.diagnostics) out.insert(d.code);
  return out;
}

class ScriptHostTest : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!scripting_enabled()) GTEST_SKIP() << "built without PYCHRON_SCRIPTING";
    host = make_script_host();
  }
  Rig rig;
  CancelToken token;
  std::unique_ptr<IScriptHost> host;
};

}  // namespace

#if PYCHRON_SCRIPTING_ENABLED

TEST_F(ScriptHostTest, BoundCommandsMatchVocabulary) {
  auto bound = bound_commands();
  std::set<std::string> a(bound.begin(), bound.end());
  std::set<std::string> b;
  for (const auto& c : vocabulary()) b.insert(std::string(c.name));
  EXPECT_EQ(a, b);
}

TEST_F(ScriptHostTest, RunsExampleLaserScriptInOrder) {
  auto script = load("extraction/laser_default.py", ScriptKind::Extraction);
  auto r = host->run(script, rig.env, token);
  ASSERT_TRUE(r) << to_string(r.error());
  EXPECT_EQ(r->sha, sha256_hex(script.text));
  EXPECT_EQ(r->header.get("eqtime"), "20");
  EXPECT_EQ(r->header.get("label"), "CO2 default");
  ASSERT_FALSE(r->messages.empty());
  EXPECT_EQ(r->messages.front(), "extracting 12345-01A");
  std::vector<std::string> expected = {"close A",   "open B",     "enable",
                                       "move_to_position 2",      "extract 5 percent",
                                       "fire_laser", "end_extract", "disable"};
  EXPECT_EQ(rig.log.snapshot(), expected);
}

TEST_F(ScriptHostTest, SameHostServesAllScriptKinds) {
  auto pe = host->run(load("post_equilibration/close_inlet.py", ScriptKind::PostEquilibration),
                      rig.env, token);
  ASSERT_TRUE(pe) << to_string(pe.error());

  bool pump_started = false;
  rig.env.on_pump_time_start = [&] { pump_started = true; };
  auto pm = host->run(load("post_measurement/pump.py", ScriptKind::PostMeasurement), rig.env,
                      token);
  ASSERT_TRUE(pm) << to_string(pm.error());
  EXPECT_TRUE(pump_started);
  EXPECT_EQ(pm->messages, std::vector<std::string>{"large signal; extra pumping"});
  EXPECT_TRUE(rig.log.contains("close B"));
  EXPECT_TRUE(rig.log.contains("open A"));
}

TEST_F(ScriptHostTest, MeasurementHookGetsTypedApi) {
  auto hook = load("measurement_hooks/whiff.py", ScriptKind::MeasurementHook);
  auto r = host->call_hook(hook, "before_main", {}, rig.env, token);
  ASSERT_TRUE(r) << to_string(r.error());
  auto r2 = host->call_hook(hook, "on_whiff_result", {{"Ar40", 50.0}}, rig.env, token);
  ASSERT_TRUE(r2) << to_string(r2.error());
  auto r3 = host->call_hook(hook, "on_whiff_result", {{"Ar40", 1.0}}, rig.env, token);
  ASSERT_TRUE(r3) << to_string(r3.error());
  auto missing = host->call_hook(hook, "after_main", {}, rig.env, token);
  EXPECT_TRUE(missing);  // undefined entry is a no-op
  std::vector<std::string> expected = {"position Ar40 H1", "add_conditional Ar40 > 1000 -> truncate",
                                       "log hook ready", "truncate quick", "acquire 5 1.048576"};
  EXPECT_EQ(rig.log.snapshot(), expected);
}

TEST_F(ScriptHostTest, ContextGlobalsAndOptionsAreReadable) {
  auto r = host->run(inline_script("def main():\n"
                                   "    info(duration)\n"
                                   "    info(extract_units)\n"
                                   "    info(opt.flag)\n"
                                   "    info(opt.get('absent', 'dflt'))\n"
                                   "    info('settle' in opt)\n"),
                     rig.env, token);
  ASSERT_TRUE(r) << to_string(r.error());
  EXPECT_EQ(r->messages, (std::vector<std::string>{"0.02", "percent", "True", "dflt", "True"}));
}

TEST_F(ScriptHostTest, OptionsAreReadOnlyAtRuntime) {
  auto r = host->run(inline_script("def main():\n    o = opt\n    o.flag = False\n"), rig.env,
                     token);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Config);
  EXPECT_NE(r.error().what.find("read-only"), std::string::npos) << r.error().what;
  EXPECT_NE(r.error().what.find("inline.py:3"), std::string::npos) << r.error().what;
}

TEST_F(ScriptHostTest, EachExecutionGetsAFreshNamespace) {
  auto first = host->run(inline_script("counter = 1\n"
                                       "def main():\n"
                                       "    global counter\n"
                                       "    counter += 1\n"
                                       "    info(counter)\n"),
                         rig.env, token);
  ASSERT_TRUE(first) << to_string(first.error());
  auto second = host->run(inline_script("def main():\n    info(counter)\n"), rig.env, token);
  ASSERT_FALSE(second);  // statically unknown in the second script
  EXPECT_EQ(first->messages, std::vector<std::string>{"2"});
}

TEST_F(ScriptHostTest, StaticCheckCatchesVocabularyMistakes) {
  auto r = host->check(inline_script("import os\n"
                                     "def main():\n"
                                     "    opne('A')\n"            // unknown command
                                     "    sleep(1, 2, 3, 4)\n"     // arity
                                     "    dump_sample()\n"         // furnace on a laser
                                     "    open('Z')\n"             // unknown valve
                                     "    duration = 3\n"          // context is read-only
                                     "    get_intensity('Ar40')\n" // post_measurement only
                                     "    x = eval('1')\n"         // not a builtin here
                                     "    y = (1).__class__\n"     // dunder access
                                     "    while is_open('A'):\n"
                                     "        sleep(1)\n"),
                       rig.env);
  ASSERT_TRUE(r) << to_string(r.error());
  EXPECT_FALSE(r->ok());
  auto c = codes(*r);
  for (auto code : {"import", "unknown-command", "arity", "not-supported", "unknown-valve",
                    "read-only", "not-allowed", "restricted", "unbounded-loop"})
    EXPECT_TRUE(c.contains(code)) << code;
  EXPECT_TRUE(c.contains("unknown-command") || c.contains("unknown-name"));
  auto errors = r->errors();
  auto arity = std::find_if(errors.begin(), errors.end(), [](auto& d) { return d.code == "arity"; });
  ASSERT_NE(arity, errors.end());
  EXPECT_EQ(arity->line, 4);
  EXPECT_EQ(r->warnings().size(), 1u);
}

TEST_F(ScriptHostTest, StaticCheckOnCleanScriptsAndOverrides) {
  auto clean = host->check(load("extraction/laser_default.py", ScriptKind::Extraction), rig.env);
  ASSERT_TRUE(clean);
  EXPECT_TRUE(clean->ok()) << (clean->diagnostics.empty() ? "" : clean->diagnostics[0].message);

  // Validation without hardware: capabilities and valve names from config.
  ScriptEnvironment offline;
  offline.capabilities = extraction::CapabilitySet{extraction::Capability::Furnace};
  offline.context = make_context();
  auto furnace = host->check(load("extraction/furnace_step.py", ScriptKind::Extraction), offline);
  ASSERT_TRUE(furnace);
  EXPECT_TRUE(furnace->ok());
  auto laser = host->check(load("extraction/laser_default.py", ScriptKind::Extraction), offline);
  ASSERT_TRUE(laser);
  auto c = codes(*laser);
  EXPECT_TRUE(c.contains("not-supported"));  // no valves, laser or stage
  EXPECT_TRUE(c.contains("unknown-gosub"));  // no resolver

  offline.capabilities->add(extraction::Capability::Valves);
  offline.valve_names = std::vector<std::string>{"A"};
  auto valves = host->check(inline_script("def main():\n    open('A')\n    close('B')\n"), offline);
  ASSERT_TRUE(valves);
  ASSERT_EQ(valves->errors().size(), 1u);
  EXPECT_EQ(valves->errors()[0].code, "unknown-valve");
  EXPECT_EQ(valves->errors()[0].line, 3);
}

TEST_F(ScriptHostTest, StaticCheckRecursesIntoGosubs) {
  MapScriptResolver resolver;
  resolver.add("sub", "def main():\n    frobnicate()\n");
  rig.env.resolver = &resolver;
  auto r = host->check(inline_script("def main():\n    gosub('sub')\n    gosub('missing')\n"),
                       rig.env);
  ASSERT_TRUE(r);
  auto c = codes(*r);
  EXPECT_TRUE(c.contains("unknown-gosub"));
  EXPECT_TRUE(c.contains("unknown-command"));
  auto errs = r->errors();
  EXPECT_TRUE(std::any_of(errs.begin(), errs.end(), [](auto& d) { return d.script == "sub.py"; }));
}

TEST_F(ScriptHostTest, StaticCheckSyntaxAndMissingMain) {
  auto syntax = host->check(inline_script("def main(:\n"), rig.env);
  ASSERT_TRUE(syntax);
  EXPECT_TRUE(codes(*syntax).contains("syntax"));
  auto nomain = host->check(inline_script("x = 1\n"), rig.env);
  ASSERT_TRUE(nomain);
  EXPECT_TRUE(codes(*nomain).contains("no-main"));
  auto hook = host->check(inline_script("x = 1\n", ScriptKind::MeasurementHook), rig.env);
  ASSERT_TRUE(hook);
  EXPECT_TRUE(hook->ok());
}

TEST_F(ScriptHostTest, RunRefusesScriptsThatFailTheCheck) {
  auto r = host->run(inline_script("import os\ndef main():\n    os.system('true')\n"), rig.env,
                     token);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Config);
  EXPECT_NE(r.error().what.find("import"), std::string::npos) << r.error().what;
}

TEST_F(ScriptHostTest, SandboxBlocksDynamicImportsAndOpenFiles) {
  auto r = host->run(inline_script("import math\n"
                                   "def main():\n"
                                   "    info(math.floor(2.5))\n"
                                   "    m = 'o' + 's'\n"
                                   "    try:\n"
                                   "        __import__(m)\n"
                                   "    except ImportError as e:\n"
                                   "        info('blocked')\n"),
                     rig.env, token);
  ASSERT_TRUE(r) << to_string(r.error());
  EXPECT_EQ(r->messages, (std::vector<std::string>{"2", "blocked"}));
}

TEST_F(ScriptHostTest, EstimateSumsTimeAndRecursesGosubs) {
  auto laser = host->estimate(load("extraction/laser_default.py", ScriptKind::Extraction), rig.env);
  ASSERT_TRUE(laser) << to_string(laser.error());
  // settle 0.01 (gosub) + duration 0.02 + cleanup 0.01
  EXPECT_NEAR(std::chrono::duration<double>(laser->total).count(), 0.04, 1e-9);
  EXPECT_TRUE(laser->bounded());
  EXPECT_TRUE(std::any_of(laser->entries.begin(), laser->entries.end(), [](auto& e) {
    return e.script == "lib/common/prepare_line.py" && e.line == 4;
  })) << [&] {
    std::string s;
    for (auto& e : laser->entries) s += e.script + ":" + std::to_string(e.line) + " " + e.command + "; ";
    return s;
  }();
  EXPECT_TRUE(rig.log.snapshot().empty());  // no hardware touched

  auto ramp = host->estimate(load("extraction/pattern_ramp.py", ScriptKind::Extraction), rig.env);
  ASSERT_TRUE(ramp) << to_string(ramp.error());
  // ramp 10 s + waitfor timeout 12 + delay 2 + remainder of the 30 s interval 16
  EXPECT_NEAR(std::chrono::duration<double>(ramp->total).count(), 40.0, 1e-9);

  ScriptEnvironment offline;
  offline.capabilities = extraction::CapabilitySet{extraction::Capability::Furnace};
  offline.context = make_context({{"duration", 600.0}, {"extract_value", 900.0}});
  auto furnace = host->estimate(load("extraction/furnace_step.py", ScriptKind::Extraction), offline);
  ASSERT_TRUE(furnace) << to_string(furnace.error());
  EXPECT_NEAR(std::chrono::duration<double>(furnace->total).count(), 600.0, 1e-9);
}

TEST_F(ScriptHostTest, EstimateFlagsUnboundedLoopsAndWaits) {
  rig.env.limits.estimate_max_lines = 10'000;
  auto r = host->estimate(inline_script("def main():\n"
                                        "    sleep(5)\n"
                                        "    while not is_open('A'):\n"
                                        "        sleep(1)\n"
                                        "    waitfor(lambda: True)\n"
                                        "    pause()\n"),
                          rig.env);
  ASSERT_TRUE(r) << to_string(r.error());
  EXPECT_FALSE(r->bounded());
  EXPECT_GE(r->unbounded.size(), 2u);  // the while loop and the exhausted line budget
  EXPECT_GE(r->total, 5s);

  auto waits = host->estimate(inline_script("def main():\n"
                                            "    waitfor(lambda: True)\n"
                                            "    pause()\n"
                                            "    pause(3)\n"),
                              rig.env);
  ASSERT_TRUE(waits) << to_string(waits.error());
  EXPECT_EQ(waits->unbounded.size(), 2u);
  EXPECT_EQ(waits->total, 3s);
}

TEST_F(ScriptHostTest, HardwareErrorsKeepTheirKind) {
  rig.valves.interlocked.insert("A");
  auto r = host->run(inline_script("def main():\n    open('A')\n"), rig.env, token);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Interlock);
  EXPECT_EQ(r.error().device, "A");

  // Scripts may catch them.
  auto caught = host->run(inline_script("def main():\n"
                                        "    try:\n"
                                        "        open('A')\n"
                                        "    except HardwareError as e:\n"
                                        "        info(e.kind)\n"),
                          rig.env, token);
  ASSERT_TRUE(caught) << to_string(caught.error());
  EXPECT_EQ(caught->messages, std::vector<std::string>{"interlock"});
}

TEST_F(ScriptHostTest, MissingCapabilityAtRuntimeIsNotSupported) {
  // A dynamic call the static check cannot see.
  auto r = host->run(inline_script("def main():\n"
                                   "    f = [dump_sample][0]\n"
                                   "    f()\n"),
                     rig.env, token);
  ASSERT_FALSE(r);
  EXPECT_TRUE(extraction::is_not_supported(r.error())) << to_string(r.error());
}

TEST_F(ScriptHostTest, CancelRaisesScriptCancelledAndFinallyRuns) {
  std::thread canceller([&] {
    std::this_thread::sleep_for(100ms);
    token.cancel();
  });
  auto start = std::chrono::steady_clock::now();
  auto r = host->run(inline_script("def main():\n"
                                   "    open('A')\n"
                                   "    try:\n"
                                   "        sleep(30)\n"
                                   "    finally:\n"
                                   "        close('A')\n"
                                   "        info('cleaned up')\n"),
                     rig.env, token);
  canceller.join();
  EXPECT_LT(std::chrono::steady_clock::now() - start, 10s);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Cancelled);
  EXPECT_TRUE(r.error().what.starts_with("cancelled")) << r.error().what;
  EXPECT_TRUE(rig.log.contains("close A"));
}

TEST_F(ScriptHostTest, CancelNotSwallowedByExceptException) {
  token.cancel();
  auto r = host->run(inline_script("def main():\n"
                                   "    try:\n"
                                   "        sleep(1)\n"
                                   "    except Exception:\n"
                                   "        info('swallowed')\n"),
                     rig.env, token);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Cancelled);
}

// A script cancelled while it waits for the stage stops the stage: the beam
// may be on, and a stage that goes on to its target heats what it passes.
TEST_F(ScriptHostTest, ACancelledMoveStopsTheStage) {
  for (const char* body : {"move_to_position('2')", "set_xy(1, 2)", "set_x(3)", "set_y(3)", "set_z(3)"}) {
    CancelToken cancel;
    rig.laser.stage_stuck = true;
    rig.laser.stage_stopped = false;
    rig.log.clear();
    std::thread canceller([&] {
      std::this_thread::sleep_for(100ms);
      cancel.cancel();
    });
    auto r = host->run(inline_script(std::string("def main():\n    ") + body + "\n"), rig.env, cancel);
    canceller.join();
    ASSERT_FALSE(r) << body;
    EXPECT_EQ(r.error().kind, ErrorKind::Cancelled) << body;
    EXPECT_EQ(rig.log.count("stop"), 1u) << body;
  }
}

TEST_F(ScriptHostTest, AStageThatCannotStopStillCancels) {
  rig.laser.stage_stuck = true;
  rig.laser.stage_can_stop = false;
  std::thread canceller([&] {
    std::this_thread::sleep_for(100ms);
    token.cancel();
  });
  auto r = host->run(inline_script("def main():\n    move_to_position('2')\n"), rig.env, token);
  canceller.join();
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Cancelled);
}

// A move that arrives is not followed by a stop.
TEST_F(ScriptHostTest, AMoveThatArrivesIsNotStopped) {
  auto r = host->run(inline_script("def main():\n    move_to_position('2')\n    set_xy(1, 2)\n"), rig.env, token);
  ASSERT_TRUE(r) << r.error().what;
  EXPECT_FALSE(rig.log.contains("stop"));
}

TEST_F(ScriptHostTest, CancelStopsARunningPattern) {
  rig.laser.pattern_finishes = false;
  std::thread canceller([&] {
    std::this_thread::sleep_for(100ms);
    token.cancel();
  });
  auto r = host->run(inline_script("def main():\n    execute_pattern('spiral')\n"), rig.env, token);
  canceller.join();
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Cancelled);
  EXPECT_TRUE(rig.log.contains("stop_pattern"));
}

TEST_F(ScriptHostTest, AbortInterruptsBusyLoopAndRefusesHardware) {
  std::thread aborter([&] {
    std::this_thread::sleep_for(100ms);
    token.abort();
  });
  auto r = host->run(inline_script("def main():\n"
                                   "    x = 0\n"
                                   "    try:\n"
                                   "        while True:\n"
                                   "            x += 1\n"
                                   "    finally:\n"
                                   "        close('A')\n"),
                     rig.env, token);
  aborter.join();
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Cancelled);
  EXPECT_TRUE(r.error().what.starts_with("aborted")) << r.error().what;
  EXPECT_FALSE(rig.log.contains("close A"));
}

TEST_F(ScriptHostTest, WatchdogLimitsExecutedLines) {
  rig.env.limits.max_lines = 1000;
  auto r = host->run(inline_script("def main():\n"
                                   "    x = 0\n"
                                   "    for i in range(100000):\n"
                                   "        x += i\n"),
                     rig.env, token);
  ASSERT_FALSE(r);
  EXPECT_NE(r.error().what.find("limit"), std::string::npos) << r.error().what;
}

TEST_F(ScriptHostTest, WakeEndsPause) {
  std::thread waker([&] {
    std::this_thread::sleep_for(100ms);
    token.wake();
  });
  auto r = host->run(inline_script("def main():\n    pause()\n    info('resumed')\n"), rig.env,
                     token);
  waker.join();
  ASSERT_TRUE(r) << to_string(r.error());
  EXPECT_EQ(r->messages, std::vector<std::string>{"resumed"});
}

TEST_F(ScriptHostTest, GosubPassesKeywordsAsContext) {
  MapScriptResolver resolver;
  resolver.add("common:show", "def main():\n    info(duration)\n    info(tag)\n");
  rig.env.resolver = &resolver;
  rig.env.context.globals["tag"] = std::string("none");
  auto r = host->run(inline_script("def main():\n    gosub('common:show', duration=7, tag='x')\n"),
                     rig.env, token);
  ASSERT_TRUE(r) << to_string(r.error());
  EXPECT_EQ(r->messages, (std::vector<std::string>{"7", "x"}));
}

TEST_F(ScriptHostTest, GosubRecursionIsBounded) {
  MapScriptResolver resolver;
  resolver.add("loop", "def main():\n    gosub('loop')\n");
  rig.env.resolver = &resolver;
  auto r = host->run(inline_script("def main():\n    gosub('loop')\n"), rig.env, token);
  ASSERT_FALSE(r);
  EXPECT_NE(r.error().what.find("depth"), std::string::npos) << r.error().what;
}

TEST_F(ScriptHostTest, ResourcesAndPressure) {
  auto r = host->run(inline_script("def main():\n"
                                   "    acquire('laser')\n"
                                   "    set_resource('flag', 2)\n"
                                   "    wait('flag', 2)\n"
                                   "    info(get_resource_value('flag'))\n"
                                   "    release('laser')\n"
                                   "    info(get_pressure('bone', 'ig'))\n"
                                   "    info(get_manometer_pressure())\n"
                                   "    info(get_device().name)\n"),
                     rig.env, token);
  ASSERT_TRUE(r) << to_string(r.error());
  EXPECT_EQ(r->messages, (std::vector<std::string>{"2.0", "1e-08", "12.5", "co2"}));
  EXPECT_TRUE(rig.resources.held.empty());
}

#endif  // PYCHRON_SCRIPTING_ENABLED

TEST(StubHost, ReportsAvailability) {
  auto host = make_script_host();
  EXPECT_EQ(host->available(), scripting_enabled());
  if (scripting_enabled()) return;
  CancelToken token;
  auto r = host->run(Script{"x.py", "def main():\n    pass\n"}, ScriptEnvironment{}, token);
  ASSERT_FALSE(r);
  EXPECT_TRUE(extraction::is_not_supported(r.error()));
}
