// Script metadata, identity, context and the pieces the host builds on.

#include <gtest/gtest.h>

#include <algorithm>
#include <thread>

#include "pychron/scripting/cancel_token.hpp"
#include "pychron/scripting/duration_accumulator.hpp"
#include "pychron/scripting/script.hpp"
#include "pychron/scripting/services.hpp"
#include "pychron/scripting/vocabulary.hpp"

using namespace pychron;
using namespace pychron::scripting;

TEST(ScriptHeader, ParsesPychronHeaderLines) {
  auto h = parse_header("#! pychron: eqtime=20, duration=5\n\ndef main():\n    pass\n"
                        "  #! pychron: label=\"two words\" eqtime=25\n");
  ASSERT_TRUE(h) << to_string(h.error());
  EXPECT_EQ(h->get("label"), "two words");
  EXPECT_EQ(h->get("eqtime"), "25");  // later key wins
  auto eq = h->number("duration");
  ASSERT_TRUE(eq);
  EXPECT_DOUBLE_EQ(eq->value(), 5.0);
  EXPECT_FALSE(h->get("missing"));
  EXPECT_FALSE(h->number("label"));
}

TEST(ScriptHeader, IgnoresOrdinaryComments) {
  auto h = parse_header("# pychron: eqtime=1\n#!/usr/bin/env python\n");
  ASSERT_TRUE(h);
  EXPECT_TRUE(h->values().empty());
}

TEST(ScriptHeader, MalformedLineNamesTheLine) {
  auto h = parse_header("def main():\n    pass\n#! pychron: eqtime\n");
  ASSERT_FALSE(h);
  EXPECT_EQ(h.error().kind, ErrorKind::Config);
  EXPECT_NE(h.error().what.find("line 3"), std::string::npos);
  EXPECT_FALSE(parse_header("#! pychron: label=\"open"));
}

TEST(ScriptSha, MatchesKnownVectors) {
  EXPECT_EQ(sha256_hex(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  EXPECT_EQ(sha256_hex("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  EXPECT_EQ(sha256_hex(std::string(1000, 'a')).size(), 64u);
}

TEST(ScriptContext, DefaultsOverlaidWithOverrides) {
  auto ctx = make_context({{"duration", 12.0}, {"extra", std::string("x")}}, {{"k", true}});
  EXPECT_EQ(std::get<double>(ctx.globals.at("duration")), 12.0);
  EXPECT_EQ(std::get<std::string>(ctx.globals.at("extract_units")), "percent");
  EXPECT_TRUE(ctx.globals.contains("extra"));
  EXPECT_TRUE(ctx.globals.contains("disable_between_positions"));
  EXPECT_TRUE(std::get<bool>(ctx.options.at("k")));
}

TEST(ScriptResolver, NormalizesPychronNames) {
  EXPECT_EQ(normalize_script_name("common:wait").value(), "common/wait.py");
  EXPECT_EQ(normalize_script_name("x.py").value(), "x.py");
  EXPECT_FALSE(normalize_script_name("../etc/passwd"));
  EXPECT_FALSE(normalize_script_name("/abs"));
  EXPECT_FALSE(normalize_script_name(""));
}

TEST(ScriptResolver, DirectoryLooksInKindThenLib) {
  DirectoryScriptResolver r(PYCHRON_SCRIPTING_TEST_SCRIPTS_DIR);
  auto s = r.resolve("common:prepare_line", ScriptKind::Extraction);
  ASSERT_TRUE(s) << to_string(s.error());
  EXPECT_EQ(s->name, "lib/common/prepare_line.py");
  EXPECT_NE(s->text.find("def main"), std::string::npos);
  auto own = r.resolve("laser_default", ScriptKind::Extraction);
  ASSERT_TRUE(own);
  EXPECT_EQ(own->name, "extraction/laser_default.py");
  EXPECT_FALSE(r.resolve("nope", ScriptKind::Extraction));
}

TEST(ScriptResolver, MapResolver) {
  MapScriptResolver r;
  r.add("sub.py", "def main():\n    pass\n");
  EXPECT_TRUE(r.resolve("sub", ScriptKind::Extraction));
  EXPECT_FALSE(r.resolve("other", ScriptKind::Extraction));
}

TEST(CancelToken, WaitElapsesWakesAndCancels) {
  SteadyClock clock;
  CancelToken t;
  EXPECT_EQ(t.wait_until(clock, clock.now() + std::chrono::milliseconds(5)), WaitResult::Elapsed);

  std::thread waker([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    t.wake();
  });
  EXPECT_EQ(t.wait_until(clock, clock.now() + std::chrono::seconds(10)), WaitResult::Woken);
  waker.join();

  std::thread canceller([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    t.cancel();
  });
  EXPECT_EQ(t.wait_until(clock, clock.now() + std::chrono::seconds(10)), WaitResult::Cancelled);
  canceller.join();
  EXPECT_EQ(t.mode(), CancelMode::Cancel);
  t.abort();
  EXPECT_EQ(t.mode(), CancelMode::Abort);
  t.cancel();  // does not downgrade an abort
  EXPECT_EQ(t.mode(), CancelMode::Abort);
  t.reset();
  EXPECT_FALSE(t.requested());
}

TEST(DurationAccumulator, SumsAndFlags) {
  DurationAccumulator acc;
  acc.add(std::chrono::seconds(2), "sleep", "x.py", 3);
  acc.add(std::chrono::seconds(-1), "sleep");
  acc.flag_unbounded("while");
  acc.flag_unbounded("while");
  EXPECT_EQ(acc.total(), std::chrono::seconds(2));
  EXPECT_EQ(acc.entries().size(), 2u);
  EXPECT_EQ(acc.unbounded().size(), 1u);
  EXPECT_FALSE(acc.bounded());
  EXPECT_EQ(acc.now() - TimePoint{}, std::chrono::seconds(2));
}

TEST(Vocabulary, CoversThePyscriptCommands) {
  for (auto name : {"open", "close", "lock", "unlock", "is_open", "is_closed", "extract",
                    "end_extract", "ramp", "fire_laser", "enable", "disable", "prepare", "warmup",
                    "move_to_position", "set_x", "set_y", "set_z", "set_xy", "execute_pattern",
                    "set_tray", "dump_sample", "drop_sample", "load_pipette", "extract_pipette",
                    "set_motor", "get_value", "set_cryo", "get_cryo_temp",
                    "begin_heating_interval", "snapshot", "video_start", "video_stop",
                    "get_pressure", "get_manometer_pressure", "waitfor", "wake", "pause", "sleep",
                    "delay", "acquire", "wait", "release", "set_resource", "get_resource_value",
                    "info", "gosub", "begin_interval", "complete_interval", "set_pid_parameters",
                    "get_device", "get_intensity", "signal_pump_time_start"})
    EXPECT_NE(find_command(name), nullptr) << name;
  EXPECT_EQ(find_command("exec"), nullptr);

  auto* gi = find_command("get_intensity");
  EXPECT_FALSE(command_allowed(*gi, ScriptKind::Extraction));
  EXPECT_TRUE(command_allowed(*gi, ScriptKind::PostMeasurement));
  EXPECT_TRUE(find_command("open")->valve_argument);
  EXPECT_TRUE(find_command("sleep")->blocking);
  EXPECT_EQ(find_command("fire_laser")->capability, extraction::Capability::Laser);

  auto imports = default_import_allowlist();
  EXPECT_NE(std::find(imports.begin(), imports.end(), "math"), imports.end());
  EXPECT_EQ(std::find(imports.begin(), imports.end(), "os"), imports.end());
}
