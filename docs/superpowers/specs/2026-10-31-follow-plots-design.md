# Live follow plots and sized grain: design

Builds on `2026-10-30-dragonfly-grain-sim-design.md`.

## 1. Why

Tune tracker by eye while Follow runs. Plots show what tracker measured at each step beside what sim knows true. Sim grain today is point: no size, so nothing true to set measured glow size against. Grain gets size and shape: polygon until melted, then bead.

## 2. Scope

Two parts, landed in this order:

- A. Sized grain: polygon outline, melting to round bead, glow drawn to outline, config, bench, shipped scenarios.
- B. Plots: step data out of `Dragonfly`, `FollowSample` from `LaserSystem`, `LaserBridge` hand-off, plot window.

Out of scope:

- Recording to file, CSV export, replay.
- Plots in `elctl laser track`.
- Concave outlines, drawing outline on camera view. Random radius per grain: radius is the config's.
- Changing tracker behaviour or finder.
- Plots of hole centering, autocenter, non-dragonfly patterns.

## 3. Part A: sized grain

### 3.1 Parameters

Keys of `[<device>.sim.grain]`, fields of `GrainParams`, read in `libs/laser/src/camera.cpp`:

| Key | Default | Range | Meaning |
|---|---|---|---|
| `grain_radius_mm` | `0` | 0 to 100 | area-equivalent radius at start; 0: point grain |
| `grain_sides` | `6` | 3 to 16 | corners of outline |
| `grain_irregularity` | `0.3` | 0 to 0.6 | how far a corner's radius strays, fraction of mean |
| `body_glow` | `0.4` | 0 to 1 | share of brightness spread evenly over grain; rest is lobes |
| `melt_mm_per_s` | `0` | 0 to 10 | radius lost a second at full heat; 0: off |
| `min_radius_mm` | `0.1` | 0 to 100 | radius melting stops at |

Rule: `grain_radius_mm = 0` keeps today's point grain exactly. Same seed gives same numbers as before: no new keyed draw, no changed draw, other keys above not used.

### 3.2 Outline

Grains are polygonal until they melt. Radius above 0: grain is irregular polygon, fixed by seed at construction.

- Corner `i` of `n = grain_sides`: angle `2 * pi * (i + 0.5 * (u - 0.5)) / n`, `u` keyed `grain.corner_angle`; radius `1 + grain_irregularity * (2 * v - 1)`, `v` keyed `grain.corner_radius`. Keyed by slot at tick 0: no other draw moves.
- Unit shape: 8 points along each edge, scaled so its area is `pi`. Stored relative to grain's center.
- Outline now (`m` = roundness, 3.3; `r` = radius now): each unit point `p` becomes `r * ((1 - m) * p + m * p / |p|)`.

What radius above 0 changes in `GrainModel`:

- Heat: beam's distance to outline: 0 with beam inside, else distance to nearest edge; same Gaussian in `beam_radius_mm`. Beam anywhere on grain heats it fully.
- Lobe place: drawn even over disk of radius `r`; `lobe_spread_mm` ignored.
- Lobe wander clamp: bound `r`, not `2 * lobe_spread_mm`.
- Lobe width: `min(lobe_sigma_mm, r)`.
- Lobe peaks: sum to `(1 - body_glow) * brightness`.
- Hole clamp: center stays within `max(0, hole_radius_mm - reach)`, `reach` farthest outline point from center. A `hole_radius_mm` of 0 or less still does not clamp.

New `GrainModel::body()`: `std::optional<GlowBody>`, unset at radius 0: outline relative to hole's center, peak `body_glow * brightness`.

### 3.3 Melting

Each tick, in motion step: `radius -= melt_mm_per_s * tick * min(heat, 1)`, floored at `min_radius_mm`. Radius only falls: cooling does not restore it. New grain (new nearest hole, so new `GrainModel`) starts at `grain_radius_mm`. Melting draws nothing, so changes no keyed draw.

Roundness `m = (grain_radius_mm - radius) / (grain_radius_mm - min_radius_mm)`, in 0 to 1. Without melting 0: polygon stays polygon. At floor 1: round bead. Grain rounds as it melts; it does not round further once cold.

Every rule of 3.2 reads current radius and outline. Lobes past new bound are clamped in as bound falls.

Config reader refuses, with key named:

- `melt_mm_per_s` above 0 with `grain_radius_mm` 0: nothing to melt.
- `melt_mm_per_s` above 0 with `min_radius_mm` not above 0: radius 0 would make lobe width 0.
- `min_radius_mm` not below `grain_radius_mm` when `melt_mm_per_s` above 0.

Without melting `min_radius_mm` is not used.

### 3.4 Drawing and truth

`libs/vision` `synth.hpp`:

```cpp
// The body of a grain: light that fills an outline and stops at it.
struct GlowBody {
  std::vector<Vec2> outline_mm;  // relative to the hole's center in a model; stage mm in a GlowScene
  double edge_mm = 0.03;         // width of the soft edge
  double peak = 0;               // fraction of depth; above 1 saturates
};
```

`GlowScene::body`, `std::optional<GlowBody>`. Set: `level = background + cover * (body.peak + sum of lobes)`, `cover` 1 inside outline, falling linearly to 0 over `edge_mm` outside it. So lobes stop at outline too and blob is grain's shape. `Truth` center weighs in body at outline's centroid with `body.peak`. Unset: render exactly as now. Pixels outside outline's box plus `edge_mm` skip the outline test.

`SimTrayCamera::grab` shifts body by `scene.hole_mm`, as lobes, peak 0 with no hole in view; `lit` counts it.

`GrainTruth::radius_mm`: area-equivalent radius of outline now (shoelace), 0 for point grain. Same definition as seen size in 4.1, so lines compare. Reaches `GrainView::truth` unchanged.

### 3.5 Score

`TrackScorer` "on grain": distance to center <= beam radius + area-equivalent grain radius at that sample. `TrackScorer::sample` gains `grain_radius_mm`; `track_bench.cpp` passes `GrainTruth::radius_mm`. Radius 0: same scores as before.

### 3.6 Config and docs

- New `configs/examples/track/big_grain.toml`: `grain_radius_mm = 0.5`, `grain_sides = 6`, `lobes = 3`, rest as `hot_spots.toml`.
- New `configs/examples/track/melting.toml`: as `big_grain.toml`, with `melt_mm_per_s = 0.03`, `min_radius_mm = 0.2`. `LaserCmd.EveryShippedScenarioIsGood` covers both.
- Key commented in `configs/examples/cameras.toml` beside other grain keys.
- `docs/user/05-laser.md` 8.3: keys and what they do.

## 4. Part B: plots

### 4.1 Lines

Own window, five strips stacked, one shared time axis (seconds since dragonfly start).

| Strip | Seen / measured | True (sim only) |
|---|---|---|
| Stage X (mm) | stage x | grain x |
| Stage Y (mm) | stage y | grain y |
| Beam deviation (mm) | finder offset from aim, length | distance beam to grain center |
| Luminosity | `DragonflyStep::saturation` | `GrainTruth::brightness` |
| Grain size (diameter, mm) | `2 * sqrt(area_mm2 / pi)` | `2 * GrainTruth::radius_mm` |

Stage and grain share strips: lag reads as gap between lines. Stage x, y are measured, not true; first column names source.

True luminosity can pass 1 (flare); seen is clamped to 1. No rescale: mismatch is what saturation means.

True size strip line hidden when radius is 0: point grain has no size to show.

Seen values are NaN, drawn as break in line:

- Beam deviation, size: when step has no `seen` (miss, search, frames flooded with no blob).
- Luminosity: when reason is `Miss`, `Search`, `Elapsed` or `Invalid` (last two carry saturation 0: false point).

Real laser: true lines all NaN, legend does not list them.

### 4.2 `libs/vision` `DragonflyStep`

Gains:

```cpp
struct DragonflySeen {
  Vec2 offset_mm{};      // weighted mean offset of the glow from the aim point
  double area_mm2 = 0;   // mean area of the blob over the frames it was found in
};

struct DragonflyStep {
  // ...as now...
  Vec2 stage_mm{};                     // stage position given to step()
  double elapsed_s = 0;                // since start()
  std::optional<DragonflySeen> seen;   // unset: no blob in any frame
};
```

`offset_mm` is `map_.to_mm(mean_px)` before aggressiveness, same `mean_px` tracker uses. `area_mm2` is `Target::area_px / (px_per_mm * px_per_mm)`, unweighted mean over hits. Set before the branches on saturation, so a Saturated hold with hits has it. `stage_mm` and `elapsed_s` set on every return but the not-started one. Decisions of `step()` do not change.

### 4.3 `libs/laser` `FollowSample`

```cpp
struct FollowSample {
  vision::DragonflyStep step;
  std::optional<GrainView> grain;  // simulated grain only
};
```

`LaserSystem::set_follow_observer(std::function<void(const FollowSample&)>)`. Empty function: none.

`attach_camera` gives runner one fixed `on_step` that calls system's `report_step`, which reads `step_observer_` and `follow_observer_` as they are then. So an observer set after attach is heard; today one set after attach is ignored. `set_step_observer` keeps its signature; `track_bench` unchanged.

`report_step` runs on runner's thread under system's gate. Builds sample only when follow observer is set. `grain` from `SimTrayCamera::observe()`, as `grain_px` does. Observer only records: no device call, no wait, no Qt.

### 4.4 `apps/pychron-ui` bridge

`LaserBridge` sets follow observer in constructor and clears it, under gate, before anything observer points at goes.

Hand-off: `FollowMailbox` (`follow_mailbox.{hpp,cpp}`), Qt-free, held by `std::shared_ptr` observer captures. `add` appends under own mutex, drops oldest past 4096 rows, says whether a post is needed. Observer posts one queued call only then; queued call carries nothing. Main thread `take`s rows, emits `followSamples(std::vector<pychron::laser::FollowSample>)`.

Runs for every dragonfly on this system, whoever started it (Follow (simulated), pattern run, queue).

### 4.5 Model and view

`FollowPlotModel` (`follow_plot_model.{hpp,cpp}`), no widgets, one `TimeSeriesRing` per line, ring holds 10 min at 25 rows a second. Rules:

- `elapsed_s` below previous row's: chart resets, row is first of new run. So a new dragonfly clears chart and last run stays until then.
- Non-finite value kept as NaN (`TimeSeriesRing::range` skips NaN).

`FollowPlotView` (`follow_plot_view.{hpp,cpp}`), QCustomPlot, top-level window parented to `LaserWindow`:

- X range shared by all strips, zoom and pan on one moves all.
- Y each strip autoranges from its rings.
- Colours `plot_series_color(slot)`, background `plot_style()`, connects to `PlotStyleHub::instance()` `changed()` (`apps/pychron-ui/AGENTS.md`). Seen lines one slot, true another, same on every strip.
- Repaint capped at 20 Hz as `StripChartView`, deferred, not dropped.

### 4.6 Window

`LaserWindow` makes view hidden and feeds model from `followSamples` always, so a run started before opening shows whole. Button `Plots` on Patterns tab, object name `pattern_plots`, shows and raises it. Enabled on any system with a camera.

## 5. Rules that must hold

- Radius 0: every existing grain and bench number unchanged (`tests/vision/test_grain.cpp`, `tests/laser` bench tests pass unedited).
- Plots change no tracker decision. Same seed same steps with observer set or not.
- Observer holds no lock across clock time and calls no device. `AGENTS.md` Time rules stand.
- No `libs/` Qt: `FollowSample` plain C++.
- Sim-only data behind `dynamic_cast<SimTrayCamera*>` in `LaserSystem`, not in UI.

## 6. Tests

- `tests/vision/test_grain.cpp`: radius 0 equal to old numbers; seed fixes outline, area matches `grain_radius_mm`; beam inside polygon heats as centered, beam past flat edge less than past corner; lobes stay within `r`; center stays `reach` inside hole; truth carries radius; lobe and body peaks sum to brightness; roundness 0 unmelted, 1 at floor; radius falls at `melt_mm_per_s` at full heat, slower at less, stops at `min_radius_mm`, does not rise when cold; lobes follow falling bound; melting changes no other draw.
- `tests/vision/test_synth.cpp`: body fills polygon and stops at edge; lobes cut at outline; no body renders as before; hexagon of radius 0.5 mm found in Glow mode with default dragonfly params.
- `tests/vision`, dragonfly: `seen` set on hit, offset and area known from synthetic blob; unset on miss; set on Saturated hold with hits; decisions same.
- `tests/laser`: config reads keys, range and the three melting refusals; scorer counts on grain to beam + grain radius; observer set after attach is heard; sample has grain on sim, none without `[sim.grain]`.
- `tests/ui/test_follow_plot_model.cpp`: lines from a sample, NaN rules, reset on earlier `elapsed_s`, true lines absent on real.
- `tests/ui/test_laser_bridge.cpp`: step reaches `followSamples` on main thread; bridge gone with observer live does not touch it.
- `tests/ui/test_laser_window.cpp`: `pattern_plots` opens view; run before opening shows.
- Hand-off shares state across threads: run under `-DPYCHRON_SANITIZE=thread` before landing.

## Open questions

None.
