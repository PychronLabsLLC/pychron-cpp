# Laser system design, part 2a: trays, calibration, wiring

Date: 2026-10-04
Status: Implemented (section 11 lists what was decided differently). Not run against a real Chromium.
Owner: Jake Ross
Scope: sub-project 2 of the laser program (2026-10-03-vision-design.md §2),
first of three parts.
Builds on: 2026-10-03-legacy-laser-survey.md (§5 stage, §10 config),
2026-10-04-chromium-protocol-survey.md (§7, §9), 2026-09-29-experiment-system-design.md
(extraction device interfaces), 2026-10-02-experiment-window-design.md (§4 lab
and session).

## 1. Goal

A queue names `extract_device` and `tray`, a run names a hole, and its
extraction script's `move_to_position`, `extract` and `fire_laser` drive a real
or simulated laser. Today nothing does: `LabSession` never sets
`ExtractionServices::device`, and a hole name resolves only where a test
supplies a lookup.

Sub-project 2 is cut in three, each with its own spec and plan:

| Part | Content |
|---|---|
| **2a (this spec)** | tray maps, stage calibration, the facade, wiring into `Lab`, `LabSession` and `elctl` |
| 2b | patterns: definitions and a host-side runner (`IPatternRunner`) |
| 2c | vision in the loop: autocenter after a hole move, per-hole corrections, dragonfly, the items the vision spec left for this sub-project |

## 2. Decisions

- **[decision]** A lab serves several extraction devices. A run picks one by
  name.
- **[decision]** The facade owns trays and calibration. A driver moves a stage
  in millimetres and knows nothing about holes.
- **[decision]** One calibration model: a list of (hole, stage position)
  points. One point gives a shift, two a shift and a rotation (legacy "Tray":
  center then right), three or more a least-squares fit with scale (legacy
  "Free"/"Hole").
- **[decision]** Until the laser UI exists a calibration is made with `elctl`,
  reading the live stage position.
- **[decision]** Legacy tray map files are read as they are.

## 3. Library

`libs/laser`, namespace `pychron::laser`. Depends on `core` and `devices`
(the extraction interfaces). No Qt, no OpenCV. `libs/experiment` depends on it.

### 3.1 `TrayMap`

Reads the legacy format (survey §5.2):

- `#` starts a comment; blank lines are skipped.
- Header, the first three other lines: `shape,dimension` (`circle` or `square`,
  mm); valid hole ids, CSV (may be empty); calibration holes, CSV, in the order
  north, east, south, west, center (may be empty).
- Hole rows: `x,y` | `id,x,y` | `x,y,(assoc)` | `x,y,r<dim>` |
  `id,x,y,(assoc)`. A row without an id is numbered from 1 in file order. Ids
  are strings.

```cpp
struct Hole { std::string id; double x, y; double dimension; };   // map mm
enum class HoleShape { Circle, Square };
class TrayMap {
 public:
  static Result<TrayMap> parse(std::string_view text, std::string name);
  static Result<TrayMap> load(const std::filesystem::path& file);  // name: the stem
  const std::string& name() const;
  HoleShape shape() const;
  const std::vector<Hole>& holes() const;                // file order
  const Hole* find(std::string_view id) const;
  std::optional<std::string> center_hole() const;        // from the header
  std::optional<std::string> right_hole() const;         // the east hole
  const std::string& sha256() const;                     // of the file's bytes
};
```

A row that does not parse, a duplicate id, a non-finite number or a missing
header is a Config error naming the file and line. The associations (`(assoc)`)
are parsed and dropped. A hole id that is in the file but not in the "valid"
list is kept: legacy never enforced the list.

`TrayLibrary` holds the maps of a directory by name (`<lab>/tray_maps/*.txt`)
and the files that did not load.

### 3.2 `StageCalibration`

```cpp
struct CalibrationPoint { std::string hole; double x, y; };   // stage mm
struct Transform {                                            // stage = s R(theta) map + c
  double cx = 0, cy = 0, rotation = 0 /*rad*/, scale = 1;
  StageXY to_stage(double mx, double my) const;
  StageXY to_map(double sx, double sy) const;                 // exact inverse
};
struct Solution { Transform transform; double rms_mm = 0; std::size_t points = 0; };

Result<Solution> solve(const TrayMap& map, std::span<const CalibrationPoint> points);
```

- 0 points: Config error ("not calibrated").
- 1 point: shift; rotation 0, scale 1.
- 2 points: rotation from the two vectors, scale 1, the shift that puts the
  first point exactly on its hole. `rms_mm` is the miss at the second point,
  which is how a wrong scale or a mistaken hole shows.
- 3 or more: closed-form least-squares similarity (shift, rotation, scale);
  `rms_mm` over all points.
- A point naming a hole the map lacks, two points on the same hole, or points
  that coincide (rotation undefined) are Config errors.
- A fitted scale more than 2% from 1 is an error: stage and map are both in
  millimetres, so that is a mistake, not a calibration.

Legacy applies the scale to the center as well (`s(Rp + c)`). The two agree at
`s = 1`; this form has an exact inverse at any scale. Legacy pickled
calibrations cannot be read and are recreated.

### 3.3 `CalibrationStore`

`<lab>/stage_calibrations/<device>.<tray>.toml`, one per device and tray:

```toml
schema_version = 1
device = "co2"
tray = "221-hole"
tray_sha256 = "…"
# written for the reader; recomputed from the points on load
center = [12.345, 8.901]
rotation_deg = 0.42
scale = 1.0
rms_mm = 0.011

[[points]]
hole = "111"
x = 12.345
y = 8.901
```

`load`, `save` (write to a temporary file, then rename), `clear`. Only the
points are authoritative. A `tray_sha256` that no longer matches the map makes
the calibration **stale**: it is reported and not used. Device and tray names
are checked to be plain file-name parts (no separators).

### 3.4 `LaserSystem`

```cpp
class LaserSystem final : public extraction::IExtractionDevice, public extraction::IStage {
 public:
  // `driver`, `trays` and `calibrations` must outlive the system.
  LaserSystem(std::string name, extraction::IExtractionDevice& driver,
              const TrayLibrary& trays, const CalibrationStore& calibrations);
};
```

- `IExtractionDevice`: every call goes to the driver. `laser()`, `furnace()`
  and the other feature accessors return the driver's. `stage()` returns the
  system itself when the driver has a stage, null otherwise.
- `set_tray(name)`: Config error for an unknown tray. Loads the calibration;
  a missing or stale one is not an error here (a scan-only queue needs none)
  and is remembered. An empty name clears the tray.
- `move_to_position(p, autocenter)`:
  - `p` is a hole on the current tray: transform its map position, then the
    driver's `set_xy`. z is not touched. No tray, no such hole, or no usable
    calibration is a Config error naming the device, the tray and the reason;
    nothing is sent.
  - otherwise the call goes to the driver's `move_to_position` (Chromium's
    scan names, `s3`). The driver's own error stands for a name it does not
    know.
  - `autocenter` is accepted and has no effect until 2c.
- `set_axis`, `set_xy`, `position`, `moving`: the driver's.
- `positions()`: the tray's hole ids in file order.
- One mutex guards the tray and calibration; it is not held across a driver
  call.

The Chromium driver loses `TrayLookup` and `set_tray_lookup`: its
`move_to_position` keeps scan names only, its `set_tray` accepts any name and
its `positions()` is empty.

## 4. Which drivers are extraction devices

No new config section. `DriverSchema` gains `bool extraction_device`; the
Chromium schema sets it. Every `[drivers.<name>]` of a line config whose kind
has the flag is one extraction device named `<name>`. `extract_device` in a
queue or a run is that name, compared exactly.

## 5. Lab, queue check, session

**Lab** (`load_lab`): `Lab::trays` (the `TrayLibrary` of `<lab>/tray_maps`),
`Lab::calibrations` (the store on `<lab>/stage_calibrations`) and
`Lab::extract_devices` (names from §4, read from the line config without
hardware). A map that does not load goes to `Lab::problems`.

**Queue check** (`check_lab_queue`), for each run that has an extraction with a
device (its own or the queue's). All are errors on the run's row, each distinct
message once:

| Condition | Message names |
|---|---|
| the lab has extraction devices and the name is not one | the device, the known ones |
| `tray` is set and the lab has no such map | the tray |
| the run has holes and the queue has no tray | — |
| a hole is not on the tray | the hole, the tray |
| the run has holes and the device has no usable calibration for the tray | device, tray, "not calibrated" or "stale" |

A lab with no extraction device in its line config (every lab today) checks as
before: the device name is free text.

**Session**: `LabSession` builds one `LaserSystem` for each line device that
implements `IExtractionDevice` (the driver object, by `dynamic_cast` from the
line's `Device*`), named as its driver. `RunServices` gains

```cpp
std::function<extraction::IExtractionDevice*(std::string_view name)> devices;
```

A run resolves its device name through it once, uses that device as
`ExtractionServices::device` for its scripts and for `end_extraction`, and
calls `stage()->set_tray(queue.tray)` before the first script when the device
has a stage and the queue names a tray. A name that resolves to nothing leaves
`device` null, as today (the script's `extract` raises "not supported"); with
extraction devices in the lab the queue check has already refused it.
`RunServices::line.device`, when a caller sets it directly (tests), wins.

The script reads `position` from its context and calls `move_to_position`, as
now; the run does not move the stage itself.

## 6. `elctl laser`

All take the lab directory and the line config the way `elctl exp` does.

| Command | Does |
|---|---|
| `trays` | maps, hole counts, and per device: calibrated (points, rms), stale, or not |
| `calibrate <device> <tray> point <hole> [--x X --y Y]` | records the stage position at a hole: read from the device unless both `--x` and `--y` are given (then no hardware is opened). Replaces an earlier point on the same hole. Prints the solution. |
| `calibrate <device> <tray> center` / `right` | `point` at the map's center / east calibration hole; an error if the map names none |
| `calibrate <device> <tray> show` / `clear` | print points and solution / delete the file |
| `goto <device> <tray> <hole>` | move there, wait for arrival (Ctrl-C stops the waiting, not the stage: section 11), print where it is and the miss in mm |

A point that makes the set unsolvable (§3.2) is refused and the file left as
it was.

## 7. Simulation and example

The example lab gains a Chromium driver on a `kind = "sim"` transport, a small
tray map, a calibration for it and an extraction script that moves to the
run's hole, enables, extracts and fires. `elctl exp run` on it exercises the
whole path with no hardware. `SimExtractionDevice` stays a test fake.

## 8. Errors

Everything returns `Result`. Nothing guesses: an unknown hole, a missing or
stale calibration and an unknown tray stop the move before a byte is sent.
The driver still refuses a target outside its travel. A device failure during
a run reaches the run as a script error, as for any device call.

## 9. Testing

- `TrayMap`: each row form, auto-numbering, comments, every malformed case
  with its line number; a real legacy map (221-hole) as a fixture.
- `solve`: known rotations and shifts recovered from 1, 2 and N points;
  `to_map(to_stage(p)) == p`; degenerate sets refused; the 2% scale rule.
- `CalibrationStore`: round trip, stale hash, unsafe names, atomic save.
- `LaserSystem` over `ChromiumSim`: a hole lands on the right microns with
  signs applied, z unchanged, scan names pass through, each refusal sends
  nothing; the existing extraction-device and stage conformance suites.
- Queue check: one test per row of §5's table, and the unchanged behaviour of
  a lab without devices.
- `LabSession`: a queue of two runs on two holes on the sim; the simulator's
  log shows the moves, the output and the firing; the laser is off after.
- `elctl laser`: each command, including `point` with and without hardware.

## 10. Not in this part

Patterns (2b); autocenter, corrections and dragonfly (2c); the laser UI and
the calibration procedure on screen (sub-project 3); watts and temperature;
points, lines, polygons and transects; a z per tray or per hole; multi-hole
positions (a run with several holes passes the first to its script, as now).

## 11. As built

- **An unknown tray is one queue-level diagnostic** (`queue.tray`), not a
  per-run one, and is reported whenever the lab has extraction devices, even
  if no run names a hole: the run's `set_tray` would fail every run.
- **`elctl laser goto` could not stop the stage** when this part was built
  (`IStage` had no stop). Part 2b added `IStage::stop()`: Ctrl-C and
  `--timeout` now stop it.
- **`point` against an edited map starts again**: the earlier points belong
  to another version of the map and are dropped with a warning.
- **A poor fit is saved with a warning** when its rms is more than the hole
  dimension. Two points the wrong distance apart are never an error (scale is
  not fitted from two), so the warning is what shows it.
- **The run sets the queue's tray on every run** whose device has a stage,
  not only on runs with a hole.
- **A hole name wins over a driver name**: a tray with a hole called `s1`
  hides Chromium's scan 1 while that tray is set.
- **The Chromium driver alone no longer passes the stage conformance suite**
  (it has no named positions of its own to list); the suite runs over
  `LaserSystem` on the driver.
- `SimSystem::chromium(driver)` reaches a simulated laser by driver name.
- Laser end-to-end tests need embedded Python and are skipped without it.
- **A tray map that does not load stops only the queues that name it**, not
  every queue in the lab (the final review: a folder of old legacy maps must
  not stop an air queue). The parser is as lenient as legacy where legacy
  was: a `#` ends a line anywhere, a calibration line that is not five holes
  of the map means "none", hidden files and directories in `tray_maps` are
  skipped. An unknown tray is reported only when a run will use it.
- **`cautions()`**: a calibration says what its points cannot rule out. Two
  exchanged points fit perfectly half a turn round, and points on one line
  (center and right) cannot show a mirrored axis; neither shows in the rms.
  `elctl laser calibrate` and `trays` print them. The check is `elctl laser
  goto` on a hole off the line.
- **A queue with no tray clears the device's tray**, so a tray left by an
  earlier queue never gives a later one's hole names a meaning.

