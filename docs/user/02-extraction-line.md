# The extraction line

The extraction-line window is the first window Pychron opens and the one the
other windows hang off. It shows your gas-handling line as a drawing: valves,
gauges, pumps, pipettes and the stages (volumes) between them, with the
state of each one live. Around the drawing are docks for alarms, the log, and
(when the line has them) heaters and a cryostat.

Everything on this page is driven by two files, `extraction_line.toml` (what
the line is made of and how to talk to it) and `canvas.toml` (how it is
drawn). Both are explained at the end of this page.

Related pages: [getting started](01-getting-started.md),
[the spectrometer](03-spectrometer.md), [experiments](04-experiments.md),
[configuration reference](07-configuration-reference.md),
[troubleshooting](10-troubleshooting.md), [glossary](11-glossary.md).

## Opening the window

- It opens when Pychron starts. The title bar reads `pychron - <system name>`,
  where the name is `[system] name` in `extraction_line.toml`.
- From any other Pychron window, choose **View > Extraction Line**
  (Ctrl+Shift+L, Cmd+Shift+L on a Mac) to bring it back to the front.
- The other **View** entries (Spectrometer, Experiment, Data, Laser) are
  greyed out until the thing they open has loaded. The spectrometer, for
  example, stays greyed out if the spectrometer config failed to load or the
  line failed to start; the reason is in the log dock.
- **File > Installations...** (shown when the program was started from an
  installation) switches installation; **File > Preferences...** opens the
  preferences (on a Mac the platform may move it to the application menu). **Help > Command palette** (Ctrl+Shift+P) and
  **Help > Keyboard shortcuts** list everything else.

## Reading the canvas

| What you see | What it means |
|---|---|
| A rounded square labelled with a name | An actuated valve (`[[valve]]` / `[[rough_valve]]` in the canvas). |
| A rounded square with a small handwheel in the corner | A manual valve: nobody can drive it, you tell Pychron what you did (see below). |
| A circle | A switch (`[[switch]]`): pump power, a heater relay, a shutter. |
| Green valve | Open. |
| Red valve | Closed. |
| Grey valve | State unknown (not read yet, or the last command or read-back failed). |
| Dashed outline around a valve | A command is in flight (waiting on the controller, the settle time and the read-back). |
| Thick coloured border on a valve | The valve is **locked** in software. |
| Valve flashes yellow | The last command was refused or failed. Hover for the reason. |
| A rounded chip with a small dial and `IG1: 1.20e-08 torr` | A gauge and its latest reading. The chip text turns red while the reading is outside `alarm_high` / `alarm_low`. |
| A gauge chip that shows only its name | The canvas draws a gauge that `extraction_line.toml` does not define; it is decoration and never shows a reading. |
| Boxes (stages) | Volumes: prep line, bone, turbo, spectrometer, tanks. Some carry a glyph (turbo, ion pump, getter, spectrometer, laser, quadrupole). |
| Pipes | Connections between boxes and valves. |

The canvas is a fixed drawing; it does not zoom. Its size and scale come from
`[canvas] size` in `canvas.toml`.

### Colours show where gas can go

Boxes and pipes are not one fixed colour. Pychron works out which volumes are
connected through open valves and paints each connected region with the
colour of the most important source attached to it:

1. a pump (turbo, ion pump, rough pump) wins over
2. a tank (each tank has its own colour), which wins over
3. a pipette or a laser, which wins over
4. a spectrometer, which wins over
5. a getter.

A region with no source stays neutral grey. So opening a valve between the
prep line and a pumped turbo visibly turns the prep line the pump colour, and
opening a pipette shows whose gas it holds. A pipette wears the colour of the
tank across its valve unless the canvas gives it a colour of its own.

If `open_valve_color = "inherit"` is set in `[canvas]`, open valves take the
colour of the region they join instead of green. By default they stay green.

## Working a valve

| Action | What happens |
|---|---|
| **Left-click** a valve | Toggles it: closed or unknown opens it, open closes it. A second click while the first is still pending is ignored. |
| **Right-click** a valve | Menu with **Lock valve** or **Unlock valve**. Unlocking asks "Unlock X? It will accept open and close commands again." |
| **Hover** a valve | Tooltip: name, description, state and since when, whether it is locked or owned, last actuation time, counts of opens / closes / failures, total time open, and the last failure message. |

The tooltip history (counts, time open) is kept between runs.

### What the software checks before a valve moves

Every open or close, from the canvas, from a script, from `elctl`, or from a
running experiment, goes through one gatekeeper in this order:

1. **Lock.** A locked valve refuses every command, whoever sends it.
   Refusal text: `'X' is software locked`.
2. **Owner.** A valve can be claimed by one user of the line; a claimed valve
   accepts commands only from its owner (`'X' is owned by 'a', not 'b'`).
   The tooltip shows `owned by ...`. Pychron's own windows act as `ui` and do
   not claim valves, so you will normally never meet an owner. Ownership is
   not saved: it is gone after a restart.
3. **Interlocks**, only when **opening** (closing is never interlocked):
   - *Exclusive interlocks* (`interlocks = ["C"]`): the valve will not open
     while any listed valve is open **or unknown**, and, because the rule
     works both ways, those valves will not open while this one is not
     closed. Two valves that list each other can never be open together.
     Refusal text: `cannot open 'A': interlocked with 'C' which is open`.
   - *Required valves* (`positive_interlocks = ["B"]`): every listed valve
     must be open first. Refusal text: `cannot open 'A': requires 'B' open,
     it is closed`. Closing a prerequisite later is allowed.
4. **Command, settle, read-back.** The command is sent, Pychron waits the
   valve's `settle_ms`, then asks the hardware what state it is really in.
   If the answer is not the state commanded, the valve is recorded as what
   the hardware said and the error is `'X' read back closed after open`.

A refused command sends nothing to the hardware. Any failure (refusal or
hardware) flashes the valve, adds the reason to its tooltip, and writes an
`ERROR [ui] X rejected: ...` line to the log dock.

Important consequences:

- **Unknown counts as not-closed for interlocks.** Right after start-up, until
  the first read-back, a valve that cannot report its state blocks its
  interlock partners. Wait for the state to settle, or check the transport in
  the health bar.
- Operations are one at a time. Two people (or a script and a person) cannot
  open interlocked valves at the same instant.
- Interlocks look at the state Pychron has *recorded*. For a valve with
  `verify = false` (no read-back) that is the state last commanded, not
  measured.
- Interlocks rest on what Pychron has recorded. Actuated valves are read back
  from the hardware when the line starts and after each command; a valve
  moved by hand at the hardware afterwards is not noticed until then.

### Locks survive restarts

A lock is written to a state file beside `extraction_line.toml`
(`<config name>.state.toml`, for example `extraction_line.state.toml`) and is
restored the next time the line starts. If a valve refuses everything and you
do not know why, it is probably locked: right-click it. Manual valves cannot
be locked.

The state file also holds each valve's history, and the last state of
**manual valves and simulated valves**. A real valve's remembered state is
never used to move hardware: on start-up the hardware is read and is the
truth.

### Manual valves

A manual valve (a handwheel on the line) has no actuator. Clicking it does not
move anything; it records your report that you opened or closed it, so
interlocks and the colours know the truth. Click it after you turn the
wheel. Its state is whatever someone last told Pychron.

### Pipettes

A `[[pipettes]]` entry names an inner and an outer valve (the pipette's two
ends). The canvas draws the pipette as a box between them. The example line
also interlocks the two valves so they can never both be open.

## Gauges

Gauges are drawn as chips on the canvas. Pychron reads each gauge every
`scan_interval_ms` (default 1000 ms) in the background; there is no separate
gauge window. Readings are in the gauge's configured units (`torr`, `mbar` or
`pa`) in scientific notation.

Alarm limits are optional, per gauge: `alarm_high` and `alarm_low`.

- Crossing `alarm_high` raises a **critical** alarm once; crossing `alarm_low`
  a **warning** alarm once. The chip text goes red and goes back to normal
  when a reading is back inside the limits.
- A gauge that cannot be read raises one warning (`read failed: ...`) and
  stays quiet until a read works again.

From a terminal, `elctl read <gauge>` takes one reading and
`elctl scan --for 30s` streams readings and alarms.

## Pumps

Pychron has no "pump" widget. A pump appears in two ways:

- as a **stage** in the canvas with `kind = "pump"` (or a turbo, ion pump
  glyph): it only colours the region it is connected to, as described above;
- as a **switch** (`[[switches]]` in the line, `[[switch]]` in the canvas),
  such as `pump_power`. Switches are clicked like valves (circle, no
  interlocks of their own).

## Heaters dock

The **Heaters** dock appears on the right when `extraction_line.toml` has one
or more `[[heaters]]`. Each row has:

- the heater's name (hover for its description);
- a **power** button showing `On` / `Off`. Pressing it asks `Turn X on?` /
  `Turn X off?`; nothing is sent until you confirm;
- **Use PID** check box;
- a setpoint field: type a number and press **Enter** to send it;
- the unit label (`units`, shown only) and a large readback number.

Under the rows is a message line and a strip chart of every heater's
readback. A control is disabled while its command is pending, and permanently
greyed out when the heater's driver does not support it (for example a PLC
heater configured without a `use_pid` coil). After each command Pychron reads
the same field back; a mismatch is reported as an error and the row snaps
back to what the heater actually says. Errors go to the status line and to
the log dock as `ERROR [heater] ...`. Heaters are scanned like gauges; one
failed scan raises one warning alarm until the next good scan.

Command line: `elctl heater list|status|on|off|pid|setpoint`.

## Cryo dock

The **Cryo** dock appears when the line defines a `[cryo]` controller (a Lake
Shore temperature controller). It shows each sensor input in kelvin
(`Input A`, `Input B`, ...), a **Setpoint n** spin box plus **Set** button per
control output, the setpoint the controller reports, and a strip chart of
the temperatures. The spin box starts at what the controller holds; after
that it is yours. Pressing Set disables the button until the controller
answers; failures show under the fields and in the log as `ERROR [cryo] ...`.
`[cryo] setpoints` can name set-points for scripts (e.g. `He_freeze`); that
named list is used by scripts, not the dock.

## Health bar

The status bar along the bottom has one chip per **transport** (each serial
port, TCP connection or simulated link in `extraction_line.toml`). Each chip
shows the transport name and how long ago it last worked (`valve_bus · 3s`,
then minutes, then hours).

| Chip | Meaning |
|---|---|
| Green | Connected, no errors counted. |
| Amber | Connected but errors have been counted. |
| Red | Not connected. |
| Neutral | Nothing heard yet. |

Hover a chip for the error count and the last error message. A transport
that is red or amber is the first thing to check when a valve will not
respond or a gauge stops updating.

## Alarm dock

The **Alarms** dock (right side) lists problems the line, the heaters, the
cryostat and the spectrometer have raised: columns Source, Severity
(`info`, `warning`, `critical`), Message and Time.

- There is **one row per source** (a gauge name, a heater name, `acquisition`
  for the spectrometer). A new alarm from the same source replaces the old
  row's text rather than adding a row.
- Rows do **not** clear themselves when the condition ends. Select rows and
  press **Acknowledge**, or press **Acknowledge all**. Acknowledging only
  removes the row; it does not silence the hardware.
- The gauge chip's red text follows the live reading, independent of the dock.

## Log dock

The **Log** dock (bottom) shows what the program is doing. Columns: Time,
Level, Logger, Message. It keeps the newest 10 000 records.

Controls on its toolbar:

| Control | What it does |
|---|---|
| Level box | Hide everything below this level (trace, debug, info, warn, error). |
| `logger (prefix or glob)` | Show only matching loggers. `valve_bus` also matches `valve_bus.wire`; `*.wire` matches every wire log. |
| `message contains` | Case-insensitive text filter on the message. |
| **Pause** | Stop adding lines so you can read; a `+N new` badge counts what you are missing. Un-pause to catch up. |
| **Clear** | Empties the dock (not the log file). |
| **Autoscroll** | Follow the newest line while you are at the bottom. |
| **Save...** | Writes the lines currently visible (after filters) to a text file. |
| **Set logger level...** | Asks for a logger name or glob and a level and applies it now (for example `valve_bus.wire` at `trace` to watch bytes). Only shown when logging could be started. |

Logger names to know: `extraction_line`, `switches` (every valve actuation,
failure and lock change), `scheduler`, and `<transport name>.wire` (the raw
bytes of each transport, at trace level, so they are hidden unless you raise
them).

Lines the interface itself prints are prefixed in the Logger column as `ui`,
`cryo`, `heater`, `canvas` and so on; spectrometer and laser problems appear
here too (`ERROR [ui] spectrometer not loaded: ...`).

### The log file

If `extraction_line.toml` has `[logging] dir = "..."`, every record is also
written to `pychron.log` in that folder, rotated at `max_size_mb` (default
10) keeping `max_files` files (default 5). Lines look like
`2026-10-01T14:03:22.481Z [warn] valve_bus: message`. When the program
starts it reads the tail of that file back into the dock (up to 10 000
lines), so you see what happened before the last restart. With no `dir`
there is no file and the dock starts empty.

For hardware faults, set `"*.wire" = "trace"` under `[logging.levels]` or
use **Set logger level...** and send the log (or a `trace` capture,
below) along with the problem report.

## Simulation versus real hardware

- `kind = "sim"` on a transport makes that transport a simulated one. A
  simulated line runs the same code as a real one: opening a valve changes
  the pressures the simulated gauges report, pumped volumes pump down,
  gauges have a little noise.
- `pychron-ui --sim` forces **every** transport to `sim`, whatever the config
  says. An installation set up "for simulation" does this automatically. With
  nothing installed, `--sim` (or `--examples`) opens the example line shipped
  with Pychron.
- The splash screen shows a **SIMULATION** badge; the spectrometer, experiment
  and laser windows say `(Simulation)` in their titles. The extraction-line
  window's own title does **not**, so check the health bar chip names or the
  splash if you are unsure.
- `--sim-speed <x>` (with `--sim`) runs simulated time `x` times faster than
  the clock, handy for rehearsing a queue.
- A valve on a simulated controller starts closed on every run (locks and
  manual-valve states are still restored from the state file).
- The simulator is not a physics model of your line. It uses a handful of
  volumes, instantaneous equilibration of connected volumes, exponential
  pump-down and 1 % gauge noise. Do not use it to estimate real equilibration
  or pump-down times.

To move from simulation to a real line, change each transport from
`kind = "sim"` to `serial` or `tcp` with a port or host (see below), run
`elctl validate` and `elctl probe`, and start with `elctl state` before
opening anything. Run the setup wizard again (`pychron-ui --setup`, or
`elctl init --reconfigure`) to switch an installation out of simulation; see
[getting started](01-getting-started.md) and the
[installation runbook](../installation_runbook.md).

## Hardware status: which drivers exist

A *driver* is the piece that talks to a model of controller. These are the
`kind` values you can put in `[drivers.<name>]` (`elctl list-drivers` prints
the live list with every key each one reads).

| Driver `kind` | Hardware | Notes |
|---|---|---|
| `proxr_relay` | NCD ProXR relay board (valves) | Valve address is a relay index 0..255. Tested against scripted and replayed traces. |
| `agilent_switch` | Agilent/Keysight 34970A-family with 34903A card (valves) | Serial or LAN; the VISA-USB form is simulated only. |
| `plc2000_valves`, `plc2000_gauges`, `plc2000_heater` | AutomationDirect PLC over Modbus TCP | Valves are coils; gauges are float registers; the heater driver has optional enable / PID / setpoint / readback addresses. |
| `pychron_valves` | Valves on another Pychron's valve service | |
| `qtegra_valves`, `qtegra_gauges` | Valves and gauges through Thermo Qtegra RemoteControl | Use the same connection as the spectrometer (a `link` transport). |
| `ngx_valves` | Valves on an Isotopx NGX controller | Shares the NGX connection. |
| `pfeiffer_maxigauge` | Pfeiffer MaxiGauge TPG 256 A | Tested against scripted and replayed traces. |
| `gp_microion` | Granville-Phillips Micro-Ion + Convectrons | Tested against scripted and replayed traces. |
| `varian_xgs600` | Varian/Agilent XGS-600 gauge controller | |
| `lakeshore` | Lake Shore 325/331/335/336 (cryo) | |
| `chromium` | Photon Machines Chromium laser (see [laser](05-laser.md)) | Not run against a real Chromium. |
| `sim_valves` | Stand-in valve actuator that accepts any address | Used where a legacy controller has no driver yet. |

Statements in the repository are specific about a few of these: the
Pfeiffer, Granville-Phillips and ProXR drivers have been tested against
scripted and recorded traces; the Qtegra and NGX drivers and the Chromium
laser have **not** been run against a real instrument. For everything else
the repository does not claim hardware validation; treat any driver as
unproven on your line until you have watched it work with
`elctl probe`, `elctl state` and a trace capture.

Legacy Pychron controllers with no driver yet (the NMGRL Arduino valve
controller and the NMGRL furnace controller) are converted into `sim_valves`
drivers on simulated transports by the legacy importer, with a comment in the
file naming what is still needed. Such a valve **looks** like it works but
moves nothing; the NMGRL example line (`configs/examples/nmgrl/`) is built
that way.

### Converting a legacy line

```bash
elctl import-line <legacy setupfiles folder> --out my-line
```

reads a legacy `setupfiles` folder (`extractionline/valves.yaml` or `.xml`,
`canvas2D/`, `devices/`) and writes `extraction_line.toml` and `canvas.toml`
plus a report of what was and was not carried over. Read the report before
using the result on hardware. The setup wizard can do the same.

## How the line is described: `extraction_line.toml`

The file has one section per kind of thing. Here is the shipped example
(`configs/examples/extraction_line.toml`), shortened and annotated:

```toml
[system]
name = "example"            # shown in the window title
scan_interval_ms = 1000     # how often gauges/heaters/cryo are read

[logging]                   # optional
# dir = "~/pychron-logs"    # no dir = no log file
default_level = "info"      # trace|debug|info|warn|error
# [logging.levels]
# "*.wire" = "trace"        # per-logger overrides, glob on logger name

# A transport is a connection. "sim" = simulated. Real ones are
# kind = "serial" (port, baud, ...), "tcp" (host, port), "udp",
# "modbus_tcp", "modbus_rtu", or "link" (shares another config's connection).
[transports.valve_bus]
kind = "sim"
timeout_ms = 500

# A driver is the model of controller on a transport.
[drivers.actuator1]
kind = "proxr_relay"
transport = "valve_bus"

[drivers.ig_controller]
kind = "pfeiffer_maxigauge"
transport = "gauge_net"
channels = [1, 2]

[[valves]]                  # an actuated valve
name = "A"
description = "Furnace to prep"
actuator = "actuator1"      # which driver moves it
address = "1"               # the driver's own address (relay number, coil, ...)
interlocks = ["C"]          # cannot open while C is open or unknown (both ways)
settle_ms = 500             # wait this long before reading it back

[[valves]]
name = "C"
description = "Prep to turbo"
actuator = "actuator1"
address = "3"
interlocks = ["A"]          # written on BOTH valves of a pair, as legacy does

[[manual_valves]]           # a handwheel; you report its state
name = "M1"
description = "Turbo to roughing pump"

[[switches]]                # on/off thing that is not a gas valve
name = "pump_power"
actuator = "actuator1"
address = "9"

[[gauges]]
name = "IG1"
driver = "ig_controller"
channel = 1
units = "torr"
alarm_high = 1e-4           # raises a critical alarm once when crossed

[[pipettes]]
name = "air"
inner = "P1"
outer = "P2"

# Names that measurement plans use as '@valves.inlet', '@extraction.eqtime'.
[aliases]
valves.inlet = "B"
extraction.eqtime = 20
```

Other things a valve or switch may carry:

| Key | Use |
|---|---|
| `positive_interlocks` | valves that must be **open** before this one opens |
| `inverted = true` | the channel is wired backwards; Pychron still shows the valve's real state |
| `state_source = { driver = "...", address = "...", inverted = false }` | read the state from another device (some labs read autovalve states from a second unit) |
| `verify = false` | do not read back; trust the command (legacy `query_state = false`). Interlocks then rest on the commanded state. Cannot be combined with `state_source`. |

Other sections: `[cryo]` (`driver`, `tolerance_k`, `timeout_s`,
`[cryo.setpoints]`), `[[heaters]]` (`name`, `driver`, `description`,
`units`), and driver-specific keys inside `[drivers.<name>]`. Unknown keys are
errors, not ignored, so a typo is caught at load.
`[aliases] valves.<x>` must name a real valve.

A transport with `trace = true` also records its traffic to
`traces/<transport>.trace` for later replay; `elctl trace on` does the same
from the command line.

Check a file before starting:

```bash
elctl validate extraction_line.toml          # every error, with file:line
elctl list -c extraction_line.toml           # valves, switches, heaters, gauges
elctl list-drivers                           # every driver and its keys
```

## How the line is drawn: `canvas.toml`

The canvas holds presentation and connectivity only. Valve states and gauge
readings come from the line; the canvas just says where things are and what
is plumbed to what. Positions are pixels in a box of `[canvas] size`.

```toml
[canvas]
origin = [0, 0]
size = [1000, 700]               # the drawing area
connection_width = 6             # pipe thickness
# open_valve_color = "inherit"   # open valves wear the region colour

[[valve]]                        # one per [[valves]] in the line file
name = "A"                       # must match the line's name exactly
pos = [250, 200]
# display_name = "A"             # text on the valve face

[[manual_valve]]
name = "M1"
pos = [750, 300]

[[gauge]]                        # a gauge chip; need not exist in the line
name = "IG1"
pos = [450, 380]

[[stage]]                        # a volume (a box)
name = "turbo"
pos = [650, 300]                 # the box's CENTRE
size = [60, 40]
kind = "pump"                    # pump | tank | spectrometer | getter | laser | pipette
# symbol = "turbo"               # glyph: spectrometer quadrupole laser turbo getter ion_pump
# display_name = "Turbo"         # label ("" for none)
# color = "#8fd3ff"              # override region colour
# precedence = 120               # override who wins a shared region (0: colours nothing)

[[pipette]]                      # must match a [[pipettes]] entry in the line
name = "air"
pos = [400, 420]
# tank = "air_tank"              # whose gas it holds (colour); "" = its own colour

[[connection]]                   # a pipe between two named things
start = "prep"
end = "B"
# orientation = "v"              # h or v: force an L-shaped route

[[elbow]]
start = "prep"
end = "C"
corner = "ll"                    # ul | ur | ll | lr

[[tee]]                          # every listed end is joined, no valve between
left = "C"
right = "turbo"
mid = "IG1"

[[label]]
text = "Furnace side"
pos = [100, 50]
```

Other sections the loader accepts: `[colors]`, `[[rough_valve]]` (drawn as a
valve), `[[switch]]`, `[[cross]]`, `[[image]]` and `[legend]`.

The line and the canvas are checked against each other when Pychron starts:

- A canvas valve, manual valve, switch or pipette whose name is not defined
  in `extraction_line.toml` is an **error**; the line will not load. The
  message says when you used the wrong kind (`it is a manual valve; draw it
  as [[manual_valve]]`).
- A valve, manual valve or switch defined in `extraction_line.toml` but not
  drawn is only a **warning**: it is listed in the log dock as
  `WARN [canvas] valve 'X' is not drawn on the canvas (canvas.toml)`. It
  works from scripts and `elctl` but you cannot click it.
- A gauge drawn on the canvas but not defined in the line is allowed
  (decoration; no reading).

```bash
elctl canvas-check canvas.toml -c extraction_line.toml
```

If a name in a `[[connection]]` is not drawn anywhere, that pipe is silently
skipped; use `canvas-check` and your eyes after editing.

## Command-line companion: `elctl`

`elctl` is the same line without the window. It is the quickest way to prove a
transport works.

```bash
elctl -c extraction_line.toml validate
elctl -c extraction_line.toml probe          # open every transport, ping every driver
elctl -c extraction_line.toml state          # read back every valve and gauge
elctl -c extraction_line.toml open A         # enforces locks and interlocks
elctl -c extraction_line.toml close A
elctl -c extraction_line.toml read IG1
elctl -c extraction_line.toml scan --for 30s
elctl -c extraction_line.toml --sim sim      # interactive session on simulated hardware
elctl doctor                                 # check an installation
```

## Unverified / not yet implemented

- **Zoom or pan** of the canvas is not implemented.
- **`[[image]]` and `[legend]`** are read from the canvas file but I found no
  code that draws them in the window. Treat as not working.
- **`[colors] valve`** and the stage `fill` / `volume` keys are read but not
  used by the window as far as the source shows (`stage` and `pipette`
  colours are used).
- **Claiming valves** (owner semantics) exists in the line software but the
  window never claims or releases; there is no button for it.
- **Pump control** beyond a power switch, bake-out sequencing and automated
  gauge-driven interlocks are not part of the line window.
- The window offers no way to hide or show the docks from a menu.
- README's "Window > Experiment" wording: the real menu is **View >
  Experiment**.
