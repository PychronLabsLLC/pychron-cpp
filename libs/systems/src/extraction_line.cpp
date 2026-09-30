#include "pychron/systems/extraction_line.hpp"

#include <algorithm>
#include <chrono>

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

  scheduler_ = std::make_unique<Scheduler>(*clock_, &bus_, options_.scheduler);

  subscriptions_.push_back(bus_.subscribe<PressureSample>(
      [this](const PressureSample& s) { record_pressure(s.gauge, s.value); }));
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
}

bool ExtractionLine::running() const {
  std::lock_guard lock(lifecycle_);
  return running_;
}

Result<void> ExtractionLine::actuate(std::string_view name, SwitchOp op, std::string_view actor) {
  return switches_->actuate(name, op, actor);
}

Result<double> ExtractionLine::read_gauge(std::string_view name) {
  auto g = std::find_if(config_.gauges.begin(), config_.gauges.end(), [&](const auto& g) { return g.name == name; });
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

void ExtractionLine::log(LogLevel level, std::string message) {
  bus_.publish(Log{level, "extraction_line", std::move(message), clock_->now()});
}

}  // namespace pychron::systems
