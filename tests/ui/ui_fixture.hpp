#pragma once

// Shared set-up for the UI smoke suites: the example extraction line with
// every transport simulated. The scheduler is not started, so the only events
// are the ones a test provokes (plus start()'s Snapshot).

#include <chrono>
#include <filesystem>
#include <memory>

#include <QtTest/QtTest>

#include "pychron/systems/extraction_line.hpp"

namespace pychron::ui::test {

inline std::unique_ptr<systems::ExtractionLine> make_example_line() {
  using namespace std::chrono_literals;
  const std::filesystem::path dir = PYCHRON_EXAMPLE_CONFIGS_DIR;
  systems::ExtractionLine::Options options;
  options.force_sim = true;
  options.run_scheduler = false;
  options.sim.default_pressure = 1e-8;
  options.sim.initial_pressures = {{"bone", 1e-3}};
  options.sim.pumps = {{"turbo", {1e-9, 5s}}};
  options.sim.noise = 0.0;
  // Locks persist beside the config by default; keep tests out of the repo
  // and independent of each other.
  options.state_file = std::filesystem::temp_directory_path() /
                       ("pychron-ui-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                        ".state.toml");
  auto line = systems::ExtractionLine::load(dir / "extraction_line.toml", dir / "canvas.toml", options);
  if (!line) {
    qFatal("cannot load example line: %s", to_string(line.error()).c_str());
  }
  return std::move(*line);
}

}  // namespace pychron::ui::test
