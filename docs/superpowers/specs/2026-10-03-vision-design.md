# Vision library design (laser program, sub-project 1)

Date: 2026-10-03
Status: Implemented. Finders are not validated on real frames; see docs/vision_fixtures.md and section 20
Owner: Jake Ross
Scope: `libs/vision`: frames, target finding, autocenter, dragonfly, focus
metrics, camera-to-stage calibration, frame sources for test and replay.
Builds on: 2026-10-03-legacy-laser-survey.md

Nothing here has been run against an instrument.

## 1. Intent

Port Pychron's laser machine vision to pychron-cpp. Two operations matter:

- **Autocenter**: find a dark sample hole on a light tray and move the stage so
  the hole sits at the aim point.
- **Dragonfly**: while the laser heats a sample, find the bright glow on a dark
  frame and step the stage to keep the glow at the aim point; search in a
  spiral when no glow is visible.

Both are the same problem mirrored: one blob of known approximate size with
strong contrast.

The legacy code has no runnable tests and no test images, and from reading it
dragonfly appears not to run on legacy `main`. Screen recordings from 2016 and 2021 show both
working and are the reference for intended behaviour.

## 2. Program context

The laser port is split into sub-projects, each with its own spec and plan.

| # | Sub-project | Stage |
|---|---|---|
| 1 | Vision library (this spec) | D |
| 2 | Laser system: facade, sim laser and stage, tray maps, calibration, patterns, config, `LabSession` wiring | D |
| 3 | Laser UI and application flavor | D |
| 4 | Chromium client driver and screen-capture frame source | C |
| 5 | Fusions CO2: logic board, Newport ESP, camera backend, monitors | A |
| 6 | Fusions Diode: Watlow, pyrometer, closed loop | B |
| 7 | Remote server (`libs/rpc`, legacy `tx` vocabulary) | before A ships |

Decisions already taken for the program:

- **[decision]** Hardware order: hardware-agnostic first (D), then Chromium
  (C), Fusions CO2 (A), Fusions Diode (B).
- **[decision]** The laser system is a library facade. `pychron-ui --laser`
  hosts it in-process; a thin `pychron-laser` binary hosts the same facade
  behind the remote server later.
- **[decision]** Dragonfly must work on Chromium systems. It therefore depends
  only on a frame source and a stage, never on a laser type. The frame source
  is pluggable; screen capture of the Chromium video window is the first cut.
- **[decision]** Two target finders behind one interface: a dependency-free
  `simple` finder and a `legacy` finder that ports the Python pipeline. The
  default is chosen from evidence on real frames.
- **[decision]** OpenCV is optional. The core builds and is tested everywhere
  without it.

## 3. Shape

`libs/vision` depends on `core` only. No Qt, no devices, no threads.

```
libs/vision
  frame        Frame, FrameView, crop
  kernel       median, blur, threshold, Otsu, components, moments, circle fit
  finder       ITargetFinder, SimpleFinder            [always]
               LegacyFinder                           [OpenCV]
  calib        CameraStageMap
  autocenter   Autocenter
  dragonfly    Dragonfly, spiral generators
  focus        focus metrics
  synth        synthetic scenes
  source       IFrameSource, SyntheticSource, RecordedSource   [always]
               OpenCvSource                           [OpenCV]
  record       FrameRecorder
```

Dependency direction: `core <- vision <- systems`. Vision never sees `IStage`.

**Controllers are step functions.** They take frames and return a decision.
The caller owns the loop, the stage, the clock and cancellation. This gives
deterministic tests, lets one controller drive a sim stage, Chromium over TCP
or an ESP directly, and keeps timing in the existing `JobRunner` and
`CancelToken`.

Frames are grey inside vision: 8 or 16 bit, with `pixel_depth`, a timestamp and
a sequence number. Colour sources convert at the source boundary.

## 4. Types

```cpp
struct Target {
  Point2 center_px;
  double radius_px;
  double area_px;
  double circularity;
  double score;
};

enum class FinderMode { Hole, Glow };   // dark on light, bright on dark

struct FinderParams {
  FinderMode mode;
  double expected_radius_px;
  double radius_tol = 0.35;
  double mask_radius_px;
  double glow_fraction = 0.5;
};

class ITargetFinder {
 public:
  virtual ~ITargetFinder() = default;
  virtual std::vector<Target> find(const FrameView&, const FinderParams&,
                                   FinderDebug* debug = nullptr) = 0;
};
```

`FinderDebug` carries the threshold used, component count, a tally of rejection
reasons and the binary mask. It feeds UI overlays and failure snapshots and
costs nothing when null.

## 5. SimpleFinder

Dependency-free. The default.

1. Crop to the region of interest and apply a circular mask.
2. 3x3 median, to suppress one-pixel overlay lines such as crosshairs.
3. Box blur, radius `max(1, expected_radius_px / 8)`.
4. Threshold:
   - `Hole`: Otsu inside the mask, then the threshold is pulled halfway down
     from Otsu's split to the mode of the dark class, and the dark side is
     kept. A shadow on the tray is darker than the tray but lighter than the
     hole; plain Otsu puts it on the dark side and it fuses with the hole, and
     the pulled threshold keeps only pixels near the hole's own level. Assumes
     the hole is the largest dark mass: a shadow larger than the hole puts the
     mode on the shadow and the rule degrades to plain Otsu. The lower
     threshold biases `radius_px` slightly low (more with a larger blur
     radius).
   - `Glow`: `t = floor + glow_fraction * (max - floor)`, where `floor` is the
     mask median. Otsu is not used: a dark frame with a small glow is not
     bimodal.
5. Connected components, 8-way. Fill holes in each component.
6. Per component: area, centroid, bounding box, equivalent radius
   `sqrt(area / pi)`, circularity `4 pi area / perimeter^2`.
7. Select:

| | `Hole` | `Glow` |
|---|---|---|
| Radius window | `expected * [1 - tol, 1 + tol]` | none |
| Circularity | >= 0.6 | none |
| Minimum area | from the radius window | 9 px |
| Rank | distance to center, ascending | integrated intensity, descending |
| Center | least-squares circle fit on the boundary | intensity-weighted centroid of raw pixels |
| Score | circularity x radius match | saturation |

8. `Hole` only: reject components touching the mask edge.

The circle fit is used for holes because a shadowed rim pulls the area centroid
toward the lit side. It falls back to the centroid when the fit residual is
large.

The glow has no shape test because real glows are not circular: recordings show
kidney and teardrop shapes with a saturated core, a halo and smear.

**Saturation** is `mean(raw pixels in the component) / pixel_depth`, range 0..1.

Not handled: several glows (the brightest wins), holes larger than the crop,
colour reasoning.

## 6. LegacyFinder

Built only with OpenCV. A port of the Python pipeline for comparison:
normalise, gamma 2, unsharp mask (radius 10, amount 3), rescale, mask,
`255 - src`, rising-threshold contour sweep, `approxPolyDP`, area over 100 px²,
center-distance filter, `choose_target`. It keeps the legacy saturation formula
`sum / ((area + perimeter / 2) * pixel_depth)` so recorded values stay
comparable.

Known defects are fixed (float invert, unbound variables, divide by zero).
Nothing else changes.

## 7. Autocenter

```cpp
struct AutocenterParams {
  double hole_radius_mm;
  double tolerance_mm    = 0.03;   // about 0.7 px at 23 px/mm
  int    max_iterations  = 4;
  double max_step_mm     = 0.5;
  double max_total_mm    = 1.0;
  int    frames_per_step = 3;
};

struct AutocenterStep {
  enum class Action { Move, Converged, Failed } action;
  Vec2 move_mm;        // relative, stage frame
  Vec2 offset_mm;      // measured this step
  int  iteration;
  std::string reason;
};

AutocenterStep Autocenter::step(std::span<const FrameView> frames);
```

Per step:

1. Run the finder in `Hole` mode on each frame; take the per-axis median
   center.
2. Offset from the aim point (image center plus crosshair offset), converted to
   mm through `CameraStageMap`.
3. `|offset| < tolerance_mm`: `Converged`. Otherwise clamp to `max_step_mm` and
   return `Move`.
4. `Failed` when: no target in the majority of frames; the iteration cap is
   reached; cumulative movement exceeds `max_total_mm`; or the offset grows two
   steps running (a wrong sign in the calibration).

The default tolerance sits above the finder's noise (about 0.5 px); a tighter
value would never converge.

Differences from legacy **[decision]**: a real convergence test instead of two
fixed tries and an average; a median over frames instead of a histogram mode; a
runaway guard; the final residual is returned so the caller can store the hole
correction with a quality number.

## 8. Dragonfly

```cpp
struct DragonflyParams {
  Seconds total_duration;
  double perimeter_radius_mm  = 2.5;
  double saturation_threshold = 0.75;
  double aggressiveness       = 1.0;
  double move_threshold_mm    = 0.033;
  double max_step_mm          = 0.5;
  SpiralKind spiral           = SpiralKind::Hexagon;
  double spiral_base_mm       = 0.5;
  int    frames_per_step      = 1;
  int    miss_frames_before_search = 3;
};

struct DragonflyStep {
  enum class Action { Hold, Move, Done } action;
  Vec2 target_mm;      // offset from the start position, not from the current one
  double saturation;
  enum class Reason { Saturated, Deadband, Track, Search,
                      PerimeterClamp, Elapsed } reason;
};

DragonflyStep Dragonfly::step(std::span<const FrameView>, Seconds now,
                              Vec2 stage_pos_mm);
```

Per step:

1. Run the finder in `Glow` mode on the frames; collect offsets and
   saturations.
2. With any target, take the mean saturation.
   - At or above `saturation_threshold`: `Hold`, `Saturated`.
   - Otherwise take the saturation-weighted mean offset, convert to mm and
     multiply by `aggressiveness`.
   - Magnitude below `move_threshold_mm`: `Hold`, `Deadband`.
   - Otherwise clamp to `max_step_mm`: `Move`, `Track`. Reset the spiral and
     anchor it at the new position.
3. With no target for `miss_frames_before_search` consecutive steps: next
   spiral point from the last anchor, `Move`, `Search`.
4. A target outside `perimeter_radius_mm` of the start is projected onto the
   perimeter circle: `PerimeterClamp`.
5. `now >= total_duration`: `Done`.

Differences from legacy **[decision]**:

- The deadband applies to the vector magnitude. Legacy required both axes to
  exceed the threshold, so a pure x drift was never corrected.
- `max_step_mm` clamp. Legacy could jump the full offset on one noisy frame.
- Search waits for N misses. Legacy spiralled on the first.
- The perimeter is enforced by projection.
- Targets are offsets from the start position, so the caller issues absolute
  moves and relative-move rounding does not accumulate.

Legacy defaults are kept where they carry meaning: 0.75, 0.033, 2.5, 0.5,
hexagon.

The controller never commands laser power.

Spirals are pure, lazy, deterministic generators: `hexagon(base)` and
`square(base, growth = 1.1)`.

## 9. Caller contract

After any `Move` the caller waits for motion to finish, discards frames with
`timestamp < move_done + settle`, then calls `step()` again. The controller
rejects frames older than its last decision with an error rather than using
them.

## 10. Focus metrics

Plain functions over a `FrameView`: `focus::laplace_p99`, `focus::variance`,
`focus::sobel_sum`. The Z sweep belongs to sub-project 2.

## 11. Camera-to-stage calibration

```cpp
struct JogPair { Vec2 stage_delta_mm; Vec2 image_delta_px; };

struct CameraStageMap {
  double m[2][2];        // px -> mm, stage frame
  double residual_mm;    // rms of the fit
  Vec2 to_mm(Vec2 px) const;
  static CameraStageMap from_scale(double pxpermm, bool flip_x, bool flip_y);
  static Result<CameraStageMap> solve(std::span<const JogPair>);
};
```

`from_scale` reproduces the legacy `pxpermm` plus sign flags. `solve` is a
least-squares fit over at least two non-collinear jog pairs; it is what makes a
foreign stage or a second camera usable. Zoom dependence is the caller's: one
map per zoom, or a scaled map. Stored as TOML.

## 12. Frame sources

```cpp
class IFrameSource {
 public:
  virtual ~IFrameSource() = default;
  virtual Result<Frame> grab() = 0;
  virtual FrameInfo info() const = 0;
};
```

| Source | Build | Use |
|---|---|---|
| `SyntheticSource` | always | tests and sim; takes a stage-position callback so a moving stage shifts the scene |
| `RecordedSource` | always | replays a fixture directory in order |
| `OpenCvSource` | OpenCV | video file, USB camera |
| screen capture, Basler, ToupCam | later | sub-projects 4 and 5 |

A source does grey conversion, region-of-interest crop, flip and rotate. Its
config: `roi`, `channel` (`luma`, `r`, `g`, `b`), `flip_x`, `flip_y`, `rotate`.
There is no capture thread in vision.

## 13. Simulation

`synth::Scene` renders frames from parameters with a seeded generator and
returns the truth (center and radius in pixels) alongside each frame.

- **Hole**: tray brightness and texture noise, a hole grid at a pitch, dark
  disks with a soft edge, optional glint, shadow gradient, crosshair overlay
  lines, neighbouring holes.
- **Glow**: dark background, gaussian or elongated blob, saturated core, halo,
  smear, sensor noise, brightness as a function of distance from the sample.

## 14. Fixtures

```
tests/vision/data/<case>/
  0001.pgm ...
  case.toml
```

```toml
provenance = "screen_recording"   # synthetic | screen_recording | raw
mode = "hole"                     # hole | glow
expected_radius_px = 14
tolerance_px = 4
[[frames]]
file = "0001.pgm"
center_px = [108, 81]
```

- PGM, because it can be read and written without a library.
- About eight frames are committed, cropped to the video pane from the 2016 and
  2021 screen recordings. Their provenance is `screen_recording`: they carry
  overlays and compression artefacts, get loose tolerances, and are never used
  to tune constants.
- Full sequences stay outside the repo and are read from
  `PYCHRON_VISION_FIXTURES`. Tests skip when it is unset.
- Raw camera frames, when available, are added with `provenance = "raw"` and
  tight tolerances.
- `FrameRecorder` writes the same layout, so the first hardware session
  produces fixtures.

## 15. Tests

`tests/vision/`, GoogleTest, deterministic: `ManualClock`, seeded generator, no
sleeps.

| File | Asserts |
|---|---|
| `test_kernel.cpp` | median, blur, Otsu, components, moments, circle fit on hand-built arrays |
| `test_simple_finder.cpp` | synthetic hole and glow: center error under 0.5 px clean, under 1.5 px with noise, overlays and neighbours; edge-cut holes rejected; blank frame gives no target |
| `test_calibration.cpp` | `solve` recovers a known rotation, flip and scale; collinear pairs rejected |
| `test_autocenter.cpp` | closed loop against a synthetic scene: converges in at most 3 steps from a 0.3 mm offset; `Failed` on a wrong-sign map, on no target, on the iteration cap |
| `test_dragonfly.cpp` | tracks a drifting glow; holds when saturated; moves on a pure x drift; spirals after N misses and reacquires; never leaves the perimeter; `Done` at the duration |
| `test_spiral.cpp` | point sequences and determinism |
| `test_fixtures.cpp` | committed frames within `tolerance_px`; the external set when the variable is present |
| `test_legacy_finder.cpp` | OpenCV builds only; agrees with `SimpleFinder` on synthetic frames within 2 px |

## 16. Build

- `PYCHRON_VISION=ON` by default. The library needs only `core`.
- `PYCHRON_VISION_OPENCV=AUTO`: `find_package(OpenCV COMPONENTS core imgproc
  videoio)`, on when found. There is no fetch fallback; a source build is too
  heavy for CI. It defines `PYCHRON_VISION_OPENCV_ENABLED`.
- All CI jobs build and test the core. The macOS job installs OpenCV to cover
  the legacy finder.
- OpenCV headers are included as SYSTEM and confined to the two `.cpp` files
  that need them. They never appear in a public header.

## 17. Order of work

1. `frame`, `kernel`, tests.
2. `synth`, `SyntheticSource`.
3. `SimpleFinder`, tests.
4. `CameraStageMap`, tests.
5. `Autocenter`, closed-loop tests.
6. Spirals, `Dragonfly`, closed-loop tests.
7. Focus metrics.
8. `RecordedSource`, `FrameRecorder`, fixture format, committed fixtures.
9. OpenCV option, `LegacyFinder`, `OpenCvSource`, comparison test.

## 18. Out of scope

Stage motion, laser control, UI, capture threads, screen capture, camera SDKs,
the autofocus sweep, hole-correction storage, the Seek hill-climber, grain
polygons, video degas.

## 19. Open decisions

- Default finder. Decided after both run on raw frames.
- Grey channel for the colour hole camera. Luma is the starting point; the
  recordings suggest a single channel may separate holes from the tray better.
- Whether overlay suppression needs more than a 3x3 median for screen-capture
  sources with thicker overlay lines.
- The meaning of the legacy `dim` (radius or diameter of the hole), and so the
  legacy autocenter crop size. `docs/vision_fixtures.md` reads it as about 2.55
  hole radii on a side; `AutocenterParams::crop_scale` gives 5.1 radii
  (`crop_scale * 2 * radius`). Unresolved.
- `LegacyFinder` in `Hole` mode needs the target to fill 25 to 75 percent of
  the view, so at `Autocenter`'s default `crop_scale` it returns no target.
- Neither finder is validated on real camera frames (see
  `docs/vision_fixtures.md`); the committed fixtures are synthetic or screen
  recordings.

## 20. As built

Implemented by docs/superpowers/plans/2026-10-03-vision.md. Decisions taken
during implementation, where they add to or depart from the sections above:

**Finder**
- Hole threshold is not plain Otsu (section 5 step 4 as amended): a shadow
  fused with the hole under Otsu.
- `mask_radius_px <= 0` means no mask. A component touching the frame border
  counts as touching the mask edge.
- Non-finite parameters, or a zero pixel depth, give no targets.
- Pixels are stored as `uint16_t` at every depth.

**Autocenter**
- A crop that does not fit inside the frame is not searched: `Failed
  "clipped"`. The aim point must be kept away from the frame edge.
- Stale frames are rejected without a clock: a frame not later than the newest
  frame of the previous step gives `Failed "stale_frame"`.
- Rejections (`invalid`, `clipped`, `stale_frame`) do not consume an iteration.
- Adds `crop_scale` and `aim_offset_px`.

**Dragonfly**
- Crop side is 2.5 x the target diameter; mask radius 1.05 x the diameter.
- Step order: invalid, elapsed, stale, detect. `step` returns a `Result`; a
  frame older than the `now` of the step that last returned `Move` is an error.
- A frame with no target whose masked region is at or above the saturation
  threshold counts as saturated, so a glow that fills the mask holds instead
  of starting a search. A uniformly bright frame with no glow also holds.
- Saturation keeps the legacy meaning (mean blob intensity over depth), so a
  wide unclipped glow can read as saturated. `saturation_threshold` must be in
  (0, 1].
- After perimeter projection, a move shorter than the deadband is a `Hold`
  with reason `PerimeterClamp`.
- The search restarts from the anchor when a whole spiral ring falls outside
  the perimeter.
- Any step that sees the glow re-anchors the spiral there.
- Square spiral: lap n visits (+s,0), (0,+s), (-s,0), (0,-s), s = base x
  growth^(n-1).
- Adds `start()`, `target_radius_mm`, `aim_offset_px`, and reasons `Miss` and
  `Invalid`. `frames_per_step` is a hint to the caller in both controllers.

**Sources and fixtures**
- `RecordedSource` and `OpenCvSource` stamp frames from an injectable clock
  that defaults to the steady clock. A frame's timestamp must be on the clock
  the caller uses for `now`.
- Fixture frames carry `skip`; a case carries `channel` and `note`. Frame
  paths may not leave the case directory.
- Committed real frames were cut with the target deliberately off-center.
  Three of eight pass `SimpleFinder`; the rest are `skip = true` with the
  reason recorded. `LegacyFinder` passes two of eight.

**OpenCV**
- Built and tested with OpenCV 5.0, where contour functions are in the
  `geometry` module; the 4.x path is written and untested.
- `LegacyFinder` uses `0.75 x 2 x expected_radius_px` where the legacy code
  used `0.75 x px_per_mm`, because `FinderParams` carries no scale.

**Left for the laser-system sub-project**
- One stale-frame rule for both controllers, on frame timestamps only.
  (Autocenter's is in use: laser autocenter design, 2c-1. Dragonfly's waits
  for 2c-2.)
- An enum for Autocenter's reasons, separating rejections from failures.
  (Done in 2c-1: `AutocenterReason`.)
- One shared crop, find and offset helper for the two controllers.
- Scale has two sources of truth: `px_per_mm` and `CameraStageMap`.
- Dragonfly with its aim point outside the frame searches; Autocenter fails.
- The synthetic glow has no halo, smear or dependence on distance from the
  beam.
