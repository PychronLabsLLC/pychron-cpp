#include "pychron/systems/line_server.hpp"

#include <filesystem>
#include <map>
#include <utility>

namespace pychron::systems {

namespace {

const char* kind_name(config::TransportKind kind) {
  switch (kind) {
    case config::TransportKind::Serial: return "serial";
    case config::TransportKind::Tcp: return "tcp";
    case config::TransportKind::Udp: return "udp";
    case config::TransportKind::ModbusRtu: return "modbus_rtu";
    case config::TransportKind::ModbusTcp: return "modbus_tcp";
    case config::TransportKind::Sim: return "sim";
    case config::TransportKind::Link: return "link";
  }
  return "?";
}

// The address a network transport names, for the shared-address check.
std::optional<std::string> address_of(const config::TransportConfig& tc) {
  if (const auto* p = std::get_if<config::TcpParams>(&tc.params)) return p->host + ":" + std::to_string(p->port);
  if (const auto* p = std::get_if<config::UdpParams>(&tc.params)) return p->host + ":" + std::to_string(p->port);
  if (const auto* p = std::get_if<config::ModbusTcpParams>(&tc.params))
    return p->tcp.host + ":" + std::to_string(p->tcp.port);
  return std::nullopt;
}

// The kind of the first driver (by name) on `transport`; empty when none.
std::string driver_kind(const config::SystemConfig& config, const std::string& transport) {
  for (const auto& [name, d] : config.drivers)
    if (d.transport == transport) return d.kind;
  return {};
}

}  // namespace

LineServer::~LineServer() { stop(); }

Result<std::unique_ptr<LineServer>> LineServer::start(const config::SystemConfig& config,
                                                      const canvas::Canvas* canvas, const Clock& clock,
                                                      LineServerOptions options) {
  auto built = build_line_sim(config, canvas, clock, std::move(options.sim));
  if (!built) return fail(built.error());
  std::unique_ptr<LineServer> server(new LineServer());
  server->system_ = std::move(built->system);
  server->notes_ = std::move(built->notes);
  server->config_file_ = config.source_file;
  server->host_ = options.advertise.empty() ? options.bind : options.advertise;
  server->wire_ = std::make_unique<sim::WireServer>(
      sim::WireServer::Options{.bind = options.bind, .port_base = options.port_base, .clock = &clock});

  std::map<std::string, std::string> named_by;  // address -> the transport that named it first
  for (const auto& [name, tc] : config.transports) {
    switch (tc.kind) {
      case config::TransportKind::Sim:
        server->in_process_.push_back(name);
        continue;
      case config::TransportKind::Link:
        continue;
      case config::TransportKind::ModbusRtu:
        server->notes_.push_back({LogLevel::Warn, name + ": modbus_rtu is not supported: not served"});
        continue;
      case config::TransportKind::Serial:
        server->notes_.push_back(
            {LogLevel::Info, name + ": serial served as TCP; baud, parity and port name are not exercised"});
        break;
      case config::TransportKind::Tcp:
      case config::TransportKind::Udp:
      case config::TransportKind::ModbusTcp:
        break;
    }
    if (const auto address = address_of(tc)) {
      const auto [first, fresh] = named_by.emplace(*address, name);
      if (!fresh) {
        return fail(ErrorKind::Config, "sim serve: transports '" + first->second + "' and '" + name +
                                           "' both name " + *address +
                                           "; one emulator answering both is not built yet");
      }
    }
    const WireProtocol protocol = tc.kind == config::TransportKind::Udp ? WireProtocol::Udp : WireProtocol::Tcp;
    auto wire = server->system_->wire_for_transport(name, config);
    if (wire.emulator.empty()) {
      const std::string kind = driver_kind(config, name);
      std::string said = name;
      if (kind.empty()) {
        said += ": no driver uses it: silent wire";
      } else {
        said += ": no emulator for driver kind '";
        said += kind;
        said += "': silent wire";
      }
      server->notes_.push_back({LogLevel::Warn, std::move(said)});
    }
    server->served_.push_back({name, kind_name(tc.kind), protocol, 0, wire.emulator});
    if (auto added = server->wire_->add(name, protocol, std::move(wire)); !added) return fail(added.error());
  }
  // Making the hooks may refuse a gauge's volume.
  if (auto refused = server->system_->build_error()) return fail(*refused);

  auto endpoints = server->wire_->start();
  if (!endpoints) return fail(endpoints.error());
  for (auto& s : server->served_) s.port = endpoints->at(s.name).port;
  return server;
}

SimEndpoints LineServer::endpoints() const {
  SimEndpoints out;
  std::error_code ec;
  auto canonical = std::filesystem::weakly_canonical(config_file_, ec);
  out.config = ec ? std::filesystem::path(config_file_) : canonical;
  out.host = host_;
  for (const auto& s : served_) out.transports.emplace(s.name, Endpoint{host_, s.port, s.protocol, s.emulator});
  return out;
}

void LineServer::stop() {
  if (wire_) wire_->stop();
}

}  // namespace pychron::systems
