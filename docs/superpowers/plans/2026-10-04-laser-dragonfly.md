# Laser Dragonfly (part 2c-2) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A run's dragonfly pattern keeps the laser's stage on the glowing sample for the pattern's duration.

**Architecture:** `PatternKind::Dragonfly` in the existing pattern files; `PatternRunner` gains a vision-driven branch using `vision::Dragonfly`, with frames from the device's camera; the simulated camera shows a glow while the simulated laser fires.

**Tech Stack:** C++20, toml++, GoogleTest, `pychron::vision`; Python 3 for the export tool.

**Spec:** `docs/superpowers/specs/2026-10-04-laser-dragonfly-design.md`

## Global Constraints

- AGENTS.md: no pull requests; never skip or disable a failing test; warnings are errors on all CI compilers.
- No thread: the pattern advances only in `PatternRunner::running()`; one stage command per poll.
- Only a camera that passes `usable_for_autocenter` drives a stage.
- Scratch directories in tests have a random suffix; tests never write beside `configs/examples`.
- Build and test: `cmake --build build/dev -j8 && ctest --test-dir build/dev -j8` (about 3 minutes).

## Review Focus

1. The glow leaves the frame or is hidden: the spiral search never takes the stage beyond the perimeter, and the pattern still ends on time back at the centre. (Task 3.)
2. Camera dies with the beam on: stage back at the centre; `continue` holds and ends on time, `fail` errors and the run's ending switches the laser off. (Tasks 3, 4.)
3. A dragonfly on a device with no usable camera, a recorded camera, or via `elctl laser pattern`: refused before anything moves. (Tasks 3, 4.)
4. `stop_pattern()` or a cancelled script part way: stage stops where it is, nothing further is sent, a new pattern can start. (Task 3.)
5. Duration edge cases: a duration shorter than one settle, and a clock that jumps past the end between polls: ends, back at the centre, no negative waits. (Task 3.)

---

### Task 1: The pattern kind and its file

**Files:** `libs/laser/include/pychron/laser/pattern.hpp`, `libs/laser/src/pattern.cpp`; test `tests/laser/test_pattern.cpp`.

**Interfaces — produces:** `PatternKind::Dragonfly` ("dragonfly"); `Pattern` fields `double duration_s = 0, perimeter_radius = 2.5, saturation_threshold = 0.75, aggressiveness = 1.0, move_threshold = 0.033, max_step = 0.5, spiral_base = 0.5, target_radius = 0.5; bool square_spiral = false;` and `bool follows_glow() const`; `pattern_points` → empty, `pattern_point_count` → 0, `pattern_path` → Config error for a dragonfly.

- [ ] **Step 1: Failing tests:** `PatternFile.ReadsADragonfly` (every key); `.ADragonflyNeedsItsDuration`; `PatternFileBad` cases (duration 0, 3601, saturation 0 and 1.5, perimeter 0, max_step 0, spiral "round", `iterations` on a dragonfly, `nsides` on a dragonfly); `PatternPoints.ADragonflyHasNoPath`.
- [ ] **Step 2: Run** — Expected: FAIL to compile.
- [ ] **Step 3: Implement.** The existing every-kind tests iterate `kKinds`: the dragonfly is not added to those loops (it has no points); say so in a comment.
- [ ] **Step 4: Run** `ctest --test-dir build/dev -R Pattern` — PASS.
- [ ] **Step 5: Commit** `laser: dragonfly is a pattern kind`.

### Task 2: The simulated glow

**Files:** `libs/laser/include/pychron/laser/camera.hpp`, `src/camera.cpp`, `tray_camera.{hpp,cpp}`, `laser_system.cpp` (`sight()`); tests `test_camera.cpp`, `test_tray_camera.cpp`.

**Interfaces — produces:** `CameraConfig::sim_grain_offset_mm`, `sim_glow_drift_mm_per_s` (`StageXY`), `sim_glow_sigma_mm = 0.3`; `TraySight::firing`, `TraySight::output_percent`.

- [ ] **Step 1: Failing tests:** config keys and ranges (`glow_sigma_mm` > 0); `SimTrayCamera.ShowsAGlowAtTheGrainWhileFiring` (Glow-mode `SimpleFinder` finds it at hole + grain offset; not firing → the hole as before); `.TheGlowDriftsWhileFiring` (after 10 s of clock, moved by drift × 10; the drift clock restarts when firing restarts); `.BrightnessFollowsOutput` (saturation at 5 % below that at 40 %); `LaserSystem.SightSaysWhetherTheLaserFires`.
- [ ] **Step 2–4:** red, implement, green (`-R 'SimTrayCamera|Camera|LaserSystem'`).
- [ ] **Step 5: Commit** `laser: the simulated camera sees the sample glow`.

### Task 3: The runner follows the glow

**Files:** `libs/devices/.../interfaces.hpp` (`IPatternRunner::last_note`), `libs/laser/.../pattern_runner.{hpp,cpp}`, `laser_system.{hpp,cpp}` (gives the runner its vision when a camera is attached), `libs/vision/.../frame.hpp|kernel` (`aim_crop`, used by both controllers), `libs/scripting/src/python/host_state.cpp` (logs the note); tests `tests/laser/test_dragonfly.cpp`, `tests/vision/*`, `tests/scripting/test_script_host.cpp`.

**Interfaces — produces:**

```cpp
struct PatternVision { vision::IFrameSource* frames = nullptr; const CameraConfig* camera = nullptr; const Clock* clock = nullptr; };
void PatternRunner::set_vision(PatternVision vision);     // by LaserSystem::attach_camera
std::string PatternRunner::last_note() override;          // taken once
```

- [ ] **Step 1: Failing tests** (camera harness; pattern files `follow` (duration 20, velocity 2), `brief` (duration 0.05)); the laser is enabled, set to 20 % and fired through the system before `execute_pattern`:
  - `Dragonfly.FollowsAnOffsetGrain` — grain offset (0.3, −0.2): within a few steps the stage is within `move_threshold` + 0.02 of the grain and stays; ends after 20 s of clock back at the centre; `running()` false; note empty.
  - `Dragonfly.FollowsADriftingGrain` — drift 0.02 mm/s for 20 s: the stage tracks to within 0.1 mm throughout.
  - `Dragonfly.HoldsWhenSaturated` — output 100 %: no moves after the first hold.
  - `Dragonfly.SearchesWhenTheGlowIsLostAndFindsItAgain` — a source wrapper that blanks frames for 5 s: spiral moves appear, all within the perimeter; after it returns the stage is back on the grain.
  - `Dragonfly.NeverLeavesThePerimeter` — perimeter 0.4, grain 2 mm away: every move within 0.4 mm (+1 µm) of the centre.
  - `Dragonfly.EndsOnTimeBackAtTheCentre`; `.ADurationShorterThanASettleStillEnds`; `.AClockThatJumpsPastTheEndEndsIt`.
  - `Dragonfly.StopPartWayStopsTheStage`; `.OnePollOneStageCommand`.
  - `Dragonfly.ACameraThatDiesHoldsAtTheCentreUntilTheEnd` (continue: ends normally at the duration, note mentions the camera) and `.ACameraThatDiesIsAnErrorWhenAskedFor` (fail).
  - `Dragonfly.WithoutACameraItIsRefused` (system with no camera: Config, nothing sent).
  - `Dragonfly.ANoteIsSaidOnce`; script host: `APatternsNoteIsLogged`.
  - vision: `AimCrop.*` and the two controllers' existing suites unchanged.
- [ ] **Step 2–4:** red, implement, green; then the whole suite.
- [ ] **Step 5: Commit** `laser: a dragonfly pattern follows the glow`.

### Task 4: Queue check, session, elctl, example

**Files:** `libs/experiment/src/lab/lab.cpp`, `apps/elctl/src/laser.cpp`, `configs/examples/patterns/follow.toml`, `configs/examples/cameras.toml` (grain offset); tests `tests/experiment/test_lab_extraction.cpp`, `tests/integration/test_lab_session.cpp`, `apps/elctl/tests/test_laser_commands.cpp`.

- [ ] **Step 1: Failing tests:** `LabExtractionTest.ADragonflyNeedsACamera` (no table; recorded; a table that did not load), `.ADragonflyWithACameraChecks`; `LabSessionTest.ADragonflyRunFollowsTheGrainAndLeavesTheLaserOff` (a queue copy whose second run names `follow`; with the beam on the stage ends within 0.1 mm of hole + tray error + grain offset; laser off after); `LaserCmd.PatternsListsADragonfly` ("follow  dragonfly  follows the glow for 5.0 s"); `LaserCmd.PatternDryRunOfADragonflySaysItHasNoPath`; `LaserCmd.ADragonflyNeedsTheCameraHere` (`elctl laser pattern` on a dragonfly: refused with why, since the laser is not fired).
- [ ] **Step 2–4:** red, implement, green; both suites.
- [ ] **Step 5: Commit** `experiment, elctl: dragonfly patterns in queues`.

### Task 5: Legacy export and documentation

**Files:** `tools/export_patterns.py`, `tools/tests/test_export_patterns.py`, `docs/dev_setup.md`, the spec ("As built"), vision spec §20.

- [ ] **Step 1: Failing tests:** `test_dragonfly_exports` (attributes mapped; `limit`, `pre_seek_delay`, mask reported); the unmapped-classes test no longer lists `DragonFlyPeakPattern`.
- [ ] **Step 2–4:** red, implement, green; run by hand on the co2 folder with `ELCTL` set and ledger the result.
- [ ] **Step 5:** docs; both suites; **Commit** `tools, docs: dragonfly patterns`.
