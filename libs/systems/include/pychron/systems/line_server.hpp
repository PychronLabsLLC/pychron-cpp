#pragma once

// LineServer: a line's simulated lab on sockets, one listener per transport
// of its config (sim serve spec, docs/superpowers/specs/2026-10-09-sim-serve-design.md
// sections 3 to 5). What `elctl sim serve` runs, and doctor's rehearsal will.
//
// The lab is build_line_sim's, so the gas behind the sockets is the gas an
// in-process simulated line has. Each transport is served as its kind says:
//   tcp, modbus_tcp, serial   a TCP listener (a serial one as TCP: baud,
//                             parity and port name are not exercised)
//   udp                       a UDP listener
//   sim                       not served: in-process in the client, on its
//                             own SimSystem, not joined to this one's gas
//   link                      not served: it follows the transport it names
//   modbus_rtu                not served: not supported
// A transport whose driver kind has no emulator is served as a silent wire.
// Two served transports naming one host and port are refused (one emulator
// answering both is spec phase B).
//
// Real time only (WireServer). Every start is sim.toml's initial state,
// valves closed: no state file is kept or read.

#include <memory>
#include <string>
#include <vector>

#include "pychron/core/clock.hpp"
#include "pychron/core/config/system_config.hpp"
#include "pychron/core/error.hpp"
#include "pychron/sim/sim_system.hpp"
#include "pychron/sim/wire_server.hpp"
#include "pychron/systems/canvas/canvas.hpp"
#include "pychron/systems/line_sim.hpp"
#include "pychron/transport/endpoints.hpp"

namespace pychron::systems {

struct LineServerOptions {
  std::string bind = "127.0.0.1";
  std::string advertise;  // the host written to the endpoints file; empty: bind
  std::uint16_t port_base = 0;  // 0: OS-assigned ports (WireServer::Options)
  LineSimOptions sim;
};

struct ServedTransport {
  std::string name;
  std::string kind;  // the config's: tcp, udp, serial, modbus_tcp
  WireProtocol protocol = WireProtocol::Tcp;
  std::uint16_t port = 0;
  std::string emulator;  // "" = silent wire
};

class LineServer {
 public:
  // Builds the lab and serves it. `clock` must be a SteadyClock and outlive
  // the server.
  static Result<std::unique_ptr<LineServer>> start(const config::SystemConfig& config, const canvas::Canvas* canvas,
                                                   const Clock& clock, LineServerOptions options = {});
  ~LineServer();
  LineServer(const LineServer&) = delete;
  LineServer& operator=(const LineServer&) = delete;

  const std::vector<ServedTransport>& served() const noexcept { return served_; }  // name order
  // The `kind = "sim"` transports: in-process in the client, not joined.
  const std::vector<std::string>& in_process() const noexcept { return in_process_; }
  // The lab's notes (build_line_sim), then the server's.
  const std::vector<LineSimNote>& notes() const noexcept { return notes_; }
  // What clients are given: `config` the line's file, canonical.
  SimEndpoints endpoints() const;
  sim::SimSystem& system() noexcept { return *system_; }
  void stop();

 private:
  LineServer() = default;

  std::string config_file_;
  std::string host_;
  std::vector<ServedTransport> served_;
  std::vector<std::string> in_process_;
  std::vector<LineSimNote> notes_;
  std::unique_ptr<sim::SimSystem> system_;  // outlives the server, whose hooks it answers
  std::unique_ptr<sim::WireServer> wire_;
};

}  // namespace pychron::systems
