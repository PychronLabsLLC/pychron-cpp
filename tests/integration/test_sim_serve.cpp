// The example line, every transport on the wire, driven over sim serve (sim
// serve spec section 7): the client is an ExtractionLine with real TCP
// transports, the server a LineServer. Real time on loopback; bounds are upper
// bounds of 5 s or more, there only to tell an answer from a stall.
#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>

#include "hardware_style.hpp"
#include "pychron/core/config/loader.hpp"
#include "pychron/systems/canvas/loader.hpp"
#include "pychron/systems/extraction_line.hpp"
#include "pychron/systems/line_server.hpp"
#include "pychron/transport/endpoints.hpp"
#include "temp_path.hpp"

namespace {

using namespace pychron;
using namespace std::chrono_literals;
namespace fs = std::filesystem;
using systems::ExtractionLine;
using systems::LineServer;
using systems::SwitchOp;

const fs::path kExamples = PYCHRON_EXAMPLE_CONFIGS_DIR;

std::string read_text(const fs::path& p) {
  std::ifstream in(p);
  std::ostringstream s;
  s << in.rdbuf();
  return s.str();
}

// The example's valves and gauges, with a valve of another Pychron's valve
// service on a transport of its own.
constexpr const char* kRemoteValve = R"(
[transports.remote]
kind = "tcp"
host = "192.0.2.1"
port = 4100
timeout_ms = 2000

[drivers.remote_valves]
kind = "pychron_valves"
transport = "remote"

[[switches]]
name = "remote_switch"
actuator = "remote_valves"
address = "RS"
)";

class SimServeSteady : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = pychron::testing::unique_temp_path("pychron_sim_serve_");
    fs::create_directories(dir_);
    std::ofstream(line_file()) << pychron::testing::hardware_style(read_text(kExamples / "extraction_line.toml"))
                               << kRemoteValve;
    fs::copy_file(kExamples / "canvas.toml", dir_ / "canvas.toml");
    start_server();
  }

  void TearDown() override {
    server_.reset();
    std::error_code ec;
    fs::remove_all(dir_, ec);
  }

  fs::path line_file() const { return dir_ / "extraction_line.toml"; }
  fs::path endpoints_file() const { return dir_ / "sim-endpoints.toml"; }

  void start_server() {
    auto cfg = config::load_system_config(line_file());
    ASSERT_TRUE(cfg) << cfg.error().what;
    auto canvas = canvas::load_canvas(dir_ / "canvas.toml");
    ASSERT_TRUE(canvas) << canvas.error().what;
    systems::LineServerOptions options;
    options.sim.sim_file = fs::path();  // the simulator's own numbers, not the example's tuning
    options.sim.sim.valve_conductance = 1e3;  // a valve's gas crosses in well under a millisecond
    options.sim.sim.noise = 0.0;
    auto server = LineServer::start(*cfg, &*canvas, clock_, options);
    ASSERT_TRUE(server) << server.error().what;
    server_ = std::move(*server);
    ASSERT_TRUE(write_sim_endpoints(endpoints_file(), server_->endpoints()));
  }

  ExtractionLine::Options client_options() const {
    ExtractionLine::Options o;
    o.sim_endpoints = endpoints_file();
    o.state_file = dir_ / "client.state.toml";
    o.scheduler.threads = 0;
    o.run_scheduler = false;
    return o;
  }

  Result<std::unique_ptr<ExtractionLine>> client(ExtractionLine::Options options) const {
    return ExtractionLine::load(line_file(), dir_ / "canvas.toml", std::move(options));
  }
  Result<std::unique_ptr<ExtractionLine>> client() const { return client(client_options()); }

  const SteadyClock clock_;
  fs::path dir_;
  std::unique_ptr<LineServer> server_;
};

}  // namespace

TEST_F(SimServeSteady, OpenValveOverWire) {
  auto line = client();
  ASSERT_TRUE(line) << line.error().what;
  ASSERT_TRUE((*line)->start());
  EXPECT_FALSE(server_->system().valve_open("B"));
  ASSERT_TRUE((*line)->actuate("B", SwitchOp::Open));
  EXPECT_TRUE(server_->system().valve_open("B"));
  ASSERT_TRUE((*line)->actuate("B", SwitchOp::Close));
  EXPECT_FALSE(server_->system().valve_open("B"));
}

TEST_F(SimServeSteady, GaugeRisesOverWire) {
  // IG1 is on the turbo line, behind C from prep.
  ASSERT_TRUE(server_->system().set_pressure("prep", 1e-3));
  auto line = client();
  ASSERT_TRUE(line) << line.error().what;
  ASSERT_TRUE((*line)->start());
  auto before = (*line)->read_gauge("IG1");
  ASSERT_TRUE(before) << before.error().what;
  ASSERT_TRUE((*line)->actuate("C", SwitchOp::Open));
  double after = *before;
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (after <= 10 * *before && std::chrono::steady_clock::now() < deadline) {
    auto reading = (*line)->read_gauge("IG1");
    ASSERT_TRUE(reading) << reading.error().what;
    after = *reading;
  }
  EXPECT_GT(after, 10 * *before);
}

TEST_F(SimServeSteady, ReadbackMatchesServer) {
  {
    auto first = client();
    ASSERT_TRUE(first) << first.error().what;
    ASSERT_TRUE((*first)->start());
    ASSERT_TRUE((*first)->actuate("B", SwitchOp::Open));
  }
  // A client with no memory of it reads the valve back from the served lab.
  auto options = client_options();
  options.state_file = dir_ / "second.state.toml";
  auto second = client(options);
  ASSERT_TRUE(second) << second.error().what;
  ASSERT_TRUE((*second)->start());
  auto state = (*second)->switches().state("B");
  ASSERT_TRUE(state) << state.error().what;
  EXPECT_EQ(*state, ValveState::Open);
}

TEST_F(SimServeSteady, PychronValvesThroughDriver) {
  // One connection per command, as the real service answers.
  auto line = client();
  ASSERT_TRUE(line) << line.error().what;
  ASSERT_TRUE((*line)->start());
  for (int i = 0; i < 10; ++i) {
    ASSERT_TRUE((*line)->actuate("remote_switch", SwitchOp::Open)) << i;
    EXPECT_TRUE(server_->system().valve_open("remote_switch")) << i;
    ASSERT_TRUE((*line)->actuate("remote_switch", SwitchOp::Close)) << i;
    EXPECT_FALSE(server_->system().valve_open("remote_switch")) << i;
  }
}

TEST_F(SimServeSteady, LoadRefusedWithEntryMissing) {
  auto endpoints = read_sim_endpoints(endpoints_file());
  ASSERT_TRUE(endpoints);
  endpoints->transports.erase("gauge_net");
  ASSERT_TRUE(write_sim_endpoints(endpoints_file(), *endpoints));
  auto line = client();
  ASSERT_FALSE(line);
  EXPECT_EQ(line.error().kind, ErrorKind::Config);
  EXPECT_NE(line.error().what.find("gauge_net"), std::string::npos) << line.error().what;
}

TEST_F(SimServeSteady, LoadRefusedWithForceSim) {
  auto options = client_options();
  options.force_sim = true;
  auto line = client(options);
  ASSERT_FALSE(line);
  EXPECT_EQ(line.error().kind, ErrorKind::Config);
  EXPECT_NE(line.error().what.find("--sim"), std::string::npos) << line.error().what;
}

TEST_F(SimServeSteady, LoadRefusedForAnotherLine) {
  fs::create_directories(dir_ / "other");
  fs::copy_file(line_file(), dir_ / "other" / "extraction_line.toml");
  auto options = client_options();
  auto line = ExtractionLine::load(dir_ / "other" / "extraction_line.toml", dir_ / "canvas.toml", options);
  ASSERT_FALSE(line);
  EXPECT_NE(line.error().what.find("--sim-endpoints-any"), std::string::npos) << line.error().what;
  options.sim_endpoints_any = true;
  EXPECT_TRUE(ExtractionLine::load(dir_ / "other" / "extraction_line.toml", dir_ / "canvas.toml", options));
}

TEST_F(SimServeSteady, StaleEndpointsFailStart) {
  server_->stop();
  auto line = client();
  ASSERT_TRUE(line) << line.error().what;
  const auto start = std::chrono::steady_clock::now();
  auto started = (*line)->start();
  EXPECT_LT(std::chrono::steady_clock::now() - start, 10s);
  ASSERT_FALSE(started);
  EXPECT_EQ(started.error().kind, ErrorKind::Io);
  EXPECT_NE(started.error().what.find("valve_bus"), std::string::npos) << started.error().what;
}
