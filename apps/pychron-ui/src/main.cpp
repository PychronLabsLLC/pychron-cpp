// pychron-ui: M1 status/control window.
//
//   pychron-ui [extraction_line.toml [canvas.toml]] [--sim] [--spectrometer <file>]
//              [--lab <dir>] [--data <dir>] [--queue <file>] [--sim-speed <x>]
//
// With no files it opens the example line in configs/examples. --sim forces
// every extraction-line transport to kind = "sim". --spectrometer loads that
// spectrometer config for Window > Spectrometer; with --sim and no file the
// example sim-integrated spectrometer is used. --sim never rewrites a
// spectrometer config: one that is not simulated is refused.
//
// Window > Experiment runs queues against the lab directory (--lab, default
// the line config's directory; records under --data, default <lab>/data).
// --queue opens a queue there. --sim-speed (with --sim) puts the whole app on
// simulated time running that many times faster than real time.
//
// Window > Data browses the records under the data directory
// (<data>/records) and plots them; figure presets live in the user's config
// directory, with lab presets under <lab>/figures.

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <QApplication>
#include <QCoreApplication>
#include <QMessageBox>
#include <QStandardPaths>

#include "command_line.hpp"
#include "experiment_bridge.hpp"
#include "main_window.hpp"
#include "pychron/core/clock_pump.hpp"
#include "pychron/core/log_hub.hpp"
#include "pychron/experiment/lab/session.hpp"
#include "pychron/processing/record_source.hpp"
#include "pychron/sim/spectrometer/beam_model.hpp"
#include "pychron/systems/extraction_line.hpp"
#include "pychron/systems/spectrometer/bringup.hpp"
#include "pychron/systems/spectrometer/data_dir.hpp"
#include "pychron/systems/spectrometer/scan_service.hpp"
#include "spectrometer_bridge.hpp"

int main(int argc, char** argv) {
  pychron::LogHub::install_crash_handlers();
  QApplication app(argc, argv);
  QCoreApplication::setOrganizationName(QStringLiteral("PychronLabs"));
  QApplication::setApplicationName(QStringLiteral("pychron-ui"));

  const auto cli = pychron::ui::parse_command_line(QApplication::arguments().mid(1));
  if (!cli) {
    std::fprintf(stderr, "pychron-ui: %s\n", cli.error().what.c_str());
    return 2;
  }
  pychron::systems::ExtractionLine::Options options;
  options.force_sim = cli->sim;
  // Simulated time: the pump advances the clock and runs the line's scheduler
  // inline (no dispatcher), so polling keeps pace however fast time runs.
  std::unique_ptr<pychron::ManualClock> sim_clock;
  std::unique_ptr<pychron::ClockPump> pump;
  if (cli->sim_speed > 0) {
    sim_clock = std::make_unique<pychron::ManualClock>(pychron::TimePoint{} + std::chrono::hours(1));
    pump = std::make_unique<pychron::ClockPump>(*sim_clock, cli->sim_speed);
    options.clock = sim_clock.get();
    options.scheduler.threads = 0;
    options.run_scheduler = false;
  }
  const std::vector<std::filesystem::path>& files = cli->files;

  const std::filesystem::path examples = PYCHRON_EXAMPLE_CONFIGS_DIR;
  const std::filesystem::path system_file = files.empty() ? examples / "extraction_line.toml" : files[0];
  std::optional<std::filesystem::path> canvas_file;
  if (files.size() > 1) {
    canvas_file = files[1];
  } else if (files.empty()) {
    canvas_file = examples / "canvas.toml";
  }

  auto line = pychron::systems::ExtractionLine::load(system_file, canvas_file, options);
  if (!line) {
    const QString what = QString::fromStdString(pychron::to_string(line.error()));
    std::fprintf(stderr, "pychron-ui: %s\n", qPrintable(what));
    QMessageBox::critical(nullptr, QStringLiteral("pychron-ui"), what);
    return 1;
  }

  if (pump) pump->drive(&(*line)->scheduler());

  // The spectrometer shares the line's clock, scheduler and bus. None of these
  // is left to declaration order: the teardown after the event loop resets
  // each one explicitly, and that order is the one that matters.
  std::unique_ptr<pychron::spectrometer::Spectrometer> spectrometer;
  std::unique_ptr<pychron::spectrometer::ScanService> scan;
  std::unique_ptr<pychron::ui::SpectrometerBridge> spectrometer_bridge;
  std::optional<std::string> spectrometer_error;
  // What was actually loaded decides the window's "(Simulation)" title and the
  // table-following sim beam; --sim only demands it (require_sim).
  bool simulation = false;
  if (cli->spectrometer_file || cli->sim) {
    const std::filesystem::path file =
        cli->spectrometer_file ? *cli->spectrometer_file : examples / "spectrometer.sim-integrated.toml";
    auto loaded = [&]() -> pychron::Result<std::unique_ptr<pychron::spectrometer::Spectrometer>> {
      auto data = pychron::spectrometer::cfg::load_spectrometer(file);
      if (!data) return pychron::fail(data.error());
      simulation = pychron::spectrometer::is_simulated(*data);
      return pychron::spectrometer::load_spectrometer_for_app(
          std::move(*data),
          pychron::spectrometer::SpectrometerContext{(*line)->clock(), (*line)->scheduler(), (*line)->bus()},
          pychron::spectrometer::SpectrometerBringup{.sim_beam_from_table = simulation, .require_sim = cli->sim});
    }();
    if (loaded) {
      spectrometer = std::move(*loaded);
      scan = std::make_unique<pychron::spectrometer::ScanService>(*spectrometer, (*line)->bus(), (*line)->clock());
      spectrometer_bridge = std::make_unique<pychron::ui::SpectrometerBridge>(*spectrometer, *scan, (*line)->bus());
    } else {
      spectrometer_error = pychron::to_string(loaded.error());
    }
  }

  // The experiment session is built once the line has started (below).
  const std::filesystem::path lab_dir = cli->lab ? *cli->lab : system_file.parent_path();
  std::unique_ptr<pychron::experiment::lab::Lab> lab;
  std::unique_ptr<pychron::experiment::lab::LabSession> session;
  std::unique_ptr<pychron::ui::ExperimentBridge> experiment_bridge;

  // Data browsing: the records the experiment writes, and figure presets.
  const std::filesystem::path data_dir = cli->data ? *cli->data : lab_dir / "data";
  pychron::processing::RecordDirectorySource records(data_dir / "records");
  pychron::processing::PresetStore presets(
      std::filesystem::path(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation).toStdString()) / "presets",
      lab_dir / "figures");

  int rc = 0;
  {
    // The window (and its CoreBridge) subscribes before start() so the
    // start-up Snapshot paints the canvas before the first scan.
    pychron::ui::MainWindow window(**line);
    window.resize(1200, 850);

    // Runtime level changes go to the line's LogHub; without one (creation
    // failed) the "Set logger level..." action stays hidden.
    if (const std::shared_ptr<pychron::LogHub> hub = (*line)->log_hub()) {
      window.log_dock()->set_level_callback(
          [weak = std::weak_ptr<pychron::LogHub>(hub)](std::string pattern, pychron::LogLevel level) {
            if (auto h = weak.lock()) h->set_level(pattern, level);
          });
      hub->flush();  // records from load() are on disk before history is read
    }
    if (const auto& dir = (*line)->config().logging.dir; !dir.empty()) {
      window.log_dock()->load_history(dir / "pychron.log");
    }
    window.set_data(&records, &presets);
    window.show();
    if (spectrometer_error) {
      window.log_dock()->append_line(QStringLiteral("ERROR [ui] spectrometer not loaded: ") +
                                     QString::fromStdString(*spectrometer_error));
    }
    const auto started = (*line)->start();
    if (!started) {
      window.log_dock()->append_line(QStringLiteral("ERROR [ui] start failed: ") +
                                     QString::fromStdString(pychron::to_string(started.error())));
    }
    // The spectrometer polls on the line's scheduler, which only runs once the
    // line has started. Without it a scan would report success and never
    // produce a reading, so the window is not offered at all.
    if (spectrometer_bridge) {
      if (started) {
        window.set_spectrometer(spectrometer_bridge.get(), simulation);
      } else {
        window.log_dock()->append_line(QStringLiteral(
            "ERROR [ui] spectrometer unavailable: extraction line did not start (shared scheduler not running)"));
      }
    }
    if (started) {
      const std::filesystem::path spectrometer_config =
          spectrometer ? (cli->spectrometer_file ? *cli->spectrometer_file : examples / "spectrometer.sim-integrated.toml")
                       : std::filesystem::path();
      lab = std::make_unique<pychron::experiment::lab::Lab>(
          pychron::experiment::lab::load_lab({lab_dir, system_file, spectrometer_config}));
      for (const auto& problem : lab->problems) {
        window.log_dock()->append_line(QStringLiteral("WARN [ui] lab: ") + QString::fromStdString(problem));
      }
      session = std::make_unique<pychron::experiment::lab::LabSession>(
          *lab, pychron::experiment::lab::SessionHardware{**line, spectrometer.get(), scan.get()},
          pychron::experiment::lab::SessionOptions{data_dir, {}, {}});
      experiment_bridge = std::make_unique<pychron::ui::ExperimentBridge>(*session, (*line)->bus());
      window.set_experiment(experiment_bridge.get(), cli->sim, cli->queue);
    } else {
      window.log_dock()->append_line(
          QStringLiteral("ERROR [ui] experiment unavailable: extraction line did not start"));
    }
    rc = QApplication::exec();
    window.set_data(nullptr, nullptr);        // data windows go before the record source
    window.set_experiment(nullptr, false);    // the experiment window goes before its bridge
    window.set_spectrometer(nullptr, false);  // closes the spectrometer window before the bridge goes
  }
  // Teardown order (explicit; not the reverse of declaration):
  //   0. the experiment window (above), its bridge, then the session, which
  //      aborts and joins a running queue before anything it uses goes;
  //   1. the spectrometer window, then the main window (the block above);
  //   2. the bridge, whose executor finishes the command in flight; the scan
  //      stop the closing window asked for happens even if it was queued
  //      behind it;
  //   3. the scan service, which stops the acquisition if it is still running;
  //   4. the line: stop() halts the shared scheduler and waits for a poll
  //      already on a worker, so nothing is still using the spectrometer;
  //   5. the spectrometer;
  //   6. the beam registry, whose models refer to the line's clock, before
  //      the line itself is destroyed on return.
  // When the line never started, step 4 does nothing: the scheduler never ran
  // and the spectrometer was never offered, so no poll or command has touched
  // it and the same order is safe.
  experiment_bridge.reset();
  session.reset();
  spectrometer_bridge.reset();
  scan.reset();
  if (pump) pump->drive(nullptr);  // waits for a step in progress
  (*line)->stop();
  spectrometer.reset();
  pychron::sim::BeamModelRegistry::global().clear();
  if (pump) pump->stop();
  return rc;
}
