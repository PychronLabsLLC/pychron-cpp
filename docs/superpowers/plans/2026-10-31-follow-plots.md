# Live Follow Plots and a Polygonal Grain Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give the simulated grain a polygonal outline that melts to a round bead, and plot stage, grain, beam deviation, luminosity and grain size live while a dragonfly follows it.

**Architecture:** `vision::GrainModel` gains an outline and a falling radius, and `render` gains a `GlowBody` that clips the light to that outline. `DragonflyStep` carries what the tracker measured; `LaserSystem` joins it to the simulator's truth as a `FollowSample`; `LaserBridge` hands samples to the main thread; a widget-free `FollowPlotModel` feeds a QCustomPlot window opened from the Patterns tab.

**Tech Stack:** C++20, GoogleTest (`tests/vision`, `tests/laser`), QtTest (`tests/ui`), Qt 6, QCustomPlot.

**Spec:** `docs/superpowers/specs/2026-10-31-follow-plots-design.md`

## Global Constraints

- `grain_radius_mm = 0` keeps today's point grain exactly: no new keyed draw, no changed draw. Existing tests in `tests/vision/test_grain.cpp`, `tests/vision/test_synth.cpp` and `tests/laser/test_track_bench.cpp` pass unedited.
- `GlowScene::body` unset renders exactly as now.
- Plots change no tracker decision: same seed, same steps, with or without an observer.
- The follow observer runs on the runner's thread under the system's gate: it only records. No device call, no wait, no Qt in `libs/`.
- `FollowPlotView` colours come from `plot_style()` / `plot_series_color(slot)` and it connects to `PlotStyleHub::instance()`'s `changed()`. No colour literal, no `theme()`.
- Read `apps/pychron-ui/AGENTS.md` before Tasks 7 to 9.
- Compiler warnings met only in CI are listed in `AGENTS.md`: `const auto&` over pairs, no shadowing, no designated initializer that leaves fields out.

## Review Focus

- A polygon of radius 0.5 mm against the finder's `radius_tol` around `target_radius_mm` and its circularity gate: an unmelted grain must still be followed with default settings. Pinned in Task 3 (found) and Task 4 (`big_grain` holds lock).
- A grain whose radius is near `hole_radius_mm`: center clamp gives 0 and the grain sits at the hole's center, no NaN. Pinned in Task 1.
- Bridge destroyed while a dragonfly runs: observer must be cleared before the box goes. Pinned in Task 7.
- Dropped samples when the main thread stalls: oldest go, newest stay, chart does not reset. Pinned in Task 7.
- Follow started twice in a row, or stopped at once: second run clears first; one row runs do not crash autorange. Pinned in Task 8.

---

### Task 1: Polygonal grain

**Files:**
- Modify: `libs/vision/include/pychron/vision/grain.hpp`, `libs/vision/src/grain.cpp`, `libs/vision/include/pychron/vision/synth.hpp`
- Test: `tests/vision/test_grain.cpp`

**Interfaces:**
- Produces: in `synth.hpp`, `struct GlowBody { std::vector<Vec2> outline_mm; double edge_mm = 0.03; double peak = 0; };`. In `GrainParams`: `grain_radius_mm = 0`, `grain_sides = 6`, `grain_irregularity = 0.3`, `body_glow = 0.4`, `melt_mm_per_s = 0`, `min_radius_mm = 0.1`. `GrainTruth::radius_mm`. `std::optional<GlowBody> GrainModel::body() const`. `std::vector<Vec2> GrainModel::outline() const` (relative to the hole's center; empty at radius 0).

- [ ] **Step 1: Write the failing tests**

Helper `sized()` returns `GrainParams` with `grain_radius_mm = 0.5`, `beam_radius_mm = 0.2`. Tests in suite `GrainShape`:

```cpp
TEST(GrainShape, TheSeedFixesTheOutline)        // two models, same seed: outlines equal; seed 2: differ
TEST(GrainShape, ItsAreaIsTheRadiusGiven)       // EXPECT_NEAR(truth().radius_mm, 0.5, 1e-9) for irregularity 0 and 0.6
TEST(GrainShape, ItHasTheCornersAsked)          // outline().size() == sides * 8 for sides 3, 6, 16
TEST(GrainShape, APointGrainHasNoOutline)       // radius 0: outline().empty(), !body(), truth().radius_mm == 0
TEST(GrainShape, ABeamAnywhereOnItHeatsAsCentered) // beam at 0.3 * outline point: heat near same run with beam at center
TEST(GrainShape, ABeamPastTheEdgeHeatsLess)     // beam at 2 * outline point: heat below centered run
TEST(GrainShape, LobesStayWithinTheRadius)      // 3 lobes, wander on, 60 s: every lobe within 0.5 of center
TEST(GrainShape, ItStaysInsideItsHole)          // hole 1.0, creep outward, 120 s: every outline point within 1.0 of origin
TEST(GrainShape, AGrainBiggerThanItsHoleSitsAtTheCenter) // hole 0.2, radius 0.5: center is {0, 0}, all finite
TEST(GrainShape, BodyAndLobesShareTheBrightness) // body()->peak + sum lobe peaks near truth().brightness; body()->peak near 0.4 * brightness
```

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build --preset dev --target pychron_vision_tests && build/dev/tests/vision/pychron_vision_tests --gtest_filter='GrainShape.*'`
Expected: does not compile (`grain_radius_mm` unknown).

- [ ] **Step 3: Add the parameters, `GlowBody`, the unit shape and the outline**

Spec 3.2 pins the corners and the outline formula. Build the unit shape in the constructor only when `grain_radius_mm > 0`, using `slot_uniform("grain.corner_angle", i)` and `slot_uniform("grain.corner_radius", i)`; scale it by `sqrt(pi / shoelace_area)`. Hold `radius_`, `roundness_ = 0`, `shape_` (current outline relative to the grain's center) and `reach_`, with a private `reshape()` that rebuilds the last two. Replace `overlap`'s distance by a file-local `double gap_to_outline(const std::vector<Vec2>&, Vec2 from_center)` (0 inside by crossing number, else nearest segment); with an empty outline it returns the length, so a point grain keeps its numbers. In `step`, `hole_radius_mm_ - reach_` clamped at 0 is the bound. `draw_place` and `step_lobes` use `radius_` when `grain_radius_mm > 0`. `lobes()` uses `min(lobe_sigma_mm, radius_)` and `b * (1 - body_glow)` when sized.

- [ ] **Step 4: Run them to see them pass, with the old grain tests**

Run: `build/dev/tests/vision/pychron_vision_tests --gtest_filter='Grain*'` after rebuilding.
Expected: all pass, `tests/vision/test_grain.cpp` existing tests unedited.

- [ ] **Step 5: Commit**

```bash
git add libs/vision tests/vision/test_grain.cpp
git commit -m "feat(vision): a simulated grain has a polygonal outline"
```

### Task 2: Melting

**Files:**
- Modify: `libs/vision/src/grain.cpp`
- Test: `tests/vision/test_grain.cpp`

**Interfaces:**
- Consumes: `reshape()`, `radius_`, `roundness_` from Task 1.

- [ ] **Step 1: Write the failing tests**

Suite `GrainMelt`, `melty()` is `sized()` with `melt_mm_per_s = 0.05`, `min_radius_mm = 0.2`, wide beam:

```cpp
TEST(GrainMelt, ItLosesRadiusAtTheRateGivenAtFullHeat) // heat pinned near 1 after warm-up: 2 s later radius down 0.1 within 2 %
TEST(GrainMelt, ItLosesLessWhenCooler)                 // output 10 of reference 20: slower than full heat
TEST(GrainMelt, ItStopsAtTheFloor)                     // 60 s: truth().radius_mm near 0.2, not below
TEST(GrainMelt, ItDoesNotGrowBackWhenCold)             // melt, then output 0 for 30 s: radius same
TEST(GrainMelt, ItIsRoundAtTheFloor)                   // every outline point within 1e-9 of 0.2 from center
TEST(GrainMelt, UnmeltedIsStillAPolygon)               // melt 0, 60 s heat: outline point spread unchanged from start
TEST(GrainMelt, LobesFollowTheFallingBound)            // lobes within current radius at the floor
TEST(GrainMelt, MeltingMovesNoDraw)                    // busy() + sized, melt on vs off, wide beam, 10 s: truth().heat and flicker-driven brightness equal
```

For `MeltingMovesNoDraw` compare `truth().heat` and `truth().brightness` only, since the shape differs.

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build --preset dev --target pychron_vision_tests && build/dev/tests/vision/pychron_vision_tests --gtest_filter='GrainMelt.*'`
Expected: `ItLosesRadiusAtTheRateGivenAtFullHeat` and others FAIL.

- [ ] **Step 3: Melt in the motion step of `GrainModel::step`**

After `live` is computed, only when `melt_mm_per_s > 0`: lower `radius_` by `melt_mm_per_s * kTickS * live`, floor at `min_radius_mm`, set `roundness_` per spec 3.3 and call `reshape()`.

- [ ] **Step 4: Run them to see them pass**

Run: `build/dev/tests/vision/pychron_vision_tests --gtest_filter='Grain*'`
Expected: all pass.

- [ ] **Step 5: Commit**

```bash
git add libs/vision/src/grain.cpp tests/vision/test_grain.cpp
git commit -m "feat(vision): a simulated grain melts to a round bead"
```

### Task 3: The glow takes the grain's shape

**Files:**
- Modify: `libs/vision/include/pychron/vision/synth.hpp`, `libs/vision/src/synth.cpp`, `libs/laser/src/tray_camera.cpp`
- Test: `tests/vision/test_synth.cpp`, `tests/laser/test_tray_camera.cpp`

**Interfaces:**
- Consumes: `GlowBody`, `GrainModel::body()`.
- Produces: `std::optional<GlowBody> GlowScene::body`.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST(GlowBody, ItFillsTheOutlineAndStopsAtIt)  // square 1 mm side, peak 0.6, no noise: center pixel near 0.6 + background, pixel 0.1 mm past the edge at background
TEST(GlowBody, LobesAreCutAtTheOutline)        // lobe sigma 1 mm, square body peak 0: outside square is background
TEST(GlowBody, NoBodyRendersAsBefore)          // same scene twice, body unset vs. pre-change expectation: lobes test values unchanged
TEST(GlowBody, TruthWeighsTheBody)             // body right of a lobe, equal peaks: truth center between them
TEST(GlowBody, AHexagonIsFoundAsAGlow)         // 0.5 mm grain from GrainModel at heat 1, FinderParams as Dragonfly sets them with default target_radius_mm: SimpleFinder returns a target, area within 15 % of pi * 0.25 mm2
```
And in `tests/laser/test_tray_camera.cpp`:
```cpp
TEST(SimTrayCamera, ASizedGrainGlowsOverItsOutline) // [sim.grain] grain_radius_mm 0.5, firing: bright pixels' area within 25 % of the grain's
TEST(SimTrayCamera, ASizedGrainIsDarkWithNoHoleInView) // stage off the holes: frame has nothing lit, truth not visible
```

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build --preset dev --target pychron_vision_tests && build/dev/tests/vision/pychron_vision_tests --gtest_filter='GlowBody.*'`
Expected: does not compile (`GlowScene::body` unknown).

- [ ] **Step 3: Add `GlowScene::body`, draw it in `render_lobes`, pass it from `SimTrayCamera::grab`**

`render` goes to `render_lobes` when `body` is set, even with no lobes. Convert the outline to pixels once with `target_px`; `cover(x, y)` is 1 inside, `1 - gap / edge_px` to 0 past it; outside the outline's box grown by `edge_px` it is 0 without a test. Spec 3.4 fixes the level and the truth. If `AHexagonIsFoundAsAGlow` fails on the finder's radius or circularity gate, stop and report: the spec excludes changing the finder.

- [ ] **Step 4: Run them to see them pass**

Run: `cmake --build --preset dev --target pychron_vision_tests pychron_laser_tests && build/dev/tests/vision/pychron_vision_tests && build/dev/tests/laser/pychron_laser_tests --gtest_filter='SimTrayCamera.*'`
Expected: all pass.

- [ ] **Step 5: Commit**

```bash
git add libs/vision libs/laser/src/tray_camera.cpp tests/vision/test_synth.cpp tests/laser/test_tray_camera.cpp
git commit -m "feat(vision): a grain's glow takes its outline"
```

### Task 4: Config, score, scenarios, docs

**Files:**
- Modify: `libs/laser/src/camera.cpp`, `libs/laser/include/pychron/laser/camera.hpp` (key comment), `libs/laser/include/pychron/laser/track_score.hpp`, `libs/laser/src/track_score.cpp`, `libs/laser/src/track_bench.cpp`, `configs/examples/cameras.toml`, `docs/user/05-laser.md`
- Create: `configs/examples/track/big_grain.toml`, `configs/examples/track/melting.toml`
- Test: `tests/laser/test_camera.cpp`, `tests/laser/test_track_score.cpp`, `tests/laser/test_track_bench.cpp`

**Interfaces:**
- Produces: `void TrackScorer::sample(double t_s, double distance_mm, double heat, double grain_radius_mm = 0);`

- [ ] **Step 1: Write the failing tests**

```cpp
TEST(CameraConfig, ReadsTheGrainsSize)                  // all six keys of spec 3.1 reach GrainParams
TEST(CameraConfig, RefusesAGrainOutOfRange)             // grain_sides = 2, grain_irregularity = 0.7, body_glow = 1.5 each name their key
TEST(CameraConfig, RefusesMeltingNothing)               // melt 0.1, radius 0: error names melt_mm_per_s
TEST(CameraConfig, RefusesMeltingToNothing)             // melt 0.1, radius 0.5, min 0: names min_radius_mm
TEST(CameraConfig, RefusesAFloorAboveTheGrain)          // melt 0.1, radius 0.5, min 0.5: names min_radius_mm
TEST(TrackScorer, TheGrainsRadiusCountsAsOn)            // beam radius 0.4, distance 0.7: on with grain radius 0.5, off with 0
TEST(TrackBench, ABigGrainIsHeldOn)                     // configs/examples/track/big_grain.toml, 20 s, one seed: on_pct >= 95
TEST(TrackBench, AMeltingGrainIsHeldOn)                 // melting.toml, 20 s, one seed: on_pct >= 90
```

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build --preset dev --target pychron_laser_tests && build/dev/tests/laser/pychron_laser_tests --gtest_filter='CameraConfig.*:TrackScorer.*:TrackBench.A*Grain*'`
Expected: does not compile (`sample` takes three arguments) until the signature is added; then FAIL.

- [ ] **Step 3: Implement**

`read_grain`: `g.number`/`g.whole` with spec 3.1's ranges, then the three refusals with `g.fail(key, why)` after the numbers, as `lobe_life_s` does. `track_bench.cpp:501` passes `view->truth.radius_mm`. Write both scenarios per spec 3.6 with the header comment style of `hot_spots.toml`, the keys commented in `cameras.toml` beside `lobe_spread_mm`, and `docs/user/05-laser.md` 8.3. If a bench bound is missed, find out why before moving it (root `AGENTS.md`: never relax a failing test to pass).

- [ ] **Step 4: Run them to see them pass, with the shipped scenarios**

Run: `cmake --build --preset dev --target pychron_laser_tests elctl pychron_elctl_tests; build/dev/tests/laser/pychron_laser_tests --gtest_filter='CameraConfig.*:TrackScorer.*:TrackBench.*'; ctest --preset dev -R 'LaserCmd'`
Expected: all pass, `LaserCmd.EveryShippedScenarioIsGood` covers the two new files.

- [ ] **Step 5: Commit**

```bash
git add libs/laser configs/examples docs/user/05-laser.md tests/laser
git commit -m "feat(laser): grain size and melting in config, score and scenarios"
```

### Task 5: What the tracker measured

**Files:**
- Modify: `libs/vision/include/pychron/vision/dragonfly.hpp`, `libs/vision/src/dragonfly.cpp`
- Test: `tests/vision/test_dragonfly.cpp`

**Interfaces:**
- Produces: `struct DragonflySeen { Vec2 offset_mm{}; double area_mm2 = 0; };`, and on `DragonflyStep`: `Vec2 stage_mm{}`, `double elapsed_s = 0`, `std::optional<DragonflySeen> seen`.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST(Dragonfly, SaysWhatItSaw)                   // glow 0.2 mm off aim: seen->offset_mm within 0.02 of it, seen->area_mm2 above 0
TEST(Dragonfly, TheOffsetIsBeforeAggressiveness) // aggressiveness 0.5: seen offset same as at 1
TEST(Dragonfly, SaysNothingSeenOnAMiss)          // dark frame: !seen, reason Miss
TEST(Dragonfly, SaysWhatItSawWhenSaturated)      // saturated hold with a blob: seen set
TEST(Dragonfly, SaysWhereAndWhen)                // step at start + 3 s from stage (1, 2) given start (1, 2): stage_mm and elapsed_s 3
TEST(Dragonfly, SeeingChangesNoDecision)         // run of TracksDriftingGlow: actions and targets as asserted there
```

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build --preset dev --target pychron_vision_tests && build/dev/tests/vision/pychron_vision_tests --gtest_filter='Dragonfly.*'`
Expected: does not compile.

- [ ] **Step 3: Implement**

`Hit` gains `double area_px`. Move the `mean_px` calculation above the saturation branch, unchanged, so `seen` is set whenever `hits` is not empty; `area_mm2` per spec 4.2. Set `stage_mm` and `elapsed_s` at the top of `step` once `started_` is known.

- [ ] **Step 4: Run them to see them pass**

Run: `build/dev/tests/vision/pychron_vision_tests --gtest_filter='Dragonfly.*'`
Expected: all pass.

- [ ] **Step 5: Commit**

```bash
git add libs/vision tests/vision/test_dragonfly.cpp
git commit -m "feat(vision): a dragonfly step says what it measured"
```

### Task 6: `FollowSample` from `LaserSystem`

**Files:**
- Modify: `libs/laser/include/pychron/laser/laser_system.hpp`, `libs/laser/src/laser_system.cpp`
- Test: `tests/laser/test_laser_system.cpp`

**Interfaces:**
- Consumes: `DragonflyStep` fields of Task 5, `GrainView`, `SimTrayCamera::observe()`.
- Produces: `struct FollowSample { vision::DragonflyStep step; std::optional<GrainView> grain; };`, `void LaserSystem::set_follow_observer(std::function<void(const FollowSample&)> fn);`

- [ ] **Step 1: Write the failing tests**

```cpp
TEST(LaserSystemFollow, AnObserverSetAfterTheCameraIsHeard) // attach_camera, then set_step_observer: a dragonfly's steps arrive
TEST(LaserSystemFollow, ASimulatedGrainIsInTheSample)       // [sim.grain]: sample.grain set, grain_mm finite
TEST(LaserSystemFollow, NoGrainTableNoGrainInTheSample)     // no [sim.grain]: sample.grain empty, sample.step set
TEST(LaserSystemFollow, ClearingTheObserverStopsIt)         // set_follow_observer({}) mid-run: no more samples
TEST(LaserSystemFollow, ObservingChangesNoStep)             // same seed with and without observer: same list of targets
```

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build --preset dev --target pychron_laser_tests && build/dev/tests/laser/pychron_laser_tests --gtest_filter='LaserSystemFollow.*'`
Expected: does not compile.

- [ ] **Step 3: Implement**

`attach_camera` passes `[this](const vision::DragonflyStep& s) { report_step(s); }` as `on_step`. Private `report_step` (runner's thread, under the gate per spec 4.3) calls `step_observer_`, then, if `follow_observer_`, builds the sample with `dynamic_cast<SimTrayCamera*>(frames_.get())->observe()` when that cast holds, as `grain_px` does.

- [ ] **Step 4: Run them to see them pass, with the bench**

Run: `build/dev/tests/laser/pychron_laser_tests --gtest_filter='LaserSystem*:TrackBench.*'`
Expected: all pass.

- [ ] **Step 5: Commit**

```bash
git add libs/laser tests/laser/test_laser_system.cpp
git commit -m "feat(laser): report each followed step with the simulator's truth"
```

### Task 7: Bridge hand-off

**Files:**
- Modify: `apps/pychron-ui/src/laser_bridge.hpp`, `apps/pychron-ui/src/laser_bridge.cpp`
- Test: `tests/ui/test_laser_bridge.cpp`

**Interfaces:**
- Consumes: `LaserSystem::set_follow_observer`, `FollowSample`.
- Produces: signal `void LaserBridge::followSamples(const std::vector<pychron::laser::FollowSample>& samples);`, registered with `qRegisterMetaType` as `CameraView` is, and `static constexpr std::size_t LaserBridge::kMaxPendingSamples = 4096;`.

- [ ] **Step 1: Write the failing tests**

Slots of `LaserBridgeTest`, on the fixture's simulated laser:

```cpp
void follow_reaches_the_main_thread();       // Follow (simulated) on the fixture's dragonfly: QSignalSpy followSamples gets rows, thread() is the main one, elapsed_s rises
void a_stalled_main_thread_keeps_the_newest();// 5000 samples through the box with no event loop: 4096 delivered, last one kept
void a_bridge_gone_with_a_dragonfly_running_is_safe(); // destroy bridge mid-Follow: no crash, ASan clean
```

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build --preset dev-ui --target pychron_ui_tests && build/dev-ui/tests/ui/pychron_ui_tests LaserBridgeTest`
Expected: does not compile. Use the binary name `tests/ui/CMakeLists.txt` gives if it differs.

- [ ] **Step 3: Implement**

`struct LaserBridge::Follow` beside `Video`: `std::mutex mutex`, `std::vector<laser::FollowSample> pending`, `bool posted`. The constructor sets the observer, which appends under that mutex, erases from the front past `kMaxPendingSamples`, and posts `deliver_follow()` only if none is posted: the queued call carries nothing (root `AGENTS.md`, TSan note). The destructor calls `system_.set_follow_observer({})` before the worker is asked to quit.

- [ ] **Step 4: Run them to see them pass**

Run: `build/dev-ui/tests/ui/pychron_ui_tests LaserBridgeTest`
Expected: all pass.

- [ ] **Step 5: Commit**

```bash
git add apps/pychron-ui/src/laser_bridge.* tests/ui/test_laser_bridge.cpp
git commit -m "feat(ui): the laser bridge hands followed steps to the main thread"
```

### Task 8: `FollowPlotModel`

**Files:**
- Create: `apps/pychron-ui/src/follow_plot_model.hpp`, `apps/pychron-ui/src/follow_plot_model.cpp`, `tests/ui/test_follow_plot_model.cpp`
- Modify: `apps/pychron-ui/CMakeLists.txt` (sources at lines 33 and 138's lists), `tests/ui/CMakeLists.txt`

**Interfaces:**
- Consumes: `FollowSample`.
- Produces:

```cpp
enum class FollowLine { StageX, GrainX, StageY, GrainY, DeviationSeen, DeviationTrue,
                         LuminositySeen, LuminosityTrue, SizeSeen, SizeTrue };
inline constexpr std::size_t kFollowLines = 10;

class FollowPlotModel {
 public:
  FollowPlotModel();
  void append(const laser::FollowSample& s);
  const TimeSeriesRing& series(FollowLine line) const;
  bool has(FollowLine line) const;  // any finite value in this run
  double latest_x() const;
  void clear();
};
```

- [ ] **Step 1: Write the failing tests**

QtTest class `FollowPlotModelTest`:

```cpp
void a_sample_fills_its_lines();        // stage (1,2), grain (3,4), offset (0.3,0.4), area pi, saturation 0.5, brightness 0.7, radius 0.5: X and Y lines; DeviationSeen 0.5; SizeSeen 2.0; SizeTrue 1.0; LuminosityTrue 0.7
void the_true_deviation_is_from_the_beam(); // beam (0,0), grain (3,4): DeviationTrue 5
void no_blob_breaks_the_seen_lines();   // seen unset: DeviationSeen, SizeSeen NaN; LuminositySeen finite on Saturated
void a_miss_breaks_luminosity();        // reason Miss and Search: LuminositySeen NaN
void a_real_laser_has_no_true_lines();  // grain empty: has(...) false for every true line
void a_point_grain_has_no_true_size();  // radius 0: SizeTrue NaN, has(SizeTrue) false
void an_earlier_time_starts_a_new_run();// elapsed 5 then 1: rings hold one row
void one_row_is_a_run();                // single sample: series non-empty, ranges finite
```

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build --preset dev-ui --target pychron_ui_tests`
Expected: does not compile.

- [ ] **Step 3: Implement**

Rings of 600 s span. `append` compares `step.elapsed_s` with `latest_x()` and calls `clear()` first when it is below. Values per spec 4.1. Seen size is `2 * sqrt(area_mm2 / pi)`.

- [ ] **Step 4: Run them to see them pass**

Run: `build/dev-ui/tests/ui/pychron_ui_tests FollowPlotModelTest`
Expected: all pass.

- [ ] **Step 5: Commit**

```bash
git add apps/pychron-ui tests/ui
git commit -m "feat(ui): a model of a followed run's lines"
```

### Task 9: `FollowPlotView` and the Plots button

**Files:**
- Create: `apps/pychron-ui/src/follow_plot_view.hpp`, `apps/pychron-ui/src/follow_plot_view.cpp`
- Modify: `apps/pychron-ui/src/laser_window.hpp`, `apps/pychron-ui/src/laser_window.cpp`, `apps/pychron-ui/CMakeLists.txt`, `docs/user/05-laser.md` (8.5)
- Test: `tests/ui/test_laser_views.cpp`, `tests/ui/test_laser_window.cpp`

**Interfaces:**
- Consumes: `FollowPlotModel`, `FollowLine`, `followSamples`, `plot_style()`, `plot_series_color`, `PlotStyleHub`.
- Produces: `class FollowPlotView : public QWidget { explicit FollowPlotView(FollowPlotModel& model, QWidget* parent = nullptr); void refresh(); int strip_count() const; int point_count(FollowLine) const; bool line_visible(FollowLine) const; QCustomPlot* plot() const noexcept; static constexpr int kMinReplotMs = 50; };`. In `LaserWindow`: button object name `pattern_plots`.

- [ ] **Step 1: Write the failing tests**

```cpp
// test_laser_views.cpp
void follow_plot_has_five_strips();        // strip_count() == 5, titles as spec 4.1
void follow_plot_draws_the_model();        // 3 samples, refresh: point_count(StageX) == 3
void follow_plot_hides_lines_with_no_data(); // real-laser samples: line_visible(GrainX) false, legend omits it
void follow_plot_strips_share_time();      // zoom x on the first axis rect: all five ranges equal
void follow_plot_follows_the_palette();    // PlotStyleHub changed: seen pen is plot_series_color(0), true pen slot 1
// test_laser_window.cpp
void plots_button_opens_the_view();        // click pattern_plots: a visible FollowPlotView
void a_run_before_opening_shows();         // emit followSamples, then click: points already there
```

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build --preset dev-ui --target pychron_ui_tests`
Expected: does not compile.

- [ ] **Step 3: Implement the view and the window's wiring**

One `QCustomPlot` with five `QCPAxisRect` stacked, x ranges joined through `rangeChanged`, as spec 4.5. Y autoranges per strip from `TimeSeriesRing::range`. Repaint cap as `StripChartView::refresh`. `Qt::Window` flag, title `tr("Follow plots")`. `LaserWindow` owns the model as a member and connects `followSamples` always, whatever the view's visibility; refreshes the view only while it is visible and on show. Button added after `pattern_stop_` in `build_patterns`, so it shows on a real laser too; enabled when the bridge's system has a camera.

- [ ] **Step 4: Run them to see them pass**

Run: `build/dev-ui/tests/ui/pychron_ui_tests LaserViewsTest LaserWindowTest` (class names per `tests/ui`).
Expected: all pass.

- [ ] **Step 5: Run it and look**

Launch `Pychron --sim` on a copy of the example lab with `configs/examples/track/melting.toml`'s `[grain]` keys under `[co2.sim.grain]`, tray `example-9`, hole 5, Output 20, Follow (simulated), Plots. Take a screenshot. Expected: five strips moving, polygon glow on the camera, true size line falling.

- [ ] **Step 6: Commit**

```bash
git add apps/pychron-ui tests/ui docs/user/05-laser.md
git commit -m "feat(ui): live plots of a followed grain"
```

### Task 10: Whole-branch checks

**Files:** none of its own.

- [ ] **Step 1: Thread sanitizer on the hand-off**

Run: a build with `-DPYCHRON_SANITIZE=thread`, then the `LaserBridgeTest` and `LaserSystemFollow` suites.
Expected: no race reported.

- [ ] **Step 2: Address and undefined-behaviour sanitizers**

Run: a build with `-DPYCHRON_SANITIZE=address,undefined`, then the vision, laser and UI tests.
Expected: clean.

- [ ] **Step 3: Static analysis and compiler sweep**

Run: `python3 tools/quality_check.py && python3 tools/compiler_sweep.py --build-dir build/dev-ui`
Expected: exit 0, no warnings.

- [ ] **Step 4: Rebase and run everything**

Run: `git fetch origin && git rebase origin/develop && cmake --build --preset dev-ui && ctest --preset dev-ui`
Expected: all pass. `dev-ui` covers `dev` (`AGENTS.md`).

- [ ] **Step 5: Land**

Run: `git push origin feat/follow-plots:develop`, then remove the worktree and branch. If `origin/develop` moved, go back to Step 4.

## Open questions

None.
