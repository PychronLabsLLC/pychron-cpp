// pychron-ui: M1 status/control window.
//
//   pychron-ui [extraction_line.toml [canvas.toml]] [--sim]
//
// With no files it opens the example line in configs/examples. --sim forces
// every transport to kind = "sim".

#include <cstdio>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include <QApplication>
#include <QMessageBox>
#include <QStringList>

#include "main_window.hpp"
#include "pychron/core/log_hub.hpp"
#include "pychron/systems/extraction_line.hpp"

int main(int argc, char** argv) {
  pychron::LogHub::install_crash_handlers();
  QApplication app(argc, argv);
  QApplication::setApplicationName(QStringLiteral("pychron-ui"));

  pychron::systems::ExtractionLine::Options options;
  std::vector<std::filesystem::path> files;
  const QStringList args = QApplication::arguments().mid(1);
  for (const QString& arg : args) {
    if (arg == QStringLiteral("--sim")) {
      options.force_sim = true;
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
    window.show();
    if (auto started = (*line)->start(); !started) {
      window.log_dock()->append_line(QStringLiteral("ERROR [ui] start failed: ") +
                                     QString::fromStdString(pychron::to_string(started.error())));
    }
    rc = QApplication::exec();
    (*line)->stop();
  }
  return rc;
}
