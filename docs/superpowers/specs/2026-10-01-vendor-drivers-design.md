# Vendor spectrometer drivers (Thermo Qtegra, Isotopx NGX)

Date: 2026-10-01
Status: Draft
Owner: Jake Ross
Depends on: `2026-09-29-spectrometer-control-design.md` (roles, Frame, acquisition
engine, assembler, config), `2026-09-29-instrument-control-design.md` (transport,
driver registry, declared keys), `2026-10-01-spectrometer-window-design.md`
(ScanService, continuous scans).
Implements units `thermo_qtegra_driver` and `isotopx_ngx_driver` from
`tools/spec_router/spec_router/units.toml`, plus the infrastructure they need.
Wire ground truth: legacy pychron Python (`~/Programming/pychron/pychron`):
`spectrometer/thermo/**`, `spectrometer/isotopx/**`,
`hardware/isotopx_spectrometer_controller.py`,
`hardware/core/communicators/ethernet_communicator.py`.

## 1. Goal

Two drivers that can run a real instrument through the existing `Spectrometer`
facade and the spectrometer window, and the missing plumbing that makes that
possible: opening and reconnecting transports, credentials, a continuous scan
on a trigger-driven backend, and an acquisition engine that is safe to restart.

Success: with a config naming `kind = "thermo_qtegra"` or `kind = "isotopx_ngx"`
and a simulated wire that speaks the vendor protocol, `Spectrometer` positions,
reads and sets source parameters, and a `ScanService` scan delivers readings at
the snapped integration period; every role passes the conformance suite.

## 2. Scope

In scope:

- Infrastructure: spectrometer transports opened and closed; `IConnectable`;
  reconnect on demand; `retries`/`trace` transport keys; secret driver keys in
  `*.local.toml`; redaction of secrets in traces and wire logs;
  `AcquisitionEngine::stop()` quiescence; `Transport::try_read`;
  `SimTransport` unsolicited input.
- `thermo_qtegra` driver and its stateful sim hook.
- `isotopx_ngx` driver and its stateful sim hook.
- Scripted, replay, conformance and config-parity tests; example configs.

Out of scope: UDP transport; reconnect-per-command; `SetIonCounterVoltage`,
`Reset`, sub-cup configuration and any other command pychron's Python never
sends on the paths below; the `NoIntensityChange` heuristic; NGX valve commands
(`OpenValve`, `SAB`, ...); NGX ATONA partial (`ACQ`) reporting; AF demagnetisation;
trap-current ramping; `elctl` spectrometer commands; a general transport-level
reconnect; OS keychains.

## 3. Decisions

| Decision | Choice | Reason |
|---|---|---|
| Scope | Drivers plus what they need to run | A driver that cannot be opened, logged in or scanned is not usable. |
| Credentials | Secret driver keys in `spectrometer.local.toml` | What the unit and the control spec say; owner's choice. |
| Unverified commands | Left out | Never send an instrument a command no reference implementation sends. |
| NGX continuous scan | The driver re-arms itself while started | No engine change; matches pychron's "already triggered: send nothing". |
| Frame timestamps | Host monotonic time at receipt, both vendors | The engine's stale guard compares against its own clock; NGX time-of-day has no date. |
| NGX wire access | `write` + `try_read`/`read` through the codec demultiplexer, never `exchange` | `exchange` discards buffered input, which would drop events. |
| Connection | One persistent TCP connection per driver | The C++ transport is persistent. pychron reconnects per command by default; see Risks. |
| Wire terminators | Config keys defaulting to what pychron sends (`\r`) | The NGX codec default (`#\r\n`) is marked unverified. |

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

`libs/devices/include/pychron/devices/reconnect.hpp`: a small helper the two
drivers share.

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
state change. Acquisition does not resume by itself after a reconnect: the
acquirer returns the error from `next()`, and `ScanService`'s Restart recovers.

### 4.3 Transport keys

Spectrometer `[transports.<name>]` additionally accepts `retries` (integer >= 0,
default 0) and `trace` (boolean, default false), passed to `make_transport`.
Traces are written under `SpectrometerOptions::trace_dir` (default `traces`),
created on demand. `*.local.toml` may still override only `host`, `port`,
`baud`, `timeout_ms` on transports.

### 4.4 Secret driver keys

- `ConfigKey` gains `bool secret = false`.
- The spectrometer config loader accepts `[drivers.<name>]` tables in
  `spectrometer.local.toml` for existing drivers. Each key there must be one the
  driver kind's schema declares `secret`; anything else is the existing "key may
  not be overridden locally" diagnostic, listing the allowed keys.
- A secret key present in the main config file is a diagnostic:
  `secret key '<key>' must be set in <local file name>, not here`.
- Diagnostics and `DriverRegistry` listings print a secret key's name and
  description, never a value. Config dump or snapshot code that prints driver
  options prints `***` for secret keys.
- `.gitignore` gains `*.local.toml`.

### 4.5 Redaction

`Transport` gains `virtual void redact(std::string secret) {}`. `TraceRecorder`
implements it: every occurrence of a registered secret (as bytes) in tx or rx
data is replaced by `***` in the trace file and in the mirrored wire-log
record; it also forwards to the inner transport. Empty strings are ignored. A
driver calls `transport.redact(password)` before the first login. A redacted
trace still replays: the replaying test logs in with the password `***`.

### 4.6 Acquisition engine quiescence

`AcquisitionEngine::stop()` returns only when no `poll()` is executing: each
poll registers itself for its duration; `stop()` cancels the scheduler jobs,
then waits until the count is zero, then calls each acquirer's `stop()`. A
`stop()` called from inside a poll (same thread) does not wait for itself.
`start()` after `stop()` therefore never overlaps a `next()` from the previous
run. The wait is bounded by the acquirer's own transport timeout.

### 4.7 Non-blocking read; unsolicited input in tests

```cpp
// Transport
// A complete frame already buffered or immediately readable, without waiting.
// nullopt when none is available. Never counted as a health failure; Io or
// NotConnected only when the connection is found broken.
virtual Result<std::optional<Bytes>> try_read(ReadSpec rs);
```

Implemented by `QueuedTransport` (queued like other calls), `TcpTransport`
(reads whatever the socket has without blocking, keeps the remainder pending),
`SimTransport` and `TraceRecorder` (records the rx when a frame is returned).
The base default returns `Config` "try_read not supported".

`SimTransport::inject(Bytes rx)` appends unsolicited input that `read` and
`try_read` see; in hooked mode a hook may also return extra bytes after a reply,
as today.

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
| `settle_periods` | float | 2.0 | integration periods to wait after an integration change |

### 5.2 Behaviour

- **connect():** `GetIntegrationTime`; a numeric reply is success and seeds the
  cached integration period.
- **Every command** is one `exchange(cmd.tx, *cmd.reply)` wrapped in the
  `Reconnector` and `observe(...)`. Multi-step sequences use `transact`.
- **Positioner (axis Dac):** `set(v)` outside limits is `Config` with nothing
  sent; else `SetMagnetDAC v` (reply ignored, as pychron). `read()` is
  `GetMagnetDAC`. `moving()` is `GetMagnetMoving` decoded with the codec's bool
  vocabulary.
- **Beam blank:** `BlankBeam True|False`.
- **Detector control:** caps `Gain | Deflection | Protect`. `protect` sends
  `ProtectDetector <det>,On|Off` (the magnet-move form). `set_deflection` /
  `read_deflection` and `set_gain` / `read_gain` use the per-detector commands.
  An unknown channel is `Config`. `set_cdd_voltage` is the interface default
  (`Config`, unsupported).
- **Source:** `set_hv` is `SetHV v` expecting `ok`; `read_hv` is
  `GetHighVoltage`. `params()` advertises the codec's canonical parameter map:
  each entry writable through `SetParameter <hardware name>,v` (reply `ok`) and
  readable through `GetParameter`. `read_param` returns
  `Readback{setpoint = GetParameter <hardware name>, actual = GetParameter
  <readback name>}` when the map has a readback name, else `actual` unset.
  Ranges: HV 0..10000 V; every other parameter a wide nominal range
  (`-1e6..1e6`, `Unit::None`) documented as unverified. `Custom(name)` ids are
  accepted for hardware names not in the map (Helix DAC names), read/write
  through the same two commands.
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
    ignored. A reply containing `ERROR`, or non-numeric, is `Protocol`.
  - `trigger()`: the interface default (no-op).
- All acquirer state is guarded by one mutex; `next()` is safe against a
  concurrent `stop()`/`configure()`/`start()`.

### 5.3 Sim hook

`qtegra_sim_hook(std::shared_ptr<QtegraSimModel>)` returns a
`SimTransport::Hook` that answers every command in 5.2 from a small in-memory
model (DAC, moving-until time, blank, per-detector protect/deflection/gain, HV,
named parameters, integration time, per-channel intensities), so conformance
and parity tests run against the real driver.

## 6. `isotopx_ngx` driver

`libs/devices/include/pychron/devices/spectrometer/isotopx_ngx.hpp`, `.cpp`;
class `NgxSpectrometer final : public Device, public IConnectable,
public IMassPositioner, public IBeamSource, public IIntensityAcquirer,
public IDetectorControl`. Registered as `isotopx_ngx`. No `IBeamBlank`. All
wire text comes from `pychron::codec::ngx`.

### 6.1 Declared keys

| Key | Type | Default | Meaning |
|---|---|---|---|
| `roles` | string array | - | informational |
| `channels` | string array | `H2 H1 AX L1 L2 CDD` | detector order; events carry values in reverse of this |
| `username` | string, **secret** | empty | login user; empty skips login |
| `password` | string, **secret** | empty | login password |
| `send_terminator` | string | `cr` | `cr`, `lf`, `crlf`, or `hash_crlf` (`#\r\n`) |
| `rcs_id` | string | `NOM` | acquisition id sent with `StartAcq` and matched in events |
| `limit_min`, `limit_max` | float | 0, 200 | mass limits (amu) |
| `settle_ms` | integer | 500 | delay argument of `SetMass` |
| `reply_timeout_ms` | integer | 3000 | wait for a command reply while events interleave |
| `stale_limit` | integer | 3 | consecutive stale events before a restart |

### 6.2 Wire handling

- The driver never calls `exchange`. One mutex guards the wire and a codec
  `Demultiplexer`.
- **pump():** drain `try_read(until "#\r\n")` into the demultiplexer until
  nullopt. `AcqFrame`s go to the event handler (6.4); `Reply`s go to a one-slot
  reply buffer (a reply nobody is waiting for is dropped and counted);
  `OtherEvent`s are ignored; malformed lines are counted and dropped.
- **command(cmd):** inside `transact`: `write(cmd.tx)`, then `read(until
  "#\r\n", remaining)` feeding the demultiplexer until a `Reply` arrives or
  `reply_timeout_ms` elapses (`Timeout`). Events read meanwhile are handled as
  in pump(). The reply is decoded with the codec (`E00` ok; `Exx` mapped by
  `ngx::error_kind`).
- Commands are wrapped in the `Reconnector` (with `connect()` as the
  on-connect step) and `observe(...)`.

### 6.3 connect()

`redact(password)` if non-empty. Read one banner line (`read(until "\r\n")`,
transport timeout); a timeout is not an error (pychron: "no initial response
... skipping Login" still proceeds). If `username` is non-empty, send
`Login user,password` and require `E00`; any other reply is `Protocol`
"login rejected" (the password is not included in the message). Clear the
demultiplexer first, so a reconnect starts clean. If an acquisition was
started, mark it not armed so the next `next()` re-arms it.

### 6.4 Acquirer

- `integrates() == true`; `channels()` is the `channels` key.
- `configure(t)`: non-positive is `Config`. Period = `max(1, round(t))` whole
  seconds. Sends `StopAcq`, then `SetAcqPeriod 1000`; marks not armed.
- `start()`: marks started and arms: `StopAcq`, then `StartAcq <N>,<rcs_id>`.
- **Re-arming:** when an `ACQ.B` for our `rcs_id` completes a period and the
  acquirer is still started, the driver sends the next `StartAcq` itself.
  `trigger()` arms only if started and not armed; otherwise it sends nothing.
- `stop()`: `StopAcq`; marks stopped and not armed; clears queued frames.
- Events: only `#EVENT:ACQ.B,<rcs_id>,...` produces a frame. Plain `ACQ`
  partials and events for another `rcs_id` are ignored. A value-count mismatch
  with `channels` drops the event and counts it.
- **Stale guard:** an `ACQ.B` whose instrument clock time does not advance on
  the previous accepted one is dropped (a jump backwards of more than 12 h is
  treated as midnight rollover and accepted). `stale_limit` consecutive drops:
  `StopAcq`, re-arm, counter reset.
- Frame: `ts` = host clock when the event was read, `seq` = driver counter
  (never reset), `integrated = true`, `span = N` seconds, values in `channels`
  order.
- `next(timeout)`: when not started, `Config`. pump(); if a frame is queued,
  return it. Otherwise wait on the injected clock in short slices (pump each)
  up to `timeout`, then nullopt. A zero timeout pumps once.
- If started and no `ACQ.B` arrives within `2 * max(3, 1.5 N + 2)` seconds of
  arming (pychron's deadline), the driver sends `StopAcq` and re-arms once; the
  engine's stall alarm covers a persistent failure.

### 6.5 Positioner (axis Mass)

`set(m)` outside limits is `Config`, nothing sent. Else `StopAcq`, then
`SetMass m,<settle_ms>`; marks not armed so acquisition re-arms. `read()` is
`StopAcq` then `GETMASS`. `moving()` is the interface default (false). The
`,deflect` form is not sent (no beam-blank role).

### 6.6 Source

`set_hv` / `read_hv` are `SSO IE, v` / `GSO IE` (actual). `params()` advertises
the codec's `ngx::Param` list mapped to canonical `SourceParam` where one
exists (IonEnergy->HV, TrapCurrent, TrapVoltage, ElectronEnergy, IonRepeller,
EmissionCurrent->Emission, ...; the mapping table is part of the driver and
tested), others as `Custom(mnemonic)`. Writes are `SSO <mn>, v`; reads are
`GSO <mn>` giving `Readback{setpoint, actual}`. Ranges as in 5.2: HV 0..10000,
others wide nominal, unverified.

### 6.7 Detector control

Caps `Protect` only. `protect(channel, on)` records a flag per channel and
sends nothing (pychron sets a flag only). Unknown channel is `Config`.

### 6.8 Sim hook

`ngx_sim_hook(std::shared_ptr<NgxSimModel>)`: answers Login (checking
credentials), GETMASS/SetMass, SSO/GSO, StopAcq, SetAcqPeriod, and on
`StartAcq N,id` queues `N-1` `ACQ` lines and one `ACQ.B` line (reverse channel
order, advancing clock time), released as the model's clock advances. The model
can also emit a stale event, a foreign `rcs_id` event and a malformed line on
request, and can drop the connection.

## 7. Config and examples

- `configs/examples/spectrometer.qtegra.toml` and `spectrometer.ngx.toml`:
  complete configs in the shape of `spectrometer.sim-integrated.toml` (same
  detectors and field table for Qtegra; mass axis, no `beam_blank_threshold`,
  no deflection for NGX), with placeholder hosts.
- `configs/examples/spectrometer.ngx.local.toml.example`: host, port and
  `[drivers.ngx] username/password`.
- Both example configs load and validate in a test (no transport opened).
- `bringup.cpp`'s `is_simulated` already classifies both kinds as real, so
  `pychron-ui --sim` refuses them.
- `docs/dev_setup.md`: how to point a config at an instrument and record a
  trace; the bring-up checklist in section 9.
- `tools/spec_router/spec_router/units.toml`: mark both driver units as
  delivered here (goal text points at this spec), so the router does not
  rebuild them.

## 8. Error handling

- Config errors (limits, unknown channel or parameter, non-positive
  integration) never touch the wire.
- Protocol errors (bad reply, `ERROR`, `Exx`) are returned with the reply text,
  never with a secret.
- Io / NotConnected: one reconnect attempt (4.2), then the error.
- `next()` errors reach the engine, which raises its existing alarm; the scan
  status shows the error and Restart recovers.
- Driver counters (dropped replies, malformed lines, stale events, reconnects)
  are exposed through a `stats()` accessor for tests and logging.

## 9. Risks (cannot be removed without an instrument)

1. No hardware capture exists; every wire detail is from reading pychron's
   Python. Synthetic traces are marked SYNTHETIC.
2. pychron reconnects per Qtegra command by default; this driver keeps one
   connection. If RemoteControlServer requires a fresh connection per command,
   a `reconnect_per_command` key is the follow-up.
3. pychron's default transport is UDP; only TCP is supported.
4. Source parameter units and ranges are unknown; ranges are nominal.
5. NGX login reply, banner and `SetAcqPeriod` units are as pychron's simulator
   and tests show them, not as an instrument does.

Bring-up checklist (manual, first contact with each instrument): record a
trace with `trace = true`; confirm terminators; confirm `GetData` layout and
detector names; confirm login; confirm one integration change and one magnet
move; commit the (redacted) trace under `tests/traces/<vendor>/` and replace
the synthetic one.

## 10. Testing

Infrastructure:

- Assembler opens transports and calls `connect()`; an open failure and a
  connect failure each close what was opened and return the error; destructor
  closes.
- `Reconnector`: Io triggers one reopen + on_connect + retry; rate limit;
  non-Io errors untouched; failed reopen returns its error.
- Loader: `retries`/`trace` accepted; secret key in main config rejected;
  secret key accepted from local; non-secret driver key in local rejected;
  values never appear in any diagnostic text.
- `TraceRecorder` redaction in the trace file and the wire log; replay of a
  redacted trace.
- Engine: `stop()` returns only after an in-flight `next()` finished (blocking
  fake acquirer on a threaded scheduler); `start()` after `stop()` never
  overlaps the previous `next()`.
- `try_read` on Sim, Tcp (loopback) and through `TraceRecorder`;
  `SimTransport::inject`.

Each driver:

- Scripted tests, one per behaviour in sections 5.2 / 6.2-6.7, including every
  error path, and that Config errors write nothing.
- Registry: `create` from a TOML table; undeclared key rejected; schema lists
  the keys in 5.1 / 6.1 with `secret` set on NGX credentials.
- Conformance: `PositionerConformance`, `AcquirerConformance`,
  `SourceConformance`, `DetectorControlConformance` instantiated on the sim
  hook with a `ManualClock`.
- Replay of a synthetic trace under `tests/traces/thermo/` and
  `tests/traces/isotopx/`.
- Qtegra: cadence without drift; settle after an integration change; missing
  and extra names in `GetData`; `ERROR` reply; reconnect then `connect()`.
- NGX: login accepted, rejected, skipped with empty username; banner timeout;
  password absent from the trace, the wire log and every error message; event
  arriving in place of a reply; reverse channel order; re-arm after `ACQ.B`;
  foreign `rcs_id`; count mismatch; stale guard and restart; midnight rollover;
  re-login after a dropped connection; `SetMass` stops and re-arms.

System:

- Config parity: the sim-integrated example with its driver kind swapped for
  `thermo_qtegra` on the Qtegra sim hook assembles and positions, sets HV and
  acquires through `Spectrometer`.
- `ScanService` continuous scan on each driver over its sim hook delivers
  readings at the snapped period, and survives `set_integration`.
- Both example configs load and validate.

## 11. Rollout

1. Engine quiescence.
2. `try_read`, `SimTransport::inject`, `redact`.
3. Secret keys, `retries`/`trace` keys, `.gitignore`.
4. `IConnectable`, assembler open/connect/close, `Reconnector`.
5. Qtegra driver, sim hook, tests.
6. NGX driver, sim hook, tests.
7. System tests, example configs, docs, units.toml.

Each step builds and passes `ctest --preset dev` and `dev-ui` on its own.
