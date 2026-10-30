// `elctl sim serve` (sim serve spec 4.7): the example lab, every transport on
// the wire, served on loopback until the test says stop.
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

#include "elctl_fixture.hpp"
#include "hardware_style.hpp"
#include "pychron/transport/endpoints.hpp"
#include "sim_serve.hpp"

namespace {

using namespace std::chrono_literals;
using elctl::testing::contains;
namespace fs = std::filesystem;

class SimServeCmd : public elctl::testing::ElctlTest {
 protected:
  void SetUp() override {
    ElctlTest::SetUp();
    std::ifstream in(path("extraction_line.toml"));
    std::ostringstream text;
    text << in.rdbuf();
    in.close();
    write("extraction_line.toml", pychron::testing::hardware_style(text.str()));
  }

  // Runs the command on a thread until stop(); `ready` is set once the
  // endpoints file is there (or the command returned).
  struct Served {
    std::atomic<bool> stop{false};
    std::thread thread;
    std::ostringstream out;
    std::ostringstream err;
    std::istringstream in;
    int code = -1;
  };

  void serve(Served& s, std::vector<std::string> args, const fs::path& install_spectrometer = {}) {
    s.thread = std::thread([this, &s, args = std::move(args), install_spectrometer] {
      s.code = elctl::sim_serve_command(args, path("extraction_line.toml"), {}, install_spectrometer,
                                        elctl::Io{s.in, s.out, s.err}, [&s] { return s.stop.load(); });
    });
  }

  // The example's Qtegra spectrometer and its tables, copied beside the line.
  fs::path qtegra_spectrometer() const {
    const fs::path examples = PYCHRON_EXAMPLE_CONFIGS_DIR;
    fs::copy_file(examples / "spectrometer.qtegra.toml", path("spectrometer.toml"),
                  fs::copy_options::overwrite_existing);
    fs::copy(examples / "tables", path("tables"),
             fs::copy_options::recursive | fs::copy_options::overwrite_existing);
    return path("spectrometer.toml");
  }

  // Waits (real time, at most 5 s) for `file` to be there.
  static bool appears(const fs::path& file) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
      if (fs::exists(file)) return true;
      std::this_thread::sleep_for(10ms);
    }
    return false;
  }
};

}  // namespace

TEST_F(SimServeCmd, WritesEndpointsAndRemovesThemOnStop) {
  Served s;
  serve(s, {});
  const auto file = path("sim-endpoints.toml");
  ASSERT_TRUE(appears(file));
  auto endpoints = pychron::read_sim_endpoints(file);
  ASSERT_TRUE(endpoints) << endpoints.error().what;
  EXPECT_EQ(endpoints->transports.size(), 6U);
  for (const auto& [name, e] : endpoints->transports) EXPECT_NE(e.port, 0) << name;
  EXPECT_EQ(endpoints->config, fs::weakly_canonical(path("extraction_line.toml")));
  s.stop = true;
  s.thread.join();
  EXPECT_EQ(s.code, elctl::kOk) << s.err.str();
  EXPECT_FALSE(fs::exists(file));
}

TEST_F(SimServeCmd, PrintsTable) {
  // One transport left in-process.
  std::ifstream in(path("extraction_line.toml"));
  std::ostringstream text;
  text << in.rdbuf();
  in.close();
  std::string t = text.str();
  const auto at = t.find("[transports.cryostat]\n");
  ASSERT_NE(at, std::string::npos);
  const auto kind = t.find("kind = \"tcp\"", at);
  const auto end = t.find("port = ", kind);
  t.replace(kind, t.find('\n', end) - kind, "kind = \"sim\"");
  write("extraction_line.toml", t);

  Served s;
  serve(s, {"--endpoints", path("ep.toml").string()});
  ASSERT_TRUE(appears(path("ep.toml")));
  s.stop = true;
  s.thread.join();
  EXPECT_EQ(s.code, elctl::kOk) << s.err.str();
  const std::string out = s.out.str();
  for (const char* name : {"valve_bus", "gauge_net", "cg_controller", "laser_pc", "bakeout_plc"})
    EXPECT_TRUE(contains(out, name)) << out;
  EXPECT_TRUE(contains(out, "proxr_relay")) << out;
  EXPECT_TRUE(contains(out, "in-process, not joined")) << out;
  EXPECT_TRUE(contains(out, "serving; interrupt to stop")) << out;
}

TEST_F(SimServeCmd, WarnsOnNonLoopbackBind) {
  Served s;
  serve(s, {"--bind", "0.0.0.0"});
  ASSERT_TRUE(appears(path("sim-endpoints.toml")));
  s.stop = true;
  s.thread.join();
  EXPECT_TRUE(contains(s.err.str(), "no authentication")) << s.err.str();
}

TEST_F(SimServeCmd, BadPortBaseIsUsage) {
  const auto o = run({"sim", "serve", "--port-base", "70000"});
  EXPECT_EQ(o.code, elctl::kUsage);
  EXPECT_TRUE(contains(o.err, "--port-base")) << o.err;
}

TEST_F(SimServeCmd, UnknownSubcommandIsUsage) {
  const auto o = run({"sim", "dance"});
  EXPECT_EQ(o.code, elctl::kUsage);
}

TEST_F(SimServeCmd, ReadAndOpenOverEndpoints) {
  Served s;
  serve(s, {});
  const auto file = path("sim-endpoints.toml").string();
  ASSERT_TRUE(appears(file));

  const auto read = run({"--sim-endpoints", file, "read", "CG1"});
  EXPECT_EQ(read.code, elctl::kOk) << read.out << read.err;
  EXPECT_TRUE(contains(read.out, "CG1")) << read.out;
  EXPECT_TRUE(contains(read.out, "torr")) << read.out;

  const auto opened = run({"--sim-endpoints", file, "open", "B"});
  EXPECT_EQ(opened.code, elctl::kOk) << opened.out << opened.err;
  EXPECT_TRUE(contains(opened.out, "B open")) << opened.out;
  // Another process reads the valve back from the served lab.
  const auto state = run({"--sim-endpoints", file, "state"});
  EXPECT_EQ(state.code, elctl::kOk) << state.err;
  bool b_open = false;
  std::istringstream lines(state.out);
  for (std::string line; std::getline(lines, line);)
    if (line.starts_with("B ") && contains(line, "open")) b_open = true;
  EXPECT_TRUE(b_open) << state.out;

  s.stop = true;
  s.thread.join();
}

TEST_F(SimServeCmd, SimEndpointsWithSimRefused) {
  const auto o = run({"--sim", "--sim-endpoints", path("sim-endpoints.toml").string(), "list"});
  EXPECT_EQ(o.code, elctl::kUsage);
  EXPECT_TRUE(contains(o.err, "cannot be used with --sim")) << o.err;
}

TEST_F(SimServeCmd, SimEndpointsWithSimSpeedRefused) {
  const auto o = run({"--sim-endpoints", path("sim-endpoints.toml").string(), "exp", "run", path("q.toml").string(),
                      "--sim-speed", "10"});
  EXPECT_EQ(o.code, elctl::kUsage);
  EXPECT_TRUE(contains(o.err, "--sim-speed is refused")) << o.err;
}

TEST_F(SimServeCmd, MissingEntryFailsTheCommand) {
  pychron::SimEndpoints endpoints;
  endpoints.config = fs::weakly_canonical(path("extraction_line.toml"));
  endpoints.host = "127.0.0.1";
  ASSERT_TRUE(pychron::write_sim_endpoints(path("ep.toml"), endpoints));
  const auto o = run({"--sim-endpoints", path("ep.toml").string(), "read", "CG1"});
  EXPECT_NE(o.code, elctl::kOk);
  EXPECT_TRUE(contains(o.err, "has no entry in the sim endpoints file")) << o.err;
}

TEST_F(SimServeCmd, ServesTheSpectrometerWhenGiven) {
  Served s;
  serve(s, {"--spectrometer", qtegra_spectrometer().string()});
  const auto file = path("sim-endpoints.toml");
  ASSERT_TRUE(appears(file));
  auto endpoints = pychron::read_sim_endpoints(file);
  s.stop = true;
  s.thread.join();
  ASSERT_TRUE(endpoints) << endpoints.error().what;
  EXPECT_EQ(endpoints->spectrometer, fs::weakly_canonical(path("spectrometer.toml")));
  EXPECT_TRUE(endpoints->spectrometer_transports.contains("qtegra"));
  EXPECT_EQ(s.code, elctl::kOk) << s.err.str();
  EXPECT_TRUE(contains(s.out.str(), "qtegra (spectrometer)")) << s.out.str();
}

TEST_F(SimServeCmd, NoSpectrometerLeavesItOut) {
  Served s;
  serve(s, {"--no-spectrometer"}, qtegra_spectrometer());
  const auto file = path("sim-endpoints.toml");
  ASSERT_TRUE(appears(file));
  auto endpoints = pychron::read_sim_endpoints(file);
  s.stop = true;
  s.thread.join();
  ASSERT_TRUE(endpoints) << endpoints.error().what;
  EXPECT_TRUE(endpoints->spectrometer.empty());
}

TEST_F(SimServeCmd, InstallSpectrometerServedByDefault) {
  Served s;
  serve(s, {}, qtegra_spectrometer());
  const auto file = path("sim-endpoints.toml");
  ASSERT_TRUE(appears(file));
  auto endpoints = pychron::read_sim_endpoints(file);
  s.stop = true;
  s.thread.join();
  ASSERT_TRUE(endpoints) << endpoints.error().what;
  EXPECT_FALSE(endpoints->spectrometer.empty());
}
