# Isotopx NGX driver

Date: 2026-10-03
Status: Draft
Owner: Jake Ross
Builds on: `2026-10-01-ngx-driver-notes.md` (history, failure modes,
invariants), `libs/codecs/.../isotopx_ngx.hpp` (wire text), the
installation wizard spec (the NGX profile needs this driver).

Nothing here has been run against an instrument. Every protocol fact below
comes from legacy pychron HEAD (`NMGRL/pychron` 6ccadb4) and is marked
**[legacy]**; anything decided here without legacy evidence is marked
**[decision]**; anything only an instrument can settle is a **bring-up
item** (section 9).

## 1. Shape

```
                 ┌───────────── NgxLink (one per NGX controller) ──────────────┐
 Transport (tcp) │ reader thread: frames lines, routes                          │
 ──────────────► │   "#EVENT..." -> EventQueue (generation-stamped)            │
                 │   other lines  -> ReplySlot (the one in-flight command)     │
                 │ ask(cmd): short command mutex, write, await reply          │
                 │ session: banner + Login on connect and on every reconnect  │
                 └──────────────────────────────────────────────────────────────┘
        ▲                          ▲                               ▲
 NgxSpectrometer            NgxSpectrometer                  NgxValves
 (positioner, source)       (acquirer: arm/StartAcq/frames)  (IValveActuator: SAB 1, Open/Close, SAB 0)
```

- `NgxLink` (`libs/devices`, `pychron/devices/spectrometer/ngx_link.hpp`)
  owns the only reader of the socket. Clients never touch the transport.
- `NgxSpectrometer`, config kind `isotopx_ngx`: `IMassPositioner` (native
  axis **mass**), `IBeamSource`, `IIntensityAcquirer`, `IConnectable`. No
  `IDetectorControl` and no `IBeamBlank`: legacy sends no detector, gain,
  deflection or blank commands **[legacy]** (the magnet's `deflect` flag is
  the only blanking, section 5).
- `NgxValves`, config kind `ngx_valves`: `IValveActuator` for valves wired to
  the NGX controller's outputs.

## 2. Sharing one connection across two config files

The extraction line (`extraction_line.toml`) and the spectrometer
(`spectrometer.toml`) each open every transport they declare. The NGX
controller gets exactly one TCP transport, declared in one of them; the other
declares a transport of the new kind `link`:

```toml
# spectrometer.toml (owns the socket)
[transports.ngx]
kind = "tcp"
host = "192.168.0.20"
port = 1099
[drivers.ngx]
kind = "isotopx_ngx"
transport = "ngx"
link = "ngx"                 # registry name; default: the driver name

# extraction_line.toml (no socket of its own)
[transports.ngx]
kind = "link"
link = "ngx"
[drivers.ngx_valves]
kind = "ngx_valves"
transport = "ngx"
```

- `kind = "link"` builds a `LinkTransport`: open/close succeed and do
  nothing; exchange/read/write fail Config ("transport 'ngx' is a link to
  NGX link 'ngx'; only NGX drivers can use it"), so a link transport under
  any other driver fails at its first command with that message.
- `NgxLinkRegistry` (process-wide, mutex-protected, `weak_ptr` entries):
  a driver whose transport is a real one creates the `NgxLink` on it and
  registers it under `link`; a driver whose transport is a `LinkTransport`
  looks the link up **at use time** (not at construction, so load order does
  not matter). A missing link is NotConnected: "NGX link 'ngx' is not running
  (is the spectrometer loaded?)". Registering a second link under one name is
  Config.
- Either side may own the socket. With only an extraction line loaded
  (`elctl state`, `elctl probe`), the line owns it and valves work without a
  spectrometer **[decision]**.

## 3. The link

### 3.1 Framing **[legacy + decision]**

Legacy reads events with `readline("#\r\n")` and replies with one `recv`
(no terminator), unstripped; the banner with `readline("\r\n")`. Replies
therefore end in `\r\n` at least; events end in `#\r\n`. The link frames on
`\n`, strips a trailing `\r` and, for event lines, the trailing `#`. A line
that starts with `#EVENT` is an event, anything else a reply. This accepts
both reply forms (`E00\r\n` and `E00#\r\n`) **[decision]**. The codec's
`Demultiplexer` changes to this framing (it framed everything on `#\r\n`,
which would never complete a reply ending `\r\n`).

Send terminator: legacy appends the communicator's `write_terminator`,
default `\r` **[legacy]**. Option `send_terminator` (escape-aware string),
default `"\r"`; the codec's default changes from `"#\r\n"` to `"\r"`.
Bring-up item 1.

### 3.2 Reader

One thread per link: `transport.read(until "\n", 50 ms)` in a loop.
Timeout -> loop (the TCP transport keeps partial input across reads, so a
line split by a timeout is not lost). Io / NotConnected -> the link is
**down**: the reply slot fails NotConnected, the acquisition (if any) fails,
the reader backs off (250 ms .. 5 s) and reconnects (section 3.4). The read
timeout bounds how long a write waits behind a read on the transport's
worker (writes and reads share the queued transport).

### 3.3 Commands

`Result<std::string> ask(command, timeout)`:
- one command in flight (command mutex, held only for the exchange, never
  across an integration) **[invariant 2, 3]**;
- the reply slot is armed with a command number before the write; the reader
  hands the next non-event line to the armed slot; a reply with no armed slot
  is logged and dropped;
- on timeout the slot stays "owed": the next unsolicited reply within
  `late_reply_window` (default 2 s) is consumed as the late reply and
  dropped, so it is never handed to the next command **[invariant 2]**;
- `Exx` replies (other than `E00` where success is `E00`) become errors via
  `codec::ngx::error_kind` / `error_text` ("NGX E41 ERR_BUSY: StartAcq 10,NOM");
- no automatic re-send. Callers that may retry say so (valve status re-read,
  section 6) **[invariant 13]**.

### 3.4 Session

On open and after every reconnect, before any queued command
**[invariant 17]**: wait up to `banner_timeout` (default 2 s) for one line
(the banner; logged, content not checked **[legacy]**); then, if
credentials are configured, `Login user,password`. The reply must not be an
`Exx` (legacy ignores it; `E42 ACCESS_DENIED` here is a connect failure with
that message) **[decision]**. No banner: legacy skips Login; here Login is
still sent when credentials exist, and a bring-up item records which is
right (bring-up item 2).

Credentials: `user` in the driver options; the password from
`password_env` (an environment variable) or a `password` key in the
spectrometer's `*.local.toml` (machine-local, git-ignored). The link's own
errors show the Login as "Login user,****". Wire traces (`trace = true`)
record bytes verbatim, Login included: do not enable a trace on an NGX
transport with credentials until the trace recorder can redact (follow-up).

Implementation notes: the reader uses `Transport::poll()` (added for this),
a read for unsolicited input whose "nothing yet" is neither a failure nor a
health change, so an idle link keeps its transport Connected; it polls every
10 ms (`read_timeout`), which also bounds how long a command's write waits
behind a read. `SimTransport::hooked(hook, options, unsolicited)` gained the
event source.

A reconnect bumps the acquisition generation (section 4) so nothing from the
old session counts.

## 4. Acquisition **[legacy unless marked]**

- Period fixed at 1 s: `SetAcqPeriod 1000` once per session and after every
  integration change. Integration time N s is `StartAcq N,<rcs_id>` with
  N in {1, 2, 3, 4, 5, 10, 20, 100} (pychron's `ISOTOPX_INTEGRATION_TIMES`);
  `configure()` snaps to the nearest, reported as `Frame::span`.
- The instrument emits one `#EVENT:ACQ,<rcs>,...` per second and a buffered
  `#EVENT:ACQ.B,<rcs>,...` at the end.
- **Completion** (option `completion`): `"acq_b"` (default) -> the `ACQ.B`
  event is the integrated frame; `"last_acq"` -> the N-th `ACQ` is. Legacy
  uses ACQ.B when CDD/ATONA detectors are configured and otherwise treats
  each ACQ as complete, which reads a 1 s value for any integration time;
  this driver does not copy that **[decision]**; bring-up item 3.
- Values arrive in reverse detector order; `channels` is the detector list in
  pychron order and the codec returns channel order **[legacy]**. A frame
  whose value count differs from `channels` is Protocol (legacy warns and
  skips).
- Intermediate ACQ frames (with `completion = "acq_b"`) are not returned by
  `next()` in this version **[decision]**; legacy plots them for CDD/ATONA
  only.
- **State machine** (one mutex): idle -> armed (StartAcq sent, `E00`
  received) -> complete | aborted. `trigger()` from idle or complete arms;
  from armed it is a no-op (never a second `StartAcq`: `E43`)
  **[invariant 5]**. Each arm has a generation; the reader stamps events with
  the generation current when they arrive and the acquirer drops older ones
  **[invariant 6]**; events while idle are dropped.
- `start()`: `StopAcq` (reply ignored, legacy), `SetAcqPeriod 1000`, idle.
  `stop()`: `StopAcq`, idle, flush. `next(timeout)`: waits for the
  completing event of the current generation; returns nullopt on timeout;
  an arm older than `integration + 2 s + timeout` with no events is a
  Timeout error and returns to idle with `StopAcq` **[invariant 16]**.
- Timestamps: `Frame::ts` is the host clock when the completing event was
  read **[decision]**; the instrument clock time from the event is kept in
  the frame's metadata for diagnostics only (no date, legacy combines with
  today and never handles midnight).
- `SAB` (valve actuation, section 6) does not touch acquisition state
  **[invariant 11]**.

## 5. Magnet and source **[legacy]**

- Native axis **mass** (`IMassPositioner::Axis::Mass`); limits 0..200
  (pychron `dacmin/dacmax`). The spectrometer facade's field table maps
  isotope/detector to this axis (the NGX "mftable" is a mass table).
- `set(mass)`: if armed, the acquisition is **aborted** (StopAcq; `next()`
  returns Cancelled "aborted by magnet move", not a timeout)
  **[invariant 7, 18]**; then `SetMass <mass>,<settle_ms>[,deflect]`
  (`settle_ms` option, default 500; `deflect` when `|Δmass| >
  deflect_threshold`, option, default 0.5, 0 disables); reply must not be
  `Exx`.
- `read()`: `GETMASS` when idle; while armed, the last commanded value
  (legacy sends StopAcq first, killing the integration; this does not)
  **[invariant 9, decision]**.
- Source: HV = `IonEnergy` (`SSO IE, <v>` / `GSO IE` -> "setpoint,readback",
  readback used). Every `codec::ngx::Param` is advertised readable and
  writable through `SSO` / `GSO` under its pychron name (IonEnergy, YFocus,
  ..., ESA+Plate) with nominal ranges; `TrapCurrent` and `EmissionCurrent`
  read-only (legacy only reads them) **[decision]**.

## 6. Valves (`ngx_valves`) **[legacy + invariants]**

- Actuate: one unit under the link's valve mutex (concurrent actuations
  serialised): `SAB 1`; `OpenValve <addr>` / `CloseValve <addr>` -> must be
  `E00`; `SAB 0` on every exit path, including errors **[invariant 10]**.
  Never stops, re-arms or discards an integration **[invariant 11]**.
- Read: `GetValveStatus <addr>` -> `OPEN` | `CLOSED`; `E00` in place of a
  status (legacy's known oddity) -> re-read once; anything else -> Protocol,
  never "closed" **[invariant 12]**. Retries bounded by `status_retries`
  (default 2).
- Options: `link`, `status_retries`. Addresses are the valves' `address`
  strings from the line config.

## 7. Simulation

- `SimTransport` gains unsolicited input: `hooked(hook, options,
  unsolicited)` where `unsolicited()` is polled by `do_read` when no reply is
  buffered and returns bytes that are due (empty: none). Lets a simulator
  emit events on its own clock **[notes section 6]**.
- `NgxSimModel` + `ngx_sim_hook` / `ngx_sim_events`
  (`thermo_qtegra_sim`'s pattern): banner on open, Login, StartAcq N ->
  `E00` then N ACQ events one per second and an ACQ.B on the model's clock,
  StopAcq ends the run, SetMass/GETMASS, SSO/GSO, valves with SAB tracking
  (records whether every actuation was wrapped), fault injection (event
  interleaved before a reply, late reply, E-code replies, E00 for status,
  dropped connection, missing ACQ.B).
- Application `--sim` keeps using `sim_integrated` for the spectrometer
  (beam model from the field table); `isotopx_ngx` against `NgxSimModel` is
  for driver tests and `elctl` bring-up rehearsal.

## 8. Tests

- Codec: framing on `\n`, both reply forms, events with/without trailing
  `#`, send terminator default.
- Link: reply routing with events interleaved before, after and between
  reply bytes; late reply consumed, never misattributed; Exx mapping; down ->
  reconnect -> banner + Login before the next command; Login redacted in
  traces.
- Acquirer: N ACQ + ACQ.B completes once; `last_acq` mode; old-generation
  events dropped after stop/reconfigure/magnet move; second trigger while
  armed sends nothing; timeout path sends StopAcq and returns idle; value
  count mismatch is Protocol.
- Magnet: move while armed aborts with Cancelled; deflect threshold; GETMASS
  policy.
- Valves: SAB 1/0 bracket on success and on every failure; status E00
  re-read bounded; garbage is Protocol; actuation during a 10 s integration
  completes without disturbing it.
- Stress under TSan: acquisition loop + valve actuations + magnet moves
  concurrently for a few thousand simulated seconds.
- Registry / `link` transport: either config owning the socket; missing
  link NotConnected; duplicate name Config; `LinkTransport` misuse Config.

## 9. Bring-up checklist (instrument)

1. Send terminator (`\r` vs `#\r\n`): watch a trace of `GETMASS`.
2. Banner and Login: does the instrument always greet; what does Login
   reply.
3. Completion: does ACQ.B arrive on a Faraday-only configuration; does the
   N-th ACQ equal ACQ.B.
4. Value order vs detector list (reverse, legacy) with one known beam.
5. `SAB` semantics: actuate a valve during a 20 s integration; compare the
   integrated value with and without the actuation.
6. `E00` answering `GetValveStatus`: frequency; is the re-read enough.
7. Event clock vs host clock offset (diagnostics only).

## 10. Out of scope

Peak hopping specifics beyond magnet moves, detector calibration (done in
IsoLinx at WiscAr per legacy notes), ATONA intermediate-frame plotting, the
NGX "read_port" second socket (legacy option, unused).
