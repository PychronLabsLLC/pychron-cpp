#include "line.hpp"

#include <algorithm>
#include <deque>

#include "pychron/devices/capabilities.hpp"
#include "pychron/devices/channel_gauge.hpp"
#include "pychron/devices/driver_registry.hpp"
#include "pychron/devices/pfeiffer_maxigauge.hpp"
#include "pychron/devices/proxr_board_sim.hpp"
#include "pychron/transport/factory.hpp"

namespace elctl {

using namespace pychron;

// Device models behind sim transports. Deques keep addresses stable because
// transport hooks borrow them.
struct Line::Sims {
  std::deque<ProxrBoardSim> boards;

  SimTransport::Hook hook_for(const std::string& driver_kind) {
    if (driver_kind == "proxr_relay") {
      boards.emplace_back();
      return boards.back().hook();
    }
    if (driver_kind == "pfeiffer_maxigauge") {
      // A quiet, pumped-down line: channel n reads n * 1e-9.
      MaxiGaugeSimModel model;
      model.pressure = [](int channel) -> std::optional<double> { return channel * 1e-9; };
      return maxigauge_sim_hook(std::move(model));
    }
    return {};  // silent wire: the driver sees timeouts
  }
};

Line::Line(config::SystemConfig config) : config_(std::move(config)), sims_(std::make_unique<Sims>()) {}

Line::~Line() {
  stop_scan();
  close_all();
}

Result<std::unique_ptr<Line>> Line::build(config::SystemConfig config, LineOptions options) {
  std::unique_ptr<Line> line(new Line(std::move(config)));
  const auto& cfg = line->config_;

  if (options.trace.enabled()) {
    std::error_code ec;
    std::filesystem::create_directories(options.trace_dir, ec);
    if (ec) return fail(ErrorKind::Io, "cannot create trace directory " + options.trace_dir.string());
  }

  for (auto [name, tc] : cfg.transports) {
    if (options.force_sim) {
      tc.kind = config::TransportKind::Sim;
      tc.params = config::SimParams{};
    }
    tc.trace = tc.trace || options.trace.traces(name);

    TransportContext context;
    context.clock = &line->clock_;
    context.bus = &line->bus_;
    context.trace_dir = options.trace_dir.string();
    if (tc.kind == config::TransportKind::Sim) {
      // The first driver on the wire decides which device model answers.
      auto driver = std::find_if(cfg.drivers.begin(), cfg.drivers.end(),
                                 [&](const auto& d) { return d.second.transport == name; });
      if (driver != cfg.drivers.end()) context.sim_hook = line->sims_->hook_for(driver->second.kind);
    }
    auto transport = make_transport(tc, context);
    if (!transport) return fail(transport.error());
    line->transports_.emplace_back(name, std::move(*transport));
  }

  for (const auto& [name, dc] : cfg.drivers) {
    Transport* transport = line->transport(dc.transport);
    if (!transport) return fail(ErrorKind::Config, "unknown transport '" + dc.transport + "'", name);
    auto device = DriverRegistry::global().create(dc, *transport, &line->clock_);
    if (!device) return fail(device.error());
    line->devices_.emplace_back(name, std::move(*device));
  }

  Line* self = line.get();
  auto switches = systems::SwitchManager::from_config(
      cfg,
      [self](const std::string& name) -> IValveActuator* {
        Device* d = self->device(name);
        return d ? capability<IValveActuator>(*d) : nullptr;
      },
      systems::SwitchManagerOptions{&line->clock_, &line->bus_});
  if (!switches) return fail(switches.error());
  line->switches_ = std::move(*switches);

  return line;
}

std::vector<Line::Opened> Line::open_all() {
  std::vector<Opened> out;
  for (auto& [name, transport] : transports_) out.push_back({name, transport->open()});
  return out;
}

void Line::close_all() {
  for (auto& [name, transport] : transports_) transport->close();
}

Transport* Line::transport(const std::string& name) const {
  for (const auto& [n, t] : transports_) {
    if (n == name) return t.get();
  }
  return nullptr;
}

Device* Line::device(const std::string& name) const {
  for (const auto& [n, d] : devices_) {
    if (n == name) return d.get();
  }
  return nullptr;
}

const config::GaugeConfig* Line::gauge(const std::string& name) const {
  for (const auto& g : config_.gauges) {
    if (g.name == name) return &g;
  }
  return nullptr;
}

Result<double> Line::read_gauge(const std::string& name) {
  const auto* g = gauge(name);
  if (!g) return fail(ErrorKind::Config, "unknown gauge '" + name + "'");
  Device* d = device(g->driver);
  if (!d) return fail(ErrorKind::Config, "gauge driver '" + g->driver + "' not built", name);
  if (auto* multi = capability<IChannelPressureGauge>(*d)) return multi->read_pressure(static_cast<int>(g->channel));
  if (auto* single = capability<IPressureGauge>(*d)) return single->read_pressure();
  return fail(ErrorKind::Config, "driver '" + g->driver + "' is not a pressure gauge", name);
}

Result<void> Line::start_scan(Duration interval) {
  stop_scan();
  scheduler_ = std::make_unique<Scheduler>(clock_, &bus_);
  scanner_ = std::make_unique<GaugeScanner>(*scheduler_, bus_, clock_);
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
      stop_scan();
      return added;
    }
  }
  scheduler_->start();
  return {};
}

void Line::stop_scan() {
  // Cancelling a job does not interrupt a scan already running on a worker,
  // and that scan still uses the scanner's state: drain before destroying.
  if (scanner_) scanner_->stop();
  if (scheduler_) {
    scheduler_->stop();
    scheduler_->wait_idle();
  }
  scanner_.reset();
  scheduler_.reset();
}

}  // namespace elctl
