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

  void serve(Served& s, std::vector<std::string> args) {
    s.thread = std::thread([this, &s, args = std::move(args)] {
      s.code = elctl::sim_serve_command(args, path("extraction_line.toml"), {}, elctl::Io{s.in, s.out, s.err},
                                        [&s] { return s.stop.load(); });
    });
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
