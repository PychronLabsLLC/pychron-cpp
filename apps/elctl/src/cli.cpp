#include "cli.hpp"

#include <chrono>
#include <fstream>
#include <iomanip>
#include <istream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ostream>
#include <sstream>
#include <thread>

#include "duration.hpp"
#include "entry.hpp"
#include "exp.hpp"
#include "laser.hpp"
#include "import.hpp"
#include "pychron/setup/installer.hpp"
#include "setup.hpp"
#include "line.hpp"
#include "pychron/core/config/loader.hpp"
#include "pychron/devices/capabilities.hpp"
#include "pychron/devices/channel_gauge.hpp"
#include "pychron/devices/driver_registry.hpp"
#include "pychron/experiment/conditionals/conditional.hpp"
#include "pychron/experiment/conditionals/validate.hpp"
#include "pychron/systems/spectrometer/data_dir.hpp"
#include "pychron/systems/canvas/cross_validate.hpp"
#include "pychron/systems/canvas/loader.hpp"
#include "trace_settings.hpp"

namespace elctl {

namespace fs = std::filesystem;
using namespace pychron;

namespace {

constexpr const char* kUsageLine = "usage: elctl [-c <extraction_line.toml> | --install <name>] [--sim] <command> [args...]\n";

constexpr const char* kUsageText =
    "usage: elctl [-c <extraction_line.toml> | --install <name>] [--sim] <command> [args...]\n"
    "       elctl --version\n"
    "\n"
    "Offline:\n"
    "  validate [file]             check a system config; print every error\n"
    "  canvas-check [canvas.toml]  check a canvas and cross-check it against the config\n"
    "  list-drivers                driver kinds and the keys each one reads\n"
    "  list                        configured valves, manual valves, switches and gauges\n"
    "  conditionals-check <file> [--spectrometer <spectrometer.toml>]\n"
    "                              parse conditionals, print their canonical form, and check\n"
    "                              names against the config's gauges and the spectrometer\n"
    "\n"
    "Experiments:\n"
    "  exp validate <experiment.toml> [--lab <dir>] [--spectrometer <file>]\n"
    "                              check a queue against the lab's plans, scripts and conditionals\n"
    "  exp run <experiment.toml> [--lab <dir>] [--data <dir>] [--spectrometer <file>]\n"
    "          [--from <row> | --resume] [--dry-run] [--sim-speed <x>]\n"
    "                              run a queue; Ctrl-C stops after the run, again cancels, again aborts\n"
    "  exp notify [--lab <dir>]    send a test message on each channel in <lab>/notifications.toml\n"
    "  laser trays                 tray maps and their stage calibrations\n"
    "  laser calibrate <device> <tray> point <hole>|center|right|show|clear [--x X --y Y]\n"
    "  laser goto <device> <tray> <hole>   move the stage to a hole and report the miss\n"
    "  laser autocenter <device> <tray> <hole>   move to a hole and center it with the camera\n"
    "  laser corrections <device> <tray> [clear [<hole>]]   where autocenter found holes\n"
    "  laser look <device> [--tray <tray>]   what the camera's finder sees; moves nothing\n"
    "  laser patterns              the lab's laser patterns\n"
    "  laser pattern <device> <name> [--dry-run]   run a pattern (laser not fired), or print its points\n"
    "\n"
    "Setup (no line needed):\n"
    "  init --list                 the setup profiles (argus, helix, ngx, data-reduction)\n"
    "  init <profile> [--root DIR] [--name NAME] [--answers FILE] [--set id=value]... [--yes]\n"
    "                              install a profile; asks its questions unless --yes or --answers\n"
    "  init --reconfigure [--set id=value]... [--yes]\n"
    "                              re-render an install with new answers; edited files are kept\n"
    "  doctor [--strict] [--probe] check an install (the default, or --install NAME)\n"
    "  import-line <folder> [--out DIR] [--force]\n"
    "                              convert a legacy Pychron setupfiles extraction line and canvas\n"
    "  --install NAME              before a command: use that install's files (see doctor)\n"
    "\n"
    "Legacy data (elctl import help lists every option):\n"
    "  import add --db <url> --kind legacy_db|meta_repo|project_repo --source <path|url> --tz <zone>\n"
    "                              register a legacy database dump or repository\n"
    "  import run --db <url> [--source <id|name> | --all] [--replay] [--dry-run]\n"
    "                              import, resuming where the last run stopped\n"
    "  import status|conflicts|verify --db <url>\n"
    "                              progress, what could not be imported, and whether to trust it\n"
    "\n"
    "Sample and package entry (elctl entry help lists every option):\n"
    "  entry samples import <file.csv> --db <url> [--dry-run]\n"
    "                              add samples with their PIs, projects and materials\n"
    "  entry package add|show|set-kind <name> --db <url>\n"
    "                              packages (irradiations) and their levels\n"
    "  entry positions import <package> <file.csv> --db <url>\n"
    "                              put samples in a package's positions\n"
    "  entry identifiers generate <package> --db <url> [--dry-run]\n"
    "                              number the positions with the next identifiers\n"
    "\n"
    "Hardware (or simulation, for kind = \"sim\" transports or --sim):\n"
    "  probe                       open every transport, ping every driver, print health\n"
    "  state                       read back every switch and gauge\n"
    "  open <valve>                actuate, enforcing locks and interlocks\n"
    "  close <valve>\n"
    "  read <gauge>                one pressure reading\n"
    "  scan --for <dur> [--interval <dur>]\n"
    "                              stream gauge samples and alarms (dur: 500ms, 10s, 2m)\n"
    "  trace [on|off [transport...]]\n"
    "                              record transport traffic to <config dir>/traces for replay\n"
    "  sim                         interactive session against simulated hardware\n"
    "\n"
    "Options:\n"
    "  -c, --config <file>         system config (default: extraction_line.toml)\n"
    "  --sim                       run every transport as kind = \"sim\"\n";

constexpr const char* kActor = "elctl";

struct Globals {
  fs::path config = "extraction_line.toml";
  bool config_given = false;
  bool sim = false;
  std::optional<std::string> install;  // --install NAME (site config)
  ExpGlobals exp;                      // defaults --install fills in
};

std::string_view units_name(config::PressureUnits u) {
  switch (u) {
    case config::PressureUnits::Torr:
      return "torr";
    case config::PressureUnits::Mbar:
      return "mbar";
    case config::PressureUnits::Pa:
      return "pa";
  }
  return "?";
}

std::string_view kind_name(systems::SwitchKind k) {
  switch (k) {
    case systems::SwitchKind::Valve:
      return "valve";
    case systems::SwitchKind::ManualValve:
      return "manual";
    case systems::SwitchKind::Switch:
      return "switch";
  }
  return "?";
}

std::string_view state_name(ValveState s) {
  switch (s) {
    case ValveState::Open:
      return "open";
    case ValveState::Closed:
      return "closed";
    case ValveState::Unknown:
      return "unknown";
  }
  return "?";
}

std::string pressure(double value, config::PressureUnits units) {
  std::ostringstream s;
  s << std::scientific << std::setprecision(3) << value << ' ' << units_name(units);
  return s.str();
}

std::string join(const std::vector<std::string>& items, std::string_view sep = ", ") {
  std::string out;
  for (const auto& i : items) {
    if (!out.empty()) out += sep;
    out += i;
  }
  return out;
}

// One CLI invocation, or one `sim` REPL. The line is built on first use and
// kept, so REPL commands share simulated hardware state.
class Session {
 public:
  Session(Globals globals, Io io) : g_(std::move(globals)), io_(io) {}

  int execute(const std::vector<std::string>& argv) {
    const std::string& cmd = argv.front();
    const std::vector<std::string> args(argv.begin() + 1, argv.end());
    if (cmd == "help") return help();
    if (cmd == "validate") return validate(args);
    if (cmd == "canvas-check") return canvas_check(args);
    if (cmd == "conditionals-check") return conditionals_check(args);
    if (cmd == "exp") {
      ExpGlobals e = g_.exp;
      e.config = g_.config;
      e.sim = g_.sim;
      return exp_command(args, e, io_);
    }
    if (cmd == "laser") {
      ExpGlobals e = g_.exp;
      e.config = g_.config;
      e.sim = g_.sim;
      return laser_command(args, e, io_);
    }
    if (cmd == "import") return import_command(args, io_);
    if (cmd == "entry") return entry_command(args, io_);
    if (cmd == "list-drivers") return list_drivers();
    if (cmd == "list") return list();
    if (cmd == "probe") return probe();
    if (cmd == "state") return state();
    if (cmd == "open") return actuate(args, systems::SwitchOp::Open);
    if (cmd == "close") return actuate(args, systems::SwitchOp::Close);
    if (cmd == "read") return read(args);
    if (cmd == "scan") return scan(args);
    if (cmd == "trace") return trace(args);
    if (cmd == "sim") return repl();
    return usage("unknown command '" + cmd + "'");
  }

 private:
  int usage(const std::string& message) {
    io_.err << "elctl: " << message << '\n' << kUsageLine << "run 'elctl help' for commands\n";
    return kUsage;
  }

  int failed(const Error& error) {
    io_.err << "error: " << to_string(error) << '\n';
    return kFailed;
  }

  int help() {
    io_.out << kUsageText;
    return kOk;
  }

  // --- offline --------------------------------------------------------------

  Result<config::SystemConfig> load_config(const fs::path& path) {
    auto report = config::load_report(path);
    if (report.ok()) return std::move(*report.config);
    for (const auto& d : report.diagnostics) io_.err << to_string(d) << '\n';
    return fail(ErrorKind::Config, std::to_string(report.diagnostics.size()) + " error(s) in " + path.string());
  }

  int validate(const std::vector<std::string>& args) {
    if (args.size() > 1) return usage("validate takes at most one file");
    const fs::path path = args.empty() ? g_.config : fs::path(args[0]);
    auto cfg = load_config(path);
    if (!cfg) return failed(cfg.error());

    int problems = 0;
    for (const auto& [name, driver] : cfg->drivers) {
      if (auto ok = DriverRegistry::global().validate(driver.kind, driver.options); !ok) {
        io_.err << driver.path << ": " << ok.error().what << '\n';
        ++problems;
      }
    }
    if (problems > 0) {
      io_.err << "error: " << problems << " driver error(s) in " << path.string() << '\n';
      return kFailed;
    }
    io_.out << "ok: " << path.string() << '\n';
    return kOk;
  }

  int canvas_check(const std::vector<std::string>& args) {
    if (args.size() > 1) return usage("canvas-check takes at most one file");
    const fs::path path = args.empty() ? g_.config.parent_path() / "canvas.toml" : fs::path(args[0]);
    auto report = canvas::load_canvas_report(path);
    if (!report.ok()) {
      for (const auto& d : report.diagnostics) io_.err << to_string(d) << '\n';
      io_.err << "error: " << report.diagnostics.size() << " error(s) in " << path.string() << '\n';
      return kFailed;
    }
    auto cfg = load_config(g_.config);
    if (!cfg) return failed(cfg.error());

    auto cross = canvas::cross_validate(*report.canvas, *cfg);
    for (const auto& d : cross.errors) io_.err << to_string(d) << '\n';
    for (const auto& d : cross.warnings) io_.out << "warning: " << to_string(d) << '\n';
    if (!cross.ok()) {
      io_.err << "error: " << cross.errors.size() << " error(s) in " << path.string() << '\n';
      return kFailed;
    }
    io_.out << "ok: " << path.string() << " (" << cross.warnings.size() << " warning(s))\n";
    return kOk;
  }

  int conditionals_check(const std::vector<std::string>& args) {
    std::optional<fs::path> file, spectrometer;
    for (std::size_t i = 0; i < args.size(); ++i) {
      if (args[i] == "--spectrometer" && i + 1 < args.size()) {
        spectrometer = args[++i];
      } else if (!file && !args[i].starts_with("-")) {
        file = args[i];
      } else {
        return usage("conditionals-check <file> [--spectrometer <spectrometer.toml>]");
      }
    }
    if (!file) return usage("conditionals-check needs a file");
    std::ifstream in(*file);
    if (!in) return failed(Error{ErrorKind::Io, "cannot read " + file->string(), {}});
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    auto set = experiment::parse_conditionals(text, file->string());
    if (!set) return failed(set.error());

    // Names to check against: the config's gauges (when it loads) and the
    // spectrometer's detectors and table isotopes.
    experiment::MetricCatalog catalog;
    std::error_code ec;
    if (fs::exists(g_.config, ec)) {
      if (auto report = config::load_report(g_.config); report.ok())
        for (const auto& gauge : report.config->gauges) catalog.gauges.insert(gauge.name);
    }
    if (spectrometer) {
      auto data = spectrometer::cfg::load_spectrometer(*spectrometer);
      if (!data) return failed(data.error());
      for (const auto& d : data->config.detectors) catalog.detectors.insert(d.name);
      for (const auto& [name, table] : data->tables)
        for (const auto& point : table.points) catalog.isotopes.insert(point.isotope);
    }

    for (const auto& c : set->items) {
      io_.out << experiment::to_string(c.kind) << ' ' << c.name << ": " << c.effective_check() << "  [start="
              << c.start << " frequency=" << c.frequency << " ntrips=" << c.ntrips;
      if (const auto a = experiment::to_string(c.action); !a.empty()) io_.out << " action=" << a;
      if (!c.analysis_types.empty()) io_.out << " analysis_types=" << join(c.analysis_types, ",");
      io_.out << "]\n";
    }
    int errors = 0;
    for (const auto& d : experiment::validate_conditionals(*set, catalog)) {
      (d.error ? io_.err : io_.out) << (d.error ? "error: " : "warning: ") << d.conditional << ": " << d.message
                                    << '\n';
      errors += d.error ? 1 : 0;
    }
    if (errors > 0) {
      io_.err << "error: " << errors << " error(s) in " << file->string() << '\n';
      return kFailed;
    }
    io_.out << "ok: " << file->string() << " (" << set->items.size() << " conditional(s))\n";
    return kOk;
  }

  int list_drivers() {
    for (const auto& schema : DriverRegistry::global().schemas()) io_.out << describe(schema) << '\n';
    return kOk;
  }

  int list() {
    auto cfg = load_config(g_.config);
    if (!cfg) return failed(cfg.error());
    for (const auto& v : cfg->valves) {
      io_.out << "valve   " << v.name << "  " << v.actuator << ':' << v.address;
      if (!v.interlocks.empty()) io_.out << "  interlocks=[" << join(v.interlocks) << ']';
      if (!v.positive_interlocks.empty()) io_.out << "  requires=[" << join(v.positive_interlocks) << ']';
      if (!v.description.empty()) io_.out << "  # " << v.description;
      io_.out << '\n';
    }
    for (const auto& m : cfg->manual_valves) {
      io_.out << "manual  " << m.name;
      if (!m.description.empty()) io_.out << "  # " << m.description;
      io_.out << '\n';
    }
    for (const auto& s : cfg->switches) {
      io_.out << "switch  " << s.name << "  " << s.actuator << ':' << s.address;
      if (!s.description.empty()) io_.out << "  # " << s.description;
      io_.out << '\n';
    }
    for (const auto& gc : cfg->gauges) {
      io_.out << "gauge   " << gc.name << "  " << gc.driver << " ch" << gc.channel << "  " << units_name(gc.units);
      if (gc.alarm_high) io_.out << "  alarm_high=" << *gc.alarm_high;
      if (gc.alarm_low) io_.out << "  alarm_low=" << *gc.alarm_low;
      io_.out << '\n';
    }
    return kOk;
  }

  // --- line -----------------------------------------------------------------

  Result<Line*> built() {
    if (line_) return line_.get();
    auto cfg = load_config(g_.config);
    if (!cfg) return fail(cfg.error());
    LineOptions options;
    options.force_sim = g_.sim;
    options.trace = load_trace_settings(g_.config);
    options.trace_dir = trace_dir(g_.config);
    auto line = Line::build(std::move(*cfg), options);
    if (!line) return fail(line.error());
    line_ = std::move(*line);
    return line_.get();
  }

  // Built, transports opened and switch states read back, so interlocks see
  // real hardware state instead of Unknown. Failures are warnings: a dead
  // gauge link must not stop valve work.
  Result<Line*> ready() {
    auto line = built();
    if (!line || opened_) return line;
    for (const auto& o : (*line)->open_all()) {
      if (!o.result) io_.err << "warning: transport " << o.transport << ": " << to_string(o.result.error()) << '\n';
    }
    if (auto r = (*line)->switches().refresh(); !r) io_.err << "warning: " << to_string(r.error()) << '\n';
    opened_ = true;
    return line;
  }

  int probe() {
    auto built_line = built();
    if (!built_line) return failed(built_line.error());
    Line& line = **built_line;
    bool ok = true;

    io_.out << "TRANSPORT\n";
    for (const auto& o : line.open_all()) {
      ok = ok && o.result.has_value();
      io_.out << "  " << std::left << std::setw(16) << o.transport
              << (o.result ? std::string(to_string(line.transport(o.transport)->health().state))
                           : "FAIL  " + to_string(o.result.error()))
              << '\n';
    }
    opened_ = true;

    io_.out << "DEVICE\n";
    for (const auto& [name, dc] : line.config().drivers) {
      auto pinged = ping(line, name);
      ok = ok && pinged.has_value();
      io_.out << "  " << std::left << std::setw(16) << name << std::setw(20) << dc.kind
              << (pinged ? "ok  " + *pinged : "FAIL  " + to_string(pinged.error())) << '\n';
    }
    return ok ? kOk : kFailed;
  }

  // Exercises every configured use of one driver; returns what it did.
  Result<std::string> ping(Line& line, const std::string& driver) {
    Device* device = line.device(driver);
    if (!device) return fail(ErrorKind::Config, "not built", driver);
    int ops = 0;
    if (auto* actuator = capability<IValveActuator>(*device)) {
      for (const auto& v : line.config().valves) {
        if (v.actuator != driver) continue;
        if (auto r = actuator->read(ValveAddress{v.address}); !r) return fail(r.error());
        ++ops;
      }
      for (const auto& s : line.config().switches) {
        if (s.actuator != driver) continue;
        if (auto r = actuator->read(ValveAddress{s.address}); !r) return fail(r.error());
        ++ops;
      }
    }
    for (const auto& gc : line.config().gauges) {
      if (gc.driver != driver) continue;
      auto p = line.read_gauge(gc.name);
      if (!p) return fail(p.error());
      ++ops;
    }
    if (ops == 0) return std::string("nothing configured to ping");
    return std::to_string(ops) + " read(s), health " + std::string(to_string(device->health().state));
  }

  int state() {
    auto line = ready();
    if (!line) return failed(line.error());
    int rc = kOk;
    if (auto r = (*line)->switches().refresh(); !r) {
      io_.err << "error: " << to_string(r.error()) << '\n';
      rc = kFailed;
    }
    for (const auto& info : (*line)->switches().list()) {
      io_.out << info.name << "  " << kind_name(info.kind) << "  " << state_name(info.state);
      if (info.locked) io_.out << "  locked";
      if (!info.owner.empty()) io_.out << "  owner=" << info.owner;
      io_.out << '\n';
    }
    for (const auto& gc : (*line)->config().gauges) {
      auto p = (*line)->read_gauge(gc.name);
      io_.out << gc.name << "  gauge  " << (p ? pressure(*p, gc.units) : "FAIL " + to_string(p.error())) << '\n';
      if (!p) rc = kFailed;
    }
    return rc;
  }

  int actuate(const std::vector<std::string>& args, systems::SwitchOp op) {
    if (args.size() != 1) return usage(std::string(op == systems::SwitchOp::Open ? "open" : "close") + " takes one name");
    auto line = ready();
    if (!line) return failed(line.error());
    if (auto r = (*line)->switches().actuate(args[0], op, kActor); !r) return failed(r.error());
    io_.out << args[0] << ' ' << (op == systems::SwitchOp::Open ? "open" : "closed") << '\n';
    return kOk;
  }

  int read(const std::vector<std::string>& args) {
    if (args.size() != 1) return usage("read takes one gauge name");
    auto line = ready();
    if (!line) return failed(line.error());
    const auto* gc = (*line)->gauge(args[0]);
    if (!gc) return failed(Error{ErrorKind::Config, "unknown gauge '" + args[0] + "'", {}});
    auto p = (*line)->read_gauge(args[0]);
    if (!p) return failed(p.error());
    io_.out << gc->name << "  " << pressure(*p, gc->units) << '\n';
    return kOk;
  }

  int scan(const std::vector<std::string>& args) {
    std::optional<Duration> duration;
    std::optional<Duration> interval;
    for (std::size_t i = 0; i < args.size(); i += 2) {
      const bool is_for = args[i] == "--for";
      if ((!is_for && args[i] != "--interval") || i + 1 >= args.size()) {
        return usage("scan --for <dur> [--interval <dur>]");
      }
      auto d = parse_duration(args[i + 1]);
      if (!d || *d <= Duration::zero()) return usage("scan: invalid duration '" + args[i + 1] + "'");
      (is_for ? duration : interval) = *d;
    }
    if (!duration) return usage("scan needs --for <dur>");

    auto line = ready();
    if (!line) return failed(line.error());
    Line& l = **line;
    if (!interval) interval = std::chrono::milliseconds(l.config().system.scan_interval_ms);

    std::map<std::string, config::PressureUnits> units;
    for (const auto& gc : l.config().gauges) units[gc.name] = gc.units;
    const TimePoint start = l.clock().now();
    std::mutex out_mutex;
    auto seconds = [&](TimePoint ts) {
      std::ostringstream s;
      s << std::fixed << std::setprecision(3) << std::chrono::duration<double>(ts - start).count() << 's';
      return s.str();
    };
    auto samples = l.bus().subscribe<PressureSample>([&](const PressureSample& s) {
      std::lock_guard lock(out_mutex);
      io_.out << seconds(s.ts) << "  " << s.gauge << "  " << pressure(s.value, units[s.gauge]) << '\n';
    });
    auto alarms = l.bus().subscribe<Alarm>([&](const Alarm& a) {
      std::lock_guard lock(out_mutex);
      io_.out << seconds(a.ts) << "  ALARM " << a.source << "  " << a.message << '\n';
    });

    if (auto r = l.start_scan(*interval); !r) return failed(r.error());
    std::this_thread::sleep_for(*duration);
    l.stop_scan();
    return kOk;
  }

  int trace(const std::vector<std::string>& args) {
    TraceSettings settings = load_trace_settings(g_.config);
    if (args.empty()) {
      if (!settings.enabled()) {
        io_.out << "trace: off\n";
      } else {
        io_.out << "trace: on ("
                << (settings.all ? std::string("all transports")
                                 : join({settings.transports.begin(), settings.transports.end()}))
                << ") -> " << trace_dir(g_.config).string() << '\n';
      }
      return kOk;
    }
    const std::string& mode = args[0];
    if (mode != "on" && mode != "off") return usage("trace on|off [transport...]");
    const std::vector<std::string> names(args.begin() + 1, args.end());

    auto cfg = load_config(g_.config);
    if (!cfg) return failed(cfg.error());
    for (const auto& n : names) {
      if (!cfg->transports.contains(n)) return failed(Error{ErrorKind::Config, "unknown transport '" + n + "'", {}});
    }

    if (mode == "on") {
      if (names.empty()) settings.all = true;
      settings.transports.insert(names.begin(), names.end());
    } else if (names.empty()) {
      settings = {};
    } else {
      for (const auto& n : names) settings.transports.erase(n);
    }
    if (auto r = save_trace_settings(g_.config, settings); !r) return failed(r.error());
    io_.out << "trace " << mode << (line_ ? " (takes effect on the next run)" : "") << '\n';
    return kOk;
  }

  // --- sim REPL -------------------------------------------------------------

  int repl() {
    if (in_repl_) return usage("already in a sim session");
    in_repl_ = true;
    g_.sim = true;
    io_.out << "elctl sim: every transport is simulated. 'help' lists commands, 'quit' exits.\n";
    std::string text;
    while (true) {
      io_.out << "sim> " << std::flush;
      if (!std::getline(io_.in, text)) break;
      std::istringstream words(text);
      std::vector<std::string> argv{std::istream_iterator<std::string>(words), std::istream_iterator<std::string>()};
      if (argv.empty() || argv.front().starts_with('#')) continue;
      if (argv.front() == "quit" || argv.front() == "exit") break;
      execute(argv);
    }
    io_.out << '\n';
    return kOk;
  }

  Globals g_;
  Io io_;
  std::unique_ptr<Line> line_;
  bool opened_ = false;
  bool in_repl_ = false;
};

}  // namespace

int run(const std::vector<std::string>& args, Io io) {
  Globals globals;
  std::size_t i = 0;
  for (; i < args.size(); ++i) {
    const std::string& a = args[i];
    if (a == "-c" || a == "--config") {
      if (i + 1 >= args.size()) {
        io.err << "elctl: " << a << " needs a file\n" << kUsageLine;
        return kUsage;
      }
      globals.config = args[++i];
      globals.config_given = true;
    } else if (a == "--install") {
      if (i + 1 >= args.size()) {
        io.err << "elctl: --install needs a name\n" << kUsageLine;
        return kUsage;
      }
      globals.install = args[++i];
    } else if (a == "--sim") {
      globals.sim = true;
    } else if (a == "-h" || a == "--help") {
      io.out << kUsageText;
      return kOk;
    } else if (a == "--version") {
      // Where the shipped profiles were found: checks an installed layout.
      const auto r = pychron::setup::find_resources();
      io.out << "elctl " << pychron::setup::version() << "\nprofiles: " << r.profiles.string()
             << "\nexamples: " << r.examples.string() << "\n";
      return kOk;
    } else {
      break;
    }
  }
  if (i >= args.size()) {
    io.err << kUsageText;
    return kUsage;
  }
  const std::string& command = args[i];
  const std::vector<std::string> rest(args.begin() + static_cast<std::ptrdiff_t>(i) + 1, args.end());
  // Setup commands need no line.
  if (command == "init") {
    std::vector<std::string> init_args = rest;
    if (globals.install) init_args.insert(init_args.end(), {"--install", *globals.install});
    return init_command(init_args, io);
  }
  if (command == "doctor") return doctor_command(rest, globals.install, io);
  if (command == "import-line") return import_line_command(rest, io);
  // --install NAME (or, with no -c, the default install): its files are the defaults.
  if (globals.install || !globals.config_given) {
    auto install = resolve_install(globals.install);
    if (install) {
      if (!globals.config_given) globals.config = install->path(install->line);
      globals.sim = globals.sim || install->simulation;
      globals.exp.lab = install->root;
      globals.exp.spectrometer = install->path(install->spectrometer);
      globals.exp.canvas = install->path(install->canvas);
      globals.exp.data = install->path(install->data.empty() ? "data" : install->data);
    } else if (globals.install) {
      io.err << "error: " << install.error().what << "\n";
      return kFailed;
    }
  }
  Session session(std::move(globals), io);
  return session.execute(std::vector<std::string>(args.begin() + static_cast<std::ptrdiff_t>(i), args.end()));
}

}  // namespace elctl
