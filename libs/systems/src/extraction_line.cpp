#include "pychron/systems/extraction_line.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>

#include <toml++/toml.hpp>

#include "pychron/core/config/loader.hpp"
#include "pychron/devices/capabilities.hpp"
#include "pychron/devices/channel_gauge.hpp"
#include "pychron/devices/driver_registry.hpp"
#include "pychron/systems/canvas/cross_validate.hpp"
#include "pychron/systems/canvas/loader.hpp"
#include "pychron/transport/factory.hpp"

namespace pychron::systems {

namespace {

// Sim topology from the canvas plumbing: stage volumes (cc) where declared.
sim::SimTopology topology_of(const NetworkGraph& graph, const canvas::Canvas& canvas) {
  sim::SimTopology t;
  for (const auto& v : graph.volumes()) {
    double cc = 1.0;
    for (const auto& s : canvas.stages) {
      if (s.name == v && s.volume) cc = *s.volume;
    }
    t.volumes.push_back({v, cc});
  }
  for (const auto& v : graph.valves()) t.valves.push_back(v);
  for (const auto& n : graph.volumes()) {
    for (const auto& m : graph.neighbors(n)) t.edges.emplace_back(n, m);
  }
  for (const auto& n : graph.valves()) {
    for (const auto& m : graph.neighbors(n)) t.edges.emplace_back(n, m);
  }
  return t;
}

const char* state_name(ValveState s) {
  switch (s) {
    case ValveState::Open: return "open";
    case ValveState::Closed: return "closed";
    default: return "unknown";
  }
}

}  // namespace

ExtractionLine::ExtractionLine(config::SystemConfig config, std::optional<canvas::Canvas> canvas, Options options)
    : config_(std::move(config)),
      canvas_(std::move(canvas)),
      options_(std::move(options)),
      clock_(options_.clock ? options_.clock : &steady_clock_) {}

ExtractionLine::~ExtractionLine() {
  stop();
  subscriptions_.clear();
}

Result<std::unique_ptr<ExtractionLine>> ExtractionLine::load(const std::filesystem::path& system_file,
                                                             const std::optional<std::filesystem::path>& canvas_file,
                                                             Options options) {
  auto config = config::load_system_config(system_file);
  if (!config) return fail(config.error());
  std::optional<canvas::Canvas> canvas;
  if (canvas_file) {
    auto loaded = canvas::load_canvas(*canvas_file);
    if (!loaded) return fail(loaded.error());
    canvas = std::move(*loaded);
  }
  if (options.state_file.empty()) {
    options.state_file = std::filesystem::path(system_file).replace_extension(".state.toml");
  }
  return create(std::move(*config), std::move(canvas), std::move(options));
}

Result<std::unique_ptr<ExtractionLine>> ExtractionLine::create(config::SystemConfig config,
                                                               std::optional<canvas::Canvas> canvas,
                                                               Options options) {
  std::unique_ptr<ExtractionLine> line(new ExtractionLine(std::move(config), std::move(canvas), std::move(options)));
  if (auto built = line->build(); !built) return fail(built.error());
  return line;
}

Result<void> ExtractionLine::build() {
  if (canvas_) {
    auto checked = canvas::check_canvas(*canvas_, config_);
    if (!checked) return fail(checked.error());
    warnings_ = std::move(*checked);
    network_ = NetworkGraph::from_canvas(*canvas_);
  }

  const bool any_sim =
      options_.force_sim || std::any_of(config_.transports.begin(), config_.transports.end(), [](const auto& t) {
        return t.second.kind == config::TransportKind::Sim;
      });
  if (any_sim) {
    sim_ = std::make_unique<sim::SimSystem>(*clock_, network_ ? topology_of(*network_, *canvas_) : sim::SimTopology{},
                                            options_.sim);
  }

  log_hub_ = options_.log_hub;
  if (!log_hub_) {
    // Logging must never stop the line: warn and carry on without a hub.
    if (auto hub = LogHub::create(config_.logging, *clock_, &bus_)) {
      log_hub_ = std::move(*hub);
    } else {
      std::fprintf(stderr, "pychron: logging disabled: %s\n", hub.error().what.c_str());
    }
  }
  if (log_hub_) {
    logger_.emplace(log_hub_->logger("extraction_line"));
    switches_logger_.emplace(log_hub_->logger("switches"));
  }

  bool tracing = false;
  for (auto [name, tc] : config_.transports) {
    if (options_.force_sim) {
      tc.kind = config::TransportKind::Sim;
      tc.params = config::SimParams{};
    }
    tc.trace = tc.trace || options_.trace.contains(name);
    if (tc.trace && !tracing) {
      std::error_code ec;
      std::filesystem::create_directories(options_.trace_dir, ec);
      if (ec) return fail(ErrorKind::Io, "cannot create trace directory " + options_.trace_dir.string());
      tracing = true;
    }

    TransportContext context;
    context.clock = clock_;
    context.bus = &bus_;
    context.trace_dir = options_.trace_dir.string();
    context.log_hub = log_hub_;
    if (tc.kind == config::TransportKind::Sim) {
      auto driver = std::find_if(config_.drivers.begin(), config_.drivers.end(),
                                 [&](const auto& d) { return d.second.transport == name; });
      if (driver != config_.drivers.end()) context.sim_hook = sim_->hook_for(driver->second, config_);
    }
    auto transport = make_transport(tc, context);
    if (!transport) return fail(transport.error());
    transports_.emplace_back(name, std::move(*transport));
  }

  for (const auto& [name, dc] : config_.drivers) {
    Transport* t = transport(dc.transport);
    if (!t) return fail(ErrorKind::Config, "unknown transport '" + dc.transport + "'", name);
    auto device = DriverRegistry::global().create(dc, *t, clock_);
    if (!device) return fail(device.error());
    devices_.emplace_back(name, std::move(*device));
  }

  auto switches = SwitchManager::from_config(
      config_,
      [this](const std::string& name) -> IValveActuator* {
        Device* d = device(name);
        return d ? capability<IValveActuator>(*d) : nullptr;
      },
      SwitchManagerOptions{clock_, &bus_});
  if (!switches) return fail(switches.error());
  switches_ = std::move(*switches);
  load_state();

  scheduler_ = std::make_unique<Scheduler>(*clock_, &bus_, options_.scheduler, log_hub_);

  subscriptions_.push_back(bus_.subscribe<PressureSample>(
      [this](const PressureSample& s) { record_pressure(s.gauge, s.value); }));
  // Every actuation path (operator, scheduler, protocols) goes through
  // SwitchManager, which reports outcomes as events: log them all here.
  // The hub publishes Log from inside these handlers; the bus allows that.
  subscriptions_.push_back(bus_.subscribe<ValveChanged>([this](const ValveChanged& e) {
    log_to(switches_logger_, "switches", LogLevel::Info, "valve " + e.valve + " " + state_name(e.state));
  }));
  // Remember where every valve is, for the next run. Not before the first
  // start() has restored the last run's states: its read-back would
  // overwrite them.
  subscriptions_.push_back(bus_.subscribe<ValveChanged>([this](const ValveChanged&) {
    if (!restored_) return;
    std::lock_guard lock(locks_mutex_);
    save_state();
  }));
  subscriptions_.push_back(bus_.subscribe<ActuationFailed>([this](const ActuationFailed& e) {
    log_to(switches_logger_, "switches", LogLevel::Warn,
           "actuate '" + e.valve + "' failed: " + std::string(to_string(e.error.kind)) + ": " + e.error.what);
  }));
  subscriptions_.push_back(bus_.subscribe<SwitchLockChanged>([this](const SwitchLockChanged& e) {
    log_to(switches_logger_, "switches", LogLevel::Info, "valve " + e.name + (e.locked ? " locked" : " unlocked"));
  }));
  if (sim_) {
    // Manual valves have no actuator: the operator's report is the only
    // thing that can move them in the model.
    subscriptions_.push_back(bus_.subscribe<ValveChanged>([this](const ValveChanged& e) {
      for (const auto& m : config_.manual_valves) {
        if (m.name == e.valve) sim_->set_valve(e.valve, e.state == ValveState::Open);
      }
    }));
  }
  return {};
}

Result<void> ExtractionLine::start() {
  std::lock_guard lock(lifecycle_);
  if (running_) return {};

  std::string failures;
  for (auto& [name, t] : transports_) {
    if (auto opened = t->open(); !opened) {
      if (!failures.empty()) failures += "; ";
      failures += name + ": " + opened.error().what;
    }
  }
  if (!failures.empty()) {
    for (auto& [name, t] : transports_) t->close();
    return fail(ErrorKind::Io, "cannot open transports: " + failures);
  }

  if (auto refreshed = switches_->refresh(); !refreshed) {
    log(LogLevel::Warn, "switch read-back failed: " + refreshed.error().what);
  }
  restore_valves();
  read_all_gauges();

  scanner_ = std::make_unique<GaugeScanner>(*scheduler_, bus_, *clock_);
  const Duration interval = std::chrono::milliseconds(std::max<std::int64_t>(1, config_.system.scan_interval_ms));
  for (const auto& g : config_.gauges) {
    Device* d = device(g.driver);
    Result<void> added = fail(ErrorKind::Config, "driver '" + g.driver + "' is not a pressure gauge", g.name);
    if (d) {
      if (auto* multi = capability<IChannelPressureGauge>(*d)) {
        added = scanner_->add_gauge(g, *multi, interval);
      } else if (auto* single = capability<IPressureGauge>(*d)) {
        added = scanner_->add_gauge(g, *single, interval);
      }
    }
    if (!added) {
      scanner_.reset();
      for (auto& [name, t] : transports_) t->close();
      return added;
    }
  }

  running_ = true;
  log(LogLevel::Info, "extraction line started: " + std::to_string(config_.valves.size() + config_.manual_valves.size()) +
                          " valves, " + std::to_string(config_.gauges.size()) + " gauges");
  bus_.publish(snapshot());
  if (options_.run_scheduler) scheduler_->start();
  return {};
}

void ExtractionLine::stop() {
  std::lock_guard lock(lifecycle_);
  if (!running_) return;
  // Cancelling a job does not interrupt a scan already running on a worker,
  // and that scan still uses the scanner's state: drain before destroying.
  scanner_->stop();
  scheduler_->stop();
  scheduler_->wait_idle();
  scanner_.reset();
  for (auto& [name, t] : transports_) t->close();
  running_ = false;
  log(LogLevel::Info, "extraction line stopped");
}

bool ExtractionLine::running() const {
  std::lock_guard lock(lifecycle_);
  return running_;
}

Result<void> ExtractionLine::actuate(std::string_view name, SwitchOp op, std::string_view actor) {
  return switches_->actuate(name, op, actor);
}

Result<double> ExtractionLine::read_gauge(std::string_view name) {
  auto g = std::find_if(config_.gauges.begin(), config_.gauges.end(), [&](const auto& x) { return x.name == name; });
  if (g == config_.gauges.end()) return fail(ErrorKind::Config, "unknown gauge '" + std::string(name) + "'");
  Device* d = device(g->driver);
  if (!d) return fail(ErrorKind::Config, "gauge driver '" + g->driver + "' not built", g->name);
  Result<double> value = fail(ErrorKind::Config, "driver '" + g->driver + "' is not a pressure gauge", g->name);
  if (auto* multi = capability<IChannelPressureGauge>(*d)) {
    value = multi->read_pressure(static_cast<int>(g->channel));
  } else if (auto* single = capability<IPressureGauge>(*d)) {
    value = single->read_pressure();
  }
  if (value) record_pressure(g->name, *value);
  return value;
}

Snapshot ExtractionLine::snapshot() const {
  Snapshot s;
  s.valves = switches_->states();
  for (const auto& info : switches_->list()) {
    if (info.locked) s.locked.insert(info.name);
  }
  {
    std::lock_guard lock(pressures_mutex_);
    s.pressures = pressures_;
  }
  s.ts = clock_->now();
  return s;
}

Transport* ExtractionLine::transport(std::string_view name) const {
  for (const auto& [n, t] : transports_) {
    if (n == name) return t.get();
  }
  return nullptr;
}

Device* ExtractionLine::device(std::string_view name) const {
  for (const auto& [n, d] : devices_) {
    if (n == name) return d.get();
  }
  return nullptr;
}

void ExtractionLine::read_all_gauges() {
  for (const auto& g : config_.gauges) {
    if (auto p = read_gauge(g.name); !p) {
      log(LogLevel::Warn, "initial read of gauge '" + g.name + "' failed: " + p.error().what);
    }
  }
}

void ExtractionLine::record_pressure(const std::string& gauge, double value) {
  std::lock_guard lock(pressures_mutex_);
  pressures_[gauge] = value;
}

Result<void> ExtractionLine::set_locked(std::string_view name, bool locked) {
  auto info = switches_->info(name);
  if (!info) return fail(info.error());
  if (info->kind == SwitchKind::ManualValve) {
    return fail(ErrorKind::Config, "manual valve '" + std::string(name) + "' cannot be locked", std::string(name));
  }
  {
    std::lock_guard lock(locks_mutex_);
    if (info->locked == locked) return {};
    auto changed = locked ? switches_->lock(name) : switches_->unlock(name);
    if (!changed) return changed;
    save_state();
  }
  bus_.publish(SwitchLockChanged{std::string(name), locked, clock_->now()});
  return {};
}

bool ExtractionLine::is_locked(std::string_view name) const {
  auto info = switches_->info(name);
  return info && info->locked;
}

// The state file is `locked = ["A", "B"]` and a `[valves]` table of
// "open" / "closed". A missing file means no locks and nothing remembered; a
// corrupt one or a name that no longer exists is reported in warnings() and
// skipped, never fatal.
void ExtractionLine::load_state() {
  if (options_.state_file.empty() || !std::filesystem::exists(options_.state_file)) return;
  const std::string file = options_.state_file.string();
  auto parsed = toml::parse_file(file);
  if (!parsed) {
    warnings_.push_back({config::SourceLoc{file, 0, 0}, "locked",
                         "cannot read saved state, all valves start unlocked: " +
                             std::string(parsed.error().description())});
    return;
  }
  if (const auto* names = parsed.table()["locked"].as_array()) {
    for (const auto& node : *names) {
      const auto name = node.value<std::string>();
      if (!name) continue;
      if (auto info = switches_->info(*name); !info || info->kind == SwitchKind::ManualValve) {
        warnings_.push_back(
            {config::SourceLoc{file, 0, 0}, "locked", "saved lock for unknown valve '" + *name + "' ignored"});
        continue;
      }
      (void)switches_->lock(*name);
    }
  }
  if (const auto* valves = parsed.table()["valves"].as_table()) {
    for (const auto& [key, node] : *valves) {
      const std::string name(key.str());
      const auto state = node.value<std::string>();
      if (!switches_->contains(name) || !state || (*state != "open" && *state != "closed")) {
        warnings_.push_back(
            {config::SourceLoc{file, 0, 0}, "valves", "saved state for '" + name + "' ignored: unknown valve or state"});
        continue;
      }
      remembered_[name] = *state == "open" ? ValveState::Open : ValveState::Closed;
    }
  }
}

// Callers hold locks_mutex_ (or run before any other thread can).
void ExtractionLine::save_state() {
  if (options_.state_file.empty()) return;
  toml::array names;
  toml::table valves;
  for (const auto& info : switches_->list()) {
    if (info.locked) names.push_back(info.name);
    // Until the last run's states are restored, they are what is remembered.
    ValveState state = info.state;
    if (!restored_) {
      auto it = remembered_.find(info.name);
      state = it == remembered_.end() ? ValveState::Unknown : it->second;
    }
    if (state != ValveState::Unknown) valves.insert(info.name, state == ValveState::Open ? "open" : "closed");
  }
  toml::table table;
  table.insert("locked", std::move(names));
  table.insert("valves", std::move(valves));

  // Write beside the target and rename so a crash never leaves a torn file.
  const auto tmp = std::filesystem::path(options_.state_file).concat(".tmp");
  std::error_code ec;
  if (!options_.state_file.parent_path().empty()) {
    std::filesystem::create_directories(options_.state_file.parent_path(), ec);
  }
  {
    std::ofstream out(tmp, std::ios::out | std::ios::trunc);
    if (out) out << table << '\n';
    if (!out) {
      log(LogLevel::Warn, "cannot persist valve state to " + options_.state_file.string());
      return;
    }
  }
  std::filesystem::rename(tmp, options_.state_file, ec);
  if (ec) log(LogLevel::Warn, "cannot persist valve state to " + options_.state_file.string() + ": " + ec.message());
}

// Whether the valve or switch `name` is driven through a simulated transport.
bool ExtractionLine::simulated(const std::string& name) const {
  std::string actuator;
  for (const auto& v : config_.valves)
    if (v.name == name) actuator = v.actuator;
  for (const auto& s : config_.switches)
    if (s.name == name) actuator = s.actuator;
  auto driver = config_.drivers.find(actuator);
  if (driver == config_.drivers.end()) return false;
  if (options_.force_sim) return true;
  auto transport = config_.transports.find(driver->second.transport);
  return transport != config_.transports.end() && transport->second.kind == config::TransportKind::Sim;
}

// First start() only, after the read-back. See Options::state_file.
void ExtractionLine::restore_valves() {
  if (restored_) return;
  std::map<std::string, ValveState> pending;
  for (const auto& [name, state] : remembered_) {
    auto info = switches_->info(name);
    if (!info || info->state == state) continue;
    if (info->kind == SwitchKind::ManualValve || simulated(name)) pending[name] = state;
  }
  // A valve with a positive interlock opens only after its prerequisite:
  // keep going round while any restore succeeds.
  std::string last_error;
  for (bool progress = true; progress && !pending.empty();) {
    progress = false;
    for (auto it = pending.begin(); it != pending.end();) {
      auto done = switches_->restore(it->first, it->second == ValveState::Open ? SwitchOp::Open : SwitchOp::Close);
      if (done) {
        it = pending.erase(it);
        progress = true;
      } else {
        last_error = done.error().what;
        ++it;
      }
    }
  }
  for (const auto& [name, state] : pending) {
    log(LogLevel::Warn, "valve " + name + " not restored to its saved state: " + last_error);
  }
  {
    std::lock_guard lock(locks_mutex_);
    remembered_.clear();
    restored_ = true;
    save_state();
  }
}

void ExtractionLine::log(LogLevel level, std::string message) {
  log_to(logger_, "extraction_line", level, std::move(message));
}

void ExtractionLine::log_to(const std::optional<Logger>& logger, std::string_view name, LogLevel level,
                            std::string message) {
  if (logger) {
    if (!logger->enabled(level)) return;
    logger->log(level, message);  // publishes on the hub's bus
    if (log_hub_->bus() == &bus_) return;
  }
  bus_.publish(Log{level, std::string(name), std::move(message), clock_->now()});
}

}  // namespace pychron::systems
