// Commands that build the line: probe, state, open/close, read, scan, trace
// and the sim REPL. The example config is all `kind = "sim"`, so these run
// the real drivers against simulated hardware.

#include <filesystem>
#include <fstream>
#include <iterator>

#include "elctl_fixture.hpp"

namespace elctl::testing {
namespace {

TEST_F(ElctlTest, ProbePrintsHealthTable) {
  auto o = run({"probe"});
  EXPECT_EQ(o.code, 0) << o.out << o.err;
  for (const char* name : {"valve_bus", "gauge_net", "actuator1", "ig_controller"}) {
    EXPECT_TRUE(contains(o.out, name)) << name << "\n" << o.out;
  }
  EXPECT_TRUE(contains(o.out, "connected")) << o.out;
}

TEST_F(ElctlTest, ProbeReportsUnreachableHardware) {
  std::string text;
  {
    std::ifstream f(path("extraction_line.toml"));
    text.assign(std::istreambuf_iterator<char>(f), {});
  }
  auto at = text.find("[transports.valve_bus]\nkind = \"sim\"");
  ASSERT_NE(at, std::string::npos);
  text.replace(at, std::string("[transports.valve_bus]\nkind = \"sim\"").size(),
               "[transports.valve_bus]\nkind = \"serial\"\nport = \"/dev/elctl-no-such-port\"");
  write("extraction_line.toml", text);

  auto o = run({"probe"});
  EXPECT_EQ(o.code, 1) << o.out;
  EXPECT_TRUE(contains(o.out, "valve_bus")) << o.out;
  EXPECT_TRUE(contains(o.out, "FAIL")) << o.out;

  // --sim runs the identical config against simulated hardware.
  auto sim = run({"--sim", "probe"});
  EXPECT_EQ(sim.code, 0) << sim.out << sim.err;
}

TEST_F(ElctlTest, StateReadsEverySwitchBack) {
  auto o = run({"state"});
  EXPECT_EQ(o.code, 0) << o.out << o.err;
  EXPECT_TRUE(contains(o.out, "A")) << o.out;
  EXPECT_TRUE(contains(o.out, "closed")) << o.out;
  EXPECT_TRUE(contains(o.out, "IG1")) << o.out;
}

TEST_F(ElctlTest, OpenAndCloseValve) {
  auto opened = run({"open", "B"});
  EXPECT_EQ(opened.code, 0) << opened.err;
  EXPECT_TRUE(contains(opened.out, "B open")) << opened.out;

  auto closed = run({"close", "B"});
  EXPECT_EQ(closed.code, 0) << closed.err;
  EXPECT_TRUE(contains(closed.out, "B closed")) << closed.out;
}

TEST_F(ElctlTest, OpenUnknownValveFails) {
  auto o = run({"open", "nope"});
  EXPECT_EQ(o.code, 1);
  EXPECT_TRUE(contains(o.err, "nope")) << o.err;
}

TEST_F(ElctlTest, ReadGaugePrintsValueAndUnits) {
  auto o = run({"read", "IG1"});
  EXPECT_EQ(o.code, 0) << o.err;
  EXPECT_TRUE(contains(o.out, "IG1")) << o.out;
  EXPECT_TRUE(contains(o.out, "torr")) << o.out;
}

TEST_F(ElctlTest, ReadUnknownGaugeFails) {
  auto o = run({"read", "IG9"});
  EXPECT_EQ(o.code, 1);
  EXPECT_TRUE(contains(o.err, "IG9")) << o.err;
}

TEST_F(ElctlTest, ScanStreamsSamplesForDuration) {
  auto o = run({"scan", "--for", "400ms", "--interval", "50ms"});
  EXPECT_EQ(o.code, 0) << o.err;
  EXPECT_TRUE(contains(o.out, "IG1")) << o.out;
  EXPECT_TRUE(contains(o.out, "PG1")) << o.out;
}

TEST_F(ElctlTest, TraceOnRecordsTrafficForReplay) {
  auto status = run({"trace"});
  EXPECT_EQ(status.code, 0);
  EXPECT_TRUE(contains(status.out, "off")) << status.out;

  auto on = run({"trace", "on", "valve_bus"});
  EXPECT_EQ(on.code, 0) << on.err;
  EXPECT_TRUE(contains(run({"trace"}).out, "valve_bus"));

  ASSERT_EQ(run({"open", "B"}).code, 0);
  const auto trace = path("traces") / "valve_bus.trace";
  ASSERT_TRUE(std::filesystem::exists(trace));
  EXPECT_GT(std::filesystem::file_size(trace), 0u);
  EXPECT_FALSE(std::filesystem::exists(path("traces") / "gauge_net.trace"));

  auto off = run({"trace", "off"});
  EXPECT_EQ(off.code, 0) << off.err;
  EXPECT_TRUE(contains(run({"trace"}).out, "off"));
}

TEST_F(ElctlTest, TraceOnUnknownTransportFails) {
  auto o = run({"trace", "on", "nope"});
  EXPECT_EQ(o.code, 1);
  EXPECT_TRUE(contains(o.err, "nope")) << o.err;
}

TEST_F(ElctlTest, SimReplKeepsStateBetweenCommands) {
  auto o = run({"sim"}, "open A\nstate\nquit\n");
  EXPECT_EQ(o.code, 0) << o.err;
  EXPECT_TRUE(contains(o.out, "A open")) << o.out;
  // `state` after the open reports it from hardware read-back.
  EXPECT_TRUE(contains(o.out, "A  valve  open")) << o.out;
}

TEST_F(ElctlTest, SimReplEnforcesInterlocks) {
  // A and C are mutually interlocked in the example.
  auto o = run({"sim"}, "open A\nopen C\nclose A\nopen C\n");
  EXPECT_EQ(o.code, 0) << o.err;
  EXPECT_TRUE(contains(o.err, "interlock")) << o.err;
  EXPECT_TRUE(contains(o.out, "C open")) << o.out;
}

TEST_F(ElctlTest, SimReplReportsBadCommandsAndContinues) {
  auto o = run({"sim"}, "bogus\n\nread IG1\nexit\n");
  EXPECT_EQ(o.code, 0);
  EXPECT_TRUE(contains(o.err, "bogus")) << o.err;
  EXPECT_TRUE(contains(o.out, "IG1")) << o.out;
}

TEST_F(ElctlTest, ManualValveRecordsOperatorReport) {
  auto o = run({"sim"}, "open M1\nstate\n");
  EXPECT_EQ(o.code, 0) << o.err;
  EXPECT_TRUE(contains(o.out, "M1  manual  open")) << o.out;
}

TEST_F(ElctlTest, LoggingConfigCreatesLogFile) {
  const auto logs = path("logs");
  std::string text;
  {
    std::ifstream f(path("extraction_line.toml"));
    text.assign(std::istreambuf_iterator<char>(f), {});
  }
  const std::string table = "[logging]\n";
  auto at = text.find(table);
  ASSERT_NE(at, std::string::npos);
  text.insert(at + table.size(), "dir = '" + logs.string() + "'\n");
  write("extraction_line.toml", text);

  auto o = run({"--sim", "probe"});
  EXPECT_EQ(o.code, 0) << o.out << o.err;
  EXPECT_TRUE(std::filesystem::exists(logs / "pychron.log")) << o.err;
}

}  // namespace
}  // namespace elctl::testing
