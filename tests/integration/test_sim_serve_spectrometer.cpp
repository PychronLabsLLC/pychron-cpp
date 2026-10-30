// The spectrometer over sim serve (sim serve spec, phase B): the example's
// Qtegra and NGX spectrometer files, on the wire, measure a beam fed by the
// served line's gas, through their real drivers and transports. Real time on
// loopback; bounds are upper bounds of 5 s or more, there only to tell an
// answer from a stall. No exact intensities: a reading with gas in the source
// is compared with one without.
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

#include "hardware_style.hpp"
#include "pychron/core/config/loader.hpp"
#include "pychron/systems/canvas/loader.hpp"
#include "pychron/systems/extraction_line.hpp"
#include "pychron/systems/line_server.hpp"
#include "pychron/systems/spectrometer/bringup.hpp"
#include "pychron/systems/spectrometer/data_dir.hpp"
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

class SimServeSpectrometerSteady : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = pychron::testing::unique_temp_path("pychron_sim_serve_spec_");
    fs::create_directories(dir_);
    fs::copy(kExamples / "tables", dir_ / "tables", fs::copy_options::recursive);
  }

  void TearDown() override {
    spectrometer_.reset();
    line_.reset();
    server_.reset();
    std::error_code ec;
    fs::remove_all(dir_, ec);
  }

  // The hardware-style example line (canvas, no sim.toml) and `spectrometer`
  // (an example file), served; then a client line and spectrometer on it.
  void serve(const std::string& line_text, bool with_canvas, const std::string& spectrometer) {
    std::ofstream(dir_ / "extraction_line.toml") << line_text;
    if (with_canvas) fs::copy_file(kExamples / "canvas.toml", dir_ / "canvas.toml");
    fs::copy_file(kExamples / spectrometer, dir_ / "spectrometer.toml");

    auto cfg = config::load_system_config(dir_ / "extraction_line.toml");
    ASSERT_TRUE(cfg) << cfg.error().what;
    std::optional<canvas::Canvas> canvas;
    if (with_canvas) {
      auto loaded = canvas::load_canvas(dir_ / "canvas.toml");
      ASSERT_TRUE(loaded) << loaded.error().what;
      canvas = std::move(*loaded);
    }
    auto data = spectrometer::cfg::load_spectrometer(dir_ / "spectrometer.toml");
    ASSERT_TRUE(data) << data.error().what;
    systems::LineServerOptions options;
    options.sim.sim_file = fs::path();
    options.sim.sim.valve_conductance = 1e3;
    options.sim.sim.noise = 0.0;
    options.spectrometer = std::move(*data);
    auto server = LineServer::start(*cfg, canvas ? &*canvas : nullptr, clock_, std::move(options));
    ASSERT_TRUE(server) << server.error().what;
    server_ = std::move(*server);
    ASSERT_TRUE(write_sim_endpoints(dir_ / "sim-endpoints.toml", server_->endpoints()));

    ExtractionLine::Options line_options;
    line_options.sim_endpoints = dir_ / "sim-endpoints.toml";
    line_options.state_file = dir_ / "client.state.toml";
    auto line = ExtractionLine::load(dir_ / "extraction_line.toml",
                                     with_canvas ? std::optional<fs::path>(dir_ / "canvas.toml") : std::nullopt,
                                     line_options);
    ASSERT_TRUE(line) << line.error().what;
    line_ = std::move(*line);

    auto served = client_spectrometer_endpoints(dir_ / "sim-endpoints.toml", dir_ / "spectrometer.toml", false);
    ASSERT_TRUE(served) << served.error().what;
    auto client_data = spectrometer::cfg::load_spectrometer(dir_ / "spectrometer.toml");
    ASSERT_TRUE(client_data);
    auto spec = spectrometer::load_spectrometer_for_app(
        std::move(*client_data), spectrometer::SpectrometerContext{line_->clock(), line_->scheduler(), line_->bus()},
        spectrometer::SpectrometerBringup{
            .sim_endpoints = std::make_shared<const std::map<std::string, Endpoint>>(std::move(*served))});
    ASSERT_TRUE(spec) << spec.error().what;
    spectrometer_ = std::move(*spec);
    ASSERT_TRUE(line_->start());
  }

  // The largest of one reading's detectors, with the magnet on Ar40.
  double ar40_peak() {
    auto moved = spectrometer_->position(spectrometer::PositionTarget{spectrometer::Isotope{"Ar40"}, {}});
    EXPECT_TRUE(moved) << moved.error().what;
    auto readings = spectrometer_->acquire(std::size_t{1});
    EXPECT_TRUE(readings) << readings.error().what;
    double peak = 0.0;
    if (!readings || readings->empty()) return peak;
    for (const auto& [det, value] : readings->front().values)
      if (value) peak = std::max(peak, value->mean);
    return peak;
  }

  // Gas in the spectrometer's source volume, as air.
  void fill_source(double mbar) {
    const auto source = server_->system().spectrometer_volume();
    ASSERT_TRUE(source);
    ASSERT_TRUE(server_->system().set_pressure(*source, mbar));
  }

  const SteadyClock clock_;
  fs::path dir_;
  std::unique_ptr<LineServer> server_;
  std::unique_ptr<ExtractionLine> line_;
  std::unique_ptr<spectrometer::Spectrometer> spectrometer_;
};

}  // namespace

TEST_F(SimServeSpectrometerSteady, QtegraAirShotReadsAboveBlank) {
  serve(pychron::testing::hardware_style(read_text(kExamples / "extraction_line.toml")), true,
        "spectrometer.qtegra.toml");
  ASSERT_TRUE(spectrometer_);
  fill_source(0.0);
  const double blank = ar40_peak();
  fill_source(1e-6);
  const double air = ar40_peak();
  EXPECT_GT(air, 10 * std::max(blank, 1.0)) << "air " << air << ", blank " << blank;
}

TEST_F(SimServeSpectrometerSteady, NgxAirShotReadsAboveBlank) {
  serve(pychron::testing::hardware_style(read_text(kExamples / "extraction_line.toml")), true,
        "spectrometer.ngx.toml");
  ASSERT_TRUE(spectrometer_);
  fill_source(0.0);
  const double blank = ar40_peak();
  fill_source(1e-6);
  const double air = ar40_peak();
  EXPECT_GT(air, 10 * std::max(blank, 1.0)) << "air " << air << ", blank " << blank;
}

TEST_F(SimServeSpectrometerSteady, NgxLinkValveMovesTheServedLine) {
  serve(R"(
[system]
name = "t"

[transports.ngx_link]
kind = "link"
link = "ngx"

[drivers.ngx_valves]
kind = "ngx_valves"
transport = "ngx_link"

[[valves]]
name = "N1"
actuator = "ngx_valves"
address = "1"
)",
        false, "spectrometer.ngx.toml");
  ASSERT_TRUE(line_);
  EXPECT_FALSE(server_->system().valve_open("N1"));
  ASSERT_TRUE(line_->actuate("N1", SwitchOp::Open));
  EXPECT_TRUE(server_->system().valve_open("N1"));
  ASSERT_TRUE(line_->actuate("N1", SwitchOp::Close));
  EXPECT_FALSE(server_->system().valve_open("N1"));
}
