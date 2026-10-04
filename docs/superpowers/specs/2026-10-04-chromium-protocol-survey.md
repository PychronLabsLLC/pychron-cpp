# Chromium laser: wire protocol survey and driver proposal

Date: 2026-10-04
Status: Survey (sections 1-6) and proposal (sections 7-9). Nothing is decided;
section 10 lists what is still open. Revised the same day against the vendor's
command reference (section 2a), which settled most of what the first draft
could only infer.
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
Chromium (melbourne, ASU). Nothing was run and no Chromium was available.

Vendor documents, on the owner's Drive under
`PychronConsulting/labs/support/chromium/` (not copied into the repo):

| Document | Date | Gives |
|---|---|---|
| *Chromium Software Command Interface Reference* (Photon Machines, S. Pinkham; "software 2013.12.30 or later") | 2013-12-30 | syntax, terminators, error codes, the whole command list |
| *Setting up a Chromium Laser System for Remote Control over RS232* | 2012-12-03 | serial setup, the Remote Control window, `sys.id?` as the link test |
| *Chromium 2.1 New Features and Changes* (addendum, 35 pp.) | 2014-09-17 | release notes; a few lines on the remote interface |

Also the Pychron wiki page "Chromium Laser" (setup and the scan workflow). A
web search found no public copy of the command reference.

Where the vendor reference and Pychron's behaviour disagree, both are stated.
What is still only inferred is tagged **[inferred]**.

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

## 2a. What the vendor reference says

Syntax: `<component>.<command> [value1,value2,...]<TERM>`. The first value
follows a space; values are comma separated. Commands are **not
case-sensitive**. Everything is 8-bit ASCII, numbers in decimal.

| Item | Vendor reference |
|---|---|
| Links | RS232 (19200 8N1, no handshaking), TCP/IP socket, or ActiveX COM scripting |
| TCP port | 1234 by default |
| Command terminator | **LF** (10) over TCP; **CR** (13) over RS232 |
| Reply terminator | **CR** |
| When Chromium replies | "only ... when requested by a command": queries (`...?`) answer; an action command answers nothing unless it fails |
| Error reply | `?<n>` + CR: `0` unimplemented, `1` unknown component, `2` unknown command, `3` bad or missing parameter, `4` execution error or not supported by the hardware |
| Enabling | Chromium must be running with the TCP/IP (or serial) interface ticked in its Remote Control window |

Command families (the reference lists each command; summarised here):

| Component | Sets | Queries |
|---|---|---|
| `Laser` | `Enable 0/1`, `Fire`, `Stop`, `Output <0-100 %>`, `Shutter 0/1`, `Mode` (Continuous, Burst, 1 Shot), `Burst <shots>`, `BurstTime <s>`, `Rate <Hz>`, `SpotSize <123um>`, `Slit` | `Enable?`, `Output?`, `Shutter?`, `Mode?`, `Rate?`, `Meter?` (power or energy, if fitted), `SpotSize?`, `SpotSizes?`, `Status?` (0 when every interlock is satisfied), `Interlocks?` (the tripped ones, comma separated) |
| `Stage` | `MoveTo X,Y,Z,SpeedX,SpeedY,SpeedZ`, `Step dX,dY,dZ,speeds`, `Jog speeds` (µm/s), `Home <X\|Y\|Z>`, `Stop` | `Pos?` (`X,Y,Z` µm), `Status?` (limit switches: 0, -1, +1 per axis), `Home?` (`Done`) |
| `Scans` | `MoveTo <n>`, `Run <n>`, `RunAll`, `Stop`, `Load <file>` (answers `OK`), `Save <file>`, `Clear`, `Settings <n>,<k=v;k=v>`, `Status_Verbosity <v>`; `Scan.Name <n>,<text>` | `Count?`, `Info? <n>`, `InPos? <n>` (1 or 0), `Pos? <n>`, `Settings? <n>` (`Laser.Output=27;LineSpacing=110`), `Status?`; `Scan.Name? <n>` |
| `PID` | `On`, `Off`, `Setpoint <°C>`, gains, `MaxOutput <%>`, `SeekErrLimit`, `SeekTimeout` | `Status?` (`OFF`, `SEEKING`, `LOCKED`, `TIMEOUT`), `Setpoint?`, gains |
| `Pyro` | `Emiss <%>` | `Temp?` (°C), `Emiss?` |
| `Gas` | `Mode` (Online, Bypass, Purge, Evacuate), `AutoFlow`, `MFC <n>,<l/min>` | `Mode?`, `AutoFlow?`, `MFC? <n>`, `MFC_Count` |
| `Sys` | `Msg <text>` (a pop-up on the laser PC) | `ID?` (`CHROMIUM 2013.1.1.0`), `Ver?`, `Time?` |
| `Svc` | `Home_Mtr <0 zoom \| 1 attenuator \| 2 spot>` | `Home_Mtr? <n>` |

Scan numbers are 1-based. From the 2.1 addendum: `Scans.InPos?`, the homing
commands and the PID gain commands were added in 2.x; commands for the optical
zoom and the lights exist but are **not in the reference we have**, so a newer
command set document exists; "problems with TCP/IP socket connections not
closing properly" were fixed in 2.x; a pre-ablation pass at 0 % output is how
Chromium itself warms a laser.

## 2. Transport and framing (as Pychron does it)

| Item | Value | Where from |
|---|---|---|
| Transport | TCP | both lab `initialization.xml`: `<kind>TCP</kind>` |
| Port | 1234 | both labs |
| Host | the laser PC (`excite_laser_pc…` at ASU; `setMe` at melbourne, plugin disabled) | lab config |
| Write terminator | `\r\n` (vendor: LF; the CR is tolerated, evidently) | `setup_communicator` |
| Read terminator | `\r\n` (vendor: replies end in CR only, so this never matches and the read ends on the timeout **[inferred]**) | `setup_communicator` |
| Connection | **opened for each command and closed after its reply** | `<use_end>True</use_end>`: the communicator resets the socket after every `ask` |
| Reply | one line, whitespace stripped | communicator `strip = True` |
| Timeout | the plugin's `timeout`; communicator default 1 s | |

One command, one read, always: every call goes through `_ask`. But per the
vendor reference an action command (`laser.fire`, `stage.moveto`, ...) sends
**nothing back**, so each of those reads waits out the timeout (4 s in the
wiki's setup) before Pychron moves on **[inferred]**. That, with a connection
per command, is why the legacy client is slow, and why its "replies" to those
commands are discarded: there are none.

## 3. Commands

Exactly these twelve verbs are sent. Arguments are shown as Python formats them.

| Command | Sent when | Reply as Pychron uses it |
|---|---|---|
| `laser.output <v>` | set the output; `<v>` is percent, `str(float)` (`12.5`, `0`) | parsed as a float and compared with `<v>` within 0.1; a reply that is not a number is ignored. The vendor reference documents no reply to the set form (`Laser.Output?` is the query), so the comparison probably never runs **[inferred]** |
| `laser.fire` | open the beam | ignored |
| `laser.stop` | close the beam | ignored |
| `stage.pos?` | read position | `x,y,z` in **microns** |
| `stage.moveto x,y,z,xs,ys,zs` | move; microns, then three speeds | ignored |
| `stage.stop` | stop motion | ignored |
| `Scans.Status_Verbosity 1` | once, when the connection is first proved | ignored |
| `Scans.MoveTo <id>` | go to scan `<id>`'s start | ignored |
| `Scans.InPos? <id>` | poll after `MoveTo` | `1` in position, `0` not (vendor) |
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

The vendor reference says of `Stage.MoveTo` only "all values are in microns";
`Stage.Jog` speeds are µm/s, so µm/s is the reading for these **[inferred]**.
For `Stage.Step` "if the speed ... is 0, the stage is not moved": the `0` z
speed Pychron sends on axis moves presumably means z stays put.

What Pychron never sends, though Chromium has it (§2a): `Laser.Enable`, any
interlock or status query, `Sys.ID?`, `Laser.Output?`, the shutter, firing
mode, repetition rate and spot size, limit-switch status, PID temperature
control, the pyrometer, scan settings. `enable_laser` in particular only sets
a flag in Pychron.

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
`laser.output` + `laser.fire` like the others. The Pychron wiki describes the
scan workflow as the way a UV is used (positions `s1,s2,s3`; move, then
`warmup(block=True)`, then `extract()` runs the scan), so the live code and
the documented workflow disagree (§10).

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

- **Silent failure.** Chromium answers a refused command with `?<n>`; Pychron
  never looks, so a refused `fire` or `moveto` is indistinguishable from an
  accepted one. A timed-out move is only a log line.
- **No interlock or enable check.** `Laser.Status?`, `Laser.Interlocks?` and
  `Laser.Enable` exist and are not used.
- **Every action command costs a timeout** (§2).
- **Stale z.** Hole moves send the cached z, not a freshly read one.
- **No state read-back.** "Firing" and "enabled" are Pychron's own flags.
- **Inconsistent number formats and sign handling** between hole and axis moves
  (§3).
- **A connection per command**: every poll is a TCP connect and close.

## 7. Mapping to pychron-cpp interfaces [proposal]

`libs/devices/.../extraction/interfaces.hpp` already has what a Chromium needs.

| Interface | Method | Chromium |
|---|---|---|
| `IExtractionDevice` | `prepare` | `Sys.ID?` (the link, and which Chromium), `Scans.Status_Verbosity 1` |
| | `enable` | `Laser.Status?` must be 0, else an Interlock error naming `Laser.Interlocks?`; then `Laser.Enable 1`, confirmed with `Laser.Enable?` |
| | `disable` | `Laser.Stop`, `Scans.Stop`, `Laser.Enable 0` |
| | `is_enabled` | `Laser.Enable?` |
| | `extract(v, Percent)` | `Laser.Output v`, confirmed with `Laser.Output?` |
| | `extract(v, Watts)` | calibration → percent → as above; unsupported with no calibration |
| | `extract(v, Celsius)` | `PID.Setpoint v`, `PID.On`: only where a pyrometer is fitted (a `?4` says it is not) |
| | `end_extract` | `Laser.Stop`, `PID.Off` if it was on, `Laser.Output 0` |
| | `output` | the last accepted setpoint |
| `ILaserDevice` | `fire_laser` / `stop_laser` | `Laser.Fire` / `Laser.Stop` |
| | `is_firing` | tracked: the reference has no firing query (`Laser.Shutter?` may serve, §10) |
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
There is no "moving?" query in the reference, so arrival is still position
against target; `Stage.Status?` adds a limit-switch check.

**Action commands answer nothing**, so the codec has two kinds of exchange:

- a *query*: write, read one CR-terminated line; `?<n>` is an error.
- an *action*: write, then confirm with the matching query where one exists
  (`Output?`, `Enable?`, `Shutter?`, `PID.Status?`). Where none exists (`Fire`,
  `Stop`, `MoveTo`, `Scans.Run`), read with a short timeout: silence is
  success, `?<n>` is the error. (Or follow with any cheap query and take an
  error line that arrives first; §10.)

Error mapping: `?1`/`?2` → Protocol (the driver sent something Chromium does
not know: a version mismatch), `?3` → Config (a bad value), `?4` → Io, with
"not supported by this hardware" in the message, `?0` → Protocol.

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
# one connection, kept open, unless a real Chromium proves to need one per
# command as legacy Pychron does (§10)

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

Framing is the vendor's: commands end in LF, replies in CR. A serial transport
(19200 8N1, CR both ways) is the same driver with a different terminator.

Deliberate departures from legacy:

- A move that does not arrive in time is a `Timeout` error, not a log line.
- Limits are checked before a move is sent (legacy reads them and never uses
  them).
- One number format for every move (integers, microns) and signs applied on
  every path.
- z for a hole move is read, not remembered.
- The laser is enabled, its interlocks checked and its output confirmed on
  the wire, none of which legacy does.
- `?<n>` replies are errors, not ignored.

## 9. What this does not cover

Tray maps, stage calibration, patterns, the laser UI, `LabSession` wiring and
the config section for an extraction device are sub-projects 2 and 3. The
driver can be written and tested against the sim before them, and wired in
when they exist. Screen capture of Chromium's video window is the vision
library's frame source.

## 10. Open questions

Settled by the vendor reference: reply and error conventions, terminators, the
percent scale of `Laser.Output`, `Scans.InPos?` values, and that enable,
interlock, status and identity queries exist.

Still open (a real Chromium, or the person who runs one, answers these):

1. **Silence after an action command.** How long to wait for a possible `?<n>`
   before calling it success? Is an error line ever late?
2. **Connection.** Does Chromium hold one TCP connection open across commands
   and across hours? Legacy opens one per command; 2.x release notes mention
   sockets "not closing properly", fixed.
3. **CR vs CRLF.** Pychron sends CRLF over TCP where the reference says LF.
   Does the stray CR matter on any version?
4. **`Scans.Status?` texts.** Only two are known, from Pychron's code
   (`Running: Warming up laser...`, `Idle: Idle`). The full set at verbosity 1?
5. **UV extraction.** Run the scan (`Scans.Run`, wait for idle: the wiki's
   workflow, commented out in the code) or fixed output and fire?
6. **Firing state.** Does `Laser.Shutter?` or anything else report "firing"?
7. **Newer command set.** The reference is from 2013; zoom and light commands
   were added after. Is there a later document?
8. **Which Chromium first**: CO2 (melbourne, disabled), UV (ASU), or another
   lab's? It decides whether scans or stage moves and PID matter most.
9. **Speeds.** µm/s is inferred for `Stage.MoveTo`; confirm on a stage.
