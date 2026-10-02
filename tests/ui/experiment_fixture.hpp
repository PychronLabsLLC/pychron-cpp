#pragma once

// Shared set-up for the experiment UI suites: a scratch copy of the example
// lab (configs/examples) on a simulated clock pumped 400x that drives the
// line's scheduler, the sim spectrometer and a LabSession, as
// `pychron-ui --sim --sim-speed 400` builds them.

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>

#include <QtTest/QtTest>

#include "pychron/core/clock_pump.hpp"
#include "pychron/experiment/lab/session.hpp"
#include "pychron/experiment/model/queue_file.hpp"
#include "pychron/scripting/script_host.hpp"
#include "pychron/sim/spectrometer/beam_model.hpp"
#include "pychron/systems/extraction_line.hpp"
#include "pychron/systems/spectrometer/bringup.hpp"
#include "pychron/systems/spectrometer/scan_service.hpp"

namespace pychron::ui::test {

inline std::filesystem::path scratch_lab() {
  namespace fs = std::filesystem;
  const fs::path dir = fs::temp_directory_path() /
                       ("pychron-ui-lab-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  fs::copy(fs::path(PYCHRON_EXAMPLE_CONFIGS_DIR), dir, fs::copy_options::recursive);
  fs::remove_all(dir / "data");
  return dir;
}

inline experiment::lab::LabPaths lab_paths(const std::filesystem::path& dir) {
  return {dir, dir / "extraction_line.toml", dir / "spectrometer.sim-integrated.toml"};
}

// The example queue; without embedded Python its scripts are dropped.
inline experiment::QueueSpec example_queue(const std::filesystem::path& dir, const experiment::IdentifierRules& ids) {
  auto q = experiment::load_queue_file((dir / "experiment.toml").string(), ids);
  if (!q) qFatal("cannot load the example queue: %s", q.error().what.c_str());
  if (!scripting::make_script_host()->available()) {
    for (auto& r : q->runs) {
      r.extraction.script.clear();
      r.post_measurement.reset();
    }
  }
  return *q;
}

// Destroy any bridge or window built on this before the fixture itself.
struct SimLab {
  std::filesystem::path dir = scratch_lab();
  ManualClock clock{TimePoint{} + std::chrono::hours(1)};
  ClockPump pump{clock, 400};
  std::unique_ptr<systems::ExtractionLine> line;
  std::unique_ptr<spectrometer::Spectrometer> spec;
  std::unique_ptr<spectrometer::ScanService> scan;
  experiment::lab::Lab lab;
  std::unique_ptr<experiment::lab::LabSession> session;

  // `notifications` replace the lab's; `notify` runs their programs.
  explicit SimLab(std::optional<experiment::lab::NotificationConfig> notifications = std::nullopt,
                  experiment::lab::ProcessRunner notify = {}) {
    systems::ExtractionLine::Options options;
    options.clock = &clock;
    options.force_sim = true;
    options.scheduler.threads = 0;
    options.run_scheduler = false;
    options.state_file = dir / "line.state.toml";
    auto made = systems::ExtractionLine::load(dir / "extraction_line.toml", dir / "canvas.toml", options);
    if (!made) qFatal("cannot load the example line: %s", made.error().what.c_str());
    line = std::move(*made);
    pump.drive(&line->scheduler());
    if (!line->start()) qFatal("the example line did not start");
    auto loaded = spectrometer::load_spectrometer_for_app(
        dir / "spectrometer.sim-integrated.toml", spectrometer::SpectrometerContext{clock, line->scheduler(), line->bus()},
        spectrometer::SpectrometerBringup{.sim_beam_from_table = true});
    if (!loaded) qFatal("cannot load the sim spectrometer: %s", loaded.error().what.c_str());
    spec = std::move(*loaded);
    scan = std::make_unique<spectrometer::ScanService>(*spec, line->bus(), clock);
    lab = experiment::lab::load_lab(lab_paths(dir));
    if (notifications) lab.notifications = std::move(*notifications);
    session = std::make_unique<experiment::lab::LabSession>(
        lab, experiment::lab::SessionHardware{*line, spec.get(), scan.get()},
        experiment::lab::SessionOptions{dir / "data", {}, std::move(notify)});
  }
  SimLab(const SimLab&) = delete;
  SimLab& operator=(const SimLab&) = delete;

  ~SimLab() {
    session.reset();  // aborts and joins a running queue
    scan.reset();
    pump.drive(nullptr);
    pump.stop();
    line->stop();
    spec.reset();
    sim::BeamModelRegistry::global().clear();
    line.reset();
    std::filesystem::remove_all(dir);
  }

  experiment::QueueSpec queue() const { return example_queue(dir, lab.ids); }
};

}  // namespace pychron::ui::test
