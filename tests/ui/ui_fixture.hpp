#pragma once

// Shared set-up for the UI smoke suites: the example extraction line with
// every transport simulated. The scheduler is not started, so the only events
// are the ones a test provokes (plus start()'s Snapshot).

#include <chrono>
#include <filesystem>
#include <memory>

#include <QtTest/QtTest>

#include "pychron/core/clock.hpp"
#include "pychron/core/config/loader.hpp"
#include "pychron/systems/extraction_line.hpp"

namespace pychron::ui::test {

// `canvas`: another drawing of the example line (default: the example's own).
// `clock`: the line's clock, which must outlive it (default: real time).
inline std::unique_ptr<systems::ExtractionLine> make_example_line(const std::filesystem::path& canvas = {},
                                                                  const Clock* clock = nullptr) {
  using namespace std::chrono_literals;
  const std::filesystem::path dir = PYCHRON_EXAMPLE_CONFIGS_DIR;
  systems::ExtractionLine::Options options;
  options.force_sim = true;
  options.run_scheduler = false;
  options.clock = clock;
  options.sim.default_pressure = 1e-8;
  options.sim.initial_pressures = {{"bone", 1e-3}};
  options.sim.pumps = {{"turbo", {1e-9, 5s}}};
  options.sim.noise = 0.0;
  // Locks persist beside the config by default; keep tests out of the repo
  // and independent of each other.
  options.state_file = std::filesystem::temp_directory_path() /
                       ("pychron-ui-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                        ".state.toml");
  auto line =
      systems::ExtractionLine::load(dir / "extraction_line.toml", canvas.empty() ? dir / "canvas.toml" : canvas, options);
  if (!line) {
    qFatal("cannot load example line: %s", to_string(line.error()).c_str());
  }
  return std::move(*line);
}

// A line with nothing but a simulated Lake Shore as its [cryo] cryostat:
// inputs A and B, output 1 up to 400 K, output 2 up to 300 K.
inline std::unique_ptr<systems::ExtractionLine> make_cryo_line() {
  auto cfg = config::load_system_config_from_string(R"(
[system]
name = "cryo"
scan_interval_ms = 1000
[transports.cryo]
kind = "sim"
[drivers.cryostat]
kind = "lakeshore"
transport = "cryo"
ranges = [
  { output = 1, range = 3, min = 0.0, max = 400.0 },
  { output = 2, range = 2, min = 0.0, max = 300.0 },
]
[cryo]
driver = "cryostat"
)",
                                                    "cryo.toml");
  if (!cfg) {
    qFatal("cannot load cryo line: %s", to_string(cfg.error()).c_str());
  }
  systems::ExtractionLine::Options options;
  options.run_scheduler = false;
  auto line = systems::ExtractionLine::create(*cfg, std::nullopt, options);
  if (!line) {
    qFatal("cannot create cryo line: %s", to_string(line.error()).c_str());
  }
  return std::move(*line);
}

// A line with two simulated PLC heaters: "furnace" with every field (°C),
// "bake" with only enable and readback.
inline std::unique_ptr<systems::ExtractionLine> make_heater_line() {
  auto cfg = config::load_system_config_from_string(R"(
[system]
name = "heaters"
scan_interval_ms = 1000
[transports.plc]
kind = "sim"
timeout_ms = 100
[transports.plc2]
kind = "sim"
timeout_ms = 100
[drivers.furnace_plc]
kind = "plc2000_heater"
transport = "plc"
enable = 2
use_pid = 1
setpoint = 11
readback = 21
[drivers.bake_plc]
kind = "plc2000_heater"
transport = "plc2"
enable = 1
readback = 3
[[heaters]]
name = "furnace"
driver = "furnace_plc"
description = "Furnace heater"
units = "C"
[[heaters]]
name = "bake"
driver = "bake_plc"
)",
                                                    "heaters.toml");
  if (!cfg) {
    qFatal("cannot load heater line: %s", to_string(cfg.error()).c_str());
  }
  systems::ExtractionLine::Options options;
  options.run_scheduler = false;
  auto line = systems::ExtractionLine::create(*cfg, std::nullopt, options);
  if (!line) {
    qFatal("cannot create heater line: %s", to_string(line.error()).c_str());
  }
  return std::move(*line);
}

}  // namespace pychron::ui::test
