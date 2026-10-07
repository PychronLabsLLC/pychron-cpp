# Laser extraction

This chapter covers heating samples with a laser: how a queue reaches a hole
on a sample tray, the laser window, calibrating a tray, centering a hole with
a camera, laser patterns, and the lasers supported today. Read
[Experiments](04-experiments.md) first for queues, and
[Scripting](09-scripting.md) for the scripts that drive the laser.

Related: [Getting started](01-getting-started.md),
[Extraction line](02-extraction-line.md),
[Configuration reference](07-configuration-reference.md),
[Troubleshooting](10-troubleshooting.md), [Glossary](11-glossary.md).

## 1. What is supported, honestly

| Part | Status |
|---|---|
| **Photon Machines Chromium** CO2 laser system, over TCP (driver kind `chromium`) | The only laser driver in this build. Written from the protocol survey and **proven against a built-in simulator only. It has not been run against a real Chromium.** Treat first use as commissioning: check a real Chromium with the laser window and the `elctl laser` commands, with the beam path safe, before running a queue. |
| Other lasers (diode, UV, other CO2 systems) and furnaces, pipettes, motors | The scripting language has commands for them (`dump_sample()`, `load_pipette()`, `set_motor()`...), but no driver for them ships in this build, so those commands stop the run with "not supported". |
| Output units | The Chromium takes **percent only** (0 to 100). A run that asks for watts fails: `not supported: extract in watts (a Chromium takes percent)`. |
| Simulated camera and simulated laser | Complete. Autocenter, dragonfly and calibration are proven here. |
| Live camera through OpenCV (a built-in or USB camera, or a video file) | Works for **looking** (`use = "view"`) and, over a real laser, may center holes. The finders have **not been validated on real frames** (see `docs/vision_fixtures.md`): until you have checked autocenter on your own optics, use it on a spare tray. |
| Basler (pylon) camera | The configuration is read and checked, but the driver is not part of this build: opening one says "built without pylon". |
| Screen capture of Chromium's own video window | Not implemented (dropped from the plan). |
| Seek, autofocus, watts and temperature control | Not implemented. |

## 2. The pieces

| What | Where | Purpose |
|---|---|---|
| The laser | `[drivers.<name>]` in `extraction_line.toml` | The name is what a queue calls `extract_device`. |
| Tray map | `<lab>/tray_maps/<tray>.txt` | The holes of a tray, in millimetres. |
| Stage calibration | `<lab>/stage_calibrations/<device>.<tray>.toml` | Where that tray sits on that laser's stage. |
| Camera | `<lab>/cameras.toml` | The camera of each laser, for looking and centering. |
| Corrections | `<lab>/stage_corrections/<device>.<tray>.toml` | Where centering found each hole (written for you). |
| Camera scale | `<lab>/camera_scales/<device>.toml` | A measured pixel scale (written for you). |
| Patterns | `<lab>/patterns/<name>.toml` | Paths for the beam. |
| Snapshots | `<lab>/snapshots/<device>/<name>.png` | Pictures saved from the camera. |

Several lasers may be on one line; each is a separate driver with its own
calibration.

## 3. Setting up a Chromium

On the laser PC, in Chromium: **View > Control Panels > Show/Hide Remote
Control Window**, tick **TCP/IP Interface** (port 1234). Chromium must be
running; pychron keeps one connection open to it. Then add a transport and a
driver to the line's `extraction_line.toml` (see
`configs/examples/laser.chromium.toml.example`):

```toml
[transports.laser_pc]
kind = "tcp"
host = "laser-pc.lab"
port = 1234
timeout_ms = 2000

[drivers.co2]
kind = "chromium"
transport = "laser_pc"
x_limits = [0, 50]        # stage travel in mm; a move outside it is refused
y_limits = [0, 50]
z_limits = [0, 50]
signs = [1, 1, 1]         # 1 or -1 per axis, for a stage whose axes run the other way
move_speed = [5000, 5000, 100]   # x, y, z in microns per second
in_position_um = 10       # how near the target counts as arrived
# use_enable = true       # false for a unit that refuses Laser.Enable
```

All keys after `transport` are optional; the values above are the defaults.
With `kind = "sim"` in the transport, the driver talks to a built-in simulated
Chromium instead (the example lab does this). The driver's name (`co2` here)
is the device's name everywhere: in queues, in `cameras.toml`, in calibration
file names.

What the driver does for you:

* It checks that the program on the port really is a Chromium.
* It refuses to enable or fire while an interlock is tripped, and says which.
* After every output setting it reads the value back, and stops the beam or
  zeroes the output rather than guess if the outcome is uncertain.
* A move outside the configured travel is refused before anything is sent.
* Moves are straight lines at the requested speed, and arrival is confirmed
  by three good position readings in a row.

## 4. The laser window

Open **View > Laser** (Ctrl+Shift+B); with several lasers this is a submenu
with one entry each. A laser PC that runs no experiments can open only the
laser window:

```bash
pychron-ui --laser line.toml                      # the line's only laser
pychron-ui --laser --device co2 line.toml         # one of several
pychron-ui --examples --sim --laser               # the simulated example
```

(`--laser` takes no `--queue` or `--spectrometer`.) The window refreshes four
times a second.

**Top left.** Choose the **Tray**. The tray is drawn to scale, x to the right
and y up. With a calibration, the stage is drawn where it is over the tray.
Ringed holes are those the calibration was taken at; a green dot is a hole a
camera has found before; the hole the stage was last sent to is filled. Click
a hole to go there. With **Center holes** ticked (the default when the laser
has a camera) the move ends by centering the hole under the beam, as a queue
does. A right click offers the move with or without centering, and "The stage
is on hole N now: calibration point".

**Camera.** What the camera sees, with the aim point as a crosshair, a faint
circle the size a hole should be, and the target the finder sees outlined
while a hole is being centered. Under it is the frame rate. If the camera
stops, the last picture stays, greyed, with its age and the reason, and the
window tries to reopen it every second. **Autocenter** centers the hole the
stage was last sent to; **Snapshot** saves the picture.

**Control tab.**

* **Stage:** arrows jog by the Step (in mm); Z+ / Z-; **Stop stage** halts a
  move.
* **Laser:** **Enable**, then **Fire**. Fire sets the Output (percent) and
  opens the beam, and is live only while the laser is enabled and no
  interlock is tripped. While it fires, a new output is sent only when you
  press Enter in the box. **Stop** closes the beam and zeroes the output.
  The status line shows "beam off", "FIRING at N %" and "interlocks: ok" or
  "interlock tripped: ...".
* Closing the window, or quitting, with the beam on closes the beam and
  disables the laser.

**Calibration tab** (section 6) and **Patterns tab** (section 8).

**EMERGENCY STOP** is always live. It turns the beam off, sets the output to
0, disables the laser, stops the stage and any pattern, and **aborts a running
queue**. It latches: until you press **Reset**, nothing will enable, fire or
move, by hand or from a script, and no queue will start. Reset is offered once
a running queue has ended.

**While a queue runs the window only watches**: every control except the
emergency stop is off, and a banner says "Queue running: watch only". The
reverse holds too. A queue will not start while a command made by hand is
still running, while a beam opened by hand is on, or while the emergency stop
has not been reset.

## 5. Trays

### 5.1 Tray map files

A tray map is a text file, `tray_maps/<tray>.txt`, in legacy Pychron's tray
map format, so existing files can be copied from `setupfiles/tray_maps`. The
file's name without `.txt` is the tray's name, which is what a queue's `tray`
says. From `configs/examples/tray_maps/example-9.txt`:

```
# A 3 x 3 example tray: holes 5 mm apart, numbered left to right, top to bottom.
#   shape, hole dimension (mm)
#   valid holes
#   calibration holes: north, east, south, west, center
circle,2.0
1,2,3,4,5,6,7,8,9
2,6,8,4,5
1,-5,5
2,0,5
3,5,5
4,-5,0
5,0,0
6,5,0
7,-5,-5
8,0,-5
9,5,-5
```

`#` starts a comment. The first three lines are the shape (`circle` or
`square`) and hole size in millimetres, the list of valid holes, and the five
calibration holes in the order north, east, south, west, center. Each hole
line then gives `x,y` (numbered from 1 in file order) or `id,x,y`. A line
that cannot be read, a duplicate id, or a missing header is an error that
names the file and line. Hole names in a queue are these ids; the queue's
`position = 5` is hole `5`.

### 5.2 The queue's tray and holes

The queue names the laser and the tray, and each run names a hole:

```toml
[queue]
name = "sim-laser-example"
mass_spectrometer = "sim"
extract_device = "co2"
tray = "example-9"

[[runs]]
identifier = "66001"
extraction = { script = "laser_extract", position = "3", value = 20, units = "percent", duration = 10 }
measurement = { plan = "sim_multicollect" }
post_measurement = "sim_pump"
```

Use `units = "percent"` with a Chromium. The queue's tray is set on the laser
before any script runs, and a script moves with `move_to_position()`
([Scripting](09-scripting.md)). See `configs/examples/experiment.laser.toml`.

### 5.3 What stops a queue from starting

The Experiment window and `elctl exp validate` check all of this before a run,
so a mistake is found at the desk, not with the beam on. Each shows as an
error on the run's row (field `extraction`) or on the queue (field `tray`):

| Message | Fix |
|---|---|
| `unknown extraction device 'x' (the line has: ...)` | The name must match a driver in `extraction_line.toml`. |
| `no tray map 'x' in <lab>/tray_maps (known: ...)` | Put the file there, or correct `tray`. If the file exists, the message names the line that did not load. |
| `the run names a hole and the queue has no tray` | Set `tray` in `[queue]`. |
| `no hole N on tray T` | The hole is not in the tray map. |
| `<device> / <tray> is not calibrated` | Calibrate it (section 6). |
| `... the calibration is stale (the tray map has changed since it was made)` | The tray map was edited after calibration; calibrate again. |
| `unknown pattern x (the lab has: ...)` | Make it in the pattern maker, or correct the name. If the file exists, the message says why it did not load. |
| `camera of <device>: ...` | A camera that was meant to be there cannot be used (section 7). |
| `pattern x follows the glow and <device> has no camera to see it (...)` | A dragonfly needs a camera that centers (section 8.2). |
| `pattern x has no duration: the run gives none and the pattern has none of its own` | Give the run a `duration`. |

A device with no `extraction_line.toml` extraction drivers at all is not
checked this way; the device name is then free text.

## 6. Stage calibration

A calibration tells pychron where a tray sits on a laser's stage. It is a
list of points: "the stage was at (x, y) when it was on hole H".

* One point places the tray.
* Two points also turn it (the legacy "Tray" calibration: center, then right).
* Three or more points are fitted, with scale, and an rms error is reported.

A fitted scale more than 2% away from 1 is refused: stage and map are both in
millimetres, so it means a wrong hole. A refused point leaves the calibration
file as it was.

### 6.1 In the laser window

1. Jog the stage until a hole is under the aim point (the crosshair).
2. On the **Calibration** tab choose the hole ("The stage is on hole").
3. Press **Set point**. Repeat for other holes. **Remove** and **Clear**
   undo points.

The table lists the points. The line under it gives the solution ("Center
x, y mm - rotation - scale - rms - N point(s)") and cautions the points
cannot rule out. The files written are the ones `elctl` writes.

### 6.2 At the command line

Jog onto a hole with the laser's own software (Chromium), then record it:

```bash
elctl -c extraction_line.toml laser calibrate co2 221-hole center
```

```bash
elctl -c extraction_line.toml laser calibrate co2 221-hole right
```

`center` and `right` are the tray map's center and east calibration holes;
`point <hole>` records any hole; `show` and `clear` display and delete the
calibration. `--x` and `--y` give the position instead of reading it from the
stage, with no hardware opened. List all trays and their status:

```bash
elctl -c extraction_line.toml laser trays
```

### 6.3 Check it before firing

Two caveats that `calibrate` and `trays` print:

* Center and right lie on one line, so they cannot show a mirrored axis.
* Two exchanged holes fit perfectly with the tray half a turn round; neither
  shows in the rms.

So always test a new calibration by sending the stage to a hole **off that
line**, with the beam off:

```bash
elctl -c extraction_line.toml laser goto co2 221-hole 17
```

This moves the stage and reports the miss. Ctrl-C, or `--timeout <seconds>`
running out, stops the stage. A hole move does not change z.

## 7. Camera, live view and autocenter

### 7.1 `cameras.toml`

A laser has a camera when `cameras.toml` has a table named like its driver.
A laser with no table has no picture and no centering; everything else works,
and moves go to the calibrated positions. The shipped file
(`configs/examples/cameras.toml`) is fully commented.

```toml
[co2]
source = "opencv"        # sim | recorded | opencv | pylon
use = "view"             # center | view
px_per_mm = 23.0
flip_x = false           # which way the picture moves when the stage does
flip_y = true            # (the usual camera: stage +y is image up)
aim_offset_px = [0, 0]   # the beam's place in the picture, from its center
settle_ms = 200          # after a move, before a frame is trusted

[co2.opencv]
device = 0               # a camera's index, or a video file
# width = 1280
# height = 720
# fps = 30
channel = "luma"         # luma | r | g | b
rotate = 0               # 0 | 90 | 180 | 270, clockwise
# roi = [0, 0, 640, 480]
timeout_ms = 1000

[co2.autocenter]
tolerance_mm = 0.03
max_iterations = 4
max_step_mm = 0.5
frames_per_step = 3
on_failure = "continue"  # continue | fail
```

**`source`.** `sim` is the tray as a camera would see it from the simulated
stage. `recorded` replays a folder of frames, to look at what the finder
sees; it never moves the stage. `opencv` is a camera OpenCV can open, or a
video file (this needs a build with OpenCV). `pylon` is not available (section
1). **If you leave `source` out the camera is `sim`**, and a simulated camera
over a real laser stops queues on that device from starting, with a message,
until you remove or correct the table.

**`use`.** `center` (the default) lets the camera center holes and follow the
glow. A live camera may do that only over a real laser; a simulated one only
over a simulated laser. `view` is a picture only: nothing it sees moves the
stage, hole moves go to their calibrated positions, and queues run as before.
To try a computer's own camera, set `source = "opencv"` and `use = "view"`.
On macOS the system asks once whether Pychron may use the camera.

```bash
elctl -c extraction_line.toml laser cameras
```

lists the cameras OpenCV can see. `elctl laser look <device>` says what the
camera's finder sees and moves nothing; `elctl laser snapshot <device> [name]`
saves a picture.

The picture is video, up to 25 frames a second, read on a thread of its own
so a command waiting on the laser cannot freeze it. Nothing is looked for in
the picture unless a hole is being centered or a dragonfly is following the
glow; then what the finder sees is drawn on it.

### 7.2 Pixel scale

`px_per_mm`, `flip_x` and `flip_y` may be measured instead of typed. With a
hole under the aim point:

```bash
elctl -c extraction_line.toml laser camera-scale co2 221-hole 111
```

or **Measure camera scale** on the laser window's Calibration tab. The stage is
jogged half a millimetre in x, in y and in both, and the scale is read from
what the picture does. `px_per_mm` has to be roughly right to begin with: a
move nothing like the expected one is refused, as is a measurement whose axes
disagree by more than 3% or are more than 3 degrees from square. The result
is kept in `camera_scales/<device>.toml` and used instead of the values in
`cameras.toml`; `laser camera-scale co2 clear` forgets it. A scale measured
with one camera geometry (size, rotation, roi) is not used with another.

### 7.3 Autocenter

A calibration puts every hole within a fraction of a millimetre. With a
camera that centers, a hole move goes on to center the hole under the beam:
the stage arrives, waits `settle_ms`, the camera looks, the stage is nudged,
and it looks again, until the hole is within `tolerance_mm` of the aim point.
A script's `move_to_position()` does this by default
(`move_to_position(autocenter=False)` does not).

What is found is saved per hole in `stage_corrections/` and is where the next
move to that hole starts. A move that asks for autocenter still checks and
updates it. Corrections are dropped when the tray map or the stage calibration
changes. What a hole move did ("hole 3: centered, moved 0.150, -0.100 mm", or
why it was not centered) goes into the run's log, shown in the Events list
and kept in the record as notes.

What it will not do:

* **Find the neighbour.** A hole is never taken more than 45% of the way to
  the nearest other hole (at most 1 mm) from its calibrated position; beyond
  that it is a failure, and a saved correction further off is ignored. A tray
  off by more than about half the hole spacing cannot be told from one that is
  right: calibrate again.
* **Guess.** When the hole is not seen, the camera fails, or the nudges make
  things worse (a wrong `flip_x`/`flip_y` shows as this), the stage goes back
  to where centering started. With `on_failure = "continue"` (legacy Pychron's
  behaviour) the run carries on there; with `"fail"` the move is an error and
  the run stops **before the laser fires**.

The reasons given when centering fails: `no_target` (the hole was not seen),
`max_iterations`, `runaway` (nudges made it worse), `max_total` (it would have
moved more than allowed), `clipped`, `stale_frame`, `camera`, `invalid`.

A script must wait for a move that centers:
`move_to_position(block=False)` is refused on a laser with a camera that
centers, unless it also says `autocenter=False`.

```bash
elctl -c extraction_line.toml --sim laser autocenter co2 example-9 5
```

moves to a hole, centers it and saves the correction (exit status 1 if it
could not, whatever `on_failure` says). `laser corrections <device> <tray>`
lists what was saved and `laser corrections <device> <tray> clear [<hole>]`
forgets it.

## 8. Patterns

A pattern is a path the beam is moved along while it heats, to spread the
heat or follow a grain. A run names one (`pattern = "hexagon"`) and its
extraction script runs it with `execute_pattern()`.

### 8.1 Pattern files

A pattern is `<lab>/patterns/<name>.toml`:

```toml
kind = "polygon"
velocity = 1.0      # mm/s
iterations = 1      # the whole pattern, repeated
radius = 1.0        # the kind's own keys
nsides = 6
rotation = 0
```

Lengths are millimetres and angles degrees, in the stage's axes, about
wherever the stage is when the pattern starts; the stage returns there at the
end. Each segment is a straight line at the pattern's `velocity`, never above
the driver's `move_speed`. The kinds and their keys (names, geometry and
defaults are legacy Pychron's):

| kind | keys |
|---|---|
| `polygon` | `radius`, `nsides`, `rotation` |
| `linear` | `length`, `rotation`, `npasses` |
| `circular_contour` | `radius`, `nsteps`, `percent_change` |
| `line_spiral` | `radius`, `nsteps`, `percent_change`, `step_scalar` |
| `square_spiral` | `radius`, `nsteps`, `percent_change` |
| `random` | `walk_x`, `walk_y`, `npoints`, `seed` (none: a new walk each run) |
| `rubberband` | `length`, `offset`, `rotation` |
| `raster` | `length`, `offset`, `rotation`, `dx`, `single_pass` |
| `trough` | `length`, `width`, `rotation`, `use_x` |
| `dragonfly` | see section 8.2 |

A mistyped key, a key of another kind, a wrong type, or a value out of range
is an error when the file is read ("not when the beam is on"). A pattern may
have at most 10 000 points over all its iterations. A queue is not started if
a run names a pattern the lab lacks or whose file does not load.

List the lab's patterns, with their points, length and time:

```bash
elctl laser patterns --lab configs/examples
```

To see where the beam would go, print a pattern's points with `--dry-run`:

```bash
elctl -c extraction_line.toml laser pattern co2 hexagon --dry-run
```

Without `--dry-run` the pattern is run from where the stage is, **with the
laser not fired**. Ctrl-C and `--timeout` stop the stage.

Notes on running patterns:

* A script must wait for its pattern: `execute_pattern(block=False)` is
  refused because the pattern advances only while the script waits.
* The stage's arrival at each point is confirmed before the next is sent,
  which pauses the beam for about 0.15 s at every point. A pattern with
  hundreds of points is slow and heats its vertices more.
* A pattern is not checked against the stage's travel beforehand: a point
  outside it ends the pattern there (the error names the point) and the run's
  end switches the laser off.
* Whatever a script leaves running is stopped when the run's extraction
  ends; stopping a pattern stops the stage where it is.

Legacy patterns are Python pickles (`setupfiles/patterns/*.lp`). Export them
once per lab, nothing in a pickle is run:

```bash
python3 tools/export_patterns.py /path/to/setupfiles/patterns /path/to/lab/patterns
```

It reports what it could not carry over: arc and seek patterns, z and power
series, a spiral's inward direction, and a dragonfly's dwell, limit, delay and
mask.

### 8.2 The dragonfly

A dragonfly is not a path. For its duration the stage follows the glow of the
heated sample, so the beam stays on a grain that sits off its hole's center
or creeps as it melts; then the stage returns to where it started.
`configs/examples/patterns/follow.toml` is commented:

```toml
kind = "dragonfly"
duration = 30                 # s: for a run that gives none of its own
velocity = 1.0                # mm/s for its moves
perimeter_radius = 2.5        # mm: never further from where it started
saturation_threshold = 0.75   # a glow this bright (0 to 1): hold still
aggressiveness = 1.0
move_threshold = 0.033        # mm: smaller corrections are not made
max_step = 0.5                # mm
spiral = "hexagon"            # the search when the glow is lost: hexagon | square
spiral_base = 0.5             # mm
```

The script fires the laser and then calls `execute_pattern()`, which follows
for the **run's** extraction `duration` (the pattern's own `duration` is used
if the run has none; a queue with neither is not started). The script must
not also sleep. With the beam on and no glow to follow, the search spirals
outward, never beyond `perimeter_radius` and never beyond the room the hole
has before its neighbours (45% of the way to the nearest). What it could not
do (never saw the glow, was held in, lost the camera) is written to the run's
log.

A dragonfly needs a camera that follows the stage: a camera with `use =
"center"` and a source that is a real stage-following one (not `recorded`). A
queue naming a dragonfly on a laser without one is not started. If the camera
fails part way, the stage returns to where the pattern began and, by the
camera's `on_failure`, holds there for the rest of the duration (and says so)
or the run fails. A dragonfly cannot be run by hand (`elctl laser pattern`
never fires the laser, so there is no glow, and the window lists it as "runs
from a queue"). It is proven on the simulated camera only.

### 8.3 The pattern maker

In the laser window, **Patterns tab > Pattern maker...** opens a form for
making a pattern: choose the **Kind** and the **Name** (what a queue calls it)
and fill in the fields; only those the kind uses are shown. The path is drawn
with its point count, length and time ("N points - X mm - Y s at Z mm/s"); for
a dragonfly it describes how it will follow the glow. A pattern that could not
run is explained and cannot be saved. **Open...** loads an existing one and
**New** starts afresh. **Save** writes `<lab>/patterns/<name>.toml`, **over a
pattern of the same name without asking**, and the next run of that name uses
it (a run already in progress keeps the path it started with).

The **Patterns tab** itself lists the lab's patterns with kind, length and
time. **Run** runs the chosen pattern about where the stage is now, moving the
stage only: the window does not fire the laser for the pattern. **Stop** stops
it.

## 9. Snapshots

A script's `snapshot()`, the window's **Snapshot** button and
`elctl laser snapshot <device> [name]` save what the camera sees as
`snapshots/<device>/<name>.png` (named by the time when no name is given),
never over a picture that is already there.

## 10. A first laser session, step by step

1. Set up the driver (section 3) and confirm with `elctl -c line.toml list`
   that the line loads. Start with the Chromium's own software working.
2. Put the tray map in `tray_maps/`.
3. Open the laser window. **Check the interlocks** line says "interlocks: ok".
   Practise **Enable**, a **Fire** at a low output on a safe target, and
   **Stop**. Know where the **EMERGENCY STOP** is.
4. Calibrate the tray (section 6) and test with `laser goto` to a hole off the
   center line.
5. Optionally add a camera with `use = "view"`, then measure its scale, then
   change to `use = "center"` and try **Autocenter** on a spare tray.
6. Make a queue with `extract_device`, `tray`, `units = "percent"`, a
   position on each run and an extraction script
   (`configs/examples/scripts/extraction/laser_extract.py` is a start).
   Validate it with `elctl exp validate`. Try it first with `--sim`.

## 11. Unverified / not yet implemented

* **Real hardware.** No real Chromium has been driven by this software; the
  stage motion, interlock handling, and output read-back were exercised only
  against the simulator.
* **Real camera frames.** The target finders were checked on synthetic and
  screen-recorded frames; not on raw camera frames.
* **A queue's `position` with several holes** (`"1-4"`) is accepted in a file
  but a script sees only the first hole (the context name `position` is a
  single number). Use one run per hole; the run factory does this for you.
* **Watts and temperature** on a laser are not implemented; only percent.
* **Beam diameter, light and ramp values** (`beam_diameter`, `ramp_rate`) are
  passed to scripts as variables, but the Chromium driver does not set a beam
  diameter or light by itself.
