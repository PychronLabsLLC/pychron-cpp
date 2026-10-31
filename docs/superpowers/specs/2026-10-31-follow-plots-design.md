# Live follow plots and sized grain: design

Builds on `2026-10-30-dragonfly-grain-sim-design.md`.

## 1. Why

Tune tracker by eye while Follow runs. Plots show what tracker measured at each step beside what sim knows true. Sim grain today is point: no size, so nothing true to set measured glow size against. Grain gets size.

## 2. Scope

Two parts, landed in this order:

- A. Sized grain: `grain_radius_mm` in `vision::GrainModel`, config, bench, shipped scenario.
- B. Plots: step data out of `Dragonfly`, `FollowSample` from `LaserSystem`, `LaserBridge` hand-off, plot window.

Out of scope:

- Recording to file, CSV export, replay.
- Plots in `elctl laser track`.
- Grain shape other than disk. Random radius per grain: radius is the config's.
- Changing tracker behaviour or finder.
- Plots of hole centering, autocenter, non-dragonfly patterns.

## 3. Part A: sized grain

### 3.1 Parameter

`GrainParams::grain_radius_mm`, default `0`. Key `grain_radius_mm` in `[<device>.sim.grain]`, read in `libs/laser/src/camera.cpp`, range 0 to 100.

Rule: `0` keeps today's point grain exactly. Same seed gives same numbers as before: no new keyed draw, no changed draw, when radius is 0.

### 3.2 What radius `r > 0` changes in `GrainModel`

- Heat: distance in overlap measured to grain's edge, not center: `d = max(0, hypot(beam - center) - r)`, then same Gaussian in `beam_radius_mm`. Beam anywhere on grain heats it fully.
- Lobe place: drawn even over disk of radius `r`; `lobe_spread_mm` ignored.
- Lobe wander clamp: bound `r`, not `2 * lobe_spread_mm`.
- Lobe width: `min(lobe_sigma_mm, r)`.
- Hole clamp: center stays within `max(0, hole_radius_mm - r)`, so grain stays in hole. A `hole_radius_mm` of 0 or less still does not clamp.

### 3.3 Truth

`GrainTruth::radius_mm` (from `params_.grain_radius_mm`). Reaches `GrainView::truth` unchanged.

### 3.4 Score

`TrackScorer` "on grain": distance <= beam radius + grain radius. Constructor takes `grain_radius_mm`; `track_bench.cpp` passes it from scenario's table. Radius 0: same scores as before.

### 3.5 Config and docs

- New `configs/examples/track/big_grain.toml`: `grain_radius_mm = 0.5`, `lobes = 3`, rest as `hot_spots.toml`. `LaserCmd.EveryShippedScenarioIsGood` covers it.
- Key commented in `configs/examples/cameras.toml` beside other grain keys.
- `docs/user/05-laser.md` 8.3: key and what it does.

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
- Luminosity: when reason is `Miss` or `Search`.

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

Hand-off, as `Video` box of bridge: shared box with own mutex, `std::vector<laser::FollowSample> pending`, `posted` flag. Observer appends under mutex and posts one queued call only if none posted; queued call captures nothing but box's pointer. Main thread takes pending, emits `followSamples(std::vector<pychron::laser::FollowSample>)`. Pending capped 4096 rows; oldest dropped.

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

- `tests/vision/test_grain.cpp`: radius 0 equal to old numbers; beam inside grain heats as centered; lobes stay within `r`; center stays `r` inside hole; truth carries radius.
- `tests/vision`, dragonfly: `seen` set on hit, offset and area known from synthetic blob; unset on miss; set on Saturated hold with hits; decisions same.
- `tests/laser`: config reads key, range refused; scorer counts on grain to beam + grain radius; observer set after attach is heard; sample has grain on sim, none without `[sim.grain]`.
- `tests/ui/test_follow_plot_model.cpp`: lines from a sample, NaN rules, reset on earlier `elapsed_s`, true lines absent on real.
- `tests/ui/test_laser_bridge.cpp`: step reaches `followSamples` on main thread; bridge gone with observer live does not touch it.
- `tests/ui/test_laser_window.cpp`: `pattern_plots` opens view; run before opening shows.
- Hand-off shares state across threads: run under `-DPYCHRON_SANITIZE=thread` before landing.

## Open questions

1. Grain radius fixed for a run (this spec), or shrinking as grain melts? Real grains melt to bead; fixed radius gives flat true line on size strip. Shrink would add `melt_mm_per_s` (radius lost per second at full heat) and floor `min_radius_mm`, both default off.
