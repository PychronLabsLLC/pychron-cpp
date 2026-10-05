#include "pychron/transport/factory.hpp"

#include <chrono>
#include <filesystem>
#include <optional>

#include "pychron/transport/link_transport.hpp"
#include "pychron/transport/serial_transport.hpp"
#include "pychron/transport/tcp_transport.hpp"
#include "pychron/transport/trace_recorder.hpp"
#include "pychron/transport/udp_transport.hpp"

namespace pychron {

namespace {

const Clock& default_clock() {
  static const SteadyClock clock;
  return clock;
}

SerialSettings serial_settings(const config::SerialParams& p) {
  SerialSettings s;
  s.port = p.port;
  s.baud = static_cast<std::uint32_t>(p.baud);
  s.data_bits = static_cast<unsigned>(p.data_bits);
  s.stop_bits = static_cast<unsigned>(p.stop_bits);
  s.parity = p.parity == config::Parity::Even  ? SerialSettings::Parity::Even
             : p.parity == config::Parity::Odd ? SerialSettings::Parity::Odd
                                               : SerialSettings::Parity::None;
  return s;
}

}  // namespace

Result<std::unique_ptr<Transport>> make_transport(const config::TransportConfig& config,
                                                  const TransportContext& context) {
  const Clock& clock = context.clock ? *context.clock : default_clock();

  TransportOptions options;
  options.name = config.name;
  options.timeout = std::chrono::milliseconds(config.timeout_ms);
  options.retries = static_cast<int>(config.retries);
  options.clock = &clock;
  options.bus = context.bus;

  std::unique_ptr<Transport> transport;
  switch (config.kind) {
    case config::TransportKind::Sim:
      transport = SimTransport::hooked(context.sim_hook ? context.sim_hook : [](const Bytes&) { return Bytes{}; },
                                       std::move(options));
      break;
    case config::TransportKind::Serial:
      if (const auto* p = std::get_if<config::SerialParams>(&config.params))
        transport = std::make_unique<SerialTransport>(serial_settings(*p), std::move(options));
      break;
    case config::TransportKind::Tcp:
      if (const auto* p = std::get_if<config::TcpParams>(&config.params))
        transport = std::make_unique<TcpTransport>(TcpSettings{p->host, static_cast<std::uint16_t>(p->port)},
                                                   std::move(options));
      break;
    case config::TransportKind::Udp:
      if (const auto* p = std::get_if<config::UdpParams>(&config.params))
        transport = std::make_unique<UdpTransport>(UdpSettings{p->host, static_cast<std::uint16_t>(p->port)},
                                                   std::move(options));
      break;
    case config::TransportKind::ModbusTcp:
      // A TCP stream; the Modbus framing is the driver's codec (codec::modbus).
      if (const auto* p = std::get_if<config::ModbusTcpParams>(&config.params))
        transport = std::make_unique<TcpTransport>(
            TcpSettings{p->tcp.host, static_cast<std::uint16_t>(p->tcp.port)}, std::move(options));
      break;
    case config::TransportKind::ModbusRtu:
      return fail(ErrorKind::Config, "modbus_rtu transports are not supported yet", config.name);
    case config::TransportKind::Link:
      if (const auto* p = std::get_if<config::LinkParams>(&config.params))
        return std::unique_ptr<Transport>(std::make_unique<LinkTransport>(config.name, p->link));
      break;
  }
  if (!transport) return fail(ErrorKind::Config, "transport parameters do not match its kind", config.name);

  std::optional<Logger> wire;
  if (context.log_hub) wire = context.log_hub->logger(config.name + ".wire");
  if (config.trace) {
    const auto path = (std::filesystem::path(context.trace_dir) / (config.name + ".trace")).string();
    auto recorder = TraceRecorder::to_file(std::move(transport), path, clock, std::move(wire));
    if (!recorder) return fail(recorder.error());
    return std::unique_ptr<Transport>(std::move(*recorder));
  }
  if (wire) {
    // No trace file: the recorder only mirrors bytes to "<name>.wire", and
    // formats nothing unless a rule enables Trace on it.
    return std::unique_ptr<Transport>(
        std::make_unique<TraceRecorder>(std::move(transport), nullptr, clock, std::move(wire)));
  }
  return transport;
}

}  // namespace pychron
