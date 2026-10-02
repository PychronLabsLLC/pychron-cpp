#include "pychron/systems/spectrometer/assembler.hpp"

#include <algorithm>
#include <filesystem>

#include "pychron/devices/capabilities.hpp"
#include "pychron/devices/connectable.hpp"
#include "pychron/devices/driver_registry.hpp"
#include "pychron/systems/spectrometer/config_validate.hpp"
#include "pychron/transport/factory.hpp"

namespace pychron::spectrometer {

namespace {

Device* find_device(const std::vector<std::pair<std::string, Device*>>& devices, const std::string& name) {
  auto it = std::ranges::find(devices, name, &std::pair<std::string, Device*>::first);
  return it == devices.end() ? nullptr : it->second;
}

template <class Role>
Role* bind_role(const std::vector<std::pair<std::string, Device*>>& devices, const std::string& driver,
                std::string_view role, std::vector<std::string>& problems) {
  Device* d = find_device(devices, driver);
  if (d == nullptr) {
    problems.push_back(std::string(role) + ": driver '" + driver + "' was not built");
    return nullptr;
  }
  Role* r = capability<Role>(*d);
  if (r == nullptr) problems.push_back(std::string(role) + ": driver '" + driver + "' does not implement it");
  return r;
}

std::string join(const std::vector<std::string>& lines) {
  std::string out;
  for (const auto& l : lines) out += (out.empty() ? "" : "\n") + l;
  return out;
}

// Opens every transport, then connects every IConnectable driver, in the order
// given. On the first failure closes whatever was opened and returns the error,
// naming the transport or driver when the error does not.
Result<void> open_and_connect(const std::vector<std::pair<std::string, Transport*>>& transports,
                              const std::vector<std::pair<std::string, Device*>>& devices) {
  auto fail_with = [&](Error e, const std::string& name) -> Unexpected<Error> {
    if (e.device.empty()) e.device = name;
    for (const auto& [n, t] : transports) t->close();
    return fail(std::move(e));
  };
  for (const auto& [name, t] : transports) {
    if (auto r = t->open(); !r) return fail_with(r.error(), name);
  }
  for (const auto& [name, d] : devices) {
    auto* c = capability<IConnectable>(*d);
    if (c == nullptr) continue;
    if (auto r = c->connect(); !r) return fail_with(r.error(), name);
  }
  return {};
}

}  // namespace

Result<SpectrometerRoles> SpectrometerAssembler::bind(const cfg::SpectrometerConfig& config,
                                                      const std::vector<std::pair<std::string, Device*>>& devices) {
  std::vector<std::string> problems;
  SpectrometerRoles roles;

  roles.positioner = bind_role<IMassPositioner>(devices, config.magnet.positioner, "positioner", problems);
  if (roles.positioner != nullptr && roles.positioner->native_axis() != to_axis(config.magnet.native_axis)) {
    problems.push_back("positioner: driver '" + config.magnet.positioner + "' is natively " +
                       std::string(to_string(roles.positioner->native_axis())) + ", config says " +
                       std::string(cfg::to_string(config.magnet.native_axis)));
  }
  if (!config.source.driver.empty()) {
    roles.source = bind_role<IBeamSource>(devices, config.source.driver, "source", problems);
  }
  if (config.detector_control) {
    roles.detector_control =
        bind_role<IDetectorControl>(devices, config.detector_control->driver, "detector_control", problems);
  }
  // No section names the beam blank; any driver declaring the role provides it,
  // preferring the positioner's.
  std::vector<std::string> blankers;
  for (const auto& [name, dc] : config.drivers) {
    if (dc.has_role(cfg::Role::BeamBlank)) blankers.push_back(name);
  }
  std::ranges::stable_partition(blankers, [&](const std::string& n) { return n == config.magnet.positioner; });
  if (!blankers.empty()) roles.beam_blank = bind_role<IBeamBlank>(devices, blankers.front(), "beam_blank", problems);

  for (const auto& name : config.acquisition.acquirers) {
    auto* acquirer = bind_role<IIntensityAcquirer>(devices, name, "acquirer", problems);
    if (acquirer == nullptr) continue;
    roles.acquirers.emplace_back(name, acquirer);
    const auto live = acquirer->channels();
    const auto* dc = config.driver(name);
    std::vector<std::string> wanted = dc != nullptr ? dc->channels : std::vector<std::string>{};
    for (const auto& d : config.detectors) {
      auto ref = cfg::parse_channel_ref(d.channel);
      if (ref && ref->driver == name) wanted.push_back(ref->channel);
    }
    for (const auto& ch : wanted) {
      if (std::ranges::find(live, ch) == live.end()) {
        problems.push_back("acquirer '" + name + "' has no channel '" + ch + "'");
      }
    }
  }

  if (!problems.empty()) {
    std::ranges::sort(problems);
    problems.erase(std::unique(problems.begin(), problems.end()), problems.end());
    return fail(ErrorKind::Config, join(problems), "spectrometer");
  }
  return roles;
}

Result<std::unique_ptr<Transport>> SpectrometerAssembler::default_transport(const cfg::TransportConfig& c,
                                                                            const SpectrometerContext& context,
                                                                            const std::filesystem::path& trace_dir) {
  config::TransportConfig tc;
  tc.name = c.name;
  tc.loc = c.loc;
  tc.timeout_ms = c.timeout_ms;
  tc.retries = c.retries;
  tc.trace = c.trace;
  switch (c.kind) {
    case cfg::TransportKind::Tcp:
      tc.kind = config::TransportKind::Tcp;
      tc.params = config::TcpParams{c.host, c.tcp_port};
      break;
    case cfg::TransportKind::Serial:
      tc.kind = config::TransportKind::Serial;
      tc.params = config::SerialParams{c.serial_port, c.baud};
      break;
    case cfg::TransportKind::ModbusTcp:
      tc.kind = config::TransportKind::ModbusTcp;
      tc.params = config::ModbusTcpParams{config::TcpParams{c.host, c.tcp_port}};
      break;
    case cfg::TransportKind::ModbusRtu:
      tc.kind = config::TransportKind::ModbusRtu;
      tc.params = config::ModbusRtuParams{config::SerialParams{c.serial_port, c.baud}};
      break;
    case cfg::TransportKind::LabjackU3:
      return fail(ErrorKind::Config, "transport kind labjack_u3 is not supported yet", c.name);
    case cfg::TransportKind::Sim:
      tc.kind = config::TransportKind::Sim;
      tc.params = config::SimParams{};
      break;
  }
  TransportContext tctx;
  tctx.clock = &context.clock;
  tctx.bus = &context.bus;
  tctx.trace_dir = trace_dir.string();
  if (c.trace) {
    std::error_code ec;
    std::filesystem::create_directories(trace_dir, ec);
    if (ec) return fail(ErrorKind::Io, "cannot create trace directory " + trace_dir.string(), c.name);
  }
  return make_transport(tc, tctx);
}

Result<std::unique_ptr<Device>> SpectrometerAssembler::default_driver(const cfg::DriverConfig& dc,
                                                                      Transport& transport,
                                                                      const SpectrometerContext& context) {
  toml::table options = dc.options;
  if (!dc.channels.empty() && !options.contains("channels")) {
    toml::array channels;
    for (const auto& c : dc.channels) channels.push_back(c);
    options.insert("channels", std::move(channels));
  }
  return DriverRegistry::global().create(dc.kind, transport, options, DriverContext{dc.name, &context.clock});
}

Result<std::unique_ptr<Spectrometer>> SpectrometerAssembler::load(const std::filesystem::path& config_file,
                                                                  SpectrometerContext context, Options options) {
  auto data = cfg::load_spectrometer(config_file);
  if (!data) return fail(data.error());
  return assemble(std::move(*data), context, std::move(options));
}

Result<std::unique_ptr<Spectrometer>> SpectrometerAssembler::assemble(cfg::SpectrometerData data,
                                                                      SpectrometerContext context, Options options) {
  if (auto diags = cfg::validate(data.config, data.tables); !diags.empty()) return fail(config::to_error(diags));
  if (!options.make_transport) {
    options.make_transport = [dir = options.spectrometer.trace_dir](const cfg::TransportConfig& tc,
                                                                    const SpectrometerContext& ctx) {
      return default_transport(tc, ctx, dir);
    };
  }
  if (!options.make_driver) options.make_driver = default_driver;

  std::vector<std::string> problems;
  std::vector<std::unique_ptr<Transport>> transports;
  std::map<std::string, Transport*> by_name;
  std::vector<std::pair<std::string, Transport*>> transport_ptrs;  // config order
  for (const auto& [name, tc] : data.config.transports) {
    auto t = options.make_transport(tc, context);
    if (!t) {
      problems.push_back("transport '" + name + "': " + t.error().what);
      continue;
    }
    by_name[name] = t->get();
    transport_ptrs.emplace_back(name, t->get());
    transports.push_back(std::move(*t));
  }
  std::vector<std::unique_ptr<Device>> devices;
  std::vector<std::pair<std::string, Device*>> device_ptrs;
  for (const auto& [name, dc] : data.config.drivers) {
    auto t = by_name.find(dc.transport);
    if (t == by_name.end()) continue;  // already reported
    auto d = options.make_driver(dc, *t->second, context);
    if (!d) {
      problems.push_back("driver '" + name + "': " + d.error().what);
      continue;
    }
    device_ptrs.emplace_back(name, d->get());
    devices.push_back(std::move(*d));
  }
  if (!problems.empty()) return fail(ErrorKind::Config, join(problems), "spectrometer");

  auto roles = bind(data.config, device_ptrs);
  if (!roles) return fail(roles.error());
  roles->transports = std::move(transports);
  roles->devices = std::move(devices);

  std::map<std::string, FieldTable> tables;
  for (const auto& [name, tf] : data.tables) tables.emplace(name, to_field_table(tf));
  if (options.spectrometer.data_root.empty()) options.spectrometer.data_root = data.root;
  auto spectrometer = Spectrometer::create(std::move(data.config), MolecularWeights(std::move(data.weights)),
                                           std::move(tables), std::move(*roles), context,
                                           std::move(options.spectrometer));
  if (!spectrometer) return spectrometer;
  // Last step: nothing above touches the wire. On failure the spectrometer
  // (and with it the transports) is destroyed on return.
  if (auto up = open_and_connect(transport_ptrs, device_ptrs); !up) return fail(up.error());
  return spectrometer;
}

}  // namespace pychron::spectrometer
