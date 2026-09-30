# Instrument Control Layer — Design

Date: 2026-09-29
Status: Approved (design), pending implementation plan
Scope: Phase 1 of a staged C++ rewrite of pychron. This spec covers the
instrument-control foundation and the first vertical slice (extraction line:
valves + gauges).

## 1. Intent

Pychron (Python) is being replaced, not ported. This project is a fresh C++
design that uses pychron as the domain reference (which devices exist, which
protocols they speak, what lab workflows require) but deliberately discards its
architecture where it has proven weak.

Pain points this design must fix:

1. **Device model** — pychron's `CoreDevice` mixin hierarchy fuses I/O,
   protocol formatting, parsing and device behavior; untestable without
   hardware.
2. **Config sprawl** — `initialization.xml`, per-device `.cfg`, `valves.yaml`,
   `canvas.xml/yaml`, env-var path resolution.
3. **Concurrency** — ad-hoc threads, per-device locks, race-prone scan timers.
4. **Simulation** — bolted on via `if self.simulation`; no trace replay.

Constraints (stated by the owner):

- Full replacement, staged. Instrument control first; experiment engine, UI
  shell, DVC, reduction in later phases.
- No interop with the Python codebase. Nothing here talks to pychron.
- Stack: C++20, CMake, Qt6 for UI only. Core libraries are Qt-free.
- Platforms: macOS and Windows.
- Repo: new sibling repository (`pychron-cpp`, name provisional).

Success criteria for Milestone 1 (M1):

> A real extraction line runs from `extraction_line.toml` + `canvas.toml`:
> valves actuate with interlocks enforced, gauges scan and display, and the
> identical binaries behave the same under `kind = "sim"` with no hardware.

## 2. Architecture decision: hybrid concurrency (ADR-0001)

Chosen over fully-async coroutines (B) and actor model (C).

- **Transport-owned serialization.** Each `Transport` owns one physical channel
  and a FIFO command queue serviced by one worker thread. It is the only
  component in the system that holds a lock. Bus sharing (N devices on one
  RS-485/Modbus line) is safe by construction.
- **Synchronous driver API.** Drivers call `transport.exchange(...)` and block
  until reply or timeout. Driver code reads top-to-bottom; interlock and safety
  logic is easy to audit.
- **One Scheduler.** All periodic work (scans, alarms, watchdogs) is registered
  with a single scheduler on a small pool. Drivers and managers own no
  threads.
- **Typed pub/sub.** State leaves the core only via `SignalBus` events passed
  by value.

Rationale: lab devices are slow serial (ms–s latency) and number in the tens;
coroutines buy little and cost debuggability. Actors add message-type overhead
and make synchronous queries awkward.

Ownership rules:

1. Transport owns the channel and its worker.
2. Driver borrows a `Transport&`, holds no threads.
3. Scheduler owns all periodic work.
4. Managers own drivers and publish state via `SignalBus`.
5. Anything crossing a thread boundary is a `Result<T>` or event by value.

## 3. Repository layout and build

```
pychron-cpp/
  CMakeLists.txt            # C++20; options BUILD_UI, BUILD_TESTS, sanitizers
  CMakePresets.json         # mac-debug, mac-release, win-msvc-debug, win-msvc-release
  cmake/                    # warnings-as-errors, sanitizer flags, helpers
  vcpkg.json                # asio (standalone), tomlplusplus, spdlog, fmt, gtest; Qt6 (UI only)
  libs/
    core/                   # Result, Error, Clock, Scheduler, SignalBus, Logger, config loader
    transport/              # Transport iface; Serial, Tcp, Modbus, Sim, TraceRecorder
    codecs/                 # per-vendor byte codecs, zero I/O
    devices/                # capability interfaces + drivers (transport + codec)
    systems/                # SwitchManager, GaugeScanner, NetworkGraph, canvas model, ExtractionLine facade
    sim/                    # SimSystem stateful lab model
  apps/
    elctl/                  # headless CLI
    pychron-ui/             # Qt6 M1 status/control window
  tests/                    # gtest; see section 8
  configs/examples/         # extraction_line.toml, canvas.toml
  tools/                    # pychron_canvas_to_toml.py (migration)
  docs/
    superpowers/specs/
    adr/
```

Rules:

- `libs/*` compile with no Qt. Enforced by target link lists and a CI job
  building with `-DBUILD_UI=OFF`.
- One `libs/<x>` = one CMake target with public headers under
  `include/pychron/<x>/`. Dependency direction is strictly downward:
  `core <- transport <- codecs <- devices <- systems <- apps`. `sim` depends on
  `transport` and `devices` only.
- Dependencies via vcpkg manifest (works on macOS and MSVC).
- Vendor SDKs (LabJack, etc.) are optional CMake components, off by default,
  implemented as `Transport` subclasses. None in M1.
- CI matrix: macOS + Windows; ASan/UBSan on macOS; `-Werror`; tests run
  against simulation only.
- `elctl` exists from the first commit so hardware bring-up never depends on
  the UI.

## 4. Core types

### 4.1 Errors

```cpp
enum class ErrorKind { Timeout, Io, Protocol, Config, NotConnected, Interlock, Cancelled };
struct Error { ErrorKind kind; std::string what; std::string device; };
template <class T> using Result = std::expected<T, Error>;
```

No exceptions cross library boundaries. Every `ErrorKind` has at least one
test exercising its path.

### 4.2 Transport

```cpp
struct ReadSpec;   // terminator | fixed length | modbus frame

class Transport {
public:
  virtual Result<void>  open() = 0;
  virtual void          close() = 0;
  virtual Result<Bytes> exchange(Bytes tx, ReadSpec rs, Duration timeout) = 0; // write then read, atomic on the bus
  virtual Result<void>  write(Bytes tx) = 0;
  virtual Result<Bytes> read(ReadSpec rs, Duration timeout) = 0;
  Health                health() const;   // Connected | Degraded | Down, last_ok, consecutive_failures
};
```

- Each transport owns one worker and a FIFO queue. `exchange()` may be called
  from any thread; it enqueues and blocks the caller until completion.
- Retries and timeouts are configured per transport and applied here, once.
- `TraceRecorder` is a decorator around any `Transport`; it logs tx/rx bytes
  with timestamps to a file that `SimTransport` can replay.
- Implementations: `SerialTransport` (asio `serial_port`), `TcpTransport`,
  `ModbusTransport` (RTU/TCP framing layered on the above), `SimTransport`.

### 4.3 Codec

A codec is a set of pure functions for one vendor protocol: encode a command
to bytes, provide the `ReadSpec` for the expected reply, decode reply bytes
to a value or a `Protocol` error. Codecs have no I/O, threads, time, or
config dependencies and are tested with literal byte strings.

### 4.4 Device and capabilities

```cpp
class Device { std::string name; DeviceHealth health() const; };  // identity + health only

struct IPressureGauge { virtual Result<double>     read_pressure() = 0; };
struct IValveActuator { virtual Result<void>       open(ValveAddress) = 0;
                        virtual Result<void>       close(ValveAddress) = 0;
                        virtual Result<ValveState> read(ValveAddress) = 0; };
struct IScannable     { virtual Result<Sample>     sample() = 0; };
```

A driver composes `Transport&` + codec and implements one or more capability
interfaces. There is no shared behavioral base class. Managers depend on
capability interfaces only and are vendor-blind.

Distinction, stated once:

| Layer | Answers | Knows | Never knows |
|---|---|---|---|
| Transport | how bytes reach the wire | port/socket, framing, timeouts, retries, bus serialization | byte meaning, device identity |
| Codec | what bytes the vendor wants / what replies mean | command grammar, checksums, terminators | I/O, threads, time, config |
| Driver | how this device fulfils a capability | codec choice, transport ref, channel/address, sequencing, local state | serial vs TCP, other devices |

### 4.5 Scheduler

- One instance per process; small pool (default 4 threads); monotonic
  injected `Clock`.
- Job kinds: periodic (`IScannable` + interval), one-shot delayed, watchdog
  (fires callback if heartbeat missed).
- Per-job non-overlap: a slow scan never stacks on itself.
- Emits `Sample{device, ts, value}` to `SignalBus`.

### 4.6 SignalBus

Thread-safe typed pub/sub. Events (all by value): `ValveChanged`,
`PressureSample`, `Alarm`, `TransportHealth`, `Log`, `Snapshot`,
`ActuationFailed`. Devices never call UI; UI never calls devices.

## 5. Configuration

One TOML file per system, schema-validated on load. Replaces
`initialization.xml`, per-device `.cfg`, `valves.yaml` for the instrument
layer.

### 5.1 `extraction_line.toml`

```toml
[system]
name = "jan"
scan_interval_ms = 1000

[transports.valve_bus]
kind = "serial"            # serial | tcp | modbus_rtu | modbus_tcp | sim
port = "/dev/tty.usbserial-A1"   # "COM4" on Windows
baud = 9600
timeout_ms = 500
retries = 2
trace = true

[transports.gauge_net]
kind = "tcp"
host = "192.168.0.51"
port = 8000

[drivers.actuator1]
kind = "proxr_relay"
transport = "valve_bus"

[drivers.ig_controller]
kind = "pfeiffer_maxigauge"
transport = "gauge_net"
channels = [1, 2, 3]

[[valves]]
name = "A"
description = "Furnace to bone"
actuator = "actuator1"
address = "1"
interlocks = ["B"]           # cannot open while any of these are open
positive_interlocks = []     # all of these must be open before this opens
settle_ms = 1000

[[valves]]
name = "B"
actuator = "actuator1"
address = "2"

[[manual_valves]]
name = "M1"

[[switches]]                 # actuated on/off things that are not gas valves
name = "pump_power"          # shares actuators + address space with valves; no interlocks
actuator = "actuator1"
address = "9"

[[gauges]]
name = "IG1"
driver = "ig_controller"
channel = 1
units = "torr"
alarm_high = 1e-4

[[pipettes]]
name = "air"
inner = "P1"
outer = "P2"
```

### 5.2 Loading pipeline (`libs/core/config`)

1. Parse with tomlplusplus into typed structs (`SystemConfig`,
   `TransportConfig` variant, `DriverConfig`, `ValveConfig`, `GaugeConfig`,
   ...).
2. Validate: required fields; enum values; cross-references (`actuator`,
   `driver`, `transport`, interlock names all resolve); addresses unique per
   actuator; no self-interlock; no interlock cycles.
3. Return `Result<SystemConfig>`; errors carry `file:line:field`. Loading is
   all-or-nothing.
4. `elctl validate <file>` prints every error and exits non-zero.

### 5.3 Driver registry

`DriverRegistry::create(kind, Transport&, const toml::table&)`. Drivers
self-register with `REGISTER_DRIVER("pfeiffer_maxigauge", PfeifferMaxiGauge)`
and declare their expected keys; `elctl list-drivers` prints kinds and keys
from those declarations so documentation cannot drift.

### 5.4 Overrides

`extraction_line.local.toml`, if present, is merged on top and may only set
transport `port`/`host`/`baud`-class keys. Main file stays committed and
portable.

### 5.5 Dropped from pychron

Env-var path resolution, INI-style `.cfg`, plugin toggles inside device
config, canvas geometry mixed into device config.

## 6. Canvas (`canvas.toml`)

Separate file per system; pure presentation + connectivity. References
domain objects from `extraction_line.toml` by name. Loader cross-validates:
every canvas `valve` must exist in the system config; system valves missing
from the canvas produce a warning.

Element vocabulary preserved from pychron:

```toml
[canvas]
origin = [0, 0]
size = [1000, 700]
connection_width = 6

[colors]
valve = "#1e90ff"
pipette = "#cccccc"

[[valve]]         name = "A"   pos = [100, 200]
[[manual_valve]]  name = "M1"  pos = [300, 200]
[[rough_valve]]   name = "R1"  pos = [400, 200]
[[switch]]        name = "pump_power" pos = [500, 100]

[[stage]]   name = "bone"  pos = [150, 300] size = [80, 40] volume = 12.5 fill = true
[[stage]]   name = "spec"  pos = [800, 300] size = [120, 60] display_name = "Spectrometer" use_symbol = true

[[pipette]] name = "air" pos = [600, 400] vlabel = "Air Pipette"

[[connection]] start = "A" end = "B"                 # orientation inferred or "h"/"v"
[[elbow]]      start = "B" end = "bone" corner = "ul"
[[tee]]        left = "bone" right = "spec" mid = "IG1"
[[cross]]      left = "x" right = "y" top = "z" bottom = "w"

[[label]] text = "Furnace side" pos = [100, 50] font = "Arial 12"
[[image]] path = "logo.png" pos = [900, 20]
[legend] pos = [20, 650]
```

Design points:

- **No state in the canvas.** Valve state, gauge readings and locks arrive via
  `SignalBus`.
- **Connectivity is semantic.** Connections build a `NetworkGraph`
  (`libs/systems`): nodes are volumes and valves, edges are connections.
  `connected_volumes(valve_states)` drives region coloring now and volume /
  pipette logic later. Qt-free and unit-tested.
- Geometry structs live in `libs/systems/canvas` (Qt-free) so
  `elctl canvas-check` validates layouts headless. Rendering lives in
  `apps/pychron-ui`.
- Migration: `tools/pychron_canvas_to_toml.py` converts existing
  `canvas.yaml`/`.xml` + `valves.yaml` into `canvas.toml` +
  `extraction_line.toml`.
- Dropped: XML format, Chaco/Enable layer stack, in-app designer (deferred).

M1 canvas scope: valves, stages, connections, click-to-actuate with interlock
rejection feedback, network coloring, gauge value labels. No designer, no
image/legend polish.

## 7. Simulation

Simulation uses the same code path as hardware and is selected by
`kind = "sim"` in config. There is no `if simulation` branch in drivers or
managers.

| Layer | Fakes | Tests |
|---|---|---|
| Codec tests | nothing (pure bytes) | vendor parsing, checksums, edge frames |
| `SimTransport` | the wire | driver retries, timeouts, malformed replies |
| `SimSystem` | the physical lab | managers, interlocks, scans, UI |

`SimTransport` (`libs/transport/sim`):

- Modes: `scripted` (ordered `expect tx -> reply rx [, delay]`; unexpected tx
  fails the test), `replay` (a `TraceRecorder` file), `hook` (callback
  `Bytes -> Bytes`, used by `SimSystem`).
- Fault injection: `drop_next(n)`, `delay_next(ms)`, `garble_next()`.
- Real traces captured at the lab are committed under
  `tests/traces/<vendor>/` and replayed in CI.

`SimSystem` (`libs/sim`):

- Stateful model behind `hook` transports: valve states, pressure per volume,
  gas equilibration across open valves using `NetworkGraph`, pump-down curve,
  gauge noise.
- One instance serves every driver in a config, so cross-device causality
  (open valve -> pressure changes) emerges naturally.
- Uses the injected `Clock`; tests advance time manually.

## 8. Testing

```
tests/
  codecs/         gtest, byte fixtures
  transport/      SimTransport; real serial loopback (skipped when no port)
  devices/        each driver vs scripted + replay
  systems/        SwitchManager interlocks, GaugeScanner, NetworkGraph, config validation errors
  integration/    example config with kind=sim -> SimSystem -> actuate -> assert pressures
  ui/             Qt Test (BUILD_UI only): canvas loads, click valve -> command issued
```

- Interlock logic has property-style tests: random open/close sequences never
  violate declared interlocks.
- Every `ErrorKind` has at least one test.
- Sanitizers on the macOS job.

Hardware bring-up (not CI): `elctl probe` opens every transport, pings each
driver, prints a health table; `elctl trace on` records for later replay.

## 9. Milestone 1 vertical slice

### 9.1 Drivers

| Kind | Transport | Notes |
|---|---|---|
| `proxr_relay` (NCD ProXR) | serial / tcp | valve actuator; pychron `proxr_actuator.py` is the protocol reference |
| `pfeiffer_maxigauge` | serial / tcp | multi-channel ASCII with ENQ/ACK handshake |
| `gp_microion` (Granville-Phillips Micro-Ion) | serial | second gauge vendor proves the capability interface is vendor-blind |

Each driver ships with codec tests, scripted-transport tests, a replay trace,
a registry schema declaration, and a `SimSystem` hook.

Deferred to M2: `labjack_u3` (vendor SDK behind `Transport`).

### 9.2 Systems (`libs/systems`)

- `SwitchManager`: owns valves, manual valves, switches (`[[switches]]`).
  `actuate(name, Open|Close, actor)` checks software lock and owner, negative
  and positive interlocks, sends via `IValveActuator`, waits `settle_ms`,
  reads back, publishes `ValveChanged` or `ActuationFailed{Interlock}`.
  Locks/ownership are in-memory in M1.
- `GaugeScanner`: registers gauges with the Scheduler, publishes
  `PressureSample`, evaluates `alarm_high`/`alarm_low`, publishes `Alarm`.
- `NetworkGraph`: built from canvas connections.
- `ExtractionLine` facade: loads both configs, constructs transports ->
  drivers -> managers, `start()`/`stop()`, exposes `SignalBus`. The only entry
  point for `elctl` and the UI.

### 9.3 `elctl`

`validate`, `canvas-check`, `list-drivers`, `probe`, `list`, `state`,
`open <valve>`, `close <valve>`, `read <gauge>`, `scan --for <dur>`,
`trace on|off`, `sim` (SimSystem REPL).

### 9.4 Order of work

Each step ends green in CI.

1. `core`: Result, Clock, Scheduler, SignalBus, config loader + validation
2. `transport`: Sim, Serial, Tcp, TraceRecorder
3. `device_kit`: Device base, capability interfaces, Sample/ValveState,
   DriverRegistry, empty `codecs` target (single owner for everything the
   vendor drivers share)
4. `pfeiffer_maxigauge`: codec -> driver -> sim hook
5. `proxr_relay`: same (parallel with 4)
6. `SwitchManager` + interlock property tests
7. `GaugeScanner` (parallel with 6, 8)
8. `NetworkGraph` + canvas loader
9. `elctl`
10. `ExtractionLine` facade + `SimSystem` + integration test
11. Qt UI
12. `gp_microion` (parallel with 11)
13. Hardware bring-up, capture traces, commit as fixtures (manual)

### 9.5 Explicitly deferred

Pipette logic, remote/client mode, cryo/heater managers, canvas designer,
valve-history persistence, experiment scripting, LabJack, Modbus drivers.

## 10. Qt UI and the core/UI boundary

Rule: the UI never touches drivers or transports. It uses `ExtractionLine`
(commands in) and `SignalBus` (state out). Enforced by CMake:
`pychron-ui` links `systems` only.

### 10.1 Threading bridge

- Core events are emitted on scheduler/transport threads.
- `apps/pychron-ui/CoreBridge` (a `QObject`) subscribes to `SignalBus`,
  marshals each event to the main thread via queued `invokeMethod`, and
  re-emits Qt signals: `valveChanged`, `pressureSample`, `alarm`,
  `transportHealth`, `logLine`, `snapshot`.
- Commands: `CoreBridge::actuate(name, op)` runs `ExtractionLine::actuate` on
  a single command-executor `QThread` so the UI never blocks during settle;
  completion is posted back as `actuationFinished(name, Result)`.
- No core object holds a `QObject*`; no Qt types in `libs/*` headers.

### 10.2 State model

`CoreBridge` keeps a main-thread snapshot (valve states, latest pressures,
transport health). Canvas items read the snapshot and never query the core
synchronously. The facade emits a full `Snapshot` after `start()` so the UI
paints correctly before the first scan tick.

### 10.3 Widgets (M1)

```
MainWindow
 |- CanvasView (QGraphicsView)
 |    ValveItem       click -> actuate; color by state; lock badge; pending indicator
 |    StageItem       network coloring from NetworkGraph
 |    ConnectionItem, LabelItem, GaugeLabelItem (value + units; red on alarm)
 |- LogDock           tails SignalBus log/errors
 |- AlarmDock         active alarms; ack (local only in M1)
 |- HealthBar         per-transport chip: green/amber/red + last-ok age
```

Plain `QGraphicsItem` subclasses, no QML in M1. Interlock rejection flashes
the item and shows `Error.what` in a tooltip and the log.

### 10.4 Deferred

Task/dock framework, preferences, multiple windows, valve history table,
designer.

## 11. Open decisions carried into the plan

None blocking. Items the implementation plan may settle without re-approval:
exact `ReadSpec` representation, `SignalBus` subscription API shape, whether
`ModbusTransport` lands in M1 as an untested stub or waits for M2, and the
repo's final name.
