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
Wire ground truth, since 2026-10-30: `PROTOCOL.md` and `protocol.json` of the
qtegra repository (RemoteControlServer.cs, protocol 1, version 20.2), copy at
`tests/traces/thermo/protocol.json`. Section 5.4 says what follows from it.
Before that date: legacy pychron Python (`~/Programming/pychron/pychron`):
`spectrometer/thermo/**`, `hardware/thermo_spectrometer_controller.py`,
`hardware/core/communicators/ethernet_communicator.py`. Where this spec and
`PROTOCOL.md` disagree, `PROTOCOL.md` wins.

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
over the spectrometer link); UDP to RemoteControlServer (5.4); reconnect-per-command;
any command the driver does not need on the paths below; the `NoIntensityChange`
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
| Wire terminator | Config key defaulting to `\r`, unchanged | RemoteControlServer ends a command at CR, LF or CRLF; `\r` is what legacy clients send. |
| Standard path | TCP with `KeepAlive` | No gRPC gateway. See 5.4. |

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

- **connect():** the session of 5.4, then `GetIntegrationTime`; a numeric reply
  is success and seeds the cached integration period.
- **Every command** is one `exchange(cmd.tx, *cmd.reply)` through the
  `QtegraLink`'s `Link` and `observe(...)`. All are `Repeat::Safe` but
  `ProtectDetector`: see 5.4.
- **Setter replies:** `SetMagnetDAC`, `BlankBeam`, `ProtectDetector`,
  `SetDeflection`, `SetGain` and `SetIntegrationTime` accept any reply that is
  not an explicit `Error` (pychron ignores these replies); an `Error` reply is
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
  `GetMagnetMoving` error, or `max_wait`), the protocol waits the configured
  settle time (`[magnet].settle_ms`), or the caller's settle if that is longer,
  before `BlankBeam false` and `ProtectDetector Off`. This holds for every
  move, including sweep steps and experiment peak hops, which pass a zero
  settle because they settle themselves after a successful move. An error
  before any set cleans up at once. The normal path is unchanged: poll
  `GetMagnetMoving`, and settle only if motion was never reported.
- **Beam blank:** `BlankBeam true|false`.
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
    ignored; names match case-sensitively. `Error: no data` (no scan yet) is
    no frame: `next()` returns nullopt, the period is used up, and the engine's
    health check reports a scan that never starts. Any other reply containing
    `Error`, in any case, an empty reply, an odd field count, a non-numeric, `nan` or `inf` value, or
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

Answers as RemoteControlServer on a `KeepAlive` connection: replies end in LF,
`Error: ...`, `Error: Invalid Command` for unknown command, `KeepAlive`,
`Describe` (`detectors`, `tag_data`, `describe_override` in the model), and every
command the codec can send. Hook sees bytes, not connections: never refuses
second `KeepAlive`, never closes connection that did not start with one.
`StrictRcs` in `tests/devices/spectrometer/test_qtegra_link.cpp` enforces that
rule on top of it.

### 5.4 RemoteControlServer session (2026-10-30)

What the driver does to talk to current RemoteControlServer (RCS). Read from
RCS source and `PROTOCOL.md`; never run against RCS in Qtegra.

Session. Owned by `QtegraLink`, not spectrometer driver: `qtegra_valves` or
`qtegra_gauges` alone on a connection need it too.

1. `KeepAlive`, expects `OK`. Without it RCS closes after one reply, and that
   reply has no line ending. Failure says to check RCS version and
   `Config.use_udp`, kind unchanged.
2. `Describe`, one line of JSON, parsed with nlohmann-json (`decode_describe`).
3. `check_server`: see below.
4. Owner's handshake: channels check, `GetIntegrationTime`.

Rules:

- `KeepAlive` anywhere but first on connection is RCS error, so link tracks it.
  Session opens before first command on connection, whoever sends it, and from
  `connect()` and `Link::probe()`. On session already open, `KeepAlive` is not
  sent; `Describe` and owner's handshake are repeated.
- Transient error (Timeout, Io, NotConnected) ends session; next one starts with
  `KeepAlive`. Every reconnect does.
- A definite refusal keeps `KeepAlive` state, so next try repeats the refusal
  and not RCS's complaint about second `KeepAlive`. Caller's command is not sent
  while session is refused: each attempt repeats `Describe` (and owner's
  handshake only as far as its own check) and returns the refusal.
- Idle 60 s (`Config.tcp_idle_timeout`) closes connection. Next command gets
  `Io`; `Link` reconnects and repeats. No periodic ping.
- Session state needs no generation: every `exchange()` runs inside `Link::call`'s
  transport transaction and a reconnect is one transaction, so a command never
  spans two connections.

Refused at connect, never carried on with:

| Condition | Error |
|---|---|
| `protocol` > `kProtocolVersion` (1) | `Protocol`, "update pychron" |
| `protocol` < 1 | `Protocol`, install current script |
| `Error: Invalid Command` to `Describe` (RCS older than `Describe`) | `Protocol`, install current script. No fallback: older RCS has no contract this driver can check |
| `tag_data` false | `Protocol`: `decode_data` needs names; set `Config.tag_data = true` |
| channel not in `detectors[].name` | `Config`, naming missing and known |

A detector not in `channels` is fine: `GetData` extra names already ignored.

Framing. Reply ends at LF only (`reply_spec`, `ReadSpec::until("\n")`): `lines`
replies hold CRs. `request_spec` stays CR or LF, for simulators. Write terminator
stays `\r` by default: RCS treats CR, LF, CRLF alike, config and legacy bytes
unchanged.

Errors. `Error` in any case is device error in `body()` and `data_body()`; message
kept in `Error::what`. `Error: no data` has `Error::code` `kNoData`.

Commands RCS lacks, replaced:

| Was | Now |
|---|---|
| `SetYSymmetry v` | `SetParameter Y-Symmetry Set,v` |
| `SetZSymmetry v` | `SetParameter Z-Symmetry Set,v` |
| `SetExtractionLens v` | `SetParameter Extraction Lens Set,v` |
| `GetExtractionSymmetry` | `GetParameter Extraction Symmetry Set` |
| `SetSubCupConfiguration n` | removed; `activate_cup_configuration(cup, sub_cup)` sends `ActivateCupConfiguration <cup> <sub_cup>`, no driver path yet |
| `Reset` | removed: no equivalent |
| `SetParameter ProtectDetector,<det>,On` | removed: `Error: invalid number`. `ProtectDetector <det>,On` |
| `BlankBeam True` | `BlankBeam true`: contract enum (RCS lower-cases, so old form also worked) |

`SetIonCounterVoltage`: in contract, sets `CDD Supply Set`. Kept.

`ProtectDetector` is never resent. RCS `On` saves current deflection, then sets
2000: second `On` saves 2000, later `Off` restores 2000 and detector stays
deflected. `protect()` is one Safe call of `Ping`, then `ProtectDetector` with
`Resend::Never`: connection dropped while idle fails at `Ping` and is repaired,
so first move after idle does not abort; lost reply to `ProtectDetector` itself is
error with `kOutcomeUnknown`. `BlankBeam` is safe: RCS guards it with `IsBLANKED`.

UDP. Not supported. RCS UDP replies have no line ending; `KeepAlive` is TCP only.
`make_qtegra_link` refuses `UdpTransport` with `Config`. Needs datagram framing
in `read_frame` first. `legacy_line` still writes `kind = "udp"` for a Qtegra
actuator with no `kind=`; such a config fails at create with that error.

Contract test: `tests/codecs/test_thermo_qtegra_contract.cpp` checks every encoder
against `tests/traces/thermo/protocol.json`, name and argument shape, and reads
codec source so a new verb cannot skip it. Replace the copy when qtegra's
`protocol.json` changes (copied from qtegra `develop` at 1f2f856).

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
- Protocol errors (bad reply, `Error: ...`) are returned with the reply text. A
  `GetData` reply that matches no configured channel is one of them.
- Only verified parameter names are writable; a write to an unverified or
  read-only parameter is `Config` and never touches the wire.
- A move whose detector protection cannot be planned (a failed correction,
  e.g. no reply to `GetHighVoltage`) fails with that error before anything is
  set, protected or blanked.
- A move that fails at or after the first `SetMagnetDAC` waits the configured
  settle time (`[magnet].settle_ms`), or the caller's settle if longer, before
  unblanking and unprotecting, since the magnet may still be moving. Sweep
  steps and peak hops are included: their zero settle override applies only
  to a successful move. If the cleanup itself then fails, the first error is returned and
  the detector may be left protected or the beam blanked.
- There is no source ramping: HV and trap current change in a single step.
- Timeout / Io / NotConnected: the `Link` reconnects and repeats a Safe command
  until its call window passes, then returns the error (executor hardening spec).
  An Unsafe command is not repeated: its error has code `outcome_unknown`.
- `next()` errors reach the engine, which raises its existing alarm and keeps
  polling; the scan status shows the error until Restart clears it, and
  readings resume without a Restart once the link is back.
- A `reconnects()` counter is exposed for tests and logging.

## 8. Risks (cannot be removed without an instrument)

1. No hardware capture exists, and RemoteControlServer has never been run in
   Qtegra with `KeepAlive` or `Describe`. Every wire detail is from reading
   `PROTOCOL.md` and RCS source. Synthetic traces are marked SYNTHETIC.
2. RCS serves UDP or TCP, not both (`Config.use_udp`, default true, port 1069).
   This driver needs `use_udp = false`, and then UDP clients (Mass Spec, legacy
   pychron) cannot reach that server.
3. Source parameter units and ranges are unknown; ranges are nominal. Names such
   as `Extraction Symmetry Set` must be in the instrument's hardware database
   (`CheckParameters`).
4. A repeated `SetMagnetDAC` during a stepped move answers `Error: magnet is
   moving`. After a lost reply and reconnect the repeat can get that error for
   a move that did start.
5. The driver never sends `ProtectDetector On` twice for one call. Two `protect(det, true)` calls in a row still reach RCS; the second
   overwrites the saved deflection.

Bring-up checklist (manual, first contact with the instrument):

1. In `RemoteControlServer.cs` set `Config.use_udp = false`; leave
   `Config.tag_data = true`.
2. Set `channels` to detector names RCS reports. Run `Describe`:
   `detectors[].name`. Argus CDDs may be `L2(CDD)`, not `CDD`.
3. Check parameter names: `CheckParameters Y-Symmetry Set,Z-Symmetry Set,Extraction Lens Set,...`
   or `tools/smoke_test.py --parameters` in the qtegra repository.
4. Record a trace with `trace = true`. Check `connect()` sends `KeepAlive`,
   `Describe`, `GetIntegrationTime` and that replies end in LF.
5. Leave it idle over 60 s, send a command: expect one reconnect and success.
6. Confirm what Qtegra replies to each setter (a setter that gets no reply
   fails with a timeout). Check how long `SetIntegrationTime` takes against
   `timeout_ms`.
7. Confirm the `GetData` layout and detector names; `Error: no data` before the
   monitor scan starts.
8. Confirm one integration change and one magnet move, and one move large
   enough to be stepped.
9. Confirm how soon `GetMagnetMoving` reports motion after `SetMagnetDAC`
   (the move protocol polls it immediately and settles only if motion was
   never reported; legacy pychron settles first).
10. Confirm `ProtectDetector` On then Off restores the deflection.
11. Commit the trace under `tests/traces/thermo/` and replace the synthetic
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
  extra names in `GetData`; `Error` reply, any case; `Error: no data`;
  reconnect then `connect()`.
- Session (5.4): `KeepAlive` first on every connection, no second one on an open
  session, refusals, owner-less link, UDP refused.
- Contract: every encoder against `protocol.json`.

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
