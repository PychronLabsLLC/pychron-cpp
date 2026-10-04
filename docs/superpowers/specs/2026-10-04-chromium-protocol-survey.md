# Chromium laser: wire protocol survey and driver proposal

Date: 2026-10-04
Status: Survey (sections 1-6) and proposal (sections 7-9). Nothing is decided;
section 10 lists what has to be answered before a plan is written.
Owner: Jake Ross
Scope: the Photon Machines / Teledyne "Chromium" laser-ablation software as
legacy Pychron drives it: one TCP text protocol covering laser output, stage
and scan lists.
Builds on: 2026-10-03-legacy-laser-survey.md (§2 remote managers, §8 other
vocabularies), 2026-10-03-vision-design.md §2 (sub-project 4 of the laser
program).

Source: Python Pychron `main` at `26e77ad17`, read only:
`lasers/laser_managers/chromium_laser_manager.py`, `ethernet_laser_manager.py`,
`remote_laser_manager.py`, `base_lase_manager.py` (`_block`),
`hardware/pychron_device.py`, `hardware/core/communicators/ethernet_communicator.py`,
`lasers/stage_managers/remote_stage_manger.py`,
`experiment/utilities/position_regex.py`; and the two lab configs that have a
Chromium (melbourne, ASU). Nothing was run and no Chromium was available:
**everything about what Chromium replies is inferred from how Pychron parses
it**, and is tagged **[inferred]**. No vendor document was read.

## 1. What Chromium is to Pychron

A separate program on the laser PC that owns the laser, the stage and the
camera. Pychron is a thin client: it sends one line, reads one line. There is
no hardware driver on the Pychron side, no interlock logic, no monitor thread.

Three legacy classes, one behaviour:

| Class | Config dir | Difference |
|---|---|---|
| `ChromiumCO2Manager` | `chromium` | none |
| `ChromiumDiodeManager` | `chromium` | none |
| `ChromiumUVManager` | `chromium_uv` | `warmup()` runs the active scan |

## 2. Transport and framing

| Item | Value | Where from |
|---|---|---|
| Transport | TCP | both lab `initialization.xml`: `<kind>TCP</kind>` |
| Port | 1234 | both labs |
| Host | the laser PC (`excite_laser_pc…` at ASU; `setMe` at melbourne, plugin disabled) | lab config |
| Write terminator | `\r\n` | `setup_communicator` |
| Read terminator | `\r\n` | `setup_communicator` |
| Connection | **opened for each command and closed after its reply** | `<use_end>True</use_end>`: the communicator resets the socket after every `ask` |
| Reply | one line, whitespace stripped | communicator `strip = True` |
| Timeout | the plugin's `timeout`; communicator default 1 s | |

One command, one reply, always: every call goes through `_ask`. No unsolicited
messages are read.

## 3. Commands

Exactly these nine verbs are sent. Arguments are shown as Python formats them.

| Command | Sent when | Reply as Pychron uses it |
|---|---|---|
| `laser.output <v>` | set the output; `<v>` is percent, `str(float)` (`12.5`, `0`) | parsed as a float and compared with `<v>` within 0.1: an echo of the setpoint **[inferred]**. A reply that is not a number is ignored |
| `laser.fire` | open the beam | ignored |
| `laser.stop` | close the beam | ignored |
| `stage.pos?` | read position | `x,y,z` in **microns** |
| `stage.moveto x,y,z,xs,ys,zs` | move; microns, then three speeds | ignored |
| `stage.stop` | stop motion | ignored |
| `Scans.Status_Verbosity 1` | once, when the connection is first proved | ignored |
| `Scans.MoveTo <id>` | go to scan `<id>`'s start | ignored |
| `Scans.InPos? <id>` | poll after `MoveTo` | an integer; non-zero means in position **[inferred]** |
| `Scans.Run <id>` | UV `warmup()` | ignored |
| `Scans.Status?` | poll after `Run` | free text; two values are known (below) |
| `Scans.Stop` | on `disable_laser` | ignored |

Known `Scans.Status?` texts, compared case-insensitively:

- `Running: Warming up laser...` (warmup waits while the reply is this)
- `Idle: Idle` (the commented-out UV `extract` waited for this)

`stage.moveto` argument forms:

| Caller | x, y, z | Speeds |
|---|---|---|
| Move to a hole or an (x, y) | integers (`{:0.0f}`), mm × 1000, sign-corrected | `5000,5000,100` |
| Single axis (`set_x` / `set_y` / `set_z`) | **unformatted floats** (`v * 1000`, so `12500.0`), not sign-corrected | `10,10,0` |

The speed units are not stated anywhere **[unknown]**; µm/s is the natural
reading. A z speed of `0` on axis moves is as written.

What is never sent: any query of laser state, interlocks, errors or firmware;
any UV-specific setting (energy, repetition rate, spot size); any camera
command. Those live in Chromium's own UI and in its scan lists.

## 4. Behaviour around the commands

### 4.1 Opening

`open` builds the communicator; `opened()` reads `stage.pos?`, and only if that
answers sends `Scans.Status_Verbosity 1`. A position reply is the connection
test.

### 4.2 Output

`extract(value, units="watts", fire_laser=True)`:

1. watts: `value = calibration.get_input(value)` (the power-meter polynomial,
   `calibrated_power.cfg`); a negative result is clamped to 0 with a warning.
   percent: passed through.
2. `laser.output <value>`.
3. if `fire_laser`: sleep 1 s, `laser.fire`.
4. returns whether the echoed value is within 0.1 of the request; returns
   nothing at all if the reply is not a number.

`end_extract`: `laser.stop`, then `laser.output 0`, then stop a running
pattern. `enable_laser` sends nothing (it sets a flag). `disable_laser`:
`laser.stop`, `Scans.Stop`.

### 4.3 Moving

A position is one of:

- `s<N>` (regex `^[sS]\d+$`): a **scan** defined in Chromium. `Scans.MoveTo N`,
  then poll `Scans.InPos? N` until non-zero. `N` becomes the active scan.
- a hole name on the current tray, or an `(x, y)` pair in mm: clears the active
  scan, `stage.moveto` with the cached z, sleep 1 s, then poll `stage.pos?`
  once a second until every axis is within **10 µm** of the target.
- single-axis moves poll until that axis is within **2 µm**. They are refused
  unless "axis moves" have been enabled in the UI.

Position read-back divides by 1000 (mm) and multiplies by the configured signs
when `use_sign_position_correction` is set.

### 4.4 The poll loop (`_block`)

Every wait is the same loop: sleep `period`, ask, apply a test. It ends when
the test has passed on **three consecutive** polls (`cnt > nsuccess`, with
`nsuccess = 2`), on timeout, or when cancelled.

| Wait | Period | Timeout |
|---|---|---|
| Hole / xy move | 1 s | 50 s |
| Axis move | 0.25 s | 50 s |
| Scan `InPos?` | 0.25 s | 50 s |
| UV warmup | 0.25 s | 120 s |

A timeout **logs a warning and carries on**: the caller is not told the move
did not finish. A reply that fails to parse resets the count.

### 4.5 Patterns

Chromium has no pattern command. Pychron's own `PatternExecutor` drives the
stage point by point through `linear_move`, i.e. a `stage.moveto` per point.

### 4.6 UV

`warmup()`: with an active scan, `Scans.Run <id>` and (if blocking) wait while
status is `Running: Warming up laser...`. With no active scan it does nothing.

The scan-running `extract` for UV (run the scan, wait for `Idle: Idle`, up to
300 s) is **commented out** in this revision: a UV extraction today is
`laser.output` + `laser.fire` like the others. Whether that is intended is an
open question (§10).

## 5. Configuration

`devices/chromium/stage.cfg` (identical at melbourne and ASU, so probably the
shipped default, not a tuned file):

```ini
[Axes Limits]
x=0,50
y=0,50
z=0,50
[Signs]
x=1
y=1
z=1
```

Limits are mm. `[Signs] use_sign_position_correction` is optional and absent in
both. Connection settings are in `initialization.xml` (§2). Watts need
`calibrated_power.cfg`; neither lab has one in its Chromium directory.

No lab tree surveyed has an extraction script written for a Chromium.

## 6. Hazards in the legacy client

- **Silent failure.** Replies to `fire`, `stop`, `moveto` and the scan verbs
  are discarded; a refused command is indistinguishable from an accepted one.
  A timed-out move is only a log line.
- **Stale z.** Hole moves send the cached z, not a freshly read one.
- **No state read-back.** "Firing" and "enabled" are Pychron's own flags.
- **Inconsistent number formats and sign handling** between hole and axis moves
  (§3).
- **A connection per command**: every poll is a TCP connect and close.

## 7. Mapping to pychron-cpp interfaces [proposal]

`libs/devices/.../extraction/interfaces.hpp` already has what a Chromium needs.

| Interface | Method | Chromium |
|---|---|---|
| `IExtractionDevice` | `enable` | no wire command; proves the link with `stage.pos?` |
| | `disable` | `laser.stop`, `Scans.Stop` |
| | `extract(v, Percent)` | `laser.output v` |
| | `extract(v, Watts)` | calibration → percent → `laser.output`; unsupported with no calibration |
| | `end_extract` | `laser.stop`, `laser.output 0` |
| | `output` | the last accepted setpoint |
| `ILaserDevice` | `fire_laser` / `stop_laser` | `laser.fire` / `laser.stop` |
| | `is_firing` | tracked, not read: there is no query |
| | `warmup` | UV with an active scan: `Scans.Run`; otherwise nothing |
| `IStage` | `move_to_position("s12")` | `Scans.MoveTo 12` |
| | `move_to_position(hole)` | tray lookup → `stage.moveto` |
| | `set_axis` / `set_xy` | `stage.moveto` |
| | `position` | `stage.pos?` |
| | `moving` | one poll: `stage.pos?` against the target (or `Scans.InPos?`) |
| | `set_tray` / `positions` | tray maps: the laser system's (sub-project 2) |
| `IPatternRunner` | — | not the driver's: the laser system runs patterns over `IStage` |
| `IImaging` | — | not the driver's: screen capture is a vision frame source |

The interfaces say "start, then poll `moving()`", which fits: the driver does
one poll per `moving()` call and the caller owns the wait, the cancel token and
the timeout. The three-in-a-row rule moves into `moving()` as a small counter.

## 8. Driver shape [proposal]

Two pieces, as the NGX driver is built:

1. **Codec** (pure functions, no I/O): format each command, parse each reply.
   Tested against the exact strings of §3.
2. **`ChromiumLaser`** (a `Device` over a `Transport`): implements
   `IExtractionDevice`, `ILaserDevice`, `IStage`. Hole names are resolved by a
   function it is given, so it can be built and tested before tray maps exist.

Plus a **`ChromiumSim`** behind the sim transport: a stage that moves at the
commanded speed, an output echo, a firing flag, a few scans with a warmup
period. That is what tests and `--sim` talk to.

Config sketch:

```toml
[transports.laser_pc]
kind = "tcp"
host = "laser-pc.lab"
port = 1234
timeout_ms = 1000
connect_per_request = true    # Chromium closes after each reply (§2)

[drivers.laser]
kind = "chromium"             # or chromium_uv
transport = "laser_pc"
x_limits = [0, 50]            # mm
y_limits = [0, 50]
z_limits = [0, 50]
signs = [1, 1, 1]
move_speed = [5000, 5000, 100]
in_position_um = 10
```

`connect_per_request` does not exist on the TCP transport today and would be
added, unless Chromium turns out to keep a connection open (§10).

Deliberate departures from legacy:

- A move that does not arrive in time is a `Timeout` error, not a log line.
- Limits are checked before a move is sent (legacy reads them and never uses
  them).
- One number format for every move (integers, microns) and signs applied on
  every path.
- z for a hole move is read, not remembered.
- A non-numeric reply to `laser.output` is a `Protocol` error.

## 9. What this does not cover

Tray maps, stage calibration, patterns, the laser UI, `LabSession` wiring and
the config section for an extraction device are sub-projects 2 and 3. The
driver can be written and tested against the sim before them, and wired in
when they exist. Screen capture of Chromium's video window is the vision
library's frame source.

## 10. Open questions

Needed from someone with a Chromium, its API document, or a packet capture:

1. **Replies.** What does Chromium answer to `laser.fire`, `laser.stop`,
   `stage.moveto`, `stage.stop`, `Scans.MoveTo`, `Scans.Run`, `Scans.Stop`? Is
   there an `OK` / error convention the driver can check?
2. **Errors.** What comes back for a refused command (interlock open, out of
   range, unknown scan)?
3. **Connection.** Does Chromium require a new connection per command, or does
   Pychron just happen to do that?
4. **`laser.output`.** Is the reply the setpoint echoed, or the achieved
   output? Percent of what?
5. **Speeds.** Units of the three `stage.moveto` speeds; is `0` "default"?
6. **Scan status.** The full set of `Scans.Status?` texts, and what
   `Status_Verbosity 1` changes.
7. **UV.** Should a UV extraction run the scan (the commented-out path) or fire
   at a fixed output? Are energy and repetition rate ever set from Pychron?
8. **More of the API.** Is there a state/interlock query, a firing query or a
   moving query that would replace the client-side flags and position polling?
9. **Which Chromium first**: CO2 (melbourne, disabled), UV (ASU), or another
   lab's?
