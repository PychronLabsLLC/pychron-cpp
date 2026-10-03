# Legacy Pychron laser, stage and vision survey

Date: 2026-10-03
Status: Survey (input to a laser application design; no design decisions here)
Owner: Jake Ross
Scope: `pychron/lasers`, `pychron/stage`, `pychron/mv`, `pychron/image`, the
laser hardware drivers, and the `pychron/tx` laser protocol.
Builds on: 2026-09-30-legacy-config-survey.md (lasers, stages and cameras
config), 2026-09-29-experiment-system-design.md §extraction device interfaces.

Source: Python Pychron `main` at `26e77ad17`, read only. Nothing was run; no
Python with numpy was available. Items tagged **[inferred]** rest on reading and
library semantics, not on execution. Paths are relative to `pychron/`.

## 1. Size

| Area | Lines | Comment lines |
|---|---|---|
| lasers/laser_managers | 6,580 | 2,698 |
| lasers/tasks (all) | 4,072 | ~1,000 |
| lasers/power | 1,990 | 523 |
| lasers/points | 1,180 | 256 |
| lasers/pattern | 3,353 | 583 |
| lasers/stage_managers | 3,015 | 868 |
| stage/ (maps, calibration) | 2,354 | |
| mv/ | 4,330 | |
| image/ | 6,367 | |
| hardware: fusions, kerr, watlow | 5,268 | |
| hardware: newport, aerotech, zaber, kinesis, motion base | 4,015 | |
| monitors | 762 | 334 |
| tx (laser protocol and server) | 1,082 | |

About 41% of `laser_managers/` is commented-out code. The live surface is much
smaller than the line count.

## 2. Laser managers

| Class | Base | Role |
|---|---|---|
| `BaseLaserManager` | Manager | Stub API; pattern execution; `_block` poll loop; power calibration factory; stage manager factory |
| `LaserManager` | Base | Local hardware: `enable_laser`, `disable_laser`, `set_laser_power`, `emergency_shutoff`, monitor, pulse, degasser |
| `FusionsLaserManager` | LaserManager | Logic board, Kerr motors (beam, zoom, mask, attenuator), fiber light, chiller, snapshot/video, `extract`/`end_extract` |
| `FusionsCO2Manager` | Fusions | CO2 logic board; internal power meter |
| `FusionsDiodeManager` | Fusions + Watlow + pyrometer mixins | Watlow, Mikron pyrometer, VueMetrix module, DPi32 monitor |
| `FusionsUVManager` | Fusions | ATL excimer: burst/continuous fire, rep rate, N2 purge, trace path, drill point; Aerotech stage |
| `OsTechDiodeManager` | LaserManager + mixins | OsTech driver, Zaber stage, MicroEpsilon pyrometer |
| `UC2000CO2Manager` | LaserManager | Novanta UC-2000 PWM, percent only |
| `TAPDiodeManager` | LaserManager | Empty shell on the OsTech config |
| `PychronLaserManager` (+UV) | Ethernet remote | Client that drives another Pychron over the text protocol (§8) |
| `ChromiumLaserManager` (CO2/Diode/UV) | Ethernet remote | Photon Machines Chromium ASCII API; microns |
| `AblationCO2Manager` | Serial remote | Third-party ablation software |

Two families: **local** managers own hardware; **remote** managers are thin
clients of another program (Pychron, Chromium, ablation).

## 3. Device roles

| Role | Driver | Used by | Wire notes |
|---|---|---|---|
| Logic board | `FusionsLogicBoard` | CO2, Diode | ASCII with prefix: `VER`, `INTLK`, `ENBL 1/0`, `DRV1` (pointer) |
| CO2 board | `FusionsCO2LogicBoard` | CO2 | `PWE 1`, `PDC <0-100>`, `ADC1` (internal meter) |
| Diode board | `FusionsDiodeLogicBoard` | Diode | `DRV0`, `IOWR1` |
| Optics motors | Kerr | Fusions | Share the logic-board port |
| Excimer | `ATLLaserControlUnit` | UV | STX/ETX/BCC frames |
| Diode module | `VueDiodeControlModule` | Diode | |
| Temperature controller | `WatlowEZZone` | Diode, OsTech | Modbus or Watlow standard bus; PID bins by temperature |
| Pyrometer | Mikron GA140; MicroEpsilon | Diode; OsTech | |
| Power meter | `AnalogPowerMeter` (ADC) | calibration, power map | |
| Chiller | `ThermoRack`, `PychronChiller` | Fusions | |
| Fiber light | `FiberLight` | Fusions, OsTech | |
| Stage | Newport ESP (default), Aerotech (UV), Zaber, Kinesis (HTTP) | all local | §5 |
| Laser controller | `OsTechLaserController`; `UC2000` | OsTech; UC2000 | |
| Camera | §7 | when `use_video` | |

## 4. Power and safety

Power:

- `set_laser_power(power, units)`: `percent` passes through; `lumens` goes to
  the video degasser; otherwise a calibration is applied when
  `use_calibrated_power` is set. A failed calibration triggers
  `emergency_shutoff`.
- Calibration: `devices/<dir>/calibrated_power.cfg`, `[PowerOutput]
  coefficients, normal_mapping`. With `normal_mapping` it is a polynomial
  evaluation; otherwise the polynomial is solved by `brentq` on 0-100.
- Closed loop: `set_laser_output(value, "temp")` puts the Watlow in closed loop,
  sets PID from the bin table, then the setpoint.
- Power map: rasters the stage, reads the meter, writes HDF5.
- Achieved output is implemented only for UV.
- **Probable break [inferred]:** `LaserManager` calls `set_laser_power_hook` but
  the mixin defines `_set_laser_power_hook`, and the diode board does not
  override the base no-op. Open-loop power on Diode and OsTech appears not to
  reach hardware.

Safety:

- Enable: controller enable, then start a monitor thread and run one check. On
  failure: "Check coolant and manual interlocks", then disable.
- Fusions `INTLK` bitmask: bit 0 external, 1 e-stop, 2 coolant flow. `ENBL 1` is
  retried up to 200 times at 3 s. CO2 disable sends `PDC 0.00` then `ENBL 0`.
- Monitors (`monitors/<name>.cfg`): `max_duration` (minutes, default 60),
  `max_coolant_temp` (25), `max_temp`, `max_unavailable`, `sample_delay`.
- `emergency_shutoff(reason)`: disable, Watlow to open loop 0, set the error
  code returned by `GetError`.
- No shutter abstraction exists.
- Timeouts: `_block` 50 s at 0.25 s; client pattern block 200 s; N2 purge delay
  30 s.

## 5. Stage

### 5.1 Hierarchy

`BaseStageManager` (maps, calibration, threaded `_move`) → `StageManager`
(controller factory, hole/point/polyline/polygon moves, points programmer) →
`VideoStageManager` (video, camera, lumen detector, autocenter, autofocus,
snapshot, recording). `RemoteStageManager` reads limits and signs only.

### 5.2 Tray map file

`#` starts a comment. The first three non-comment lines are the header:

1. `shape,dimension` (`circle|square`, mm), e.g. `circle,1.0`
2. valid hole ids, CSV
3. calibration holes, CSV: north, east, south, west, center

Hole rows: `x,y` | `id,x,y` | `x,y,(assoc)` | `x,y,r<dim>` | `id,x,y,(assoc)`.
Ids are strings, auto-numbered from 1. Rows are grouped by exact `y`.

### 5.3 Calibration

Model: center `(cx, cy)`, rotation θ (from a second "right" point), scale `s`
(1 except for fitted styles).

- Map → stage: `x' = s(x cosθ − y sinθ + cx)`, `y' = s(x sinθ + y cosθ + cy)`.
- Stage → map as implemented is `(1/s) R(−θ)(p' − c)`, an exact inverse only
  when `s = 1`.

| Style | Behaviour |
|---|---|
| Tray | Locate center, then locate right |
| Linear | Origin only |
| Free / Hole | ≥3 (reference, stage) pairs, least-squares similarity fit, reports rms |
| Irregular | Per-hole corrections instead of a transform |
| SemiAuto / Auto | Center, autocenter walk to the east/west calibration holes, traverse |

Stored as pickles under `<root>/.appdata`: `<map>_stage_calibration`,
`<map>_calibrations/<md5>` (per hole subset), `<map>_correction_file`
(`[(id, x, y)]` in stage coordinates), and
`<map>_correction_affine_file.yaml` (per zoom level).

### 5.4 Hole move

1. Look up the hole; take its stored corrected position if it has one.
2. Otherwise transform the nominal position through the calibration.
3. `linear_move(block=True)`. A target-position error triggers
   `emergency_shutoff`.
4. If video and the position was not already corrected: autocenter, and save the
   correction.

Bug: `has_correction` is referenced without being called, so the "has a
correction" test is really `abs(x) < 1e-6`. A corrected hole at x≈0 is treated
as uncorrected.

### 5.5 Motion

- Soft limits from `[Axes Limits]`; moves beyond ±2 mm of a limit raise.
- Blocking polls every 0.15 s and stops after two consecutive not-moving reads.
- A motion profiler recomputes trapezoid parameters per displacement.
- No software backlash handling.
- Homing hard-codes −25/−25 and a z of 50.

Newport ESP commands: `nTP?`, `nPA`, `nPR`, `nMD?`, `nST`, `nOR<mode>`, `nDH0`,
`nVA`/`nAC`/`nAG`, `TB?`; grouped moves `gHN a,b`, `gHV/HA/HD/HJ`, `gHL x,y`,
`gHS?`, `1HX`. `linear_move` ignores displacements under 1e-4, uses a
single-axis move when one delta is under 0.001, and on error 51 retries at half
velocity up to four times.

Aerotech Unidex 511, Zaber (ASCII library and legacy binary) and Kinesis (HTTP
`GET /positions`, `POST /move/x/y`) are the other backends. Program mode and
smooth transitions, which the polygon and polyline paths rely on, exist only for
Aerotech.

### 5.6 Points, lines, polygons, transects

YAML under the user points directory:

- `points: [{identifier, z, mask, attenuator, xy, calibrated_xy, offset_x, offset_y}]`
- `lines: [[{xy, z, mask, attenuator, velocity}]]`
- `polygons: {"0": {points, velocity, scan_size, use_convex_hull, use_outline, offset, find_min}}`
- `transects: [{points, step_points, step}]`

Polygons are traced on the perimeter then rastered by scan lines. Used by UV
only.

## 6. Patterns

Stored as Python pickles (`*.lp`) in `setupfiles/patterns`, named
`{kind}_BR{beam_radius}-{tray}`. Executed host-side point by point with
`linear_move(block=True, velocity=...)`; no dwell. Arc and contour use the
controller's `arc_move`.

| Pattern | Parameters (defaults) | Points |
|---|---|---|
| Polygon | radius 0.5, nsides, rotation | `c + r(cos, sin)(360 i/n + rot)` |
| Linear | length, rotation, npasses | alternating ends, rotated |
| Arc | radius 0.5, degrees 90 | x to radius, `arc_move` |
| CircularContour | radius 0.1, nsteps 2, percent_change 0.8 | `r = R(1 + i p)`, full circle each |
| LineSpiral | + step_scalar 5 | `r = R(1 + (i + t/360) p)`, out then in |
| SquareSpiral | as Circular | `4 n + 1` steps cycling +x, +y, −x, −y |
| Random | walk 1, npoints 10 | uniform in a box, rejected beyond walk |
| Trough | length 10, width 10, rotation | rectangle |
| Rubberband / Raster | length 15, offset, dx 0.5 | padded rectangle; zig-zag every `dx` |
| Seek, DragonFlyPeak | §7.1 | vision driven |

Each pattern can also run a Z series and a power series (sine, square, saw) in
separate threads.

## 7. Vision

### 7.1 Dragonfly

**What it does.** While the laser heats a sample, find the glowing spot in the
video and step the XY stage so the glow sits at the image centre. With no glow
visible, search outward in a spiral. It is a centroid-following servo, not a
gradient climb. It never changes laser power.

Parameters (`DragonFlyPeakPattern`):

| Parameter | Default | Meaning |
|---|---|---|
| total duration | from the caller | s |
| `duration` | 0.1 | s of frame sampling per iteration |
| `pre_seek_delay` | 0.25 | s; counts against total duration |
| `velocity` | 1 | mm/s |
| `perimeter_radius` | 2.5 | mm; search limit around the start |
| `spiral_kind` | Hexagon | or Square |
| `base` | 0.5 | mm; spiral side |
| `saturation_threshold` | 0.75 | at or above this, do not move |
| `aggressiveness` | 1 | move gain |
| `update_period` | 150 | ms between frames |
| `move_threshold` | 0.033 | mm |
| `blur`, `min_distance`, `mask_kind`, `custom_mask_radius` | | no functional effect |

Frame: cached camera frame, cropped to a square of side `2.5 × dim` mm centred
on the crosshair offset, `dim` = hole dimension × multiplier. Must be mono.
Mask radius `1.05 × dim`.

Detection (`Locator._find_targets_bs`, inverted, no filtering):

1. Normalise to 0..1; gamma 2; unsharp mask (radius 10, amount 3); rescale to
   0..255.
2. Zero outside the centred mask disk.
3. Invert.
4. Threshold sweep from `t = mean/2` to 254: binarise at `t`, invert, fill
   holes, find external contours, simplify (`eps = 0.001 × arc length`), keep
   contours with more than 3 vertices and area over 100 px². On a hit `t += 2`;
   on a miss the step grows by one. Stop at 15 accumulated targets.
5. Keep targets whose centroid is within `0.75 × pxpermm` px of the centre.

Scoring (`choose_target`): zero pixels at or below half the frame maximum; per
target `sat = sum(pixels in mask) / ((area + perimeter/2) × pixel_depth)`; take
the highest, or the smallest area if all tie. Report the centroid offset from
the image centre in px (y down) and `sat`.

Loop, until total duration elapses or cancelled:

1. Sample frames for `duration`, one per `update_period`; collect `(x, y, sat)`.
   On a detection miss sleep `update_period / 5`.
2. If any points: mean `sat`. If mean ≥ threshold, do nothing this iteration.
   Otherwise take the `sat`-weighted centroid.
3. No points: take the next spiral point, offset from the last accepted
   position.
4. With a centroid: `dx = x / pxpermm × aggressiveness`, likewise `dy`. Skip if
   either `|dx|` or `|dy|` is under `move_threshold`. New position is
   `(px + dx, py − dy)`. Reset the spiral.
5. If the new position is outside `perimeter_radius` of the start: back up,
   rescale toward the start, or fall back to the start.
6. Blocking `linear_move` at `velocity`, uncalibrated. A target-position error
   ends the loop.

Afterwards: block on the controller, return to the start, optionally disable
the laser. Nothing is saved; output is two live image plots (position trail and
annotated frame) and the average saturation.

Spirals: hexagon is a 6-sided line spiral of side `base`; square grows its side
by 1.1 per lap.

With the defaults, each iteration sees one frame.

**Sibling: Seek hill-climber.** A triangle simplex of side `base`: reflect the
worst vertex through the midpoint of the best two; discount scores 0.99 per
step; shrink to 0.75 on a repeated point; reset to the score-weighted centroid
when shrunk and the recent slope is negative.

**As found, dragonfly probably does not run [inferred]:**

- `execute_pattern` passes arguments in the wrong order, raising `TypeError` for
  every pattern launched that way.
- `numpy.invert` is applied to a float image in step 3, raising `TypeError`.
  The exception is swallowed. The intended operation is presumably `255 − src`.
- `cv_wrapper` imports `percentile` from a numpy path absent in current numpy.
- Unbound `dx`/`dy` if the first spiral point is outside the perimeter;
  division by zero when all `sat` are 0.

A port cannot be validated against the Python by running both. It needs the
intended behaviour confirmed and recorded video.

### 7.2 Autocenter

- Frame: fresh, cropped to `ceil(2.55 × dim)` mm at the crosshair offset.
- Search: same threshold sweep, not inverted, annular mask `1.4 × dim`, and a
  threshold is rejected when the white fraction is outside 0.25..0.75.
- Filter: `area / enclosing circle area > 0.35`; centroid within 0.75 mm of
  centre; area between `π(0.5 dim)²` and `π(1.25 dim)²` (circle) or
  `0.5 (2 dim)²` and `1.25 (2 dim)²` (square).
- Error: with more than two targets, the left edge of the modal bin of a 10-bin
  histogram of centroid deviations; otherwise the mean.
- Convert px to mm by `pxpermm` and a sign; two tries, moving at half velocity
  between them, then to the mean position at 0.1.
- No convergence tolerance. On failure save a snapshot and use the nominal
  position. On success store a hole correction.

### 7.3 Autofocus

Sweep Z from start (20) to end (10), sampling a frame whenever Z changes;
smooth with an 11-point Hanning window; argmax. Second sweep over
`[z − 3, z + 1]`; argmax; move there. Metric: 99th percentile of the Laplacian
(default), variance, or summed Sobel magnitude, on a 300×300 px crop.

### 7.4 Other

- Zoom → `pxpermm`: polynomial in the zoom motor value, default constant 23.
- Lumen value (`get_value`): summed gray inside the largest target over
  `area × pixel_depth`; used by the video degasser, a PID on brightness that
  drives laser power.
- Grain polygons: `find_targets` every 0.1 s on a thread; polygons persisted
  with the analysis.
- Not present: lighting control, laser-spot detection.

### 7.5 Camera layer

- Backends by identifier string: Basler pylon, ToupCam (bundled SDK), OpenCV
  `VideoCapture`, remote zmq. No FLIR.
- No capture thread. A UI timer pulls a frame for display and caches it; vision
  code reads the cached frame. Only autocenter forces a fresh one.
- Frame: mono uint8/uint16 (12-bit assumed) or RGB uint8. Transforms: swap R/B,
  flips, rotate, centre crop.
- Recording: numbered stills, then `ffmpeg` to `.avi`.
- Snapshot: raw `.tif` plus a canvas render `.jpg`.
- Video server: zmq, JPEG replies; Python 2 code, hook commented out.

### 7.6 Library inventory

| Library | Used for |
|---|---|
| cv2 | `findContours`, `approxPolyDP`, `contourArea`, `moments`, `minEnclosingCircle`, `convexHull`, `drawContours`, drawing, `VideoCapture` |
| skimage | `rgb2gray`, `adjust_gamma`, `rescale_intensity`, `unsharp_mask`, `gaussian`, `draw.disk/polygon`, `watershed`, `peak_local_max`, `canny`, Hough lines |
| scipy.ndimage | `binary_fill_holes`, `laplace`, `variance`, `sobel`, `rotate`, `label`, `distance_transform_edt` |
| numpy | histogram, percentile, polyfit/polyval, array ops |

Everything on the live dragonfly, autocenter and autofocus paths has an OpenCV
C++ equivalent or is a few lines: hole filling (contour fill), unsharp mask
(blur and weighted add), gamma (LUT), intensity rescale (`normalize`). Watershed
and peak-local-max are only on unreached paths.

### 7.7 Tests and data

No CV tests that run, and no test images or sample `.lp` files in the repo. The
one focus test points at an image outside the repo.

## 8. Remote protocol (`pychron/tx`)

TCP. Request: `Command arg1,arg2` or JSON `{"command": ...}`. Reply: text;
`True` → `OK`, `None` → `No Response`. The server closes the connection after
each reply. Errors: `ERROR <code> : <msg>`.

| Code | Meaning | Code | Meaning |
|---|---|---|---|
| 004 | Invalid arguments | 105 | Laser monitor emergency shutdown |
| 101 | Logic board comm | 106 | Setpoint |
| 102 | Enable failed | 107 | Invalid motor |
| 103 | Disable failed | 108 | Position |
| 104 | Invalid sample holder | | |

Commands with both a server handler and a client call:

- Control: `Enable`, `Disable`, `SetLaserPower`, `SetLaserOutput`,
  `GetAchievedOutput`, `GetError`, `Prepare`, `IsReady`,
  `GetPyrometerTemperature`, `GetResponseBlob`, `GetOutputBlob`, `SetLight`.
- Stage: `SetXY`, `SetX`, `SetY`, `SetZ`, `GetPosition` (`x,y,z`),
  `GetDriveMoving`, `GoToHole hole,autocenter`, `GetAutoCorrecting`,
  `CancelAutoCorrecting`, `GoToNamedPosition`, `GoToPoint`, `GetSampleHolder`.
- Patterns: `DoPattern name,duration`, `IsPatterning`, `AbortPattern`,
  `GetPatternNames`.
- Motors: `SetMotor name,value`, `SetMotorLock`, `GetMotorMoving`.
- Imaging: `Snapshot`, `StartVideoRecording`, `StopVideoRecording`, grain
  polygon start/stop/get.
- UV: `IsTracing`, `StopTrace`.

Server only: `ReadLaserPower`, `GetLaserStatus`, `StopDrive`, home commands,
beam and zoom get/set, `SetSampleHolder`. Client only, with no handler:
`Fire`, `IsFiring`, `SetReprate`, `SetNBurst`, `TracePath`, `DrillPoint`,
`WakeScreen`. `DoJog` and friends are aliases of the pattern commands.

Known client bugs: `SetXY` is sent with Python tuple parentheses; UV
`GoToPoint` joins with a comma; UV `drill_point` never sends.

Other vocabularies: Chromium (`laser.fire`, `laser.output`, `stage.moveto`,
`stage.pos?`, `Scans.*`); ablation (`SetPosition`, `SetLaserOutput`,
`SetLaserOn`).

## 9. UI

- Central pane: stage canvas, tray map or live video.
- Stage pane: calibration, hole entry, home/stop, axes, canvas and crosshair
  options, camera, snapshot, recording, degas, points programmer.
- Control pane: enable with LED, requested power, units.
- Pulse pane: power, duration.
- Optics pane: motors.
- Per-flavor pane: fiber light; Watlow, pyrometer, control module (diode); fire
  mode, burst, rep rate, energy (UV).
- Calibration task (power map, power calibration, pyrometer calibration, PID
  tuning) is no longer registered.
- Pattern maker; points programmer.

## 10. Config files

| File | Keys |
|---|---|
| `initialization.xml` | plugin `mode`, `klass`, `<communications>`, device list |
| `devices/<dir>/laser_controller.cfg` | `[General] prefix, power min, power max`; `[Motors]`; `[PowerMeter] coefficients`; `[PowerOutput] coefficients` |
| `devices/<dir>/calibrated_power.cfg` | `[PowerOutput] coefficients, normal_mapping` |
| `temperature_controller.cfg` | `[Output] use_pid_bin, scale_*`; `[Setpoint] min, max`; `[Calibration] coefficients` |
| `stage.cfg` | `[Defaults] z`; `[Axes Limits]`; `[Signs]` |
| `stage_controller.cfg` | `[General] mapping, loadposition, group_commands, base_url`; `[Axes Limits]`; `[Optional] group, joystick` |
| `<axis>axis.cfg` | velocity, acceleration, deceleration, sign, drive_ratio, ESP parameters |
| `motion_profiler.cfg` | velocity and acceleration bounds, tolerances |
| `camera.yaml` / `camera.cfg` | `Device: identifier, exposure, hflip, size`; `General: swap_rb, hflip, vflip, rotate, width, height`; `Video: fps, ffmpeg_path, max_recording_duration`; `Zoom: coefficients, fitfunc` |
| `monitors/<name>.cfg` | `[General] sample_delay, max_duration, max_coolant_temp, max_temp` |
| `patterns/*.lp` | pickled patterns |
| `tray_maps/*.txt` | §5.2 |
| `.appdata/*` | pickled calibrations, corrections, autofocus parameters |

Config directories: `fusions_co2`, `fusions_diode`, `fusions_uv`, `chromium`,
`chromium_uv`, `ablation`, `ostech_diode`, `uc2000`.

## 11. What pychron-cpp already has

- Interfaces in `libs/devices/include/pychron/devices/extraction/interfaces.hpp`:
  `IExtractionDevice`, `ILaserDevice`, `IStage`, `IPatternRunner`, `IImaging`.
  Calls block; long motions are start-then-poll.
- Script verbs are bound. `SimExtractionDevice` is the only implementation and
  only tests wire it; `LabSession` never sets `line.device`.
- No real laser driver, no config section for an extraction device, no image or
  video code, no OpenCV, no camera backend.
- No server: `libs/rpc` speaking the legacy `tx` protocol is planned and not
  started.
- No application-flavor concept. Two binaries, assembled from command-line
  config files; the installation wizard has "profiles" with no laser profile.
- Constraints on a new dependency: Windows MSVC CI installs nothing extra;
  warnings are errors; GPLv3; `find_package` first with a pinned fetch as
  fallback; heavy dependencies are optional with a stub.

## 12. Observations for the design

- Three things are format-compatible assets worth keeping: tray maps, the
  calibration model, and the `tx` command vocabulary. Pickled patterns,
  calibrations and corrections cannot be read from C++ and need a one-time
  export or to be recreated.
- Much of the breadth is dead or untested: Synrad, TAP, the calibration task,
  joystick, watershed segmentation, video server, most of the UV path.
- Vision has no ground truth. Recorded frames from a real laser box are the
  only way to validate a port of dragonfly and autocenter.
- The camera has no capture thread in Python; vision reads whatever frame the
  UI last drew. Frame timing in dragonfly is therefore tied to UI redraw.
