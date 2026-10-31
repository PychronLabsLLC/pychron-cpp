# Follow XY plot, cold grain, simulation reset

Follow-up of `2026-10-31-follow-plots-design.md`. Developer, testing `pychron-ui --sim`, asked five things.

## 1. What is built

1. Follow plots: Stage X and Stage Y strips replaced by one 2D plot, stage track and grain track, x against y, stage mm.
2. Same 2D plot shows fence: circle limiting stage.
3. Grain seen in its hole before Follow (simulated), beam off.
4. One control resets simulation: every grain back in its hole, as first made.
5. Camera pane keeps size when tab changes (Control, Calibration, Patterns).

## 2. Fence

Fence = `DragonflyParams::perimeter_radius_mm` about `start_pos_`: what `Dragonfly::project` holds target within. `PatternRunner` passes pattern's `perimeter_radius`, cut to hole's room. Plot shows radius in force, not pattern's own.

`DragonflyStep` gains:

```cpp
Vec2 start_mm{};           // where the fence is centered: stage position at start()
double perimeter_mm = 0;   // its radius
```

Set at top of `step()`, every return path, Invalid included. `FollowPlotModel`:

```cpp
struct FollowFence { double x, y, radius_mm; };
std::optional<FollowFence> fence() const;   // of latest row whose perimeter_mm > 0; cleared by clear()
```

### 2a. Fence set by hand, beam and hole

Developer: fence adjustable, also controlled by beam size and hole size; beam never more than 1/3 out of hole's perimeter.

Read as: at most 1/3 of beam diameter past hole's edge. Beam diameter `B`, hole radius `R` (half side on square):

```cpp
double hole_fence_mm(double hole_radius_mm, double beam_diameter_mm) noexcept;  // R - B/2 + B/3, at least 1e-3
```

- Radius in force = least of: what is asked (Fence box, else pattern's `perimeter_radius`), `hole_fence_mm`, 45 % of way to nearest neighbour. Applied to every dragonfly, queue's too.
- Beam: `beam_diameter_mm` in device's table of `cameras.toml`, default 0; else simulated grain's `2 * beam_radius_mm`; else unknown, `B = 0`: beam's center stays in hole.
- Fence center stays stage at start: on a hole that is hole's corrected position. Stage not sent to a hole (jog, coordinates): no hole limit.
- A dragonfly's own moves keep the hole: before, each one cleared `last_hole_` and `hole_room_`, so a second follow on a hole had no limit and Reset simulation no hole to go back to.
- `PatternRunner::set_fence_mm(double)`: in place of `perimeter_radius`, 0 pattern's. `LaserBridge::follow_simulated(name, percent, fence_mm)` sets it for one run, sets back after.
- Window: `Fence` box `pattern_fence`, simulated laser only, set from pattern on selection; note `pattern_fence_note` says what hole allows (`LaserSnapshot::hole_room_mm`).
- Real laser has no box: `perimeter_radius` in pattern's file adjusts it.

## 3. Follow plots

`FollowPlotView`: two `QCustomPlot`s side by side. Left `xy_plot()`, right `plot()` with three strips (deviation, luminosity, size), `kStrips = 3`. Window default 980 x 620.

XY plot:

- `QCPCurve` per track (not `QCPGraph`: x not monotonic), keyed by time. Stage: `StageX`/`StageY`, slot 0. Grain: `GrainX`/`GrainY`, slot 1. Row with coordinate missing left out, track joins across it (`QCPCurve` not promised to break at NaN).
- Marker at latest finite point of each track.
- Fence: `QCPCurve`, 73 points closed, slot 2, dashed.
- Equal scale always: y axis `setScaleRatio(x axis, 1)`. Circle stays circle, whatever window shape.
- Range follows union of tracks and fence, 8 % pad, until user drags or zooms. New run, or model cleared, takes range back.
- Legend lists only what has values. No empty or non-finite range ever handed to an axis.

`FollowLine` and `FollowPlotModel` rings unchanged. Colours from `plot_style()`, as `apps/pychron-ui/AGENTS.md`.

## 4. Grain in its hole

`SimTrayCamera` holds one grain per hole, not one for nearest hole:

```cpp
struct HeldGrain { vision::GrainModel model; TimePoint at; };
std::map<std::size_t, HeldGrain> grains_;   // by hole index, made when first needed
```

Rules:

- Grain made the first time its hole is in view or nearest. Seed as today (`keyed_bits(grain_seed_, "grain.hole", index)`): same grain each time made.
- Only nearest hole's grain runs under beam. Leaving a hole does not forget it: melted stays melted, moved stays moved. On coming back, its absent time run at output 0 (cooled), then beam resumes. Departure from today: "a new nearest hole is a new grain" was true of every return too; a reset meant nothing while walking away did same.
- Beam off: every in-view hole's sized grain drawn into frame by `vision::draw_grain`, after holes, before mirroring. Beam on: picture is glow, as today.
- Point grain (`grain_radius_mm = 0`) has no outline, nothing drawn. No `[sim.grain]` table: frames byte for byte as today.

```cpp
// synth.hpp: a cold grain lying in a hole, seen by illumination: outline in
// stage mm, filled at `level` (fraction of depth), brighter than hole, darker
// than tray, 1 px soft edge.
void draw_grain(Frame& f, const HoleScene& s, std::span<const Vec2> outline_mm, Vec2 stage_mm, double level = 0.5);
```

Rule: autocenter still centers hole with grain in it. `test_laser_autocenter` gains that case. Hole finder not changed to pass it.

### 4a. Machined steel

Developer, mid-build: sim holes too black against too even a grey; real holes never completely black, often crescents under binary threshold; tray between holes looks like machined stainless steel at 10-50x.

`[<device>.sim] surface = "plain" | "machined"`, default `plain`: frames, hashes, fixtures unchanged. `CameraConfig::sim_machined`.

```cpp
struct MachinedSurface { int side = 0; std::vector<float> level; };
MachinedSurface make_machined_surface(double px_per_mm, std::uint32_t seed);
void render_machined(Frame&, const HoleScene&, const MachinedSurface&, std::span<const Vec2> holes_mm, Vec2 stage_mm);
```

- Surface: 1024 px tile, made once, fixed to stage, read by reflection (no seam): cutter marks 8 degrees off y, patches, sparse pits, thin scratches.
- Hole: floor shadow 0.17, lit part 0.47 (tray 0.56). Lit part = circle of hole's radius shifted 0.2 to 0.55 radius along light, amount per hole, same every look. Threshold between leaves crescent. Bright chamfer on wall facing lamp.
- Lamp: frame falloff 14 % at corners, slight gradient.
- Beam on: picture stays the glow.
- Cold grain drawn at 0.8 on steel.

`SimpleFinder` rejects crescent (circularity): on steel Center holes fails and stage stays where it was sent. Pinned by `Autocenter.ACrescentMovesTheStageNowhereWrong`. Finder not changed here: its own task. Section 4's autocenter rule holds for `plain`.

## 5. Reset

`SimTrayCamera::reset_grains()`: empties `grains_`, zeroes `crept_`, `lit_since_`. Same seed: same grains, so tuning run repeats. `LaserSystem::reset_simulated_grains()` under gate; nothing for other cameras. `set_tray()` to another tray calls it too: hole indices mean other holes.

`LaserBridge::reset_simulation(bool center)`: refused unless `offers_simulated_follow()`. On worker, driving: resets grains, then if a hole was last sent to, goes back to it (`move_to_position(hole, center)`, waited for). Follow leaves stage off center; next run starts about where stage is.

`LaserWindow`: button `Reset simulation`, objectName `pattern_reset_sim`, beside Follow (simulated), absent on real laser. Enabled as Follow is minus output test: `can_drive && !busy && !following_`. Passes Center holes box. On success `follow_model_.clear()` and plots refreshed.

Stage untouched when no hole known: a jog or a move to coordinates forgets the hole. Beam state untouched: disabled while following; a hand-fired beam stays on.

## 6. Camera pane

Test first: offscreen window, read `camera_->size()` on each tab, sizes equal. Cause found by that test, fixed in `LaserWindow::build`; camera keeps stretch, tab block one height for all pages. Tab content never clipped: text arriving later on a page (cautions, solution, outcome) stays readable.

## 7. Tests

- `test_dragonfly`: step carries start and perimeter on Track, Invalid, Elapsed.
- `test_tray_camera`: grain seen beam off in its hole; neighbour's too; point grain and no table unchanged; melted grain still melted after going away and back; reset gives first grain back; frames same after reset as before (same seed).
- `test_synth`: `draw_grain` fills inside, leaves outside, clips at frame.
- `test_laser_autocenter`: hole with sized grain centered.
- `test_follow_plot_model`: fence, cleared with model, latest wins.
- `test_laser_views`: three strips; XY curves carry points; NaN break; fence 73 points; aspect 1 after resize; no fence no curve shown.
- `test_laser_bridge`: reset refused off sim; grains back and stage at last hole.
- `test_laser_window`: button absent on real laser; reset clears plots; camera size same on every tab.

## 8. Out of scope

- Grain position on real laser (no truth; `grain` track stays hidden).
- Hole outline on XY plot.
- New grain seed on reset.
- Drawing point grains.
- Fence on tray view or camera.

## Open questions

None.
