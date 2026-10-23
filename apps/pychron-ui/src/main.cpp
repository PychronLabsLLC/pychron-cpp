// pychron-ui: M1 status/control window.
//
//   pychron-ui [--install <name> | --setup | --examples | extraction_line.toml [canvas.toml]]
//              [--sim] [--spectrometer <file>] [--lab <dir>] [--data <dir>]
//              [--queue <file>] [--sim-speed <x>] [--db <url>] [--sim-save]
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
// default <lab>/data). --queue opens a queue there: a relative name not found
// as given is looked up in <lab>/queues/, then <lab>/. --sim-speed (with --sim,
// or an install set up for simulation) puts the whole app on simulated time
// running that many times faster than real time, from the real time of day.
// It takes a number: unlimited speed (elctl's --sim-speed max) would finish a
// queue before the window painted.
//
// View > Data browses the records under the data directory
// (<data>/records) and plots them; with --db, or on an install that has a
// database, it browses that DVC store instead ("postgresql://user:pw@host/db"
// or "sqlite:/path/to/file.db"; the schema must be current, it is never
// migrated from here), a queue's runs are saved to it, and the data window
// shows each run when it ends. A simulated session's runs are saved to it
// only when it is the install's own file on this computer, or with
// --sim-save; otherwise they are record files, and the log says so once. An install's database that cannot be opened
// does not stop the program: the data window says why, and no queue starts.
// Figure presets live in the user's config directory, with lab presets under
// <lab>/figures.
//
// File > Preferences… sets the font sizes, the data browser page size and the
// spectrometer's large-move threshold; they are kept in the user's settings.

#include <array>
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
#include <QTimer>

#include "brand.hpp"
#include "command_line.hpp"
#include "data_main_window.hpp"
#include "data_source_page.hpp"
#include "database_choice.hpp"
#include "experiment_bridge.hpp"
#include "installations_dialog.hpp"
#include "laser_bridge.hpp"
#include "laser_window.hpp"
#include "main_window.hpp"
#include "menu_hub.hpp"
#include "no_database_persister.hpp"
#include "pattern_maker_window.hpp"
#include "preferences.hpp"
#include "pychron/setup/doctor.hpp"
#include "pychron/setup/installer.hpp"
#include "pychron/setup/site.hpp"
#include "setup_support.hpp"
#include "setup_wizard.hpp"
#include "pychron/core/log_dir.hpp"
#include "pychron/core/log_hub.hpp"
#include "pychron/core/process_log.hpp"
#include "process_log.hpp"
#include "qt_log_handler.hpp"
#include "pychron/core/virtual_clock.hpp"
#include "pychron/experiment/lab/executor_config.hpp"
#include "pychron/experiment/lab/lasers.hpp"
#include "pychron/experiment/lab/session.hpp"
#ifdef PYCHRON_EXPERIMENT_HAS_METRICS
#include "pychron/experiment/metrics/service.hpp"
#endif
#include "pychron/sim/sim_system.hpp"
#include "pychron/processing/record_source.hpp"
#ifdef PYCHRON_UI_HAS_STORE
// Qt's `signals` keyword macro would rewrite CollectionRoots::signals.
#pragma push_macro("signals")
#undef signals
#include "pychron/experiment/store/store_persister.hpp"
#include "pychron/processing/store_source.hpp"
#pragma pop_macro("signals")
#include "entry_actions.hpp"
#include "fit_actions.hpp"
#include "publish_actions.hpp"
#include "store_identifier_source.hpp"
#endif
#include "pychron/sim/spectrometer/beam_model.hpp"
#include "pychron/systems/extraction_line.hpp"
#include "pychron/systems/spectrometer/bringup.hpp"
#include "pychron/systems/spectrometer/data_dir.hpp"
#include "pychron/systems/spectrometer/scan_service.hpp"
#include "spectrometer_bridge.hpp"
#include "theme.hpp"
#include "update_checker.hpp"
#include "update_ui.hpp"
#include "pychron/core/clock.hpp"
#include "pychron/earthbank/http.hpp"
#include "pychron/update/update.hpp"

namespace {

namespace fs = std::filesystem;
namespace setup = pychron::setup;

int fatal(const std::string& what) {
  std::fprintf(stderr, "pychron-ui: %s\n", what.c_str());
  QMessageBox::critical(nullptr, QStringLiteral("pychron-ui"), QString::fromStdString(what));
  return 1;
}

// The update check of `window` (update check spec 2.3): its indicator and
// Help > Check for Updates…, then the automatic check if one is due. `now`
// is the window's wall clock; failed automatic checks go to `log`, if any.
// Null when this program's version is not one (nothing is checked).
std::unique_ptr<pychron::ui::UpdateChecker> start_update_check(QMainWindow& window, QSettings& settings,
                                                               pychron::ui::UpdateChecker::Now now,
                                                               const std::function<void(const QString&)>& log) {
  const auto running = pychron::update::parse_version(PYCHRON_VERSION);
  if (!running) {
    const QString why = QStringLiteral("no update check: ") + QString::fromStdString(running.error().what);
    if (log) {
      log(QStringLiteral("WARN [update] ") + why);
    } else {
      std::fprintf(stderr, "pychron-ui: %s\n", why.toUtf8().constData());
    }
    return nullptr;
  }
  auto checker = std::make_unique<pychron::ui::UpdateChecker>(settings, *running, std::move(now),
                                                              [] { return pychron::earthbank::make_http_client(); });
  checker->set_log(log);
  pychron::ui::install_update_check(&window, *checker);
  checker->check_on_startup();
  return checker;
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

// Preferences > Data source for install `name`: the install as the site file
// has it when the dialog opens, the store-common questions, and this build's
// openers. `session_database`: --db, which overrides the install for this run.
pychron::ui::DataSourceDepsFactory data_source_deps(std::string name, const setup::Resources& resources,
                                                    std::optional<std::string> session_database) {
  return [name = std::move(name), resources, session_database = std::move(session_database)]()
             -> std::optional<pychron::ui::DataSourcePageDeps> {
    const fs::path site_path = setup::default_site_path();
    auto site = setup::load_site(site_path);
    const auto* install = site ? site->find(name) : nullptr;
    if (install == nullptr) return std::nullopt;
    auto library = setup::ProfileLibrary::load(resources.profiles, resources.examples);
    const auto* store = library ? library->find("store-common") : nullptr;
    if (store == nullptr) return std::nullopt;
    pychron::ui::DataSourcePageDeps deps;
    deps.install = *install;
    deps.site_path = site_path;
    deps.questions = store->questions;
    deps.openers.open_database = pychron::ui::database_opener();
    deps.openers.seed_database = pychron::ui::database_seeder();
    deps.session_database = session_database;
    return deps;
  };
}

// Reopens on install `name` after its data source changed: as File >
// Installations switches, once the dialog that asked has returned. The
// window's own close guards (unsaved queue edits) still apply.
std::function<void()> reopen_on(QWidget* window, std::string name) {
  return [window, name = std::move(name)] {
    QTimer::singleShot(0, window, [window, name] {
      if (!window->close()) return;
      if (!pychron::ui::start_install(name)) {
        QMessageBox::critical(nullptr, QStringLiteral("pychron-ui"), QStringLiteral("could not start pychron-ui"));
      }
    });
  };
}

// A data-reduction install: the data browser on its database, nothing else.
int run_data_reduction(const setup::SiteInstall& install, const pychron::ui::CommandLine& cli,
                       const setup::Resources& resources) {
  pychron::processing::RecordDirectorySource records(install.path(install.data.empty() ? "data" : install.data) /
                                                     "records");
  std::unique_ptr<pychron::processing::IAnalysisSource> store_source;
  const auto chosen = pychron::ui::choose_database(cli, &install);
  if (!chosen) return fatal(chosen.error().what);
  const std::string url = chosen->value_or(std::string{});
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
  pychron::ui::MenuHub::instance().set_experiment_shown(false);  // this install runs no experiment
  pychron::processing::PresetStore presets(
      fs::path(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation).toStdString()) / "presets",
      install.root / "figures");
  pychron::ui::DataMainWindow window(source, presets, QString::fromStdString(install.name));
  window.set_installations_handler(installations_handler(&window, resources, install.name));
  window.set_data_source(data_source_deps(install.name, resources, cli.db), reopen_on(&window, install.name));
#ifdef PYCHRON_UI_HAS_STORE
  // Entry: samples and packages in the install's database; Fit: the flux of a
  // level. Both parented to the window, Entry made first (fit_actions.hpp).
  if (!url.empty()) {
    auto* entry = new pychron::ui::EntryActions(&window, url);
    new pychron::ui::FitActions(&window, url, *entry, presets,
                                [&window](const QString& uuid) { window.workspace()->open_recall(uuid); });
    // View > Publish…: releases of this database, made of the browser's selection.
    auto* publish = new pychron::ui::PublishActions(&window, url, [&window] {
      const auto* browser = window.workspace()->existing_browser();
      return browser != nullptr ? browser->selection_uuids() : QStringList();
    });
    window.workspace()->set_publish_handler([publish](const QStringList& ids) { publish->open_dialog_new(ids); });
  }
#endif
  window.apply_preferences(pychron::ui::load_preferences(QSettings()));
  window.resize(1200, 800);
  window.show();
  // No line here, so no line clock: the computer's. Made after the window,
  // so the checker goes first; a failed automatic check is not shown.
  QSettings update_settings;
  const pychron::SteadyClock wall;
  const auto updates = start_update_check(window, update_settings, [&wall] { return wall.wall_now(); }, {});
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
      // cppcheck-suppress ignoredReturnValue ; no console after all: nowhere to say so
      std::freopen("CONOUT$", "w", stdout);
      // cppcheck-suppress ignoredReturnValue ; as above
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
  // An instrument install fills in what the command line leaves out.
  const bool sim = cli->sim || (install && install->simulation);
  // Not a usage error until here: only now is it known whether the install simulates.
  if (cli->sim_speed > 0 && !sim) {
    std::fprintf(stderr, "pychron-ui: --sim-speed needs --sim\n");
    return 2;
  }
  // The process log hub, before anything that can say something: on the
  // simulated clock when there is one (made here, so the hub is on it from its
  // first record), else the computer's. Declared before the line and
  // everything else that is given the clock or the hub, so they are the last
  // to go; the guard clears the process hub first of all.
  const pychron::SteadyClock steady_clock;
  // Simulated time: a VirtualClock paced to the speed asked for, starting at
  // the real time of day. The line's threads take part in it; this one (the
  // UI's) does not, so time never waits for the event loop.
  std::unique_ptr<pychron::VirtualClock> sim_clock;
  const bool reduction = install && install->kind == "data_reduction";
  if (cli->sim_speed > 0 && !reduction) {
    pychron::VirtualClock::Options clock_options;
    clock_options.speed = cli->sim_speed;
    clock_options.epoch = std::chrono::system_clock::now();
    // The hub is cleared before this clock goes, so a stall report is also in the log.
    clock_options.on_stall = [](const std::string& report) {
      std::fprintf(stderr, "pychron-ui: %s\n", report.c_str());
      pychron::process_logger("clock").warn(report);
    };
    sim_clock = std::make_unique<pychron::VirtualClock>(std::move(clock_options));
  }
  const pychron::Clock& process_clock = sim_clock ? static_cast<const pychron::Clock&>(*sim_clock) : steady_clock;
  std::optional<fs::path> log_line_config;
  if (install) {
    if (!install->line.empty()) log_line_config = install->path(install->line);
  } else if (cli->files.empty()) {
    log_line_config = examples / "extraction_line.toml";
  } else {
    log_line_config = cli->files[0];
  }
  std::string clock_name = "steady";
  if (sim_clock) {
    std::array<char, 32> speed{};
    std::snprintf(speed.data(), speed.size(), "%g", cli->sim_speed);
    clock_name = std::string("virtual x") + speed.data();
  }
  const pychron::ui::ProcessLog process_log = pychron::ui::open_process_log(
      install, log_line_config, process_clock, sim, pychron::host_os(), pychron::process_env(), clock_name);
  const std::shared_ptr<pychron::LogHub> hub = process_log.hub;
  pychron::set_process_log_hub(hub);
  struct ProcessHubGuard {
    ~ProcessHubGuard() { pychron::set_process_log_hub(nullptr); }
  } const hub_guard;
  std::optional<pychron::ui::QtLogHandler> qt_log;
  if (hub) qt_log.emplace(*hub);

  if (install && install->kind == "data_reduction") return run_data_reduction(*install, *cli, resources);

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
  options.log_hub = hub;
  if (sim_clock) options.clock = sim_clock.get();
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

  const fs::path lab_dir = cli->lab ? *cli->lab : install ? install->root : system_file.parent_path();
  // How the links recover (executor.toml [links]) is wanted before they are
  // made; the lab, which reports a file that does not load, comes later.
  const auto executor_config = pychron::experiment::lab::ExecutorConfig::load(lab_dir / "executor.toml");
  const pychron::LinkPolicy link_defaults =
      executor_config ? executor_config->link_defaults() : pychron::LinkPolicy{};
  options.link_defaults = link_defaults;

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
              .sim_beam_from_table = simulation,
              .require_sim = sim,
              .line_sim = (*line)->sim(),
              .fed = &fed,
              .link_defaults = link_defaults});
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

  // Whose runs are a simulator's: marked in their records, and kept out of
  // shared databases (one rule for every program).
  const bool simulated_session = pychron::spectrometer::simulated_session(
      sim, spectrometer ? std::optional<bool>(simulation) : std::nullopt, (*line)->config());

  std::unique_ptr<pychron::experiment::lab::Lab> lab;
  // The lab's lasers are shared by the session (queues) and the laser
  // windows (an operator): one drives at a time.
  std::unique_ptr<pychron::experiment::lab::Lasers> lasers;
#ifdef PYCHRON_UI_HAS_STORE
  // Where a queue's runs go when there is a database: the acquisition's own
  // store, or, when the database could not be opened, a persister that refuses
  // every queue with the reason. The session saves through one of them, so
  // they are declared before it and go after it: the teardown below resets
  // the session by hand (joining a running queue), these on return.
  std::unique_ptr<pychron::experiment::store::Acquisition> acquisition;
  std::unique_ptr<pychron::ui::NoDatabasePersister> no_database;
#endif
  std::unique_ptr<pychron::experiment::lab::LabSession> session;
  std::unique_ptr<pychron::ui::ExperimentBridge> experiment_bridge;
  std::vector<std::unique_ptr<pychron::ui::LaserBridge>> laser_bridges;

  // Data browsing: the records the experiment writes, and figure presets.
  const fs::path data_dir = cli->data            ? *cli->data
                            : install && !install->data.empty() ? install->path(install->data)
                                                                : lab_dir / "data";
  pychron::processing::RecordDirectorySource records(data_dir / "records");
  std::unique_ptr<pychron::processing::IAnalysisSource> store_source;
#ifdef PYCHRON_UI_HAS_STORE
  // One database for the data window, Entry, Fit, the run factory and the
  // runs a queue saves: --db, else the install's. `database` is its url once
  // the data window's source has opened it; `database_error` is why there is
  // none to use although one was named. --db that does not open is a usage
  // error; an install's is not, since the line can still be worked.
  std::string database;
  std::optional<std::string> database_error;
  if (const auto chosen = pychron::ui::choose_database(*cli, install ? &*install : nullptr); !chosen) {
    database_error = chosen.error().what;
  } else if (chosen->has_value()) {
    splash.status(QStringLiteral("Opening the DVC store"));
    auto opened = pychron::processing::StoreSource::open(pychron::persistence::StoreConfig{**chosen, false});
    if (opened) {
      store_source = std::move(*opened);
      database = **chosen;
    } else if (cli->db) {
      std::fprintf(stderr, "pychron-ui: --db: %s\n", pychron::to_string(opened.error()).c_str());
      return 2;
    } else {
      database_error = opened.error().what;
    }
  }
  if (cli->sim_save && database.empty() && !database_error) {
    std::fprintf(stderr, "pychron-ui: --sim-save: there is no database to save to (--db, or an install that has one)\n");
    return 2;
  }
  // Said in the log once the window is up: a simulated session that does not
  // save to the database (it goes on reading it) writes record files.
  std::string run_saving_note;
  if (!database.empty()) {
    const auto saving = pychron::ui::choose_run_saving(*cli, database, simulated_session, data_dir);
    if (!saving.to_database) {
      run_saving_note = saving.note;
    } else if (auto opened = pychron::experiment::store::open_acquisition(database, data_dir, PYCHRON_VERSION)) {
      acquisition = std::move(*opened);
    } else {
      no_database = std::make_unique<pychron::ui::NoDatabasePersister>(opened.error().what);
    }
  } else if (database_error) {
    no_database = std::make_unique<pychron::ui::NoDatabasePersister>(*database_error);
  }
#else
  if (cli->db) {
    std::fprintf(stderr, "pychron-ui: --db: built without the DVC store (PYCHRON_PERSISTENCE=OFF or no Qt Sql)\n");
    return 2;
  }
  if (cli->sim_save) {
    std::fprintf(stderr, "pychron-ui: --sim-save: there is no database to save to (built without the DVC store)\n");
    return 2;
  }
  // The install has a database and this build cannot write to one: said here
  // and in the log, not left for the operator to find out.
  std::string no_store_warning;
  if (install && install->kind == "instrument" && !install->database.empty()) {
    no_store_warning = "built without the DVC store: runs are written to " + (data_dir / "records").string() +
                       ", not to the installation's database";
    std::fprintf(stderr, "pychron-ui: warning: %s\n", no_store_warning.c_str());
  }
#endif
  pychron::processing::IAnalysisSource& data_source =
      store_source ? *store_source : static_cast<pychron::processing::IAnalysisSource&>(records);
  pychron::processing::PresetStore presets(
      std::filesystem::path(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation).toStdString()) / "presets",
      lab_dir / "figures");

  // `[metrics]`: the endpoint the lab's Prometheus scrapes (docs/observability.md).
  // Built before the line starts, so the start-up Snapshot reaches it too.
#ifdef PYCHRON_EXPERIMENT_HAS_METRICS
  std::unique_ptr<pychron::experiment::metrics::MetricsService> metrics;
#endif

  int rc = 0;
  {
    // The window (and its CoreBridge) subscribes before start() so the
    // start-up Snapshot paints the canvas before the first scan.
    pychron::ui::MainWindow window(**line, std::make_unique<QSettings>());
    window.apply_preferences(pychron::ui::load_preferences(QSettings()));
    // Preferences keeps the line's logging and metrics beside this file. Not
    // for the examples that come with the application: they are part of it
    // (inside the bundle on macOS), and nothing is written there.
    if (install || !files.empty()) window.set_line_config_file(system_file);
    if (install) window.set_install_root(install->root);
    window.set_installations_handler(installations_handler(&window, resources, install ? install->name : std::string{}));
    // Not for the examples that come with the application: no install to change.
    if (install) window.set_data_source(data_source_deps(install->name, resources, cli->db), reopen_on(&window, install->name));

    // Runtime level changes go to the process hub; without one (creation
    // failed) the "Set logger level..." action stays hidden.
    if (hub) {
      window.log_dock()->set_level_callback(pychron::ui::hub_level_callback(hub));
      hub->flush();  // records from load() are on disk before history is read
      if (const auto path = hub->file_path(); !path.empty()) {
        window.log_dock()->load_history(path, 1000, hub->dock_level());
      }
    }
    // What logging had to say before there was a window (a logging.toml that
    // does not load, no folder for the file): once, in the panel.
    for (const auto& [level, text] : process_log.notices) {
      window.log_dock()->append_log(pychron::Log{level, "logging", text, {}});
    }
    // The experiment window shows the recent references only when what the
    // data window reads is where the session saves its runs: the store, with
    // the session saving to it (not the records folder, not a database that
    // could not be opened, not a simulated session that writes files).
#ifdef PYCHRON_UI_HAS_STORE
    const bool session_store = store_source != nullptr && acquisition != nullptr;
#else
    const bool session_store = false;
#endif
    window.set_data(&data_source, &presets, session_store);
#ifdef PYCHRON_UI_HAS_STORE
    // The Entry and Fit menus, parented to the window, Entry made first (fit_actions.hpp).
    if (!database.empty()) {
      auto* entry = new pychron::ui::EntryActions(&window, database);
      // The run factory picks identifiers from the same store, through Entry's bridge.
      window.set_identifier_source(new pychron::ui::StoreIdentifierSource(*entry));
      new pychron::ui::FitActions(&window, database, *entry, presets,
                                  [&window](const QString& uuid) { window.open_recall(uuid); });
      // View > Publish…: releases of this database, made of the Data browser's selection.
      auto* publish = new pychron::ui::PublishActions(&window, database, [&window] {
        const auto* browser = window.data_window();
        return browser != nullptr ? browser->selection_uuids() : QStringList();
      });
      window.set_publish_handler([publish](const QStringList& ids) { publish->open_dialog_new(ids); });
    }
    if (database_error) {
      // The data window shows the records folder, and says so and what is missing.
      const QString why =
          QStringLiteral("The database could not be opened: ") + QString::fromStdString(*database_error);
      window.set_data_unavailable(why + QStringLiteral("\nShowing the records folder %1.")
                                            .arg(QString::fromStdString((data_dir / "records").string())));
      window.log_dock()->append_line(QStringLiteral("ERROR [ui] ") + why);
    } else if (no_database) {
      // It can be read, and not written to: said here, and again to whoever starts a queue.
      window.log_dock()->append_line(QStringLiteral("ERROR [ui] ") + QString::fromStdString(no_database->why()));
    }
    if (!run_saving_note.empty()) {
      window.log_dock()->append_line(QStringLiteral("INFO [ui] ") + QString::fromStdString(run_saving_note));
    }
#else
    if (!no_store_warning.empty()) {
      window.log_dock()->append_line(QStringLiteral("WARN [ui] ") + QString::fromStdString(no_store_warning));
    }
#endif
    window.show();
    if (spectrometer_error) {
      window.log_dock()->append_line(QStringLiteral("ERROR [ui] spectrometer not loaded: ") +
                                     QString::fromStdString(*spectrometer_error));
    }
    // After the window, so the checker goes first. The line's wall clock, so
    // a simulated session's last check is in simulated time.
    QSettings update_settings;
    const auto updates = start_update_check(
        window, update_settings,
        [&window] {
          return std::chrono::system_clock::time_point(
              std::chrono::milliseconds(window.bridge().wall_now().toMSecsSinceEpoch()));
        },
        [&window](const QString& text) { window.log_dock()->append_line(text); });
#ifdef PYCHRON_EXPERIMENT_HAS_METRICS
    metrics = pychron::experiment::metrics::MetricsService::start(
        (*line)->config().metrics, (*line)->bus(), (*line)->scheduler(), (*line)->clock(), (*line)->log_hub(),
        PYCHRON_VERSION);
    if (metrics) {
      // Each transport's reconnects and repeats, read from its link at each
      // scrape: the line's, and the spectrometer's (both outlive `metrics`).
      std::vector<pychron::Link*> links = (*line)->links();
      if (spectrometer) {
        const std::vector<pychron::Link*> more = spectrometer->links();
        links.insert(links.end(), more.begin(), more.end());
      }
      metrics->watch_links(links);
    }
    // For the Metrics page of File > Preferences.
    if (!metrics) {
      window.set_metrics_status(QStringLiteral("Off"));
    } else if (metrics->listening()) {
      window.set_metrics_status(QStringLiteral("Listening on %1:%2")
                                    .arg(QString::fromStdString((*line)->config().metrics.bind))
                                    .arg(metrics->port()));
    } else {
      // The error names its subject for the log ("metrics: cannot bind..."); here the page already does.
      QString why = QString::fromStdString(metrics->error());
      if (why.startsWith(QStringLiteral("metrics: "))) why.remove(0, 9);
      window.set_metrics_status(QStringLiteral("Not listening: %1").arg(why));
    }
#else
    window.set_metrics_status(QStringLiteral("This build has no metrics"));
    if ((*line)->config().metrics.enabled) {
      window.log_dock()->append_line(
          QStringLiteral("WARN [ui] metrics: this build has no metrics (PYCHRON_METRICS=OFF)"));
    }
#endif
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
        window.set_rise_rate_file(data_dir / "rise_rates.csv");
        window.set_peak_center_file(lab_dir / "peak_center.toml");
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
      pychron::experiment::lab::SessionOptions session_options{data_dir, {}, {}};
#ifdef PYCHRON_UI_HAS_STORE
      if (acquisition) {
        session_options.persister = &acquisition->persister();
      } else if (no_database) {
        session_options.persister = no_database.get();
      }
#endif
      const bool saves_to_database = session_options.persister != nullptr;
      session = std::make_unique<pychron::experiment::lab::LabSession>(
          *lab,
          pychron::experiment::lab::SessionHardware{**line, spectrometer.get(), scan.get(), {}, lasers.get(),
                                                    simulated_session},
          std::move(session_options));
      for (const auto& note : lasers->notes()) {
        window.log_dock()->append_line(QStringLiteral("WARN [ui] laser: ") + QString::fromStdString(note));
      }
      for (const auto& problem : session->problems()) {
        window.log_dock()->append_line(QStringLiteral("WARN [ui] experiment: ") + QString::fromStdString(problem));
      }
      experiment_bridge = std::make_unique<pychron::ui::ExperimentBridge>(*session, (*line)->bus());
      experiment_bridge->set_saves_to_database(saves_to_database);
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
  //      uses goes (its persister among them: the acquisition store, declared
  //      before the session, goes on return); then the lasers the two shared;
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
#ifdef PYCHRON_EXPERIMENT_HAS_METRICS
  metrics.reset();  // its heartbeat is a job of the line's scheduler: before the line goes
#endif
  spectrometer.reset();
  pychron::sim::BeamModelRegistry::global().clear();
  return rc;
}
