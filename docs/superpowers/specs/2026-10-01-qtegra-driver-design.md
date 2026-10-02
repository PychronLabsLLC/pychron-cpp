# Thermo Qtegra spectrometer driver

Date: 2026-10-01
Status: Draft
Owner: Jake Ross
Depends on: `2026-09-29-spectrometer-control-design.md` (roles, Frame, acquisition
engine, assembler, config), `2026-09-29-instrument-control-design.md` (transport,
driver registry, declared keys), `2026-10-01-spectrometer-window-design.md`
(ScanService, continuous scans).
Implements unit `thermo_qtegra_driver` from
`tools/spec_router/spec_router/units.toml`, plus the infrastructure it needs.
The Isotopx NGX driver is deliberately not here: it shares one socket between
acquisition events, magnet commands and valve actuation, and gets its own spec
(see `2026-10-01-ngx-driver-notes.md`).
Wire ground truth: legacy pychron Python (`~/Programming/pychron/pychron`):
`spectrometer/thermo/**`, `hardware/thermo_spectrometer_controller.py`,
`hardware/core/communicators/ethernet_communicator.py`.

## 1. Goal

A driver that can run a real Thermo instrument (Argus, Helix) through the
existing `Spectrometer` facade and the spectrometer window, and the missing
plumbing that makes that possible: opening and reconnecting transports, and an
acquisition engine that is safe to restart.

Success: with a config naming `kind = "thermo_qtegra"` and a simulated wire
that speaks the Qtegra protocol, `Spectrometer` positions, reads and sets
source parameters, and a `ScanService` scan delivers readings at the snapped
integration period; every role passes the conformance suite.

## 2. Scope

In scope:

- Infrastructure: spectrometer transports opened and closed; `IConnectable`;
  reconnect on demand; `retries`/`trace` transport keys;
  `AcquisitionEngine::stop()` quiescence.
- `thermo_qtegra` driver and its stateful sim hook.
- Scripted, replay, conformance and config-parity tests; example config.

Out of scope: the Isotopx NGX driver and everything only it needs (secret
config keys, trace redaction, non-blocking transport reads, valve actuation
over the spectrometer link); UDP transport; reconnect-per-command;
`SetIonCounterVoltage`, `Reset`, sub-cup configuration and any other command
pychron's Python never sends on the paths below; the `NoIntensityChange`
heuristic; AF demagnetisation; trap-current ramping; `elctl` spectrometer
commands; a general transport-level reconnect.

## 3. Decisions

| Decision | Choice | Reason |
|---|---|---|
| Split | Qtegra now; NGX in its own spec | Qtegra is plain request/reply; NGX's shared event stream is a separate design problem. |
| Scope | Driver plus what it needs to run | A driver that cannot be opened or safely restarted is not usable. |
| Unverified commands | Left out | Never send an instrument a command no reference implementation sends. |
| Frame timestamps | Host monotonic time at reply | `GetData` carries no time; the engine's stale guard uses its own clock. |
| Connection | One persistent TCP connection | The C++ transport is persistent. pychron reconnects per command by default; see Risks. |
| Wire terminator | Config key defaulting to what pychron sends (`\r`) | No bench capture exists. |

## 4. Infrastructure

### 4.1 Opening transports; `IConnectable`

`libs/devices/include/pychron/devices/connectable.hpp`:

```cpp
// Optional driver capability: work that must follow every transport open
// (login, handshake, connection test). Blocking; scheduler/manager threads only.
struct IConnectable {
  virtual ~IConnectable() = default;
  virtual Result<void> connect() = 0;
};
```

`SpectrometerAssembler::assemble` gains a final step: for each transport, in
config order, `open()`; then for each device that is `IConnectable`,
`connect()`. The first failure closes every transport already opened and is
returned (kind unchanged, `Error::device` naming the transport or driver).
`Spectrometer`'s destructor closes its transports after its engine and devices
have stopped. Sim transports open trivially, so existing configs are unaffected.

### 4.2 Reconnect on demand

`libs/devices/include/pychron/devices/reconnect.hpp`: a small helper for drivers (Qtegra
now, NGX later).

```cpp
class Reconnector {
 public:
  Reconnector(Transport& transport, const Clock& clock, Duration min_interval = std::chrono::seconds(1));
  // Runs `op`. If it fails with Io or NotConnected and at least `min_interval`
  // has passed since the last attempt, closes and reopens the transport, runs
  // `on_connect`, and runs `op` once more. Any other error is returned as is.
  template <class T> Result<T> run(const std::function<Result<T>()>& op,
                                   const std::function<Result<void>()>& on_connect);
  std::uint64_t reconnects() const noexcept;
};
```

Rules: at most one reconnect attempt per call; a failed reopen or failed
`on_connect` returns that error; within `min_interval` of the previous attempt
the original error is returned without touching the transport. A reconnect
publishes nothing itself; the transport's own health events already report the
state change. While the link is down the acquirer returns the error from
`next()`. The engine keeps polling, so readings resume by themselves once a
reconnect succeeds; the scan status keeps showing the error until
`ScanService`'s Restart clears it.

### 4.3 Transport keys

Spectrometer `[transports.<name>]` additionally accepts `retries` (integer >= 0,
default 0) and `trace` (boolean, default false), passed to `make_transport`.
Traces are written under `SpectrometerOptions::trace_dir` (default `traces`),
created on demand. `*.local.toml` may still override only `host`, `port`,
`baud`, `timeout_ms` on transports.

### 4.4 Acquisition engine quiescence

`AcquisitionEngine::stop()` returns only when no `poll()` is executing: each
poll registers itself for its duration; `stop()` cancels the scheduler jobs,
then waits until the count is zero, then calls each acquirer's `stop()`. A
`stop()` called from inside a poll (same thread) does not wait for itself.
`start()` after `stop()` therefore never overlaps a `next()` from the previous
run. The wait is bounded by the acquirer's own transport timeout.

`start()` is serialised with itself and with `stop()`: a second `start()`
waits for the first and then fails with "already running" (nothing is
configured or registered twice), and a `stop()` that arrives while a
`start()` is in progress waits for it and then stops it. Neither waits when
called from inside a poll while another thread's `start()` or `stop()` is in
progress: `start()` fails with `Config`, `stop()` returns.

## 5. `thermo_qtegra` driver

`libs/devices/include/pychron/devices/spectrometer/thermo_qtegra.hpp`, `.cpp`;
class `QtegraSpectrometer final : public Device, public IConnectable,
public IMassPositioner, public IBeamSource, public IIntensityAcquirer,
public IDetectorControl, public IBeamBlank`. Registered as `thermo_qtegra`.
All wire text comes from `pychron::codec::qtegra`.

### 5.1 Declared keys

| Key | Type | Default | Meaning |
|---|---|---|---|
| `roles` | string array | - | informational (`legacy::kRolesKey`) |
| `channels` | string array | `H2 H1 AX L1 L2 CDD` | detector names as Qtegra reports them in `GetData` |
| `limit_min`, `limit_max` | float | 0, 10 | magnet DAC limits (volts) |
| `terminator` | string | `cr` | write terminator: `cr`, `lf`, `crlf` |
| `settle_periods` | float | 2.0 | integration periods to wait after an integration change; 0..100, anything else is `Config` at `create()` |

### 5.2 Behaviour

- **connect():** `GetIntegrationTime`; a numeric reply is success and seeds the
  cached integration period.
- **Every command** is one `exchange(cmd.tx, *cmd.reply)` wrapped in the
  `Reconnector` and `observe(...)`.
- **Setter replies:** `SetMagnetDAC`, `BlankBeam`, `ProtectDetector`,
  `SetDeflection`, `SetGain` and `SetIntegrationTime` accept any reply that is
  not an explicit `ERROR` (pychron ignores these replies); an `ERROR` reply is
  `Protocol`. `SetHV` and `SetParameter` expect `ok`. A setter that gets no
  reply is a `Timeout`: the driver cannot tell a silent success from a dead
  link.
- **Positioner (axis Dac):** `set(v)` outside `limit_min`..`limit_max` is
  `Config` with nothing sent; else `SetMagnetDAC v`. The `Spectrometer` facade
  checks first: a native value outside the positioner's limits or outside
  `[magnet].limits` (on each side the stricter bound wins) is `Config` with
  nothing read or written, so both layers enforce limits. `read()` is
  `GetMagnetDAC`. `moving()` is `GetMagnetMoving` decoded with the codec's bool
  vocabulary.
- **Magnet move (facade and move protocol, vendor-blind):** detector
  protection for a move below the beam-blank threshold is planned from the
  field table, which needs each table point corrected (deflection, and HV when
  `corrections.hv` is on: a `GetHighVoltage`). If any such correction fails,
  the move is aborted with that error and nothing is sent to the positioner,
  the blank or the detector control: an unplannable path is never treated as a
  clear one. During the move, if the first error comes at or after the first
  `SetMagnetDAC` (a set whose reply timed out was still delivered, a
  `GetMagnetMoving` error, or `max_wait`), the protocol waits the settle time
  (`[magnet].settle_ms`) before `BlankBeam False` and `ProtectDetector Off`. An
  error before any set cleans up at once. The normal path is unchanged: poll
  `GetMagnetMoving`, and settle only if motion was never reported.
- **Beam blank:** `BlankBeam True|False`.
- **Detector control:** caps `Gain | Deflection | Protect`. `protect` sends
  `ProtectDetector <det>,On|Off` (the magnet-move form). `set_deflection` /
  `read_deflection` and `set_gain` / `read_gain` use the per-detector commands.
  An unknown channel is `Config`. `set_cdd_voltage` is the interface default
  (`Config`, unsupported).
- **Source:** `set_hv` is `SetHV v` expecting `ok`; `read_hv` is
  `GetHighVoltage`. Source ramping is not implemented: `set_hv` and
  `set_param` write the value in a single step. `params()` advertises the
  codec's canonical parameter map, restricted to verified names. The codec
  marks each name `verified` when legacy pychron's Python sends it; only
  verified names are ever sent. A verified entry is writable through
  `SetParameter <hardware name>,v` (reply `ok`) and readable through
  `GetParameter`. An entry whose set name is unverified is not writable: it is
  advertised read-only under its readback name when that is verified
  (`emission`, read as `Source Current Readback`; `Electron Emission Set` is
  never sent) and not advertised at all otherwise (`esa_plus`, `esa_minus`).
  `set_param` on anything not writable is `Config` with nothing sent.
  `read_param` returns
  `Readback{setpoint = GetParameter <hardware name>, actual = GetParameter
  <readback name>}` when the map has a readback name, else `actual` unset. A
  `Custom` id is one exchange: a custom set name returns `actual` unset, and a
  custom readback name returns the same value as setpoint and actual. The
  two reads are separate exchanges, not one transaction, so another caller's
  command may fall between them. HV is always `SetHV` / `GetHighVoltage`,
  however it is addressed (`set_hv`, the HV parameter id or a custom name), and
  reads back as both setpoint and actual.
  Ranges: HV 0..10000 V; every other parameter a wide nominal range
  (`-1e6..1e6`, `Unit::None`) documented as unverified. `Custom(name)` ids are
  accepted only for verified hardware names the codec's map knows; the name is
  sent as given and range-checked through its canonical parameter's spec, and
  a readback name is read-only. Any other name is `Config` with nothing sent,
  including the unverified names in the map (`Electron Emission Set`,
  `ESA+ Set`, `ESA- Set`, and the aliases `H-Symmetry Set`, `Flatapole Set`,
  `Rotation Quad Set`, `Pole N Set`, `Pole S Set`).
- **Acquirer:** `integrates() == true`. `channels()` is the `channels` key.
  - `configure(t)`: non-positive is `Config`. Snap with
    `qtegra::snap_integration_time`. If the snapped value differs from the
    cached one, send `SetIntegrationTime` and set the next-due time to
    `now + settle_periods * snapped`; otherwise send nothing.
  - `start()` / `stop()`: local state only (Qtegra free-runs). `seq` is never
    reset.
  - `next(timeout)`: when not started, `Config`. If the next frame is not due,
    wait on the injected clock up to `timeout` and return nullopt (a zero
    timeout returns at once, with no wire traffic). When due: `GetData`,
    decoded tagged (`name,value,...`); next-due advances by one period (not by
    wall time elapsed, so the cadence does not drift; if more than one period
    behind, it resets to `now + period`).
  - Frame: `ts` = host clock at reply, `seq` incremented per attempt (a failed
    read consumes a number, as `PolledAcquirer`), `integrated = true`,
    `span = snapped period`, one value per configured channel that the reply
    names; channels the reply omits are simply absent (the engine reports them
    as no data). Names in the reply that are not configured channels are
    ignored; names match case-sensitively. A reply containing `ERROR`, an
    empty reply, an odd field count, a non-numeric, `nan` or `inf` value, or
    a duplicate name is `Protocol`. So is a well-formed reply that names none
    of the configured channels (wrong case, other detector names, untagged
    values with an even field count): it is never delivered as an empty
    frame, and the error carries the reply text (first 120 bytes).
  - The wire read runs outside the acquirer mutex, so `stop()` never waits
    for a transport timeout. A read in flight when the integration changes or
    the acquirer stops is dropped (that `next()` returns nullopt).
  - `trigger()`: the interface default (no-op).
- Acquirer state is guarded by one mutex; `next()` is safe against a
  concurrent `stop()`/`configure()`/`start()`.

### 5.3 Sim hook

`qtegra_sim_hook(std::shared_ptr<QtegraSimModel>)` returns a
`SimTransport::Hook` that answers every command in 5.2 from a small in-memory
model (DAC, moving-until time, blank, per-detector protect/deflection/gain, HV,
named parameters, integration time, per-channel intensities), so conformance
and parity tests run against the real driver. The model also logs every
command it receives, in order, for tests that check a sequence.

## 6. Config and examples

- `configs/examples/spectrometer.qtegra.toml`: a complete config in the shape
  of `spectrometer.sim-integrated.toml` (same detectors and field table), with
  a placeholder host and `port = 1069`. It has no `[source].ramp`: ramping is
  not implemented, and the example and `docs/dev_setup.md` say so.
- `configs/examples/spectrometer.qtegra.local.toml.example`: host and port.
- The example config loads and validates in a test (no transport opened).
- `bringup.cpp`'s `is_simulated` already classifies `thermo_qtegra` as real,
  so `pychron-ui --sim` refuses it.
- `docs/dev_setup.md`: how to point the config at an instrument and record a
  trace; the bring-up checklist in section 8.
- `tools/spec_router/spec_router/units.toml`: mark `thermo_qtegra_driver` as
  delivered here (goal text points at this spec) and exclude it from the
  router with `manual = true`, so the router does not rebuild it; `isotopx_ngx_driver`'s goal gains a pointer to the NGX notes.

## 7. Error handling

- Config errors (limits, unknown channel or parameter, non-positive
  integration) never touch the wire. Magnet limits are enforced twice: by the
  facade (positioner limits and `[magnet].limits`, stricter bound wins) and
  again by the driver's own `limit_min`/`limit_max`.
- Protocol errors (bad reply, `ERROR`) are returned with the reply text. A
  `GetData` reply that matches no configured channel is one of them.
- Only verified parameter names are writable; a write to an unverified or
  read-only parameter is `Config` and never touches the wire.
- A move whose detector protection cannot be planned (a failed correction,
  e.g. no reply to `GetHighVoltage`) fails with that error before anything is
  set, protected or blanked.
- A move that fails at or after the first `SetMagnetDAC` waits the settle
  time before unblanking and unprotecting, since the magnet may still be
  moving. If the cleanup itself then fails, the first error is returned and
  the detector may be left protected or the beam blanked.
- There is no source ramping: HV and trap current change in a single step.
- Io / NotConnected: one reconnect attempt (4.2), then the error.
- `next()` errors reach the engine, which raises its existing alarm and keeps
  polling; the scan status shows the error until Restart clears it, and
  readings resume without a Restart once the link is back.
- A `reconnects()` counter is exposed for tests and logging.

## 8. Risks (cannot be removed without an instrument)

1. No hardware capture exists; every wire detail is from reading pychron's
   Python. Synthetic traces are marked SYNTHETIC.
2. pychron reconnects per Qtegra command by default; this driver keeps one
   connection. If RemoteControlServer requires a fresh connection per command,
   a `reconnect_per_command` key is the follow-up.
3. pychron's default transport is UDP; only TCP is supported.
4. Source parameter units and ranges are unknown; ranges are nominal.

Bring-up checklist (manual, first contact with the instrument):

1. Confirm what Qtegra replies to each setter (a setter that gets no reply
   fails with a timeout).
2. Record a trace with `trace = true`.
3. Confirm the terminator.
4. Confirm whether RemoteControlServer accepts one persistent connection.
5. Confirm the `GetData` layout and detector names.
6. Confirm one integration change and one magnet move.
7. Confirm how soon `GetMagnetMoving` reports motion after `SetMagnetDAC`
   (the move protocol polls it immediately and settles only if motion was
   never reported; legacy pychron settles first).
8. Commit the trace under `tests/traces/thermo/` and replace the synthetic
   one.

## 9. Testing

Infrastructure:

- Assembler opens transports and calls `connect()`; an open failure and a
  connect failure each close what was opened and return the error; destructor
  closes.
- `Reconnector`: Io triggers one reopen + on_connect + retry; rate limit;
  non-Io errors untouched; failed reopen returns its error.
- Loader: `retries`/`trace` accepted and passed through; bad types are
  diagnostics.
- Engine: `stop()` returns only after an in-flight `next()` finished (blocking
  fake acquirer on a threaded scheduler); `start()` after `stop()` never
  overlaps the previous `next()`.

Driver:

- Scripted tests, one per behaviour in section 5.2, including every error
  path, and that Config errors write nothing.
- Registry: `create` from a TOML table; undeclared key rejected; schema lists
  the keys in 5.1.
- Conformance: `PositionerConformance`, `AcquirerConformance`,
  `SourceConformance`, `DetectorControlConformance` instantiated on the sim
  hook with a `ManualClock`.
- Replay of a synthetic trace under `tests/traces/thermo/`.
- Cadence without drift; settle after an integration change; missing and
  extra names in `GetData`; `ERROR` reply; reconnect then `connect()`.

System:

- Config parity: the sim-integrated example with its driver kind swapped for
  `thermo_qtegra` on the Qtegra sim hook assembles and positions, sets HV and
  acquires through `Spectrometer`.
- `ScanService` continuous scan on the driver over its sim hook delivers
  readings at the snapped period, and survives `set_integration` and a
  dropped link.
- The example config loads and validates.

## 10. Rollout

1. Engine quiescence.
2. `retries`/`trace` transport keys.
3. `IConnectable`, assembler open/connect/close, `Reconnector`.
4. Qtegra driver, sim hook, tests.
5. System tests, example config, docs, units.toml.

Each step builds and passes `ctest --preset dev` and `dev-ui` on its own.
