// The isotopx_ngx and ngx_valves drivers in their systems: the example
// spectrometer config loads and validates; assembled over the NGX simulator
// it positions by mass and acquires by events; an extraction line drives NGX
// valves in simulation, and a line whose NGX link has no owner fails its
// actuations plainly. Time is a VirtualClock: the test's thread takes part in
// it, the scheduler runs on its own threads, and nothing waits on the wall
// clock.

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "pychron/core/config/loader.hpp"
#include "pychron/core/virtual_clock.hpp"
#include "pychron/devices/spectrometer/ngx_sim.hpp"
#include "pychron/systems/extraction_line.hpp"
#include "pychron/systems/spectrometer/assembler.hpp"
#include "pychron/transport/sim_transport.hpp"
#include "virtual_time.hpp"

using namespace pychron;
using namespace pychron::spectrometer;
using namespace std::chrono_literals;

namespace {

const std::filesystem::path kNgx = std::filesystem::path(PYCHRON_EXAMPLE_CONFIGS_DIR) / "spectrometer.ngx.toml";

}  // namespace

TEST(NgxExampleConfig, LoadsAndValidates) {
  auto data = cfg::load_spectrometer(kNgx);
  ASSERT_TRUE(data.has_value()) << to_string(data.error());
  const auto& c = data->config;
  EXPECT_EQ(c.drivers.at("ngx").kind, "isotopx_ngx");
  EXPECT_EQ(c.magnet.native_axis, cfg::Axis::Mass);
  EXPECT_EQ(c.magnet.field_table, "ngx_argon");
  EXPECT_EQ(c.detectors.size(), 10u);
  EXPECT_EQ(c.transports.at("ngx").kind, cfg::TransportKind::Tcp);
}

// The extraction line may own the socket instead: then the spectrometer's
// transport is a link to it.
TEST(NgxExampleConfig, ALinkTransportLoads) {
  namespace fs = std::filesystem;
  const fs::path dir = fs::temp_directory_path() / ("pychron-ngx-link-" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()));
  fs::remove_all(dir);
  fs::create_directories(dir / "tables");
  fs::copy(kNgx.parent_path() / "tables" / "ngx_argon", dir / "tables" / "ngx_argon", fs::copy_options::recursive);
  std::ifstream in(kNgx);
  std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  const std::string tcp = "kind = \"tcp\"\nhost = \"192.168.0.20\"\nport = 1099\ntimeout_ms = 2000";
  ASSERT_NE(text.find(tcp), std::string::npos);
  text.replace(text.find(tcp), tcp.size(), "kind = \"link\"\nlink = \"ngx\"");
  std::ofstream(dir / "spectrometer.toml") << text;
  auto data = cfg::load_spectrometer(dir / "spectrometer.toml");
  ASSERT_TRUE(data.has_value()) << to_string(data.error());
  EXPECT_EQ(data->config.transports.at("ngx").kind, cfg::TransportKind::Link);
  EXPECT_EQ(data->config.transports.at("ngx").link, "ngx");
  fs::remove_all(dir);
}

class NgxSystem : public pychron::testing::VirtualTimeTest {
 protected:
  // Ten simulated minutes cost some twenty seconds under the thread sanitizer.
  NgxSystem() : VirtualTimeTest(50s) {}

  void SetUp() override {
    model_->clock = &clock_;
    model_->values = {400, 39, 0, 0, 38, 37, 36, 0, 0, 0};  // H4 .. L5
    model_->params["IE"] = 4500;
    scheduler_.start();
  }
  void TearDown() override { spec_.reset(); }

  void assemble() {
    auto data = cfg::load_spectrometer(kNgx);
    ASSERT_TRUE(data.has_value()) << to_string(data.error());
    AssemblerOptions options;
    options.make_transport = [this](const cfg::TransportConfig& tc,
                                    const SpectrometerContext& ctx) -> Result<std::unique_ptr<Transport>> {
      TransportOptions to;
      to.name = tc.name;
      to.timeout = std::chrono::milliseconds(tc.timeout_ms);
      to.clock = &ctx.clock;
      return std::unique_ptr<Transport>(SimTransport::hooked(ngx_sim_hook(model_), to, ngx_sim_events(model_)));
    };
    auto spec = SpectrometerAssembler::assemble(std::move(*data), SpectrometerContext{clock_, scheduler_, bus_},
                                                std::move(options));
    ASSERT_TRUE(spec.has_value()) << to_string(spec.error());
    spec_ = std::move(*spec);
  }

  std::vector<std::string> commands() {
    std::lock_guard lock(model_->mutex);
    return model_->commands;
  }

  VirtualClock clock_;
  // Time moves only while the test waits in the clock (a command, acquire()).
  Clock::Participant main_{clock_, "test"};
  SignalBus bus_;
  Scheduler scheduler_{clock_, &bus_};
  std::shared_ptr<NgxSimModel> model_ = std::make_shared<NgxSimModel>();
  std::unique_ptr<Spectrometer> spec_;
};

using NgxSystemVirtual = NgxSystem;

TEST_F(NgxSystem, PositionsByMassAndAcquiresByEvents) {
  assemble();
  // The table puts Ar40 on H4 four mass units below its mass.
  auto moved = spec_->position(PositionTarget{Isotope{"Ar40"}, "H4"});
  ASSERT_TRUE(moved) << to_string(moved.error());
  bool set_mass = false;
  for (const auto& c : commands()) {
    if (c.starts_with("SetMass 35.96")) set_mass = true;
  }
  EXPECT_TRUE(set_mass);
  {
    std::lock_guard lock(model_->mutex);
    EXPECT_NEAR(model_->mass, 39.9623831237 - 4, 1e-6);
  }
  auto readings = spec_->acquire(std::size_t{2});
  ASSERT_TRUE(readings) << to_string(readings.error());
  ASSERT_EQ(readings->size(), 2u);
  const auto& h4 = readings->front().values.at("H4");
  ASSERT_TRUE(h4.has_value());
  EXPECT_DOUBLE_EQ(h4->mean, 400);
  EXPECT_DOUBLE_EQ(readings->front().values.at("L2")->mean, 36);
  // One StartAcq per reading, never a second while one is armed.
  int starts = 0;
  for (const auto& c : commands()) starts += c.starts_with("StartAcq") ? 1 : 0;
  EXPECT_EQ(starts, 2);
  auto hv = spec_->read_hv();
  ASSERT_TRUE(hv) << to_string(hv.error());
  EXPECT_DOUBLE_EQ(*hv, 4500);
}

// Ten minutes of one-second integrations, each started by its own StartAcq
// and completed by the instrument's event, in no real time to speak of.
TEST_F(NgxSystemVirtual, AcquiresSixHundredFramesWithNoPump) {
  assemble();
  const TimePoint kStart = clock_.now();
  const auto real_start = std::chrono::steady_clock::now();

  auto readings = spec_->acquire(std::size_t{600});

  ASSERT_TRUE(readings) << to_string(readings.error());
  ASSERT_EQ(readings->size(), 600u);
  for (const auto& r : *readings) {
    ASSERT_TRUE(r.values.at("H4").has_value());
    EXPECT_DOUBLE_EQ(r.values.at("H4")->mean, 400);
  }
  int starts = 0;
  for (const auto& c : commands()) starts += c.starts_with("StartAcq") ? 1 : 0;
  EXPECT_EQ(starts, 600);
  // A second each, and between two the reply to StartAcq and the poll that
  // picks the frame up (one poll interval, 20 ms).
  const Duration took = clock_.now() - kStart;
  EXPECT_GE(took, 600s);
  EXPECT_LT(took, 630s);
  // The simulated peer is read every millisecond of clock time, which is
  // 600 000 reads here: two or three seconds as built for debugging, twenty
  // under the thread sanitizer. The bound tells that from the ten minutes.
  EXPECT_LT(std::chrono::steady_clock::now() - real_start, 40s);
}

namespace {

constexpr const char* kNgxLine = R"(
[system]
name = "ngx-line"
scan_interval_ms = 1000
[transports.ngx]
kind = "LINKKIND"
LINKKEY
[drivers.ngx_valves]
kind = "ngx_valves"
transport = "ngx"
link = "LINKNAME"
[[valves]]
name = "A"
actuator = "ngx_valves"
address = "3"
)";

config::SystemConfig line_config(const std::string& kind, const std::string& key, const std::string& link) {
  std::string toml = kNgxLine;
  toml.replace(toml.find("LINKKIND"), 8, kind);
  toml.replace(toml.find("LINKKEY"), 7, key);
  toml.replace(toml.find("LINKNAME"), 8, link);
  auto cfg = config::load_system_config_from_string(toml, "line.toml");
  EXPECT_TRUE(cfg) << cfg.error().what;
  return *cfg;
}

systems::ExtractionLine::Options line_options(const Clock& clock, bool sim) {
  systems::ExtractionLine::Options o;
  o.clock = &clock;
  o.scheduler.threads = 0;
  o.run_scheduler = false;
  o.force_sim = sim;
  return o;
}

// A line stuck in the clock fails with a message; it does not hang.
struct NgxLine : pychron::testing::VirtualTimeTest {};

}  // namespace

TEST_F(NgxLine, ValvesOnTheNgxControllerWorkInSimulation) {
  VirtualClock clock;
  Clock::Participant main(clock, "test");
  auto line = systems::ExtractionLine::create(
      line_config("tcp", "host = \"10.0.0.20\"\nport = 1090", "ngx-sim-line"), std::nullopt, line_options(clock, true));
  ASSERT_TRUE(line) << line.error().what;
  ASSERT_TRUE((*line)->start());
  ASSERT_TRUE((*line)->actuate("A", systems::SwitchOp::Open, "t"));
  EXPECT_EQ(*(*line)->switches().state("A"), ValveState::Open);
  ASSERT_TRUE((*line)->actuate("A", systems::SwitchOp::Close, "t"));
  EXPECT_EQ(*(*line)->switches().state("A"), ValveState::Closed);
  (*line)->stop();
}

TEST_F(NgxLine, ALinkWithNoOwnerFailsActuationsPlainly) {
  ManualClock clock;
  auto line = systems::ExtractionLine::create(line_config("link", "link = \"ngx-nobody\"", "ngx-nobody"),
                                              std::nullopt, line_options(clock, false));
  ASSERT_TRUE(line) << line.error().what;
  ASSERT_TRUE((*line)->start());  // a link transport opens nothing
  auto r = (*line)->actuate("A", systems::SwitchOp::Open, "t");
  ASSERT_FALSE(r);
  EXPECT_NE(r.error().what.find("ngx-nobody"), std::string::npos) << r.error().what;
  (*line)->stop();
}
