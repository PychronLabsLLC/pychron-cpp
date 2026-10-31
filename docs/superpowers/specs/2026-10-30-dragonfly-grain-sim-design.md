# Dragonfly grain simulator design

Date: 2026-10-30
Status: Part A implemented (§3 to §7, §9 Part A tests); Part B (§8) not built
Owner: Jake Ross
Builds on: `2026-10-04-laser-dragonfly-design.md` (§6 simulated glow, §4 runner),
`2026-10-03-vision-design.md`, `libs/sim/AGENTS.md` (keyed noise).

## 1. Goal

Tune and refine dragonfly tracking in test environment. Grain under simulated
laser heats, glows irregularly in time and space, moves. Tracker runs against
it closed loop; result is scored against ground truth.

Today (`SimTrayCamera`): one Gaussian, steady brightness (0.03 × output %),
constant drift while firing. Grain ignores beam: glow stays bright wherever
stage goes, so tracking mistake costs nothing. Only way to run tracking is
gtest; dragonfly cannot run by hand.

Two parts, each its own plan:

- Part A: grain model and headless bench (§3 to §7).
- Part B: watch it in laser window (§8). Built after Part A proves out.

## 2. Decisions

- Real `LaserSystem`, `PatternRunner`, `Dragonfly`, finder, on `ChromiumSim`.
  No tracker code replaced or forked, so settings tuned here carry to
  hardware. Rejected: `Dragonfly` with toy stage (skips settle, polls, stage
  speed, hole's room); rendered video through `RecordedSource` (does not
  follow stage, no closed loop).
- Grain model pure, no clock, in `libs/vision` beside `synth`.
- Model steps on fixed tick, not per frame.
- Random draws keyed, so one seed gives same flares and jumps at same times
  across all tracker settings: comparison of settings is fair.
- Grain heats only where beam overlaps it. Loop closed.
- Model numbers are judgement. Only real glow frames are four
  `screen_recording` stills, marked never to tune constants
  (`docs/vision_fixtures.md`). Bench compares settings against stated
  behaviour; it proves nothing about real grains. User docs say so.

## 3. Grain model

`libs/vision/include/pychron/vision/grain.hpp`, `libs/vision/src/grain.cpp`.
Standard library, `core/keyed_noise.hpp`, `core/clock.hpp`, `vision/synth.hpp` (`GlowLobe` lives there), `vision/types.hpp` only.

```cpp
struct GrainParams { /* table below */ };

struct GlowLobe {  // declared in synth.hpp, shown here for its fields
  Vec2 at_mm{};  // relative to hole's center
  double sigma_mm = 0.3;
  double peak = 0;  // fraction of depth; >1 saturates
};

struct GrainTruth {
  Vec2 center_mm{};      // relative to hole's center
  double heat = 0;       // 1: at reference_output_percent, beam centered
  double brightness = 0; // peak before lobes share it
};

class GrainModel {
 public:
  // start_mm: where grain lies, from hole's center. hole_radius_mm <= 0: no clamp.
  GrainModel(GrainParams params, std::uint64_t seed, double hole_radius_mm, Vec2 start_mm);
  // Runs whole ticks over dt. beam_mm from hole's center. Beam goes straight from where last call left it
  // to beam_mm, output constant. Remainder of a tick carried to next call.
  void advance(Duration dt, Vec2 beam_mm, double output_percent);
  std::vector<GlowLobe> lobes() const;
  GrainTruth truth() const;
  std::int64_t tick() const noexcept;  // ticks run so far
};
```

`kGrainTick` = 10 ms. Time at tick n is state after n ticks; draws keyed
(seed, name, n).

Keys `[<device>.sim.grain]`; defaults make a behaviour off, so a table naming
one behaviour has only that one.

| Key | Default | Range | Meaning |
|---|---|---|---|
| `beam_radius_mm` | 0.4 | (0, 100] | overlap's width |
| `reference_output_percent` | 20 | (0, 100] | output where heat reaches 1 |
| `heat_tau_s` | 0.5 | [0.01, 60] | lag rising |
| `cool_tau_s` | 1.0 | [0.01, 60] | lag falling |
| `glow_exponent` | 4 | [1, 12] | how steeply glow follows heat |
| `glow_peak` | 0.6 | (0, 5] | peak at heat 1 |
| `lobes` | 1 | [1, 16] | lobe slots |
| `lobe_sigma_mm` | 0.3 | (0, 100] | size of one lobe |
| `lobe_spread_mm` | 0 | [0, 100] | how far lobes sit from grain's center |
| `lobe_wander_mm_per_rt_s` | 0 | [0, 10] | lobe's random walk |
| `lobe_life_s` | 0 | 0 or [0.1, 600] | mean life of a lobe; 0: lobes never die |
| `flicker` | 0 | [0, 1] | sigma of brightness's slow flutter, as fraction |
| `flicker_tau_s` | 0.3 | [0.01, 60] | its correlation time |
| `flares_per_s` | 0 | [0, 50] | mean rate |
| `flare_gain` | 3 | [1, 100] | brightness multiple at flare's top |
| `flare_tau_s` | 0.3 | [0.01, 60] | flare's decay |
| `dropouts_per_s` | 0 | [0, 50] | mean rate of glow going dark |
| `dropout_s` | 0.5 | [0.01, 60] | how long dark |
| `creep_mm_per_s` | [0, 0] | each finite | steady creep at heat 1 |
| `wander_mm_per_rt_s` | 0 | [0, 10] | grain's random walk at heat 1 |
| `jumps_per_s` | 0 | [0, 50] | mean rate, while heat ≥ 0.5 |
| `jump_mm` | 0.3 | (0, 100] | mean size (exponential); direction uniform |

One tick, in order:

1. Beam at its interpolated place. `d` = beam to grain center.
   `overlap = exp(-d² / (2 beam_radius²))`.
2. `u = output / reference_output × overlap`.
   `heat += (u - heat) × (1 - exp(-tick / tau))`, `tau` = `heat_tau_s` when
   `u > heat`, else `cool_tau_s`.
3. Motion, each scaled by `min(heat, 1)`: creep, random walk. Jump event
   while `heat >= 0.5`. Grain center clamped to `hole_radius_mm`: grain
   stays in its hole.
4. Brightness: `glow_peak × heat^glow_exponent × flutter × flare × dark`.
   `flutter`: `exp` of Ornstein-Uhlenbeck process, sigma `flicker`.
   `flare`: `1 + (flare_gain - 1) × e`, `e` set to 1 by event, decaying with
   `flare_tau_s`. `dark`: 0 for `dropout_s` after event, else 1.
5. Lobes. Slot i: place relative to grain center, drawn within
   `lobe_spread_mm`, random walks, stays within 2 × `lobe_spread_mm`. Share
   of brightness drawn in [0.5, 1], shares normalized to sum 1. With
   `lobe_life_s` > 0: death event; share falls to 0 over `lobe_life_s` / 5,
   slot takes new place, rises back over same time.

Rules:

- A tick with `output_percent` 0 and `heat` under 1e-6 changes nothing but
  tick number, and a run of them is skipped by arithmetic: hours idle cost
  nothing.
- Every event is a draw per tick, `rate × tick`. Each name one kind of draw
  (`keyed_noise.hpp` rule).
- `heat` depends on overlap, so a grain's path depends on tracker's path;
  draws do not.

`keyed_noise.hpp` moves to `libs/core`
(`pychron/core/keyed_noise.hpp`, namespace `pychron`), since `libs/laser`
cannot reach `libs/sim`. Its includers and `libs/sim/AGENTS.md` follow.
`tests/sim/test_keyed_noise.cpp` moves to `tests/core` (its list of sources is
explicit).

## 4. Rendering

`GlowScene` gains `std::vector<GlowLobe> lobes`, in stage mm. Empty: today's
single Gaussian, byte for byte. Not empty: sum of round Gaussians over
`background` plus noise; `glow_mm`, `sigma_mm`, `elongation`, `peak` unused.
`Truth` then gives brightness-weighted center of lobes. Grain's real center is
in `GrainTruth`: the two differ, and that is the point of hot spots.

## 5. Camera

`CameraConfig` gains `std::optional<vision::GrainParams> sim_grain`. Read in
`camera.cpp` by `[<device>.sim.grain]`, ranges of §3, unknown key refused.
Absent: `SimTrayCamera` does what it does today, `grain_offset_mm` and
`glow_*` included. Present: `glow_drift_mm_per_s` and `glow_sigma_mm` are a
Config error (two grains); `grain_offset_mm` still places it.

`SimTrayCamera` with `sim_grain`:

- Holds one `GrainModel`, made first time a hole is nearest.
- Nearest hole changes: new `GrainModel`, seed mixed with hole's index: new
  hole, new grain. Stage cannot do this during a dragonfly (hole's room).
- Each grab: dt since last grab; beam = stage + `config.map().to_mm(aim_offset_px)`;
  output = `sight.output_percent` while `firing`, else 0.
- `grain_offset_mm` is `start_mm`; beam passed from hole's real position.
- Firing: glow at hole's real position + model's lobes. No hole in
  view: dark, as today. Not firing: tray picture, as today; model cools
  unseen.
- `void set_grain_seed(std::uint64_t)`, before first grab; default 1. Frame
  noise seed mixes it with `seq`.
- `std::optional<GrainView> observe()`: advances model to clock's now without
  drawing, gives truth in stage mm and beam in stage mm. Nullopt without
  `sim_grain` or hole. Bench scores between looks with it.

No new thread: model is state of `grab()`, as `crept_` is.

## 6. Runner

`PatternVision` gains
`std::function<void(const vision::DragonflyStep&)> on_step`, called from
`follow()` after each `Dragonfly::step` that returned a step. Default empty.
Nothing else in runner changes. `LaserSystem::set_step_observer(fn)`, called
before `attach_camera`, is how a caller reaches it: `attach_camera` builds the
`PatternVision`.

## 7. Bench

`libs/laser/include/pychron/laser/track_bench.hpp`,
`libs/laser/src/track_bench.cpp`; verb in `apps/elctl/src/laser_track.cpp`.

```
elctl laser track <scenario.toml> [--seeds N] [--trace <file>]
                  [--frames <dir>] [--frames-seed S] [--check]
```

Needs no `-c`, no `--sim`, no lab. Constructs own simulator in scratch
directory under `temp_directory_path()`, removed at end: `ChromiumSim`, one
small tray, calibration, pattern file, `SimTrayCamera`, `LaserSystem`. Touches
no configured device, whatever `-c` says.

Scenario:

```toml
output_percent = 20
duration = 30         # s, (0, 3600]
seeds = 20            # runs of each combination, 1 to 10000
seed = 1              # first; then seed + 1, ...
poll_ms = 100         # 10 to 1000
grain_offset_mm = [0.1, -0.1]

[grain]               # keys of [<device>.sim.grain]
[pattern]             # keys of a dragonfly pattern file; kind and duration not
[camera]              # settle_ms, frames_per_step, noise, px_per_mm, width, height

[sweep.pattern]       # any of [pattern]'s keys, as a list
aggressiveness = [0.5, 1, 2]
[sweep.grain]         # any of [grain]'s
[sweep.camera]        # any of [camera]'s
```

Every combination of `[sweep.*]`, each on same seeds. Scenario unknown key:
Config error. More than 10000 runs: Config error before anything runs.

No second reader of grain or pattern keys. For each combination, bench writes
scenario's tables as `cameras.toml` text for `CameraLibrary::parse` and a
pattern file for `PatternLibrary::load`, so ranges and unknown keys are theirs.
Every combination is loaded before first run: bad value in a sweep refused
before any time is spent. `--trace` and `--frames` targets checked writable at
same point.

Run: one thread, `ManualClock`, as `tests/laser/test_dragonfly.cpp`. Bench's camera has `on_failure = "fail"`: camera that fails is failed run, not stage held still and scored. Stage to
hole; enable; `extract(output_percent, Percent)`; fire; `execute_pattern`;
each `poll_ms`: `running()`, `observe()`, score, advance clock. At end laser
off.

Scored every poll:

- on grain: distance from beam to grain ≤ `beam_radius_mm`.
- locked: on grain for 1 s unbroken. Lost: after locked, off grain for 1 s
  unbroken; loss dated from when it went off.

Reported, one row per combination, mean and worst over seeds, best first by
mean % on grain:

| Column | Meaning |
|---|---|
| `on%` | time on grain |
| `rms`, `max` | beam to grain, mm |
| `lock_s` | time to first lock; never locked counts as duration |
| `lost` | losses (double: mean over seeds keeps fraction) |
| `back_s` | mean loss to next lock; never back counts rest of run, up to duration (not stage's trip home) |
| `moves` | stage moves (double, as `lost`) |
| `heat%` | mean heat ÷ heat of beam centered at this output |
| `track%`, `hold%`, `sat%`, `search%` | looks by reason, from `on_step` (`Track` and `PerimeterClamp`; `Deadband` and `Miss`; `Saturated`; `Search`) |

Pattern error or `Invalid` end: run counted, row marked `failed N`, first
error printed. Run still going at duration + 30 s simulated is stopped and counts as failed run.

`--trace`: CSV, one row per poll, all runs: `combo, seed, t_s, beam_x, beam_y,
grain_x, grain_y, heat, brightness, reason, saturation` (reason, saturation
empty at poll with no look). `combo` is expansion order of sweeps (first slowest, from 0); no label column.
Decimal text locale-free.
`--check`: every check, no run.
`--frames-seed` not among runs (`seed` to `seed + seeds - 1`): Config error before anything runs.
`--frames`: every frame of first combination's `--frames-seed` run (default
`seed`) as `<dir>/<nnnnn>.png` through `vision::write_png`.
Files are `mark_as_user_file`d.

Scenarios ship in `configs/examples/track/`: `steady`, `hot_spots`,
`flicker`, `jumpy`, `cooling`, `everything`. One behaviour each, last all, so
tracker's failure can be pinned on one cause. Added to install only if
`tests/setup/test_profiles.cpp` needs them; read `libs/setup/AGENTS.md`.

## 8. Part B: laser window

- `pychron-ui --sim`: lab's `[co2.sim.grain]` drives glow. Scenario's
  `[grain]` table pastes there unchanged.
- Laser window gains "Follow (simulated)" for a dragonfly pattern, at output
  in its field. Only when device's driver is simulated and camera `source` is
  `sim`; on real laser button is absent. It enables, fires, runs pattern,
  turns beam off at end, on stop, on any error.
- Overlay: cross at grain's true center, beside what finder sees, read from
  `observe()`.
- `elctl laser pattern` stays as it is (never fires).

## 9. Testing

Part A:

- `tests/vision/test_grain.cpp`:
  - same seed, same `truth()` and `lobes()`; other seed differs.
  - one 1 s `advance` equals ten 100 ms ones, beam still.
  - beam off grain: heat falls by `cool_tau_s`; on: rises by `heat_tau_s`.
  - flare and jump counts over long simulated run within 3 sigma of rate;
    mean jump size within 3 sigma of `jump_mm`.
  - cold grain does not move; grain never leaves hole.
  - all defaults, beam centered: single steady lobe, brightness `glow_peak`.
- `tests/vision/test_synth*`: lobes draw where they say; above depth
  saturates; empty lobes byte-identical.
- `tests/laser`: no `sim.grain`: frames byte-identical to today, all current
  dragonfly tests unchanged. Config keys, ranges, `glow_*` conflict.
  Beam parked off grain goes dark; dragonfly follows a jumping grain.
- `tests/laser/test_track_bench.cpp`:
  - same scenario twice: same table, same trace.
  - `steady`: on% above 95 at 20 s. Tracker's spiral search starts while glow is still building, costing about 0.5 s off grain at start when grain is off beam's first position; known finding, not tuned around. `failed` row on `steady` is fault.
  - `aggressiveness = 0` scores worse than 1 on a creeping grain.
  - `sweep` makes every combination on same seeds; 10001 runs refused.
- `apps/elctl/tests`: verb, flags, bad scenario, `--frames` and `--trace`
  write files.

No thread added: TSan not owed. Part B: `tests/ui`, button absent on real
driver, beam off after stop and error.

Docs: `docs/user/05-laser.md` (bench, what it does not prove),
`docs/user/07-configuration-reference.md` (row), `docs/dev_setup.md`,
`configs/examples/cameras.toml` (table, commented out),
`laser/camera.hpp` header comment.

## 10. Out of scope

- Changes to tracking itself: `Dragonfly`, finder, runner behaviour. Bench
  finds faults; fixing is separate work.
- Power control from brightness.
- Fitting model to real recordings.
- Motion blur, exposure, thermal spread to neighbouring grains.
- Bench against real laser or camera.
- Parallel runs of bench.

## Open questions

None.
