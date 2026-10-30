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
// Two served transports naming one host and port are refused unless every
// driver on them is of one vendor PC (Qtegra: thermo_qtegra, qtegra_valves,
// qtegra_gauges; NGX: isotopx_ngx, ngx_valves): those share one listener and
// one emulator.
//
// With a spectrometer (LineServerOptions::spectrometer, spec phase B) its
// transports are served the same way, from a simulated beam fed by the line's
// gas: a thermo_qtegra or isotopx_ngx driver's PC measures the beam
// (feed_qtegra_from_beam, feed_ngx_from_beam), and a line transport of kind
// "link" borrowing that driver's link joins its drivers to the same emulator
// (its valves move the served line). A spectrometer transport with any other
// driver is a silent wire.
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
#include "pychron/sim/spectrometer/beam_model.hpp"
#include "pychron/sim/wire_server.hpp"
#include "pychron/systems/canvas/canvas.hpp"
#include "pychron/systems/line_sim.hpp"
#include "pychron/systems/spectrometer/data_dir.hpp"
#include "pychron/transport/endpoints.hpp"

namespace pychron::systems {

struct LineServerOptions {
  std::string bind = "127.0.0.1";
  std::string advertise;  // the host written to the endpoints file; empty: bind
  std::uint16_t port_base = 0;  // 0: OS-assigned ports (WireServer::Options)
  LineSimOptions sim;
  // The spectrometer to serve too, loaded with its field tables; none: the line alone.
  std::optional<spectrometer::cfg::SpectrometerData> spectrometer;
};

struct ServedTransport {
  std::string name;
  std::string kind;  // the config's: tcp, udp, serial, modbus_tcp
  WireProtocol protocol = WireProtocol::Tcp;
  std::uint16_t port = 0;
  std::string emulator;  // "" = silent wire
  bool spectrometer = false;  // a transport of the spectrometer's file
  // The transport whose listener and emulator this one shares (`(spectrometer)`
  // after a spectrometer's name); empty: its own.
  std::string shares;
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
  // What clients are given: `config` the line's file, canonical, and the
  // spectrometer's section when one is served.
  SimEndpoints endpoints() const;
  sim::SimSystem& system() noexcept { return *system_; }
  // The served spectrometer's beam; null without one.
  std::shared_ptr<sim::BeamModel> beam() const noexcept { return beam_; }
  void stop();

 private:
  LineServer() = default;

  std::string config_file_;
  std::string spectrometer_file_;  // empty: none served
  std::string host_;
  std::vector<ServedTransport> served_;
  std::vector<std::string> in_process_;
  std::vector<LineSimNote> notes_;
  std::vector<std::string> key_of_;  // served_[i]'s listener, until start() sorts served_
  std::unique_ptr<sim::SimSystem> system_;  // outlives the server, whose hooks it answers
  std::shared_ptr<sim::BeamModel> beam_;    // likewise
  std::unique_ptr<sim::WireServer> wire_;
};

}  // namespace pychron::systems
