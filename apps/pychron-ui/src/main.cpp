// pychron-ui: M1 status/control window.
//
//   pychron-ui [--install <name> | --setup | --examples | extraction_line.toml [canvas.toml]]
//              [--sim] [--spectrometer <file>] [--lab <dir>] [--data <dir>]
//              [--queue <file>] [--sim-speed <x>] [--db <url>]
//
// With no config files it opens an install from the site config: --install
// names one, else the default (or the only) one; with several and no default
// File > Installations asks. With nothing installed (or --setup) the setup
// wizard runs first. An instrument install opens its line, canvas and
// spectrometer configs, as --sim when it was set up for simulation; a
// data-reduction install opens the data browser on its database alone.
// --examples opens the example line shipped with pychron (development), as
// does --sim when nothing is installed.
//
// --sim forces every extraction-line transport to kind = "sim".
// --spectrometer loads that spectrometer config for View > Spectrometer;
// with --sim and no file the example sim-integrated spectrometer is used.
// --sim never rewrites a spectrometer config: one that is not simulated is
// refused.
//
// View > Experiment runs queues against the lab directory (--lab, default
// the install folder or the line config's directory; records under --data,
// default <lab>/data). --queue opens a queue there. --sim-speed (with --sim)
// puts the whole app on simulated time running that many times faster than
// real time, from the real time of day. It takes a number: unlimited speed
// (elctl's --sim-speed max) would finish a queue before the window painted.
//
// View > Data browses the records under the data directory
// (<data>/records) and plots them; with --db it browses that DVC store
// instead ("postgresql://user:pw@host/db" or "sqlite:/path/to/file.db"; the
// schema must be current, it is never migrated from here). Figure presets
// live in the user's config directory, with lab presets under <lab>/figures.
//
// File > Preferences… sets the font sizes, the data browser page size and the
// spectrometer's large-move threshold; they are kept in the user's settings.

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <QApplication>
#include <QCoreApplication>
#include <QMessageBox>
#include <QSettings>
#include <QStandardPaths>

#include "brand.hpp"
#include "command_line.hpp"
#include "data_main_window.hpp"
#include "experiment_bridge.hpp"
#include "installations_dialog.hpp"
#include "laser_bridge.hpp"
#include "laser_window.hpp"
#include "main_window.hpp"
#include "pattern_maker_window.hpp"
#include "preferences.hpp"
#include "pychron/setup/doctor.hpp"
#include "pychron/setup/installer.hpp"
#include "pychron/setup/site.hpp"
#include "setup_support.hpp"
#include "setup_wizard.hpp"
#include "pychron/core/log_hub.hpp"
#include "pychron/core/virtual_clock.hpp"
#include "pychron/experiment/lab/lasers.hpp"
#include "pychron/experiment/lab/session.hpp"
#include "pychron/sim/sim_system.hpp"
#include "pychron/processing/record_source.hpp"
#ifdef PYCHRON_UI_HAS_STORE
// Qt's `signals` keyword macro would rewrite CollectionRoots::signals.
#pragma push_macro("signals")
#undef signals
#include "pychron/processing/store_source.hpp"
#pragma pop_macro("signals")
#include "entry_actions.hpp"
#endif
#include "pychron/sim/spectrometer/beam_model.hpp"
#include "pychron/systems/extraction_line.hpp"
#include "pychron/systems/spectrometer/bringup.hpp"
#include "pychron/systems/spectrometer/data_dir.hpp"
#include "pychron/systems/spectrometer/scan_service.hpp"
#include "spectrometer_bridge.hpp"
#include "theme.hpp"

namespace {

namespace fs = std::filesystem;
namespace setup = pychron::setup;

int fatal(const std::string& what) {
  std::fprintf(stderr, "pychron-ui: %s\n", what.c_str());
  QMessageBox::critical(nullptr, QStringLiteral("pychron-ui"), QString::fromStdString(what));
  return 1;
}

// Which install to open: the one named, else the default, else the wizard
// (nothing installed, or --setup), else File > Installations' choice.
// nullopt with rc: quit with rc. nullopt with rc < 0: the shipped examples.
struct Choice {
  std::optional<setup::SiteInstall> install;
  int rc = -1;
};

Choice choose_install(const pychron::ui::CommandLine& cli, const setup::Resources& resources) {
  const fs::path site_path = setup::default_site_path();
  auto site = setup::load_site(site_path);
  if (!site) return {std::nullopt, fatal(site.error().what)};
  if (cli.install) {
    if (const auto* i = site->find(*cli.install)) return {*i, 0};
    return {std::nullopt, fatal("no installation named '" + *cli.install + "' in " + site_path.string())};
  }
  if (!cli.setup) {
    if (const auto* i = site->pick()) return {*i, 0};
    if (site->installs.empty() && cli.sim) return {};  // development: the examples
  }
  auto library = setup::ProfileLibrary::load(resources.profiles, resources.examples);
  if (cli.setup || site->installs.empty()) {
    if (!library) return {std::nullopt, fatal(library.error().what)};
    pychron::ui::SetupWizard wizard(*library,
                                    {site_path, pychron::ui::database_opener(), {}, {}, pychron::ui::database_seeder()});
    if (wizard.exec() != QDialog::Accepted || !wizard.open_now()) return {std::nullopt, 0};
    return {wizard.installed(), 0};
  }
  pychron::ui::InstallationsDialog dialog(site_path, library ? &*library : nullptr, pychron::ui::database_opener(), {});
  dialog.set_seed_database(pychron::ui::database_seeder());
  if (dialog.exec() != QDialog::Accepted || !dialog.to_open()) return {std::nullopt, 0};
  auto again = setup::load_site(site_path);
  const auto* i = again ? again->find(*dialog.to_open()) : nullptr;
  if (i == nullptr) return {std::nullopt, fatal("installation '" + *dialog.to_open() + "' is gone")};
  return {*i, 0};
}

// File > Installations from an open window: switching closes this one and
// starts pychron-ui again on the other install.
std::function<void()> installations_handler(QWidget* window, const setup::Resources& resources, std::string current) {
  return [window, resources, current] {
    auto library = setup::ProfileLibrary::load(resources.profiles, resources.examples);
    pychron::ui::InstallationsDialog dialog(setup::default_site_path(), library ? &*library : nullptr,
                                            pychron::ui::database_opener(), current, window);
    dialog.set_seed_database(pychron::ui::database_seeder());
    if (dialog.exec() != QDialog::Accepted || !dialog.to_open() || *dialog.to_open() == current) return;
    if (!window->close()) return;  // e.g. unsaved queue edits, and the user chose Cancel
    if (!pychron::ui::start_install(*dialog.to_open())) {
      QMessageBox::critical(nullptr, QStringLiteral("pychron-ui"), QStringLiteral("could not start pychron-ui"));
    }
  };
}

// A data-reduction install: the data browser on its database, nothing else.
int run_data_reduction(const setup::SiteInstall& install, const pychron::ui::CommandLine& cli,
                       const setup::Resources& resources) {
  pychron::processing::RecordDirectorySource records(install.path(install.data.empty() ? "data" : install.data) /
                                                     "records");
  std::unique_ptr<pychron::processing::IAnalysisSource> store_source;
  std::string url = cli.db.value_or(std::string{});
  if (url.empty() && !install.database.empty()) {
    auto with_password = setup::database_url(install);
    if (!with_password) return fatal(with_password.error().what);
    url = *with_password;
  }
  if (!url.empty()) {
#ifdef PYCHRON_UI_HAS_STORE
    auto opened = pychron::processing::StoreSource::open(pychron::persistence::StoreConfig{url, false});
    if (!opened) {
      return fatal("installation '" + install.name + "': database " + install.database + ": " +
                   pychron::to_string(opened.error()) + "\n\nelctl doctor --install " + install.name +
                   " says more; pychron-ui --setup can set it up again.");
    }
    store_source = std::move(*opened);
#else
    std::fprintf(stderr, "pychron-ui: built without the DVC store; browsing the records folder instead\n");
#endif
  }
  pychron::processing::IAnalysisSource& source =
      store_source ? *store_source : static_cast<pychron::processing::IAnalysisSource&>(records);
  pychron::processing::PresetStore presets(
      fs::path(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation).toStdString()) / "presets",
      install.root / "figures");
  pychron::ui::DataMainWindow window(source, presets, QString::fromStdString(install.name));
  window.set_installations_handler(installations_handler(&window, resources, install.name));
#ifdef PYCHRON_UI_HAS_STORE
  // Entry: samples and packages in the install's database (parented to the window).
  if (!url.empty()) new pychron::ui::EntryActions(&window, url);
#endif
  window.apply_preferences(pychron::ui::load_preferences(QSettings()));
  window.resize(1200, 800);
  window.show();
  return QApplication::exec();
}

// --laser: one extraction device's laser window and nothing else, for a
// laser PC. The line is loaded as usual (its drivers are the laser's), with
// no spectrometer and no experiment session: nothing here runs a queue.
int run_laser(pychron::systems::ExtractionLine& line, const pychron::ui::CommandLine& cli, const fs::path& lab_dir,
              const fs::path& system_file, pychron::ui::SplashScreen& splash) {
  namespace lab = pychron::experiment::lab;
  splash.status(QStringLiteral("Starting the extraction line"));
  if (const auto started = line.start(); !started) {
    splash.close();
    return fatal("the extraction line did not start: " + pychron::to_string(started.error()));
  }
  splash.status(QStringLiteral("Loading the lab: %1").arg(QString::fromStdString(lab_dir.string())));
  lab::Lab the_lab = lab::load_lab({lab_dir, system_file, fs::path()});
  for (const auto& problem : the_lab.problems) std::fprintf(stderr, "pychron-ui: lab: %s\n", problem.c_str());
  lab::Lasers lasers(the_lab, line);
  for (const auto& problem : lasers.problems()) std::fprintf(stderr, "pychron-ui: laser: %s\n", problem.c_str());
  // What stops nothing is still said: a camera for looking that did not open.
  for (const auto& note : lasers.notes()) std::fprintf(stderr, "pychron-ui: laser: %s\n", note.c_str());

  const std::vector<std::string> names = lasers.names();
  std::string known;
  for (const auto& name : names) known += (known.empty() ? "" : ", ") + name;
  std::string device;
  if (cli.device) {
    if (lasers.find(*cli.device) == nullptr) {
      splash.close();
      return fatal("--device " + *cli.device + ": no such extraction device in " + system_file.string() + " (it has: " +
                   (known.empty() ? "none" : known) + ")");
    }
    device = *cli.device;
  } else if (names.size() == 1) {
    device = names.front();
  } else {
    splash.close();
    return fatal(names.empty() ? "--laser: " + system_file.string() + " has no extraction device (a laser's driver)"
                               : "--laser: say which with --device (" + known + ")");
  }

  int rc = 0;
  {
    pychron::ui::LaserBridge bridge(pychron::ui::LaserBridgeDeps{lasers, the_lab, device, {}});
    const bool simulated = line.sim() != nullptr && line.sim()->chromium(device) != nullptr;
    pychron::ui::LaserWindow window(bridge, simulated);
    pychron::ui::PatternMakerWindow maker(the_lab.patterns, lab_dir / "patterns", &window);
    maker.setWindowFlag(Qt::Window, true);
    QObject::connect(&maker, &pychron::ui::PatternMakerWindow::saved, &window, [&window] { window.refresh_patterns(); });
    window.set_pattern_maker([&maker] {
      maker.show();
      maker.raise();
      maker.activateWindow();
    });
    window.resize(1100, 760);
    window.show();
    splash.finish_after(&window, std::chrono::milliseconds(1200));
    rc = QApplication::exec();
  }  // the window, then its bridge: a motion being waited for is stopped
  return rc;
}

}  // namespace

int main(int argc, char** argv) {
  pychron::LogHub::install_crash_handlers();
#ifdef _WIN32
  // A GUI program has no console: --version and --self-test print to the one
  // they were started from.
  for (int i = 1; i < argc; ++i) {
    const std::string_view a(argv[i]);
    if ((a == "--version" || a == "--self-test" || a == "--write-icons") && AttachConsole(ATTACH_PARENT_PROCESS)) {
      std::freopen("CONOUT$", "w", stdout);
      std::freopen("CONOUT$", "w", stderr);
    }
  }
#endif
  // --version needs no display.
  for (int i = 1; i < argc; ++i) {
    if (std::string_view(argv[i]) == "--version") {
      std::printf("pychron-ui %s\n", std::string(setup::version()).c_str());
      return 0;
    }
  }
  QApplication app(argc, argv);
  QCoreApplication::setOrganizationName(QStringLiteral("PychronLabs"));
  QApplication::setApplicationName(QStringLiteral("pychron-ui"));
  pychron::ui::style::apply(app);
  QApplication::setWindowIcon(pychron::ui::brand::app_icon());

  const auto cli = pychron::ui::parse_command_line(QApplication::arguments().mid(1));
  if (!cli) {
    std::fprintf(stderr, "pychron-ui: %s\n", cli.error().what.c_str());
    return 2;
  }
  if (cli->self_test) return pychron::ui::self_test(std::cout);
  if (cli->write_icons) return pychron::ui::write_icons(*cli->write_icons, std::cout);
  const setup::Resources resources = setup::find_resources();
  const fs::path examples = resources.examples;

  std::optional<setup::SiteInstall> install;
  if (cli->files.empty() && !cli->examples) {
    Choice choice = choose_install(*cli, resources);
    if (!choice.install && choice.rc >= 0) return choice.rc;
    install = std::move(choice.install);
  }
  if (install && install->kind == "data_reduction") return run_data_reduction(*install, *cli, resources);

  // An instrument install fills in what the command line leaves out.
  const bool sim = cli->sim || (install && install->simulation);
  std::optional<fs::path> spectrometer_file = cli->spectrometer_file;
  if (install && !spectrometer_file && !install->spectrometer.empty() &&
      fs::exists(install->path(install->spectrometer))) {
    spectrometer_file = install->path(install->spectrometer);
  }

  // Shown from here until the main window is up (after any installation
  // choice, which it would cover); loading blocks the event loop, so each step
  // repaints it explicitly.
  pychron::ui::SplashScreen splash(sim);
  splash.show();
  splash.status(QStringLiteral("Starting"));

  pychron::systems::ExtractionLine::Options options;
  options.force_sim = sim;
  // Simulated time: a VirtualClock paced to the speed asked for, starting at
  // the real time of day. The line's threads take part in it; this one (the
  // UI's) does not, so time never waits for the event loop. Declared before
  // the line and everything else that is given it, so it is the last to go.
  std::unique_ptr<pychron::VirtualClock> sim_clock;
  if (cli->sim_speed > 0) {
    pychron::VirtualClock::Options clock_options;
    clock_options.speed = cli->sim_speed;
    clock_options.epoch = std::chrono::system_clock::now();
    // The line's log hub does not exist yet and goes before this clock does.
    clock_options.on_stall = [](std::string report) { std::fprintf(stderr, "pychron-ui: %s\n", report.c_str()); };
    sim_clock = std::make_unique<pychron::VirtualClock>(std::move(clock_options));
    options.clock = sim_clock.get();
  }
  const std::vector<fs::path>& files = cli->files;

  fs::path system_file;
  std::optional<fs::path> canvas_file;
  if (install) {
    system_file = install->path(install->line);
    if (!install->canvas.empty()) canvas_file = install->path(install->canvas);
  } else if (files.empty()) {
    system_file = examples / "extraction_line.toml";
    canvas_file = examples / "canvas.toml";
  } else {
    system_file = files[0];
    if (files.size() > 1) canvas_file = files[1];
  }

  splash.status(QStringLiteral("Loading the extraction line: %1").arg(QString::fromStdString(system_file.filename().string())));
  auto line = pychron::systems::ExtractionLine::load(system_file, canvas_file, options);
  if (!line) {
    std::string what = pychron::to_string(line.error());
    if (install) what = "installation '" + install->name + "': " + what + "\n\nelctl doctor --install " + install->name + " says more.";
    splash.close();  // it would sit over the message box
    return fatal(what);
  }

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
  // The experiment session is built once the line has started (below).
  const fs::path lab_dir = cli->lab ? *cli->lab : install ? install->root : system_file.parent_path();
  if (cli->laser) {
    const int laser_rc = run_laser(**line, *cli, lab_dir, system_file, splash);
    (*line)->stop();
    return laser_rc;
  }

  const fs::path spectrometer_config =
      spectrometer_file ? *spectrometer_file : sim ? examples / "spectrometer.sim-integrated.toml" : fs::path();
  if (!spectrometer_config.empty()) {
    const fs::path& file = spectrometer_config;
    splash.status(QStringLiteral("Bringing up the spectrometer: %1").arg(QString::fromStdString(file.filename().string())));
    // Written only when a simulated beam was fed from a simulated line.
    auto fed = pychron::spectrometer::BeamFeed::LineGas;
    auto loaded = [&]() -> pychron::Result<std::unique_ptr<pychron::spectrometer::Spectrometer>> {
      auto data = pychron::spectrometer::cfg::load_spectrometer(file);
      if (!data) return pychron::fail(data.error());
      simulation = pychron::spectrometer::is_simulated(*data);
      return pychron::spectrometer::load_spectrometer_for_app(
          std::move(*data),
          pychron::spectrometer::SpectrometerContext{(*line)->clock(), (*line)->scheduler(), (*line)->bus()},
          // A simulated spectrometer measures the simulated line's gas.
          pychron::spectrometer::SpectrometerBringup{
              .sim_beam_from_table = simulation, .require_sim = sim, .line_sim = (*line)->sim(), .fed = &fed});
    }();
    if (loaded && fed == pychron::spectrometer::BeamFeed::FixedGas) {
      std::fprintf(stderr, "pychron-ui: the simulated spectrometer is not joined to the line: %s\n",
                   (*line)->canvas() != nullptr ? "the canvas has no spectrometer stage"
                                                : "the line was loaded without its canvas");
    }
    if (loaded) {
      spectrometer = std::move(*loaded);
      scan = std::make_unique<pychron::spectrometer::ScanService>(*spectrometer, (*line)->bus(), (*line)->clock());
      spectrometer_bridge = std::make_unique<pychron::ui::SpectrometerBridge>(*spectrometer, *scan, (*line)->bus());
    } else {
      spectrometer_error = pychron::to_string(loaded.error());
    }
  }

  std::unique_ptr<pychron::experiment::lab::Lab> lab;
  // The lab's lasers are shared by the session (queues) and the laser
  // windows (an operator): one drives at a time.
  std::unique_ptr<pychron::experiment::lab::Lasers> lasers;
  std::unique_ptr<pychron::experiment::lab::LabSession> session;
  std::unique_ptr<pychron::ui::ExperimentBridge> experiment_bridge;
  std::vector<std::unique_ptr<pychron::ui::LaserBridge>> laser_bridges;

  // Data browsing: the records the experiment writes, and figure presets.
  const fs::path data_dir = cli->data            ? *cli->data
                            : install && !install->data.empty() ? install->path(install->data)
                                                                : lab_dir / "data";
  pychron::processing::RecordDirectorySource records(data_dir / "records");
  std::unique_ptr<pychron::processing::IAnalysisSource> store_source;
  if (cli->db) {
#ifdef PYCHRON_UI_HAS_STORE
    splash.status(QStringLiteral("Opening the DVC store"));
    auto opened = pychron::processing::StoreSource::open(pychron::persistence::StoreConfig{*cli->db, false});
    if (!opened) {
      std::fprintf(stderr, "pychron-ui: --db: %s\n", pychron::to_string(opened.error()).c_str());
      return 2;
    }
    store_source = std::move(*opened);
#else
    std::fprintf(stderr, "pychron-ui: --db: built without the DVC store (PYCHRON_PERSISTENCE=OFF or no Qt Sql)\n");
    return 2;
#endif
  }
  pychron::processing::IAnalysisSource& data_source =
      store_source ? *store_source : static_cast<pychron::processing::IAnalysisSource&>(records);
  pychron::processing::PresetStore presets(
      std::filesystem::path(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation).toStdString()) / "presets",
      lab_dir / "figures");

  int rc = 0;
  {
    // The window (and its CoreBridge) subscribes before start() so the
    // start-up Snapshot paints the canvas before the first scan.
    pychron::ui::MainWindow window(**line);
    window.resize(1200, 850);
    window.apply_preferences(pychron::ui::load_preferences(QSettings()));
    window.set_installations_handler(installations_handler(&window, resources, install ? install->name : std::string{}));

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
    window.set_data(&data_source, &presets);
#ifdef PYCHRON_UI_HAS_STORE
    if (cli->db) new pychron::ui::EntryActions(&window, *cli->db);  // the Entry menu, parented to the window
#endif
    window.show();
    if (spectrometer_error) {
      window.log_dock()->append_line(QStringLiteral("ERROR [ui] spectrometer not loaded: ") +
                                     QString::fromStdString(*spectrometer_error));
    }
    splash.status(QStringLiteral("Starting the extraction line"));
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
      splash.status(QStringLiteral("Loading the lab: %1").arg(QString::fromStdString(lab_dir.string())));
      lab = std::make_unique<pychron::experiment::lab::Lab>(pychron::experiment::lab::load_lab(
          {lab_dir, system_file, spectrometer ? spectrometer_config : fs::path()}));
      for (const auto& problem : lab->problems) {
        window.log_dock()->append_line(QStringLiteral("WARN [ui] lab: ") + QString::fromStdString(problem));
      }
      lasers = std::make_unique<pychron::experiment::lab::Lasers>(*lab, **line);
      session = std::make_unique<pychron::experiment::lab::LabSession>(
          *lab, pychron::experiment::lab::SessionHardware{**line, spectrometer.get(), scan.get(), {}, lasers.get()},
          pychron::experiment::lab::SessionOptions{data_dir, {}, {}});
      for (const auto& note : lasers->notes()) {
        window.log_dock()->append_line(QStringLiteral("WARN [ui] laser: ") + QString::fromStdString(note));
      }
      for (const auto& problem : session->problems()) {
        window.log_dock()->append_line(QStringLiteral("WARN [ui] experiment: ") + QString::fromStdString(problem));
      }
      experiment_bridge = std::make_unique<pychron::ui::ExperimentBridge>(*session, (*line)->bus());
      window.set_experiment(experiment_bridge.get(), sim, cli->queue);
      // A laser window per extraction device; its emergency stop aborts the queue.
      std::vector<pychron::ui::LaserBridge*> bridges;
      bool lasers_simulated = !lasers->names().empty();
      for (const auto& name : lasers->names()) {
        laser_bridges.push_back(std::make_unique<pychron::ui::LaserBridge>(pychron::ui::LaserBridgeDeps{
            *lasers, *lab, name, [raw = session.get()] { raw->abort(); }}));
        bridges.push_back(laser_bridges.back().get());
        lasers_simulated = lasers_simulated && (*line)->sim() != nullptr && (*line)->sim()->chromium(name) != nullptr;
      }
      window.set_lasers(bridges, lasers_simulated, &lab->patterns, lab_dir / "patterns");
    } else {
      window.log_dock()->append_line(
          QStringLiteral("ERROR [ui] experiment unavailable: extraction line did not start"));
    }
    splash.finish_after(&window, std::chrono::milliseconds(1200));
    rc = QApplication::exec();
    window.set_lasers({}, false);             // the laser windows go before their bridges
    window.set_data(nullptr, nullptr);        // data windows go before the data source
    window.set_experiment(nullptr, false);    // the experiment window goes before its bridge
    window.set_spectrometer(nullptr, false);  // closes the spectrometer window before the bridge goes
  }
  // Teardown order (explicit; not the reverse of declaration):
  //   0. the laser windows (above) and their bridges (a motion being waited
  //      for is stopped); the experiment window (above), its bridge, then the
  //      session, which aborts and joins a running queue before anything it
  //      uses goes; then the lasers the two shared;
  //   1. the spectrometer window, then the main window (the block above);
  //   2. the bridge, whose executor finishes the command in flight; the scan
  //      stop the closing window asked for happens even if it was queued
  //      behind it;
  //   3. the scan service, which stops the acquisition if it is still running;
  //   4. the line: stop() halts the shared scheduler and waits for a poll
  //      already on a worker, so nothing is still using the spectrometer;
  //   5. the spectrometer;
  //   6. the beam registry, whose models refer to the line's clock (and read
  //      the line's simulated gas, through a handle that outlives it), before
  //      the line itself is destroyed on return.
  // When the line never started, step 4 does nothing: the scheduler never ran
  // and the spectrometer was never offered, so no poll or command has touched
  // it and the same order is safe.
  laser_bridges.clear();
  experiment_bridge.reset();
  session.reset();
  lasers.reset();
  spectrometer_bridge.reset();
  scan.reset();
  (*line)->stop();
  spectrometer.reset();
  pychron::sim::BeamModelRegistry::global().clear();
  return rc;
}
