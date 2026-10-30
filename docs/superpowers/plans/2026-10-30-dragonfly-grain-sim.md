# Dragonfly grain simulator (Part A) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A simulated grain that heats under the beam, glows irregularly and moves, and a headless bench, `elctl laser track`, that scores the real dragonfly stack against it.

**Architecture:** `vision::GrainModel` is pure and stepped on a fixed tick with keyed draws. `SimTrayCamera` feeds it the beam and draws its lobes. The bench builds the real `LaserSystem` on `ChromiumSim` in a scratch directory on a `ManualClock`, runs every sweep combination on the same seeds, and scores beam against the model's truth.

**Tech Stack:** C++20, GoogleTest, toml++, existing `libs/vision`, `libs/laser`, `apps/elctl`.

**Spec:** `docs/superpowers/specs/2026-10-30-dragonfly-grain-sim-design.md` (Part A: sections 3 to 7, tests of section 9, docs). Read it first: the key table, tick order and scored columns are there and are not repeated here. Part B gets its own plan.

## Global Constraints

- Read `libs/sim/AGENTS.md` before Task 1, and root `AGENTS.md` "Static analysis", "Time" and the gcc/clang/MSVC notes before any C++.
- No simulator noise from a shared generator or `<random>`: draws are `keyed_*` by (seed, name, tick). One name, one kind of draw.
- `libs/vision` depends on core only. No Qt in `libs/`. Errors are `Result<T>`; `libs/` does not throw.
- `[<device>.sim.grain]` absent: `SimTrayCamera` output byte-identical to today. No existing test is edited to pass, except the mechanical include and namespace change of Task 1.
- `Dragonfly`, finder and `PatternRunner` behaviour unchanged; Task 5 adds only an observer that is empty by default.
- New `libs/laser` sources are found by glob; new `tests/laser`, `tests/vision` and `apps/elctl/src` files too (re-run configure). `tests/core` list is explicit.
- Files the bench writes for the user go through `pychron::mark_as_user_file`.
- Commits: Conventional Commits with component scope. Work on `feat/dragonfly-grain-sim`, cut from `origin/develop`, with this plan and the spec brought along by rebasing `docs/dragonfly-grain-sim`.

## Review Focus

1. Output too low for the grain ever to glow (`output_percent = 1`): the run ends on time, row shows `lock_s` = duration and `on%` as scored; no hang, no failure.
2. A long idle gap then a grab, or `advance` given hours at output 0: costs no noticeable real time. Zero or negative `dt`, or a non-finite beam, changes nothing.
3. A hole of unknown size (`hole_radius_mm` 0, as `TraySight` can give): grain moves unclamped rather than pinned at its start.
4. A bad value in a sweep (`aggressiveness = [1, -1]`), or an empty sweep list: refused naming the key, before any run. Long sweeps never die at combination 40 of 41 on something knowable.
5. `--trace` or `--frames` target that cannot be written: error before any run, exit non-zero, not after the runs have been spent.

---

### Task 1: Keyed noise in core

**Files:**
- Move: `libs/sim/include/pychron/sim/keyed_noise.hpp` to `libs/core/include/pychron/core/keyed_noise.hpp`
- Move: `tests/sim/test_keyed_noise.cpp` to `tests/core/test_keyed_noise.cpp`
- Modify: `tests/core/CMakeLists.txt`, `libs/sim/src/sim_system.cpp`, `libs/sim/src/spectrometer/beam_model.cpp`, `libs/sim/include/pychron/sim/sim_system.hpp` and `spectrometer/beam_model.hpp` (comments), `libs/sim/AGENTS.md`

**Interfaces:**
- Produces: `pychron::keyed_bits`, `pychron::keyed_gauss`, `pychron::keyed_poisson` from `pychron/core/keyed_noise.hpp`.

- [ ] **Step 1:** `git mv` both files. Change namespace to `pychron`, test's `using` lines and include to match; add test to `tests/core/CMakeLists.txt`. Point includes at the new path. `libs/sim` code needs no qualifier change (enclosing namespace). `libs/sim/AGENTS.md`: path `core/keyed_noise.hpp`, written compressed; "standard library and `core/error.hpp` only" gains it.
- [ ] **Step 2:** Run `cmake --build --preset dev --target pychron_core_tests pychron_sim_tests`, then `build/dev/tests/core/pychron_core_tests --gtest_filter='KeyedNoise*'` and `build/dev/tests/sim/pychron_sim_tests`. Expected: PASS, same count as before.
- [ ] **Step 3:** Commit `refactor(core): keyed noise moves to core for the vision simulator`.

### Task 2: GrainModel without lobes

**Files:**
- Create: `libs/vision/include/pychron/vision/grain.hpp`, `libs/vision/src/grain.cpp`
- Test: `tests/vision/test_grain.cpp`

**Interfaces:**
- Consumes: `keyed_bits`, `keyed_gauss`.
- Produces: `GrainParams` (fields named as spec's keys, `creep_mm_per_s` a `Vec2`), `GlowLobe`, `GrainTruth`, `GrainModel`, `kGrainTick` as in spec §3. In this task `lobes()` returns one lobe at the grain center, `lobe_sigma_mm`, peak = brightness.

- [ ] **Step 1: Write the failing tests** (suite `GrainModel`), helper `run(model, seconds, beam, output)`:

```cpp
TEST(GrainModel, SameSeedSameTruth);   // all behaviours on, 30 s, two models: truth equal exactly; seed 2 differs
TEST(GrainModel, FrameRateDoesNotMatter);  // beam still: advance(1s) once == advance(100ms) ten times, exactly
TEST(GrainModel, RemainderOfATickCarries);  // advance(4ms) three times == advance(12ms) (one tick, and 2 ms left)
TEST(GrainModel, DefaultsAreOneSteadyLobe);  // beam centered, 20 %, 20 s: heat 1 within 1e-6, brightness 0.6 within 1e-6, center unmoved
TEST(GrainModel, HeatRisesAndFallsByItsLags);  // after heat_tau_s: 1 - 1/e within 0.01; beam 10 mm off, after cool_tau_s: 1/e of start within 0.01
TEST(GrainModel, GlowFollowsHeatSteeply);  // heat 0.5, exponent 4: brightness 0.6 * 0.0625
TEST(GrainModel, ColdGrainDoesNotMove);  // output 0, creep, wander, jumps on, 60 s: center == start
TEST(GrainModel, CreepsAtItsRateWhenHot);  // heat 1, creep [0.01, 0], hole 0 (no clamp), beam follows it: 10 s moves 0.1 within 0.002
TEST(GrainModel, EventRatesMatchTheirKeys);  // flares_per_s 2, 1000 s: count within 3 sigma of 2000, seen as rises in brightness; jumps_per_s 1, 1000 s: count and mean size within 3 sigma of jump_mm
TEST(GrainModel, ADropoutIsDarkForItsTime);  // dropouts on, 300 s: dark time fraction within 3 sigma of rate * dropout_s
TEST(GrainModel, StaysInItsHole);  // big jumps, hole 0.5, 300 s: hypot(center) <= 0.5 always
TEST(GrainModel, AHoleOfNoSizeDoesNotClamp);  // hole 0, start (0.3, 0), jumps on: center leaves start
TEST(GrainModel, IdleHoursCostNothing);  // advance(10h, far beam, 0) under 1 s real; tick count still advanced: a later run equals one that ran through
TEST(GrainModel, BadInputChangesNothing);  // dt 0 or negative, NaN beam: truth unchanged
```

- [ ] **Step 2:** Run `cmake --build --preset dev --target pychron_vision_tests`. Expected: compile error, no `grain.hpp`.
- [ ] **Step 3:** Implement. Tick order is spec §3. Key names: `"grain.flutter"`, `"grain.flare"`, `"grain.dropout"`, `"grain.walk_x"`, `"grain.walk_y"`, `"grain.jump"`, `"grain.jump_size"`, `"grain.jump_dir"`. Uniform from bits: `keyed_bits(...) / 2^64`. Jump size: `-jump_mm * log(1 - u)`. Flutter OU: `x += -x * tick/tau + sigma * sqrt(2 * tick/tau) * g`, brightness factor `exp(x)`. Walk per tick: `rate * sqrt(tick_s) * g`. Beam interpolation: straight from last `advance`'s end to `beam_mm` over the ticks run. Idle skip per spec §3 applies only when flare envelope is under 1e-6 and dropout timer is spent as well; a skip sets flutter to 0 and adds the skipped count to the tick number.
- [ ] **Step 4:** Run `build/dev/tests/vision/pychron_vision_tests --gtest_filter='GrainModel.*'`. Expected: PASS. `IdleHoursCostNothing`'s "equals one that ran through" holds for `heat`, center and draws; compare those, not `flutter`.
- [ ] **Step 5:** Commit `feat(vision): a grain that heats under the beam, flares and jumps`.

### Task 3: Lobes and their rendering

**Files:**
- Modify: `libs/vision/include/pychron/vision/grain.hpp`, `libs/vision/src/grain.cpp`, `libs/vision/include/pychron/vision/synth.hpp`, `libs/vision/src/synth.cpp`
- Test: `tests/vision/test_grain.cpp`, `tests/vision/test_synth.cpp`

**Interfaces:**
- Consumes: `GlowLobe`, `GrainModel`.
- Produces: `GlowScene::lobes` (`std::vector<GlowLobe>`, stage mm). `GlowLobe` moves to `synth.hpp`; `grain.hpp` includes it. `GrainModel::lobes()` gives `lobes` entries relative to hole's center, shares summing to 1 × brightness.

- [ ] **Step 1: Write the failing tests.**

```cpp
TEST(Synth, NoLobesIsTheOldGlowExactly);  // frame data equal, scene with empty lobes vs golden render of same scene through the single path
TEST(Synth, LobesDrawWhereTheySay);  // two lobes: pixel maxima at each's px; Truth center is the peak-weighted mean within 0.5 px
TEST(Synth, LobesAboveDepthSaturate);  // peak 2: pixel == pixel_depth
TEST(Synth, LobeSizeIsOwn);  // two sigmas: half-height widths differ in ratio within 10 %
TEST(GrainModel, LobesShareBrightness);  // lobes = 5: peaks sum to brightness within 1e-9 at every sample
TEST(GrainModel, LobesStayNearTheGrain);  // spread 0.2, wander on, 300 s: each within 0.4 of center always
TEST(GrainModel, LobesMoveTheCenterOfLightNotTheGrain);  // lobe_life_s 1, grain motion off, 60 s: peak-weighted mean moves above 0.05, center fixed
TEST(GrainModel, ALobeDiesAndReturnsElsewhere);  // lobes = 1, spread 0.3, life 2: a share reaches 0 and the lobe's place then differs
```

- [ ] **Step 2:** Run `--gtest_filter='Synth.*:GrainModel.*'`. Expected: FAIL (`lobes` not a member).
- [ ] **Step 3:** Implement spec §3 step 5 and §4. Keys `"grain.lobe_place"`, `"grain.lobe_share"`, `"grain.lobe_walk"`, `"grain.lobe_death"`; slot number mixed into tick: `tick * 64 + slot` (lobes capped at 16). A slot's 2 × spread bound is clamped, not reflected. Lobes are a separate branch in `render`; the empty path's code is not touched.
- [ ] **Step 4:** Run same filter. Expected: PASS.
- [ ] **Step 5:** Commit `feat(vision): a glow of hot spots that wander, die and return`.

### Task 4: The camera shows the grain

**Files:**
- Modify: `libs/laser/include/pychron/laser/camera.hpp` (field and header comment), `libs/laser/src/camera.cpp`, `libs/laser/include/pychron/laser/tray_camera.hpp`, `libs/laser/src/tray_camera.cpp`, `configs/examples/cameras.toml`
- Test: `tests/laser/test_camera.cpp`, `tests/laser/test_tray_camera.cpp`, `tests/laser/test_dragonfly.cpp`

**Interfaces:**
- Consumes: `GrainModel`, `GlowScene::lobes`.
- Produces:

```cpp
// CameraConfig
std::optional<vision::GrainParams> sim_grain;
// SimTrayCamera
struct GrainView { StageXY grain_mm; StageXY beam_mm; vision::GrainTruth truth; };
void set_grain_seed(std::uint64_t seed);       // before the first grab
std::optional<GrainView> observe();            // to clock's now, no drawing
```

- [ ] **Step 1: Write the failing tests.**

```cpp
TEST(CameraLibrary, ReadsSimGrain);          // [co2.sim.grain] flares_per_s = 1: parsed; lobes = 17, a range per spec table: problem names co2.sim.grain.lobes; unknown key refused
TEST(CameraLibrary, GrainAndOldGlowKeysConflict);  // with glow_sigma_mm, and with glow_drift_mm_per_s: Config problem naming both
TEST(SimTrayCamera, NoGrainTableIsTodaysPicture);  // firing, drift on: frame data equal to a golden sum recorded before this change
TEST(SimTrayCamera, GrainGlowsWhereTheModelSays);  // grain table, offset (0.2, 0.1): brightest pixel at grain_mm within 1 px
TEST(SimTrayCamera, BeamParkedOffTheGrainGoesDark);  // stage 2 mm off, firing, 10 s of grabs: max pixel near background
TEST(SimTrayCamera, ObserveMovesTheModelWithoutDrawing);  // seq unchanged; heat rises
TEST(SimTrayCamera, ObserveThenGrabEqualsGrab);  // two cameras, same seed, one observes between grabs, beam still: frames equal
TEST(SimTrayCamera, ANewHoleIsANewGrain);  // stage to hole 3 and back to 1: heat 0, offset back at start
TEST(SimTrayCamera, GrainSeedsDiffer);  // seeds 1 and 2: frames differ; same seed, same frames
TEST(SimTrayCamera, AimOffsetIsWhereTheBeamIs);  // aim_offset_px set, stage so aim is on grain: heats; stage centered on grain: heats less
TEST(Dragonfly, FollowsAGrainThatJumps);  // tests/laser: track pattern on jumps_per_s 0.2, jump_mm 0.15: after each jump beam back within 0.12 inside 5 s
TEST(Dragonfly, ReacquiresAfterADropout);  // dropouts_per_s 0.1, dropout_s 2: last_note empty, ends on time
```

Before Step 3, record `NoGrainTableIsTodaysPicture`'s golden on unchanged code.

- [ ] **Step 2:** Run `cmake --build --preset dev --target pychron_laser_tests`, `build/dev/tests/laser/pychron_laser_tests --gtest_filter='CameraLibrary.*:SimTrayCamera.*:Dragonfly.*'`. Expected: compile error (`sim_grain`).
- [ ] **Step 3:** Read `[<device>.sim.grain]` with a `Reader` sub-table in `camera.cpp`, each key with spec's range; `creep_mm_per_s` by `pair`. `Reader` needs an integer in range for `lobes`. Then `SimTrayCamera` per spec §5. Factor the advance into one private `advance_grain(const TraySight&, TimePoint)` used by `grab()` and `observe()`, so both move model by the same dt. Lobes converted to stage mm by adding hole's real position. Add table to `configs/examples/cameras.toml` commented out, and to `camera.hpp`'s header comment.
- [ ] **Step 4:** Run same filter, then `build/dev/tests/laser/pychron_laser_tests`. Expected: all PASS, no existing test touched.
- [ ] **Step 5:** Commit `feat(laser): the simulated camera sees a grain under the beam`.

### Task 5: A step observer

**Files:**
- Modify: `libs/laser/include/pychron/laser/pattern_runner.hpp`, `libs/laser/src/pattern_runner.cpp`, `libs/laser/include/pychron/laser/laser_system.hpp`, `libs/laser/src/laser_system.cpp:512`
- Test: `tests/laser/test_dragonfly.cpp`

**Interfaces:**
- Produces: `PatternVision::on_step`, `std::function<void(const vision::DragonflyStep&)>`; `void LaserSystem::set_step_observer(std::function<void(const vision::DragonflyStep&)> fn)`, before `attach_camera`.

- [ ] **Step 1: Write the failing test.** `TEST(Dragonfly, TellsItsObserverEachLook)`: rig with observer counting; offset grain; 8 s; count above 2, at least one `Reason::Track`, and at least one `Reason::Deadband` once on grain. `Rig` gains a constructor argument for the observer, set before `attach_camera`.
- [ ] **Step 2:** Run `--gtest_filter='Dragonfly.TellsItsObserverEachLook'`. Expected: compile error.
- [ ] **Step 3:** Add both; call `on_step` in `follow()` right after a successful `step`, before any return. Empty: no call.
- [ ] **Step 4:** Run `--gtest_filter='Dragonfly.*'`. Expected: PASS.
- [ ] **Step 5:** Commit `feat(laser): a dragonfly's looks can be observed`.

### Task 6: Scoring

**Files:**
- Create: `libs/laser/include/pychron/laser/track_score.hpp`, `libs/laser/src/track_score.cpp`
- Test: `tests/laser/test_track_score.cpp`

**Interfaces:**
- Consumes: `vision::DragonflyStep::Reason`.
- Produces:

```cpp
struct TrackScore {
  double on_pct = 0, rms_mm = 0, max_mm = 0, lock_s = 0;
  int lost = 0;
  double back_s = 0;
  int moves = 0;
  double heat_pct = 0, track_pct = 0, hold_pct = 0, sat_pct = 0, search_pct = 0;
};
class TrackScorer {
 public:
  TrackScorer(double beam_radius_mm, double full_heat, double duration_s);
  void sample(double t_s, double distance_mm, double heat);   // every poll, t rising
  void look(vision::DragonflyStep::Reason reason);
  void moved();
  TrackScore finish(double end_s) const;
};
struct TrackSummary { TrackScore mean, worst; };
TrackSummary summarize(std::span<const TrackScore> runs);  // worst: lowest on_pct, heat_pct; highest of the rest
```

- [ ] **Step 1: Write the failing tests** (suite `TrackScorer`), samples each 0.1 s:

```cpp
TEST(TrackScorer, AlwaysOn);          // on 100, lock_s 1.0 (first sample + 1 s unbroken), lost 0
TEST(TrackScorer, NeverOn);           // lock_s == duration, on 0
TEST(TrackScorer, ABriefMissIsNoLoss);  // 0.5 s off after lock: lost 0, lock unbroken thereafter
TEST(TrackScorer, ALossIsDatedFromGoingOff);  // off at 10.0 for 3 s, back at 13: lost 1, back_s == 3 + 1 (off to next lock)
TEST(TrackScorer, NeverBackCountsTheRest);  // off at 10, duration 30: back_s 20
TEST(TrackScorer, ReasonsAreBinned);  // Track+PerimeterClamp, Deadband+Miss, Saturated, Search per spec; sum 100
TEST(TrackScorer, HeatIsAgainstFullHeat);  // heat 0.5 all along, full_heat 1: heat_pct 50
TEST(TrackScorer, ThereAreNoLooks);   // no look(): the four percents 0, no division by zero
TEST(TrackSummary, MeanAndWorst);     // two runs: means; worst on_pct the lower, worst lost the higher
```

- [ ] **Step 2:** Run `build/dev/tests/laser/pychron_laser_tests --gtest_filter='TrackS*'`. Expected: compile error.
- [ ] **Step 3:** Implement spec §7's scoring. `Elapsed` and `Invalid` reasons not binned.
- [ ] **Step 4:** Same filter. Expected: PASS.
- [ ] **Step 5:** Commit `feat(laser): score of a beam against a grain`.

### Task 7: The bench

**Files:**
- Create: `libs/laser/include/pychron/laser/track_bench.hpp`, `libs/laser/src/track_bench.cpp`
- Test: `tests/laser/test_track_bench.cpp`

**Interfaces:**
- Consumes: `TrackScorer`, `summarize`, `SimTrayCamera::{set_grain_seed, observe}`, `LaserSystem::set_step_observer`, `CameraLibrary::parse`, `PatternLibrary::load`, `vision::write_png`, `mark_as_user_file`.
- Produces:

```cpp
struct TrackOptions {
  std::optional<int> seeds;
  std::filesystem::path trace;           // empty: none
  std::filesystem::path frames;          // empty: none
  std::optional<std::uint64_t> frames_seed;
  bool check_only = false;               // every check, no run: returns no combos
};
struct TrackCombo {
  std::string label;    // "pattern.aggressiveness=0.5 grain.jump_mm=0.3"; "" with no sweep
  TrackSummary summary;
  int failed = 0;
  std::string first_error;
};
Result<TrackScenario> parse_track_scenario(std::string_view toml, std::string file_name);
Result<TrackScenario> load_track_scenario(const std::filesystem::path& file);
// All checks first; then every combination, best first by mean on_pct.
Result<std::vector<TrackCombo>> run_track(const TrackScenario& scenario, const TrackOptions& options);
std::string track_table(const std::vector<TrackCombo>& combos);
```

`struct TrackScenario { std::shared_ptr<const struct TrackTables> tables; };`: `TrackTables` is defined in `track_bench.cpp`, so no toml in the public header.

- [ ] **Step 1: Write the failing tests** (suite `TrackBench`, durations 5 s, seeds 2 to stay well under ctest's 60 s):

```cpp
TEST(TrackBench, SameScenarioSameTable);   // run twice: track_table equal, trace files byte-equal
TEST(TrackBench, ASteadyGrainIsHeldOn);    // no behaviours: mean on_pct above 95
TEST(TrackBench, ATrackerThatDoesNotFollowScoresWorse);  // creep 0.05, sweep aggressiveness [0, 1], duration 20: combo 1 on_pct above combo 0
TEST(TrackBench, EveryCombinationOnTheSameSeeds);  // two sweeps of 2 and 3: 6 rows, labels distinct, 12 runs scored
TEST(TrackBench, TooManyRunsAreRefused);   // seeds 10001: Config error; test takes no time
TEST(TrackBench, UnknownKeysAreRefused);   // top level, [camera], [grain], [pattern], [sweep.nothing]
TEST(TrackBench, PatternMayNotNameKindOrDuration);
TEST(TrackBench, ABadSweepValueIsRefusedFirst);   // aggressiveness [1, -1]: error names it, and no trace file was begun
TEST(TrackBench, AnEmptySweepIsRefused);          // aggressiveness = []
TEST(TrackBench, AnUnwritableTraceIsRefusedFirst);  // trace under a path that is a regular file: Io error before a run (duration 3600, returns at once)
TEST(TrackBench, AGrainTooDimIsARunNotAFailure);  // output_percent 1: failed 0, lock_s == duration
TEST(TrackBench, FramesOfOneRun);          // --frames: nnnnn.png count equals looks of that run; files open as PNG
TEST(TrackBench, TraceHasARowPerPoll);     // header exact per spec, rows == duration / poll * seeds within 5 %
TEST(TrackBench, TableIsBestFirst);
TEST(TrackBench, CheckOnlyRunsNothing);    // duration 3600, 10000 runs: returns empty at once; bad value still refused
```

- [ ] **Step 2:** Run `--gtest_filter='TrackBench.*'`. Expected: compile error.
- [ ] **Step 3:** Implement, in order:
  1. Parse: top level keys and ranges (`output_percent` (0, 100], `duration` (0, 3600], `seeds` [1, 10000], `seed`, `poll_ms` [10, 1000], `grain_offset_mm`), tables `grain`, `pattern`, `camera`, `sweep`. Each sweep entry a non-empty array. Combinations × seeds over 10000: Config error.
  2. Tables of a combination: copy, override by sweep. `[camera]` keys map: `settle_ms`, `px_per_mm` to `[co2]`; `noise`, `width`, `height` to `[co2.sim]`; `frames_per_step` to `[co2.autocenter]`. Others: Config error. Write `cameras.toml` text with toml++'s `operator<<` for `CameraLibrary::parse` with `source = "sim"`, `use = "center"`, `sim_tray_error` 0; pattern text with `kind = "dragonfly"` and `duration`.
  3. Scratch directory (`temp_directory_path()`, name from `std::random_device`, RAII removal): `tray_maps/track.txt` `circle,1.0` with holes 1 (0, 0) and 2 (5, 0); `patterns/` one file per combination, named by index. Load every combination; any problem is the error. Open trace for writing and create frames dir; any failure is the error.
  4. A `Rig` as `tests/laser/laser_harness.hpp`'s `LaserHarness`, members in that order, in `track_bench.cpp`. Calibrate 2 points. `FrameTap : vision::IFrameSource` wraps `SimTrayCamera`, writing PNGs when asked.
  5. Run per spec §7. Stage to hole 1, polling `system.moving()` and advancing clock, bound 60 s simulated. `Stage.MoveTo` count: count `on_step` with `Action::Move`. After `running()` false or error, stop laser, `disable()`.
  6. Text: `std::to_chars` fixed, 4 decimals in trace, 2 in table. `mark_as_user_file` on trace and each PNG. Table header per spec §7, `mean` then `worst` line per combination; `failed N` and first error when any.
- [ ] **Step 4:** Run `--gtest_filter='TrackBench.*'`. Expected: PASS. If `ASteadyGrainIsHeldOn` fails, that is a finding about tracker or model: stop and report it, do not widen the bound.
- [ ] **Step 5:** Commit `feat(laser): a bench that scores dragonfly against a simulated grain`.

### Task 8: `elctl laser track`, scenarios, docs

**Files:**
- Create: `apps/elctl/src/laser_track.cpp`, `configs/examples/track/{steady,hot_spots,flicker,jumpy,cooling,everything}.toml`
- Modify: `apps/elctl/src/laser.cpp`, `apps/elctl/src/laser.hpp`, `docs/user/05-laser.md`, `docs/user/07-configuration-reference.md`, `docs/dev_setup.md`, spec's `Status`
- Test: `apps/elctl/tests/test_laser_commands.cpp`

**Interfaces:**
- Consumes: `load_track_scenario`, `run_track`, `track_table`, `TrackOptions`.
- Produces: `int laser_track(const std::vector<std::string>& words, const TrackOptions& options, Io io);` in `laser.hpp`.

- [ ] **Step 1: Write the failing tests** (suite `LaserCmd`):

```cpp
TEST_F(LaserCmd, TrackPrintsItsTable);        // scratch scenario, 3 s, seeds 2: exit 0; out has on%
TEST_F(LaserCmd, TrackNeedsNoLineOrLab);      // run_raw without -c, cwd empty dir: exit 0
TEST_F(LaserCmd, TrackIgnoresAConfiguredLine);  // -c at example lab: its device log untouched, exit 0
TEST_F(LaserCmd, TrackWritesTraceAndFrames);  // both exist; --frames-seed 2 honoured
TEST_F(LaserCmd, TrackFlagsGoWithTrack);      // --seeds on `patterns`: usage error; --seeds 0: usage error
TEST_F(LaserCmd, TrackBadScenarioFails);      // missing file, bad key: failed exit, message names file
TEST_F(LaserCmd, EveryShippedScenarioIsGood);  // each configs/examples/track/*.toml with --check: exit 0
```

- [ ] **Step 2:** Run `cmake --build --preset dev --target pychron_elctl_tests`, filter `LaserCmd.Track*`. Expected: FAIL (unknown command).
- [ ] **Step 3:** In `laser_command`, parse `--seeds`, `--trace`, `--frames`, `--frames-seed`, `--check` into `Args`; refuse them on any other verb. Dispatch `track` right after parsing, before `a.lab` is worked out and before any `Laser` is made: it needs one word, the scenario. Add to `kLaserUsage`. Write six scenarios: `steady` no behaviours; one named behaviour each at values that make the shown failure mode reachable; `everything` all. Each header comment says what it stresses.
- [ ] **Step 4:** Run filter. Expected: PASS.
- [ ] **Step 5:** Run each shipped scenario by hand, default seeds:

```bash
for f in configs/examples/track/*.toml; do echo "$f"; build/dev/apps/elctl/elctl laser track "$f"; done
```

Expected: tables print, no `failed` on `steady`. Save output for the wrap-up: these numbers are the first findings.
- [ ] **Step 6:** Docs. `05-laser.md`, section after 8.2: bench, scenario, columns, and in plain words that model numbers are judgement and it compares settings, it says nothing about real grains. `07-configuration-reference.md`: extend `[<laser>.sim]` row, add `[<laser>.sim.grain]`. `dev_setup.md` line ~702 mention. Spec `Status: Part A implemented`.
- [ ] **Step 7:** Commit `feat(elctl): laser track runs a dragonfly against a simulated grain`.

### Task 9: Final checks

- [ ] **Step 1:** Rebase on `origin/develop`.
- [ ] **Step 2:** Run `python3 tools/quality_check.py`. Expected: exit 0.
- [ ] **Step 3:** Run `python3 tools/compiler_sweep.py --build-dir build/dev`. Expected: no warnings. Watch gcc's `-Wshadow`, `const auto [a, b]`, MSVC's C4702 notes in `AGENTS.md`.
- [ ] **Step 4:** Configure and run a sanitizer build, `-DPYCHRON_SANITIZE=address,undefined`, of `pychron_vision_tests`, `pychron_laser_tests`, `pychron_elctl_tests`. Expected: PASS, no report.
- [ ] **Step 5:** Run `cmake --build --preset dev` and `ctest --preset dev`. Expected: 100 % pass.
- [ ] **Step 6:** Land per `AGENTS.md`, only when developer says so.

## Open questions

None.
