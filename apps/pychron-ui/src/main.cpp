// pychron-ui: M1 status/control window.
//
//   pychron-ui [extraction_line.toml [canvas.toml]] [--sim] [--spectrometer <file>]
//
// With no files it opens the example line in configs/examples. --sim forces
// every transport to kind = "sim". --spectrometer loads that spectrometer
// config for Window > Spectrometer; with --sim and no file the example
// sim-integrated spectrometer is used.

#include <cstdio>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include <QApplication>
#include <QCoreApplication>
#include <QMessageBox>
#include <QStringList>

#include "main_window.hpp"
#include "pychron/core/log_hub.hpp"
#include "pychron/sim/spectrometer/beam_model.hpp"
#include "pychron/systems/extraction_line.hpp"
#include "pychron/systems/spectrometer/bringup.hpp"
#include "pychron/systems/spectrometer/scan_service.hpp"
#include "spectrometer_bridge.hpp"

int main(int argc, char** argv) {
  pychron::LogHub::install_crash_handlers();
  QApplication app(argc, argv);
  QCoreApplication::setOrganizationName(QStringLiteral("PychronLabs"));
  QApplication::setApplicationName(QStringLiteral("pychron-ui"));

  pychron::systems::ExtractionLine::Options options;
  std::vector<std::filesystem::path> files;
  std::optional<std::filesystem::path> spectrometer_file;
  const QStringList args = QApplication::arguments().mid(1);
  for (qsizetype i = 0; i < args.size(); ++i) {
    const QString& arg = args[i];
    if (arg == QStringLiteral("--sim")) {
      options.force_sim = true;
    } else if (arg == QStringLiteral("--spectrometer")) {
      if (i + 1 >= args.size()) {
        std::fprintf(stderr, "pychron-ui: --spectrometer needs a file\n");
        return 2;
      }
      spectrometer_file = std::filesystem::path(args[++i].toStdString());
    } else {
      files.emplace_back(arg.toStdString());
    }
  }

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

  // The spectrometer shares the line's clock, scheduler and bus. Destruction
  // order (reverse of declaration): window, bridge, scan service,
  // spectrometer, then the line. The beam registry refers to the line's clock,
  // so it is emptied before the line goes.
  std::unique_ptr<pychron::spectrometer::Spectrometer> spectrometer;
  std::unique_ptr<pychron::spectrometer::ScanService> scan;
  std::unique_ptr<pychron::ui::SpectrometerBridge> spectrometer_bridge;
  std::optional<std::string> spectrometer_error;
  const bool simulation = options.force_sim;
  if (spectrometer_file || options.force_sim) {
    const std::filesystem::path file =
        spectrometer_file ? *spectrometer_file : examples / "spectrometer.sim-integrated.toml";
    auto loaded = pychron::spectrometer::load_spectrometer_for_app(
        file, pychron::spectrometer::SpectrometerContext{(*line)->clock(), (*line)->scheduler(), (*line)->bus()},
        pychron::spectrometer::SpectrometerBringup{.sim_beam_from_table = options.force_sim});
    if (loaded) {
      spectrometer = std::move(*loaded);
      scan = std::make_unique<pychron::spectrometer::ScanService>(*spectrometer, (*line)->bus(), (*line)->clock());
      spectrometer_bridge = std::make_unique<pychron::ui::SpectrometerBridge>(*spectrometer, *scan, (*line)->bus());
    } else {
      spectrometer_error = pychron::to_string(loaded.error());
    }
  }

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
    window.set_spectrometer(spectrometer_bridge.get(), simulation);
    window.show();
    if (spectrometer_error) {
      window.log_dock()->append_line(QStringLiteral("ERROR [ui] spectrometer not loaded: ") +
                                     QString::fromStdString(*spectrometer_error));
    }
    if (auto started = (*line)->start(); !started) {
      window.log_dock()->append_line(QStringLiteral("ERROR [ui] start failed: ") +
                                     QString::fromStdString(pychron::to_string(started.error())));
    }
    rc = QApplication::exec();
    window.set_spectrometer(nullptr, false);  // closes the spectrometer window before the bridge goes
  }
  spectrometer_bridge.reset();
  scan.reset();
  (*line)->stop();
  spectrometer.reset();
  pychron::sim::BeamModelRegistry::global().clear();
  return rc;
}
