#pragma once

// Shared set-up for the laser UI suites: a scratch copy of the example lab,
// its line with every transport simulated and started, and the lab's lasers.
// Time is a VirtualClock paced fast, so a stage move takes milliseconds; the
// Qt thread does not take part in it and waits for what it expects in real
// time.

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <system_error>
#include <thread>

#include <QCoreApplication>
#include <QtTest/QtTest>

#include "laser_bridge.hpp"
#include "pychron/core/virtual_clock.hpp"
#include "pychron/devices/extraction/chromium_sim.hpp"
#include "pychron/experiment/lab/lab.hpp"
#include "pychron/experiment/lab/lasers.hpp"
#include "pychron/sim/sim_system.hpp"
#include "pychron/systems/extraction_line.hpp"

namespace pychron::ui::test {

// Destroy any bridge or window built on this before the fixture itself.
struct SimLaserLab {
  std::filesystem::path dir;
  VirtualClock clock;  // outlives everything below
  std::unique_ptr<systems::ExtractionLine> line;
  experiment::lab::Lab lab;
  std::unique_ptr<experiment::lab::Lasers> lasers;

  // `speed`: simulated seconds per real second. A test that stops a move
  // part way takes it slowly, so that the move cannot end first on a busy
  // machine: 40 mm at the stage's 5 mm/s is 1.6 s at 5x.
  explicit SimLaserLab(double speed = 50) : clock(VirtualClock::Options{.speed = speed}) {
    namespace fs = std::filesystem;
    std::random_device rd;
    dir = fs::temp_directory_path() / ("pychron-ui-laser-" + std::to_string(rd()) + std::to_string(rd()));
    fs::copy(fs::path(PYCHRON_EXAMPLE_CONFIGS_DIR), dir, fs::copy_options::recursive);
    fs::remove_all(dir / "data");
    fs::remove_all(dir / "stage_corrections");
    systems::ExtractionLine::Options options;
    options.clock = &clock;
    options.force_sim = true;
    options.state_file = dir / "line.state.toml";
    auto loaded = systems::ExtractionLine::load(dir / "extraction_line.toml", dir / "canvas.toml", options);
    if (!loaded) qFatal("cannot load example line: %s", to_string(loaded.error()).c_str());
    line = std::move(*loaded);
    if (auto started = line->start(); !started) qFatal("line did not start: %s", to_string(started.error()).c_str());
    lab = experiment::lab::load_lab({dir, dir / "extraction_line.toml", {}});
    lasers = std::make_unique<experiment::lab::Lasers>(lab, *line);
  }
  SimLaserLab(const SimLaserLab&) = delete;
  SimLaserLab& operator=(const SimLaserLab&) = delete;
  ~SimLaserLab() {
    lasers.reset();
    if (line) line->stop();
    line.reset();
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
  }

  extraction::ChromiumSim& sim() { return *line->sim()->chromium("co2"); }
  laser::LaserSystem& system() { return *lasers->find("co2"); }
  // Where the simulated stage is, mm.
  double x() { return static_cast<double>(sim().position().x) / 1000.0; }
  double y() { return static_cast<double>(sim().position().y) / 1000.0; }
  double z() { return static_cast<double>(sim().position().z) / 1000.0; }

  LaserBridgeDeps deps(std::function<void()> abort_queue = {}) {
    return LaserBridgeDeps{*lasers, lab, "co2", std::move(abort_queue), std::chrono::milliseconds(2),
                           std::chrono::milliseconds(15)};
  }
};

// The simulated stage is under way: past `x` mm. False if it never gets there.
inline bool under_way(SimLaserLab& lab, double x = 1.0) {
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(20);
  while (lab.x() < x) {
    if (std::chrono::steady_clock::now() > until) return false;
    std::this_thread::sleep_for(std::chrono::microseconds(200));
  }
  return true;
}

// Every command issued has finished and its signals have been delivered.
inline void settle(LaserBridge& bridge) {
  bridge.drain();
  QCoreApplication::processEvents();
}

}  // namespace pychron::ui::test
