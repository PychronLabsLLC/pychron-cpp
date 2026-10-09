# Sim serve: the simulated lab on the wire

Status: draft, not agreed. Nothing implemented.

Date: 2026-10-09. Builds on `2026-10-06-lab-simulator-design.md` (gas model),
`2026-10-06-virtual-clock-design.md` (time), `2026-10-03-installation-wizard-design.md`
(`elctl doctor`).

## 1. Intent

New lab's hardware config (hosts, ports, driver kinds, addresses, channel
maps) is never exercised before instrument is connected. `--sim` replaces
every transport with `kind = "sim"`: real `TcpTransport` / `UdpTransport`,
real reconnect, real framing off a socket, never run. Canvas, scripts, queue
and conditionals are rehearsed; wiring of config to devices is not.

Goal: hardware install, files byte for byte unchanged, runs whole queue
against simulated lab that answers on sockets.

Three pieces:

1. `elctl sim serve`: existing `SimSystem` and device wire sims behind
   TCP/UDP listeners, one per transport of the config.
2. `elctl doctor` uses it: `--rehearse` (serve in-process, probe, stop).
3. Traces recorded on real hardware answer in place of an emulator
   (`--replay`): only piece that tests against the real thing.

Decided, not reopened here: simulation stays in C++ and in-process sim stays
as is. Separate Python simulator rejected: process outside the `VirtualClock`
stops simulated time or needs time protocol over the socket; keyed noise
(seed, name, time) needs the shared clock; 59 test files would need Docker,
which Windows and macOS CI lack; `pychron-ui --sim` from an installer must
work with nothing else installed; two implementations of each protocol.

## 2. What exists

- `SimSystem::hook_for_transport(name, system_config)` (`libs/sim`
  `sim_system.hpp`): `Bytes -> Bytes` hook answering as device would. Kinds
  with emulator joined to gas model: `proxr_relay`, `agilent_switch`,
  `qtegra_valves`, `qtegra_gauges`, `pychron_valves`, `ngx_valves`,
  `pfeiffer_maxigauge`, `varian_xgs600`, `gp_microion`, `plc2000_valves`,
  `plc2000_gauges`. Emulator on sim clock, no gas: `lakeshore`,
  `plc2000_heater`, `chromium`. Anything else: silent wire.
- `SimTransport::hooked(hook, options, unsolicited)`: in-process only. Hook
  receives one whole driver write per call; no stream framing anywhere.
- `ExtractionLine` (`libs/systems`) builds topology from canvas, reads
  `sim.toml`, owns `SimSystem` privately (`sim_`). Not reachable without
  building client transports and drivers.
- Spectrometer: `sim_*` drivers (`libs/sim` `sim_drivers.hpp`) bound to
  `BeamModel`, transport never touched. Wire sims `ngx_sim.hpp`,
  `thermo_qtegra_sim.hpp` exist, used by tests only, not fed by `BeamModel`
  (`NgxSimModel::values` is fixed vector).
- `TraceRecorder`, trace format (`trace.hpp`), `SimTransport::replay`,
  `elctl trace on|off [transport...]`. Replay used by tests only.
- `elctl doctor --probe`: TCP connect to each `tcp` transport, then driver
  connect step. Real hardware only.
- `elctl sim`: REPL on `elctl`'s own `Line` (`apps/elctl/src/line.hpp`),
  which has own minimal device sims, not `SimSystem`.

## 3. Shape

```
elctl sim serve                          pychron-ui / elctl exp run
  SimSystem (+ BeamModel, phase B)         hardware config, unchanged
     |  hook per transport                      |
  WireServer                               make_transport
     listener per transport  <--- sockets ---   TcpTransport / UdpTransport
     |                                          ^ host:port from endpoints file
  sim-endpoints.toml  ------- read by ----------+
```

One process holds all simulated state. Clients hold none: valve state and
pressures come back over the wire, as on hardware.

## 4. Interfaces

### 4.1 `WireSim` (`libs/sim`)

```cpp
struct WireSim {
  SimTransport::Hook hook;                // one request -> its reply (empty: none)
  SimTransport::Unsolicited unsolicited;  // may be empty
  ReadSpec request;                       // how one request is cut from a stream
  std::function<void()> on_connect;       // may be empty (NGX banner)
  std::string emulator;                   // driver kind answered as; "" = silent wire
};
WireSim SimSystem::wire_for_transport(std::string_view transport, const config::SystemConfig&);
```

- `hook_for_transport` stays, returns `wire_for_transport(...).hook`.
- `request`: each emulator declares framing of requests it receives
  (terminator, or Modbus MBAP length). Value comes from that driver's own
  codec, same constant, not second copy. UDP ignores it: one datagram, one
  request.

### 4.2 Line sim without the line (`libs/systems`)

```cpp
struct LineSim { std::unique_ptr<sim::SimSystem> system; std::vector<Diagnostic> notes; };
Result<LineSim> build_line_sim(const config::SystemConfig&, const canvas::Canvas*,
                               const Clock&, LineSimOptions);
```

`topology_of`, `sim.toml` lookup and `build_error` check move out of
`ExtractionLine` into this. `ExtractionLine` calls it: one path builds the
simulated lab, for both.

### 4.3 Listeners (`libs/transport`)

`TcpListener`, `UdpListener` in `listener.hpp`: bind address and port (0 =
OS chooses), report bound port, hand accepted connection as byte stream.
Asio stays private to `libs/transport`, as now.

### 4.4 `WireServer` (`libs/sim`)

```cpp
class WireServer {
 public:
  struct Options { std::string bind = "127.0.0.1"; std::uint16_t port_base = 0; const Clock* clock; };
  Result<void> add(std::string transport, Protocol, WireSim);   // Protocol: Tcp | Udp
  Result<Endpoints> start();   // all-or-nothing, like ExtractionLine::start()
  void stop();
};
```

- TCP: per connection, cut requests with `WireSim::request`, call hook,
  write reply. Any number of connections; all share emulator's model.
- Unsolicited polled every 20 ms per connection.
- Ports: `port_base` 0: OS-assigned. Else `port_base + i`, transports in
  byte order of name.
- Transport kinds: `tcp`, `modbus_tcp` -> TCP listener. `udp` -> UDP.
  `serial` -> TCP listener (bytes same; baud, parity, port name not
  exercised; reported). `link` -> none, follows transport it links to.
  `sim` -> none. `modbus_rtu` -> none, as unsupported today.

### 4.5 Endpoints file

Written by serve, read by clients. Not edited by hand.

```toml
config = "/abs/path/extraction_line.toml"
host = "127.0.0.1"                # --advertise overrides (container)

[transports.switch_controller]
protocol = "tcp"
port = 40213
emulator = "agilent_switch"       # "" = silent wire
```

Default path: `sim-endpoints.toml` beside the line's file; `--endpoints <file>`.
Removed on clean stop.

### 4.6 Client side

- `TransportContext::endpoints` (`libs/transport` `factory.hpp`): transport
  name -> host, port, protocol. `make_transport` builds `TcpTransport` /
  `UdpTransport` to that address in place of config's params.
- `ExtractionLine::Options::sim_endpoints`, spectrometer assembler same.
- `pychron-ui --sim-endpoints <file>`, `elctl --sim-endpoints <file>`
  (global: `exp run`, `open`, `read`, `scan`, `doctor`, ...).

### 4.7 Commands

```
elctl sim serve --config <extraction_line.toml> [--spectrometer <file>]
                [--bind ADDR] [--advertise HOST] [--port-base N]
                [--endpoints FILE] [--replay DIR]
elctl doctor --rehearse [--install NAME]
elctl doctor --replay DIR [--install NAME]
```

- `sim serve` prints table (transport, protocol, port, emulator), notes
  below, then runs until interrupted. `elctl sim` alone stays the REPL.
- `doctor --rehearse`: serve in-process on OS-assigned ports, run `--probe`
  steps against it, stop. Checks: every non-`sim` transport has emulator
  (WARN `no emulator for driver kind '<kind>'`: silent wire), connect and
  driver connect step pass (FAIL), every valve and gauge of config answers
  one read (FAIL, names address or channel).
- `doctor --replay DIR`: no sockets. For each `<transport>.trace` in DIR,
  driver connect step and one read per valve and gauge run on
  `SimTransport::replay`. FAIL names first unexpected tx (`verify()`).
- `sim serve --replay DIR`: transport with `<name>.trace` in DIR is answered
  from trace, not emulator: request equal to next `tx` gets the `rx` records
  that follow it; any other request gets no reply and one log line at warn.
  Such transport is out of the gas model; table says `replay`.

## 5. Rules

- With an endpoints file given, a transport that is not `kind = "sim"` and
  not `link` and has no entry in the file fails the load: no transport may
  reach its configured address during a rehearsal.
- `--sim-endpoints` and `--sim` together are refused. `--sim-endpoints` and
  `--sim-speed` together are refused: serve is real time only.
- Client refuses endpoints file whose `config` is not the file it loaded
  (compared as canonical paths); `--sim-endpoints-any` skips check, for
  container, where paths differ.
- Serve binds `127.0.0.1` unless `--bind` says otherwise. Emulators accept
  any login and have no authentication: binding beyond loopback exposes
  open ports that move nothing real but answer anyone.
- Serve runs on `SteadyClock`. Its threads follow Time rules anyway
  (`Participant`, waits through clock): they are pass-through there, and
  nothing in `libs/sim` gains a wait outside the clock.
- Transports left `kind = "sim"` in a served config stay in-process in the
  client, on client's own `SimSystem`, not joined to server's gas. Serve and
  doctor list them (`in-process, not joined`).
- Serve changes no behaviour of in-process sim: same hooks, same model, same
  numbers for same seed and same instants. Readings over the wire are at
  real instants, so not reproducible run to run; no test asserts their values.
- Serve keeps no state file: every start is `sim.toml` initial state, valves
  closed. Client's `extraction_line.state.toml` is not replayed into it:
  hardware install reads valves back.
- Same `host:port` under two transports (line and spectrometer file both
  naming one Qtegra PC): one listener, one emulator answering both drivers'
  commands. Phase B; in phase A, second such transport is refused with
  message naming both.

## 6. Phases

A. Line on the wire. 4.1 to 4.6, `elctl sim serve` without `--spectrometer`,
   kinds of section 2 with emulators. `chromium` (laser PC) included when
   laser config names it.

B. Spectrometer on the wire. `NgxSimModel` and `QtegraSimModel` read
   intensities, mass and source parameters from `BeamModel` (provider
   function, as `beam_gas()`); serve builds beam, `feed_beam_from_line` joins
   it. Kinds: `isotopx_ngx`, `thermo_qtegra`. Shared-port rule of section 5.

C. Doctor: `--rehearse`, `--replay`. `sim serve --replay`.

D. Container: `packaging/sim/` Dockerfile and compose file, Linux image of
   `elctl`, `--bind 0.0.0.0 --port-base 47000 --advertise <host>`. Not in CI.
   Docs only say how; nothing depends on it.

Each phase lands alone. A then C give doctor for line-only installs.

## 7. Tests

- `tests/sim/test_wire_server.cpp` (`WireServerSteady`: real sockets on
  loopback, OS-assigned ports, `SteadyClock`, upper bounds >= 5 s): request
  split across two segments answers once; two requests in one segment
  answer twice in order; UDP datagram; unsolicited arrives with no request;
  two connections share model; `start()` all-or-nothing when port taken.
- `tests/transport/test_listener.cpp`: bind, port 0 reports port, close
  while accept waits.
- `tests/transport/test_factory.cpp`: endpoints override per kind; serial
  becomes TCP; missing entry is Config error.
- `tests/integration/test_sim_serve.cpp`: hardware-style config (every
  transport `tcp`/`udp`/`modbus_tcp`) against serve; open valve over wire,
  gauge over wire rises; `ExtractionLine::start()` readback matches server;
  load refused with entry missing; refused with `--sim`.
- `tests/systems`: `build_line_sim` gives same topology and notes
  `ExtractionLine` gave before the move (existing sim tests unchanged and
  passing is the check).
- `tests/setup/test_doctor.cpp`: `--rehearse` OK on example hardware config;
  WARN on kind with no emulator; FAIL names wrong address. `--replay` on
  traces under `tests/traces/`; FAIL names first unexpected tx.
- Phase B: `QtegraAcquire` and `NgxLink` tests over serve read beam fed by
  line (air shot nonzero, blank near baseline; no exact values).
- New threads: whole of the above run once under `-DPYCHRON_SANITIZE=thread`
  before landing.
- Windows: sockets tests build and run there (no persistence needed).

## 8. Docs

- `docs/simulator.md`: section "The simulated lab on the wire": what serve
  is for, table of emulated kinds, what is not exercised (serial settings,
  timing, vendor quirks), rehearsal steps.
- `docs/installation_runbook.md`: rehearse before connecting; record traces
  on first connect, keep them with install.
- `AGENTS.md`: entry when phase A lands (endpoints rule, real time only,
  `request` framing comes from driver's codec).

## 9. Out of scope

- Removing or moving in-process sim, `kind = "sim"`, `--sim`, `sim_*` drivers.
- Simulated time across the socket.
- Serial port emulation (pty, com0com). Serial transports are served as TCP.
- Wire form for `sim_valves` and legacy spectrometer pieces (`adc_bank`,
  `dac_positioner`, `pulse_counter`, `serial_hv`).
- New emulators for kinds that have none. Serve reports them.
- Fault injection and simulator control panel (lab simulator spec section
  10). `drop_next` / `delay_next` / `garble_next` are `SimTransport`'s and
  do not exist in `WireServer`.
- Replacing `elctl`'s own `Line` and its device sims with `ExtractionLine`
  (noted in `line.hpp`); serve does not use `Line`.
- Gas from laser or furnace; calibrated amounts.

## 10. Open

- Phase B shared Qtegra port: one emulator object for valves, gauges and
  acquisition needs `thermo_qtegra_sim` and `SimSystem`'s Qtegra valve hook
  merged. Size unknown until read.
- `chromium` and NGX unsolicited streams over 20 ms poll: enough for
  drivers' timeouts? Measure in phase A / B tests.
- Whether `pychron-ui` should start serve itself (menu item) or leave it to
  `elctl`. Left to `elctl` here.
- Endpoints file beside line's file is inside install root: `doctor`'s
  managed-file report must ignore it.
