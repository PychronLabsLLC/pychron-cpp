# Laser system design, part 2c-1: autocenter and hole corrections

Date: 2026-10-04
Status: Implemented (section 13 lists what was decided differently). Proven on the simulated camera only.
Owner: Jake Ross
Scope: sub-project 2 of the laser program, third part, first of its own three
(2c-1 this spec; 2c-2 dragonfly; 2c-3 autofocus).
Builds on: 2026-10-03-vision-design.md (`Autocenter`, `CameraStageMap`, frame
sources; §9 caller contract; §20 "left for the laser-system sub-project"),
2026-10-04-laser-system-design.md (2a), 2026-10-04-laser-patterns-design.md (2b).

## 1. Goal

A hole move that asks for it ends with the hole under the beam, not merely at
its calibrated position: the camera looks, the stage is nudged until the hole
is centred, and what was found is remembered for that hole.
`LaserSystem::move_to_position(hole, autocenter)` takes the flag today and
ignores it; the script verb's default is already `autocenter=True`.

## 2. Decisions

- **[decision]** This part: camera configuration and frame-source wiring,
  autocenter after a hole move, per-hole corrections, a simulated camera tied
  to the simulated stage, and the vision clean-ups it needs.
- **[decision]** Frames come from the simulator and from recordings. A live
  camera on a Chromium is sub-project 4 (screen capture); until then nothing
  changes on real hardware.
- **[decision]** A found position is saved per hole, used as the start of the
  next move there, and checked again whenever a move asks for autocenter.
- **[decision]** No thread: autocenter is driven by the caller's `moving()`
  polls, as a pattern is.
- **[decision]** Autocenter never moves further than 45% of the distance to
  the nearest other hole: finding the neighbour is a failure, not a success.

## 3. Camera configuration

`<lab>/cameras.toml`, one table per extraction device (the driver's name). A
device with no table has no camera, and the autocenter flag does nothing for
it, as now.

```toml
[co2]
source = "sim"            # sim | recorded
px_per_mm = 23.0          # above 0
flip_x = false            # image +x is stage -x
flip_y = true             # image +y is stage -y (the usual camera)
aim_offset_px = [0, 0]    # the beam's place in the image, from its centre
settle_ms = 200           # after a move, before a frame is trusted; 0 to 10000

# source = "recorded"
# frames = "recordings/co2-holes"   # a fixture case directory, relative to the lab

# source = "sim": the tray as the camera would see it, and how far the real
# tray is from where the calibration says (what autocenter has to find)
[co2.sim]
tray_error_mm = [0.15, -0.10]
noise = 0.01
width = 200
height = 200

[co2.autocenter]
tolerance_mm = 0.03
max_iterations = 4
max_step_mm = 0.5
frames_per_step = 3       # 1 to 15
on_failure = "continue"   # continue | fail
```

An unknown key, a wrong type or a value out of range is a problem naming the
file and the key. A camera whose table does not load is a lab problem only
for queues that use that device (as trays and patterns). `CameraStageMap` is
`from_scale(px_per_mm, flip_x, flip_y)`; solving it from jogs waits for a live
source.

`laser::CameraConfig` holds this; `laser::CameraLibrary` the file
(`find(device)`, `problems()`).

## 4. Frame sources for a device

```cpp
// The frames a device's camera gives. `stage` is asked where the stage is
// (the sim renders from it); `clock` stamps frames.
Result<std::unique_ptr<vision::IFrameSource>> make_frame_source(
    const CameraConfig& config, const std::filesystem::path& lab,
    std::function<StageXY()> stage, const Clock& clock, const TrayView& tray);
```

- **`sim`**: `SimTrayCamera : vision::IFrameSource`. Each `grab()` renders
  `vision::HoleScene` (neighbours on) for the hole of the current tray nearest
  the stage position, at its *true* position: where the calibration puts it,
  plus `tray_error_mm`. `TrayView` is how it learns the current tray's holes
  and transform (a callback into `LaserSystem`). With no tray, or no hole
  within the view, it renders the bare tray level.
- **`recorded`**: `vision::RecordedSource` over the fixture case in `frames`.
  It does not follow the stage: for looking, not for closing the loop.

`libs/laser` gains a dependency on `pychron::vision` (which depends on `core`
only).

## 5. Corrections

`<lab>/stage_corrections/<device>.<tray>.toml`:

```toml
schema_version = 1
device = "co2"
tray = "221-hole"
tray_sha256 = "…"
calibration = "…"     # sha256 of the calibration's points: a new calibration starts again

[holes.17]
x = 12.412            # stage mm
y = 8.877
residual_mm = 0.011   # the last measured offset
found = "2026-10-04T16:20:11Z"
```

`CorrectionStore` (as `CalibrationStore`: stateless, atomic save, safe names):
`load(map, device, calibration_fingerprint)` gives the corrections that still
apply (a file for another version of the map or another calibration is
ignored, and replaced at the next save); `put(map, device, fingerprint, hole,
correction)`; `clear(device, tray)`; `clear_hole(...)`.

Used by `LaserSystem`:

- `set_tray` loads them with the calibration.
- A hole move goes to the hole's correction if it has one **and** it lies
  within the wrong-hole guard of the calibrated position; otherwise to the
  calibrated position.
- A converged autocenter writes the hole's correction (the stage position
  found, the residual).
- A failed autocenter leaves the stored correction alone.

## 6. Autocenter in `LaserSystem`

`move_to_position(hole, autocenter)` on a device with a camera, a hole on the
tray and `autocenter == true` becomes a small state machine advanced by
`moving()`:

```
Travel ──arrived──▶ Settle ──settle_ms──▶ Look ──Move──▶ Nudge ──arrived──▶ Settle …
                                            │──Converged──▶ save, Done
                                            │──Failed──▶ Return (to the start) ──arrived──▶ Done or Error
```

- **Look**: `frames_per_step` grabs, then `vision::Autocenter::step`. A grab
  error is a failure with reason `camera`.
- **Nudge**: a relative move by the step's `move_mm`, at the stage's own speed.
- **Guard**: `max_total_mm = min(1.0, 0.45 × nearest-neighbour distance)`,
  measured from the hole's *calibrated* position, so corrections cannot walk a
  hole into its neighbour over many runs. A single hole on a tray uses 1.0.
- **Hole size**: `hole_radius_mm` is half the hole's dimension from the tray
  map.
- **Return**: on failure the stage goes back to where the autocenter started
  (the corrected or calibrated position).
- **Outcome**: with `on_failure = "continue"` the move ends normally; with
  `"fail"` the last `moving()` returns a Config error naming hole, tray and
  reason, and the run fails before its script fires the laser.
- `stop()` during any phase stops the stage and ends the autocenter (outcome
  `stopped`); nothing is saved.
- A new `move_to_position`, `set_xy` or `set_axis` while one is in progress
  abandons it (nothing saved) and does what was asked.

```cpp
struct AutocenterOutcome {
  enum class Result { None, Converged, Failed, Stopped } result = Result::None;
  vision::AutocenterReason reason = vision::AutocenterReason::None;  // why it failed
  std::string hole, tray;
  int iterations = 0;
  StageXY found{};          // stage position at the end
  StageXY moved_mm{};       // from the start position
  double residual_mm = 0;
};
AutocenterOutcome LaserSystem::last_autocenter() const;
```

`LaserSystem` takes a `const Clock&` (for the settle time) and an optional
camera (`CameraConfig` and frame source). Without a camera everything is as in
2a.

## 7. Vision changes

- `AutocenterStep::reason` becomes `enum class AutocenterReason { None,
  NoTarget, MaxIterations, Runaway, MaxTotal, Invalid, Clipped, StaleFrame,
  Camera }` with `to_string` (the old words). `Camera` is the caller's (a grab
  failed).
- The stale-frame rule is the one already in `Autocenter` (a frame not newer
  than the last decision is rejected); `LaserSystem` adds the settle time by
  not grabbing until it has passed.
- The other items of vision spec §20 belong with dragonfly (2c-2).

## 8. Queue check

In a lab with extraction devices, a run on a device whose `cameras.toml` table
did not load is an error on that row (the table's problem). Nothing else: a
device without a camera is not an error.

## 9. `elctl laser`

| Command | Does |
|---|---|
| `autocenter <device> <tray> <hole> [--timeout <s>]` | moves to the hole and centres it; prints each step (offset, move) and the outcome; a converged result is saved. Exit 1 on failure, whatever `on_failure` says. |
| `corrections <device> <tray>` | the saved corrections: hole, position, distance from calibrated, residual, when |
| `corrections <device> <tray> clear [<hole>]` | forget them (or one) |
| `look <device> [--tray <tray>]` | grabs `frames_per_step` frames and says what the finder sees: centre in px, offset in px and mm, radius; moves nothing. The hole radius is the tray's, or 0.5 mm. |

## 10. Simulation and example

`configs/examples/cameras.toml` gives `co2` a `sim` camera with a tray error of
(0.15, −0.10) mm. The example laser queue then ends each hole move 0.15 and
−0.10 mm from the calibrated position, on the true hole, and
`stage_corrections/co2.example-9.toml` appears in the data of the run (the
example's own is not committed).

## 11. Testing

- `CameraConfig`: every key and range; the library and its problems.
- `CorrectionStore`: round trip; another map or calibration is ignored; safe
  names; atomic save; clear one and all.
- `SimTrayCamera`: the hole appears where the truth says for a given stage
  position and tray error; nearest hole chosen; bare tray with none.
- `vision::AutocenterReason`: each failure path gives its reason.
- `LaserSystem` autocenter on `ChromiumSim` and the sim camera: converges on
  the true hole within tolerance and saves; the next move starts at the
  correction and converges at once; `autocenter = false` uses the correction
  and takes no frames; no hole in view returns to the start (`continue` ends
  normally, `fail` errors); flipped sign is caught as runaway; a tray error
  larger than the guard fails rather than finding the neighbour; a correction
  outside the guard is ignored; `stop()` mid-way; a second move mid-way; a
  failing camera; one poll does at most one stage command; the existing
  conformance suites with a camera attached.
- Session: the example laser queue with the sim camera fires on the true
  holes; with `on_failure = "fail"` and a hidden hole the run fails with the
  laser never fired.
- `elctl`: each command on the simulator; `look` on a recorded case.

## 12. Not in this part

Dragonfly and seek (2c-2); the autofocus sweep (2c-3); screen capture and any
live camera (sub-project 4); solving the camera-to-stage map from jogs; z;
the laser window; recording autocenter outcomes in the analysis record.

## 13. As built

- **The simulated camera draws the tray's real holes**, each where it really
  is, not `vision::HoleScene`'s square grid of neighbours: the first test run
  had autocenter converge on an imagined grid hole.
- **The guard cannot help beyond about half the hole spacing.** A tray that
  far off puts a neighbour nearer to the calibrated position than the true
  hole and inside the guard. That is a wrong calibration, not something a
  camera looking at one hole can tell; documented.
- **A converged position is checked against the guard**, as is every nudge
  before it is sent, in addition to `vision::Autocenter`'s own path limit.
- **Frames no newer than the last decision are waited out for five polls**
  before the autocenter fails as `stale_frame`.
- **`AutocenterOutcome::note`**: a converged centring whose correction could
  not be written still counts; the note says so (and `elctl laser autocenter`
  exits 1).
- **A corrections file that cannot be understood is an error on load and on
  put** (it is not written over); `LaserSystem` treats it as no corrections.
- **`set_tray` abandons an autocenter in progress**, as a new move and
  `stop()` do.
- **A camera whose frames cannot be opened** is in `LabSession::problems()`
  and the device runs without a camera.
- **`elctl laser autocenter` prints the outcome**, not each step.
- The state machine is a private part of `LaserSystem` (no separate files).
- `make_frame_source` takes a `TraySightFn` (stage position and the
  calibrated hole positions) rather than separate stage and tray callbacks.
- From the final review:
  - **Only a camera that follows the stage closes the loop**
    (`usable_for_autocenter`). A `recorded` camera is never attached to a
    `LaserSystem` and `elctl laser autocenter` refuses it: it is for `look`.
    A `sim` camera is used only over a simulated stage; over a real laser
    the session reports it and **starts no queue that uses that device**
    until the table is removed (it would have "found" its made-up tray error
    and fired there). Nothing is run uncentred without having said so.
  - **`move_to_position(block=False)` with autocenter is refused** on a stage
    with a camera (`IStage::autocenter_needs_polling`): centring happens only
    while the script waits. `autocenter=False` makes an unwaited move legal.
  - **A hole move says what happened** (`IStage::last_move_note`: "hole 3:
    centred, moved 0.150, -0.100 mm" or "not centred (no_target); at its
    calibrated position"), which the script host puts in the run's log with
    the script's own output. Nothing displays a run's log yet (true of
    script `info()` too): that is the experiment window's to show.
  - **`fingerprint()` hashes the IEEE bits** of the calibration's points, not
    a library's hexfloat text, so a lab directory keeps its corrections
    across platforms.
  - The guard tests sit close to the guard (0.60 mm converges, 0.66 mm fails,
    guard 0.636), a wrong flip is pinned as `runaway` in x and in y, and the
    half-spacing limit above is pinned by a test.
