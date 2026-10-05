# Laser Patterns (part 2b) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A run's `execute_pattern()` moves the laser's stage along a named pattern, on a real or simulated laser, and can be stopped.

**Architecture:** `libs/laser` gains pattern files, pure point generators and a poll-driven `PatternRunner` that `LaserSystem` exposes as its `IPatternRunner`. `IStage` gains a speed on `set_xy` and a `stop()`, implemented by the Chromium driver. `Lab` loads the patterns, the queue check refuses unknown ones, `elctl laser` lists and runs them, and a Python tool exports legacy `.lp` pickles.

**Tech Stack:** C++20, toml++, GoogleTest; Python 3 standard library for the export tool.

**Spec:** `docs/superpowers/specs/2026-10-04-laser-patterns-design.md`

## Global Constraints

- AGENTS.md: no pull requests; never skip or disable a failing test; warnings are errors on the CI compilers (gcc 14, clang 18, Apple clang, MSVC).
- `libs/laser` depends on `core` and `devices` only. No Qt in any `libs/` public header.
- Everything returns `Result`; nothing throws across a public function.
- Numbers from text are locale-free (`codec::parse_decimal`); numbers into text never use `std::to_string(double)`.
- Lengths mm, angles degrees, velocity mm/s. Pattern points are offsets in the stage's axes, counter-clockwise rotation.
- Limits: `iterations` 1–200; at most 10 000 points over all iterations.
- Random: `std::mt19937_64` with an own uniform mapping (`(rng() >> 11) * 2^-53`), never `std::uniform_real_distribution` (differs between standard libraries).
- Tests never write beside `configs/examples`; scratch directories have a random suffix.
- Build and test: `cmake --build build/dev -j8 && ctest --test-dir build/dev -j8`.

## Review Focus

1. A pattern started with the stage near the edge of travel: the point that falls outside ends the pattern with its number, nothing past it is sent, and the run's ending leaves the laser off. (Task 4 and 6 tests.)
2. `stop_pattern()` or a cancelled script mid-segment: the stage stops where it is (not at the next vertex), `running()` and `moving()` are false after, and a new pattern can start. (Tasks 2, 4, 6.)
3. A pattern file with a degenerate value (`radius = 0`, `nsides = 2`, `dx` larger than the box, `iterations = 0`, `velocity = 0`, a NaN): a Config error naming the key, never a zero-length or endless path. (Task 1.)
4. A velocity above the driver's `move_speed`, or below one micron per second: the wire carries a clamped whole number, never 0 (a 0 speed never arrives). (Task 2.)
5. A `.lp` pickle from an old Pychron (protocol 2, Python 2 `str` keys) or a hostile one: the export tool reads the first and executes nothing from the second (the unpickler resolves no real class). (Task 7.)

---

### Task 1: Pattern model, files and point generators

**Files:**
- Create: `libs/laser/include/pychron/laser/pattern.hpp`, `libs/laser/src/pattern.cpp`
- Test: `tests/laser/test_pattern.cpp`

**Interfaces:**
- Consumes: `StageXY` (`pychron/laser/calibration.hpp`).
- Produces (namespace `pychron::laser`):

```cpp
enum class PatternKind { Polygon, Linear, CircularContour, LineSpiral, SquareSpiral, Random, Rubberband, Raster, Trough };
std::string_view to_string(PatternKind) noexcept;   // "polygon", "linear", "circular_contour", ...

struct Pattern {
  std::string name;
  PatternKind kind = PatternKind::Polygon;
  double velocity = 1.0;
  int iterations = 1;
  // one set of fields; a kind uses the ones spec section 3 lists for it
  double radius = 0.5, rotation = 0, length = 1, width = 10, offset = 0, dx = 0.5;
  double percent_change = 0.8, walk_x = 1, walk_y = 1;
  int nsides = 6, npasses = 1, nsteps = 2, step_scalar = 5, npoints = 10;
  bool single_pass = true, use_x = true;
  std::optional<std::uint64_t> seed;

  static Pattern defaults(PatternKind kind);                           // the kind's legacy defaults
  static Result<Pattern> parse(std::string_view toml, std::string name);
  static Result<Pattern> load(const std::filesystem::path& file);      // name = stem
};

// One pass, offsets from the center. `seed` is used by Random only.
std::vector<StageXY> pattern_points(const Pattern& pattern, std::uint64_t seed);
double path_length(std::span<const StageXY> points);                   // from (0,0) through the points
// The whole path a runner follows: iterations of the points, then (0,0).
// Config error above 10 000 points.
Result<std::vector<StageXY>> pattern_path(const Pattern& pattern, std::uint64_t seed);

class PatternLibrary {
 public:
  static PatternLibrary load(const std::filesystem::path& dir);        // never fails
  const Pattern* find(std::string_view name) const;
  std::vector<std::string> names() const;                              // sorted
  const std::vector<std::string>& problems() const noexcept;           // "<name>: <what>"
};
```

Per-kind defaults differ from the struct's (`length`: linear 1, rubberband and raster 15, trough 10; `radius`: polygon 0.5, the three spirals 0.1): `defaults(kind)` is the truth and `parse` starts from it.

- [ ] **Step 1: Write the failing tests** (`near(a, b)` within 1e-9):
  - `PatternPoints.Polygon` — radius 1, nsides 4, rotation 0: `(1,0) (0,1) (-1,0) (0,-1) (1,0)`; rotation 45 turns the first to `(√½, √½)`.
  - `PatternPoints.Linear` — length 2, npasses 3: `(0,0) (2,0) (2,0) (0,0) (0,0) (2,0)`; rotation 90 gives `(0,2)` for p2.
  - `PatternPoints.CircularContour` — radius 1, nsteps 2, percent_change 0.5: 74 points; point 0 `(1,0)`, point 9 `(0,1)`, point 36 `(1,0)`, point 37 `(1.5,0)`.
  - `PatternPoints.LineSpiral` — radius 1, nsteps 2, percent_change 0.5, step_scalar 5: 5 − 1 + 7 = 11 points; first `(1,0)`; last at angle 360 with radius `1·(1 + 2·0.5) = 2` → `(2,0)`; radii never decrease.
  - `PatternPoints.SquareSpiral` — radius 1, nsteps 1, percent_change 1: 5 points `(1,0) (1,2) (-2,2) (-2,-2) (3,-2)`.
  - `PatternPoints.Rubberband` — length 4, offset 1: `(-1,1) (5,1) (5,-1) (-1,-1) (-1,1)`.
  - `PatternPoints.RasterAdjustsItsStep` — length 4, offset 1, dx 1: by legacy's rule n = 6, `6·1 <= 6` → n stays 6 (even), dx = 6/7, n = int(6/(6/7)) = 7 (or 6 by rounding: pin with the value computed in the test by the same arithmetic in `double`); points alternate y = +1, −1 and x = −1 + dx·i; `single_pass = false` appends the way back and ends on `(-1, 1)`.
  - `PatternPoints.Trough` — length 3, width 2: `use_x` → `(0,0) (3,0) (0,-2) (3,-2) (0,0)`; without → `(0,0) (3,0) (3,-2) (0,-2) (0,0)`.
  - `PatternPoints.RandomIsSeededAndBounded` — npoints 50, walk 2 × 3: same seed twice gives equal vectors; another seed differs; every point within `|x| <= 2`, `|y| <= 3` and `hypot <= 2`.
  - `PatternPoints.RandomMappingIsFixed` — seed 1, npoints 1, walk 1 × 1: the point equals the value computed in the test from `std::mt19937_64(1)` with the constraint's mapping.
  - `PatternPath.RepeatsAndReturnsToTheCenter` — polygon, iterations 3: `3·5 + 1` points, last `(0,0)`.
  - `PatternPath.TooManyPointsIsRefused` — circular contour nsteps 10 (370 points) × iterations 200.
  - `PathLength.SumsTheSegmentsFromTheCenter` — rubberband length 4 offset 1: `√2 + 6 + 2 + 6 + 2`.
  - `PatternFile.ReadsEveryKindWithItsDefaults` — parameterised over the nine kinds: `kind = "<k>"` alone parses and equals `Pattern::defaults(k)` but for the name; the defaults are the table of spec section 3.
  - `PatternFile.ReadsItsKeys` — a polygon and a raster with every key set.
  - `PatternFile.RefusesBadValues` — parameterised, each a Config error whose `what` contains the file name and the key: `radius = 0`, `radius = -1`, `nsides = 2`, `nsides = 201`, `velocity = 0`, `iterations = 0`, `iterations = 201`, `dx = 0`, `npoints = 0`, `npasses = 0`, `length = nan`, `rotation = "x"`, `kind = "arc"`, no `kind`, an unknown key, `nsides = 5` on a `linear`, `seed = -1`.
  - `PatternLibrary.LoadsADirectoryAndReportsWhatDidNot` — good, bad, `._x.toml`, a `.txt`, a directory named `d.toml`; a missing directory is empty.
- [ ] **Step 2: Run** `cmake --build build/dev -j8` — Expected: FAIL (`pattern.hpp` not found).
- [ ] **Step 3: Implement.** A key table per kind (name, which field, min, max, integer or not) drives both parsing and "a key of another kind".
- [ ] **Step 4: Run** `ctest --test-dir build/dev -R 'Pattern|PathLength'` — Expected: PASS.
- [ ] **Step 5: Commit** `laser: pattern files and their points`.

---

### Task 2: `IStage` speed and stop; the Chromium driver

**Files:**
- Modify: `libs/devices/include/pychron/devices/extraction/interfaces.hpp`
- Modify: `libs/devices/include/pychron/devices/extraction/chromium.hpp`, `libs/devices/src/extraction/chromium.cpp`
- Modify (signature only): `libs/laser/.../laser_system.{hpp,cpp}`, `libs/sim/.../sim_extraction_device.{hpp,cpp}`, `tests/scripting/fakes.hpp`, `tests/experiment/run_fakes.hpp`, `tests/devices/extraction/fake.hpp`
- Test: `tests/devices/extraction/test_chromium.cpp`, `tests/laser/test_laser_system.cpp`, `tests/devices/extraction/conformance.hpp`

**Interfaces:**
- Produces:

```cpp
// IStage
virtual Result<void> set_xy(double x, double y, double speed_mm_s = 0) = 0;
virtual Result<void> stop() { return fail(not_supported("stage stop")); }
```

Default arguments on a virtual are bound statically: every override repeats `= 0`.

Chromium: `speed_mm_s > 0` → x and y speed fields `clamp(llround(speed·1000), 1, move_speed.{x,y})`; 0 → `move_speed`. A negative or non-finite speed is a Config error. `stop()`: `act(stage_stop(), stage_position())`, then the target is cleared. `LaserSystem::set_xy` and `stop` pass through; `LaserSystem::move_to_position` keeps the stage's own speed.

- [ ] **Step 1: Write the failing tests:**
  - `ChromiumTest.AMoveAtASpeedSendsItForXAndY` — `set_xy(10, 0, 1.0)` → log has `Stage.MoveTo 10000,0,0,1000,1000,100`; arrives after 10 s of simulated time and not after 9.
  - `ChromiumTest.SpeedIsClampedToTheStagesOwn` — speed 50 → `5000,5000`; speed 0.0001 → `1,1`.
  - `ChromiumTest.ABadSpeedSendsNothing` — −1, NaN, infinity: Config, log unchanged.
  - `ChromiumTest.StopHaltsTheStageWhereItIs` — move to x = 10 at 1 mm/s, advance 3 s, `stop()`: log has `Stage.Stop`; `moving()` false at once; position ≈ 3 mm and unchanged after 10 more seconds.
  - `ChromiumTest.StopWithNothingMovingIsFine`.
  - `ChromiumTest.ARefusedStopIsAnError` — `sim.fail_next("Stage.Stop", 4)` → Io error; the target is still cleared.
  - `LaserSystemTest.SpeedAndStopAreTheDrivers`; `LaserSystemFeatures.AStageThatCannotStopSaysSo` (the fake: `is_not_supported`).
  - Conformance: `StageConformance.AMoveAtASpeedSettles` (`set_xy(1, 1, 0.5)` settles at (1,1)).
- [ ] **Step 2: Run** — Expected: FAIL to compile (three-argument `set_xy`, `stop`).
- [ ] **Step 3: Implement**, updating every implementer's signature.
- [ ] **Step 4: Run** the whole suite — Expected: PASS.
- [ ] **Step 5: Commit** `devices: a stage moves at a speed and can be stopped`.

---

### Task 3: The script host stops the stage on cancel; `elctl laser goto` stops it

**Files:**
- Modify: `libs/scripting/src/python/host_state.cpp` (the three stage waits pass a stop action to `wait_while`)
- Modify: `apps/elctl/src/laser.cpp` (`go_to`), `apps/elctl/src/laser.hpp` (comment), `docs/dev_setup.md`
- Test: `tests/scripting/` (the file that tests `move_to_position`; `tests/scripting/fakes.hpp` records `stop`), `apps/elctl/tests/test_laser_commands.cpp`

**Interfaces:**
- Consumes: `IStage::stop()` (Task 2).

- [ ] **Step 1: Write the failing tests:**
  - scripting: `ACancelledMoveStopsTheStage` — a script in `move_to_position` whose fake stage never arrives, cancelled: the fake recorded `stop` once; the same for `set_xy` and `set_x`. `AStageThatCannotStopStillCancels` — the fake's `stop` returns not-supported: the script still ends Cancelled.
  - elctl: `LaserCmd.GotoGivesUpAfterItsTimeout` gains: stderr says "stopped", and (through a second `goto` in the same test being impossible — each run is a fresh simulator) the message no longer says "may still be moving". Add `LaserCmd.GotoSaysWhenTheStageCannotBeStopped` only if a non-stopping device can be configured from a line file; otherwise ledger why not.
- [ ] **Step 2: Run** — Expected: FAIL (no `stop` recorded; old message).
- [ ] **Step 3: Implement.** In `go_to`, on interrupt or timeout: `system.stop()`; message `"stopped after <t> s at <x>, <y>"`, or, when stop is not supported or fails, the old "may still be moving" with the reason. Exit 1 either way.
- [ ] **Step 4: Run** `ctest --test-dir build/dev -R 'LaserCmd|Script|Host'` then the whole suite — Expected: PASS.
- [ ] **Step 5: Commit** `scripting, elctl: a cancelled move stops the stage`.

---

### Task 4: `PatternRunner` and `LaserSystem::pattern_runner()`

**Files:**
- Create: `libs/laser/include/pychron/laser/pattern_runner.hpp`, `libs/laser/src/pattern_runner.cpp`
- Modify: `libs/laser/include/pychron/laser/laser_system.hpp`, `libs/laser/src/laser_system.cpp`
- Test: `tests/laser/test_pattern_runner.cpp`; `tests/laser/test_laser_system.cpp` (instantiate `PatternConformance`, drop its `GTEST_ALLOW_UNINSTANTIATED` line)

**Interfaces:**
- Consumes: `PatternLibrary`, `pattern_path`, `IStage::set_xy(x, y, speed)`, `IStage::stop()`, `IStage::position()`, `IStage::moving()`.
- Produces:

```cpp
class PatternRunner final : public extraction::IPatternRunner {
 public:
  // `stage` and `patterns` must outlive the runner. `device` names it in errors.
  PatternRunner(std::string device, extraction::IStage& stage, const PatternLibrary& patterns);
  Result<void> execute_pattern(std::string_view pattern) override;
  Result<bool> running() override;
  Result<void> stop_pattern() override;
  std::vector<std::string> patterns() const override;
  // "<name>, point <i> of <n>", empty when idle: for a status line.
  std::string progress() const;
};

// LaserSystem
LaserSystem(std::string name, extraction::IExtractionDevice& driver, const TrayLibrary& trays,
            const CalibrationStore& calibrations, const PatternLibrary* patterns = nullptr);
extraction::IPatternRunner* pattern_runner() override;
// the driver's own if it has one; else the system's when there is a stage and a library; else null
```

The runner's stage is the `LaserSystem` itself (its `IStage`), so trays and a later autocenter stay out of it. The random seed is the pattern's, else `std::random_device` at each `execute_pattern`.

- [ ] **Step 1: Write the failing tests** (harness of `test_laser_system.cpp` plus `patterns/` with `square` (polygon, radius 1, nsides 4, velocity 2), `twice` (the same, iterations 2), `wide` (polygon radius 60: outside the ±50 travel), `walk` (random, seed 7), and a broken file):
  - `PatternRunner.VisitsThePointsInOrderAtThePatternsSpeed` — stage at (10, 20): the simulator's `Stage.MoveTo` log is the five polygon points then the center, each `…,2000,2000,100`; `running()` false at the end; position (10, 20).
  - `PatternRunner.RepeatsItsIterations` — `twice`: 11 moves.
  - `PatternRunner.OnePollSendsAtMostOneMove` — each `running()` adds at most one `Stage.MoveTo`.
  - `PatternRunner.StopHaltsTheStageMidSegment` — after the first move is under way: `stop_pattern()`; log ends with `Stage.Stop`; `running()` false; the position is neither the vertex nor the center; a new `execute_pattern` then works.
  - `PatternRunner.StopWhenIdleIsFine`.
  - `PatternRunner.APointOutsideTravelEndsThePattern` — `wide` from the center of travel: `execute_pattern` or the first `running()` is a Config error whose `what` contains `pattern wide, point 1 of 6`; no `Stage.MoveTo` beyond travel in the log; `running()` then false.
  - `PatternRunner.AFailedMoveEndsThePattern` — `sim.fail_next("Stage.MoveTo", 4)` on the third move: that `running()` is the error with `point 3 of 6`; the next says false.
  - `PatternRunner.ASecondPatternWhileRunningIsRefused` — Config, the first goes on.
  - `PatternRunner.UnknownAndBrokenPatternsAreConfig` — the broken file's message is its problem.
  - `PatternRunner.ASeededWalkRepeats`; `PatternRunner.ProgressSaysWhereItIs`.
  - `LaserSystem.PatternRunnerNeedsAStageAndALibrary` — null without a library, null for a stageless driver, the driver's own when it has one.
  - `PatternConformance` over the harness.
- [ ] **Step 2: Run** — Expected: FAIL to compile.
- [ ] **Step 3: Implement.** State: path, next index, center, name, an "ended with error" slot. `running()`: `moving()` first; an error from it ends the pattern.
- [ ] **Step 4: Run** `ctest --test-dir build/dev -R 'PatternRunner|LaserSystem|PatternConformance'` — Expected: PASS.
- [ ] **Step 5: Commit** `laser: patterns run point by point over the stage`.

---

### Task 5: `Lab`, the queue check, `LabSession`, the example

**Files:**
- Modify: `libs/experiment/include/pychron/experiment/lab/lab.hpp`, `libs/experiment/src/lab/lab.cpp`, `libs/experiment/src/lab/session.cpp`
- Create: `configs/examples/patterns/hexagon.toml`; modify `configs/examples/experiment.laser.toml` (second run: `pattern = "hexagon"`), `configs/examples/scripts/extraction/laser_extract.py`
- Test: `tests/experiment/test_lab_extraction.cpp`, `tests/integration/test_lab_session.cpp`

**Interfaces:**
- Consumes: `PatternLibrary`, the five-argument `LaserSystem` constructor.
- Produces: `laser::PatternLibrary Lab::patterns;` (`<lab>/patterns`). Diagnostics: field `"extraction"`, the run's row.

`laser_extract.py` after `fire_laser()`:

```python
    if pattern:
        execute_pattern()
    else:
        sleep(duration)
```

(`pattern` must be in the script context: check `Run::script_context`; add `g["pattern"]` — empty string when the run has none — with a `test_run.cpp` test if it is missing.)

- [ ] **Step 1: Write the failing tests:**
  - `LabExtractionTest.ListsPatterns`; `LabExtractionTest.UnknownPattern` (names the pattern and the known ones); `LabExtractionTest.APatternThatDidNotLoad` (the file's problem); `LabExtractionTest.ABadPatternStopsOnlyRunsThatUseIt`; `LabExtractionTest.ALabWithoutDevicesIgnoresPatterns`.
  - `LabSessionTest.ALaserQueueMovesFiresAndLeavesTheLaserOff` gains: the second run's hexagon — 7 moves at the pattern's speed around hole 7's position, between `Laser.Fire` and the laser going off; the stage ends on hole 7.
  - `LabSessionTest.APatternOutOfTravelFailsTheRunAndTheLaserIsOff` — the tray calibrated so hole 7 is 0.2 mm from the edge of travel.
- [ ] **Step 2: Run** — Expected: FAIL to compile (`Lab::patterns`), then FAIL (no runner).
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** the whole suite, both `build/dev` and `build/dev-ui` — Expected: PASS but for the four known macOS offscreen UI failures; any test listing the example lab's files is updated to the new truth.
- [ ] **Step 5: Commit** `experiment: runs execute the lab's patterns`.

---

### Task 6: `elctl laser patterns` and `pattern`

**Files:**
- Modify: `apps/elctl/src/laser.cpp`, `apps/elctl/src/laser.hpp`, `apps/elctl/src/cli.cpp` (help)
- Test: `apps/elctl/tests/test_laser_commands.cpp`

**Interfaces:**
- Consumes: `Lab::patterns`, `pattern_path`, `path_length`, `PatternRunner` through `LaserSystem::pattern_runner()`.

```
elctl ... laser patterns [--lab <dir>]
elctl ... laser pattern <device> <name> [--dry-run] [--timeout <s>]
```

- [ ] **Step 1: Write the failing tests:**
  - `LaserCmd.PatternsListsKindPointsLengthAndTime` — `hexagon  polygon  8 points  <len> mm  <t> s`; a broken file is reported on stderr and the exit code is 1.
  - `LaserCmd.PatternDryRunPrintsThePointsAndOpensNoHardware` (with the unplugged laser).
  - `LaserCmd.PatternRunsOnTheSimulatorAndReturns` — exit 0; "ended at 0.000, 0.000".
  - `LaserCmd.PatternTimeoutStopsTheStage` — `--timeout 0.2`: exit 1, "stopped".
  - `LaserCmd.UnknownPatternNamesTheKnownOnes`; usage errors (missing name, `--dry-run` with `patterns`).
- [ ] **Step 2: Run** — Expected: FAIL (unknown command).
- [ ] **Step 3: Implement.** The run loop is `goto`'s: poll `running()` every 100 ms; interrupt or timeout → `stop_pattern()`.
- [ ] **Step 4: Run** `ctest --test-dir build/dev -R LaserCmd` — Expected: PASS.
- [ ] **Step 5: Commit** `elctl: laser patterns and pattern`.

---

### Task 7: The legacy export tool

**Files:**
- Create: `tools/export_patterns.py`, `tools/tests/test_export_patterns.py`
- Modify: `tests/laser/CMakeLists.txt` (an `add_test` running `python3 -m unittest` when `find_program(python3)` succeeds; otherwise a message and no test)

**Interfaces:**
- Produces: `python3 tools/export_patterns.py <dir of .lp> <out dir> [--force]`; module functions `read_lp(path) -> (class_name, state_dict)` and `to_toml(class_name, state) -> (text | None, notes)`.

Mapping: `PolygonPattern`→polygon (`radius`, `nsides`, `rotation`); `LinearPattern`→linear (`length`, `rotation`, `npasses`); `CircularContourPattern`→circular_contour, `LineSpiralPattern`→line_spiral (+`step_scalar`), `SquareSpiralPattern`→square_spiral (`radius`, `nsteps`, `percent_change`); `RandomPattern`→random (`walk_x`, `walk_y`, `npoints`); `RubberbandPattern`→rubberband (`nominal_length`→`length`, `offset`, `rotation`); `RasterRubberbandPattern`→raster (+`dx`, `single_pass`); `TroughPattern`→trough (`length`, `width`, `rotation`, `use_x`). Common: `velocity`, `niterations`→`iterations`. A missing attribute takes the kind's default (traits pickles omit defaults).

- [ ] **Step 1: Write the failing tests** (pickles are built in the test with stand-in classes whose `__module__` is `pychron.lasers.pattern.patterns`, protocols 2 and 3):
  - `test_polygon_round_trip`; `test_every_mapped_class`; `test_missing_attributes_take_defaults`.
  - `test_unmapped_classes_are_reported_and_skipped` — `ArcPattern`, `SeekPattern`, `DragonFlyPeakPattern`, `DiamondPattern`, an unknown class.
  - `test_series_and_direction_are_reported` — `z_pattern_enabled`, `power_pattern_enabled`, `disable_at_end`, `direction = "in"`.
  - `test_a_pickle_cannot_run_code` — a pickle whose reduce calls `os.system`: nothing is executed (a sentinel file is not created) and the file is reported as unreadable.
  - `test_does_not_overwrite_without_force`.
  - `test_written_files_load` — when the build's `elctl` is given in `PYCHRON_ELCTL`: `elctl laser patterns --lab <out>` exits 0 (skipped otherwise).
  - `test_real_lab_files` — when `PYCHRON_LEGACY_PATTERNS` names a directory: every non-vision `.lp` exports (skipped otherwise; run by hand against the co2 folder and the result ledgered).
- [ ] **Step 2: Run** `python3 -m unittest discover tools/tests` — Expected: FAIL (no module).
- [ ] **Step 3: Implement.** `pickle.Unpickler` subclass whose `find_class` returns a stub class for any `(module, name)`, recording both; stubs accept `__setstate__` and `__reduce__` arguments without running anything. TOML is written by hand (the values are numbers and booleans).
- [ ] **Step 4: Run** the unittest and `ctest --test-dir build/dev -R export_patterns` — Expected: PASS.
- [ ] **Step 5: Commit** `tools: export legacy laser patterns to TOML`.

---

### Task 8: Documentation

**Files:**
- Modify: `docs/dev_setup.md` (patterns: the file, the kinds, running one, exporting legacy ones, the vertex-pause limit; `goto` now stops)
- Modify: `docs/superpowers/specs/2026-10-04-laser-patterns-design.md` (Status; "As built")
- Modify: `docs/superpowers/specs/2026-10-04-laser-system-design.md` (section 11: `goto` stops the stage now)
- Modify: `libs/experiment/include/pychron/experiment/lab/lab.hpp` header comment (`<lab>/patterns/`)

- [ ] **Step 1:** Write the changes.
- [ ] **Step 2: Run** both suites — Expected: `build/dev` all pass; `build/dev-ui` only the four known failures.
- [ ] **Step 3: Commit** `docs: laser patterns`.
