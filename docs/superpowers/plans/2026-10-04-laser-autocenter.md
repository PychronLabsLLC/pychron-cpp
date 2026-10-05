# Laser Autocenter (part 2c-1) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A hole move that asks for autocenter ends on the hole the camera sees, and the position found is remembered per hole.

**Architecture:** `libs/laser` gains camera configuration, a corrections store, a simulated tray camera and an autocenter state machine inside `LaserSystem`, advanced by `moving()` polls and driven by `vision::Autocenter`. `Lab` loads the cameras, `LabSession` attaches them, `elctl laser` gets `autocenter`, `corrections` and `look`.

**Tech Stack:** C++20, toml++, GoogleTest, `pychron::vision` (no OpenCV needed).

**Spec:** `docs/superpowers/specs/2026-10-04-laser-autocenter-design.md`

## Global Constraints

- AGENTS.md: no pull requests; never skip or disable a failing test; warnings are errors (gcc 14, clang 18, Apple clang, MSVC).
- `libs/laser` depends on `core`, `devices` and now `vision`; `vision` depends on `core` only. No Qt in any `libs/` public header.
- Everything returns `Result`; nothing throws across a public function.
- No thread: autocenter advances only in `LaserSystem::moving()`; one `moving()` call sends at most one stage command.
- Guard: total autocenter movement ≤ `min(1.0, 0.45 × nearest-neighbour distance)` mm from the hole's **calibrated** position.
- Files: `<lab>/cameras.toml`; `<lab>/stage_corrections/<device>.<tray>.toml`, `schema_version = 1`, written to a temporary file and renamed.
- Numbers into text are locale-free; names in file paths are plain file-name parts.
- Tests never write beside `configs/examples`; scratch directories have a random suffix.
- Build and test: `cmake --build build/dev -j8 && ctest --test-dir build/dev -j8`.

## Review Focus

1. A tray error just under and just over the guard (0.45 × pitch): under converges on the true hole, over fails; in neither case does the stage end on the neighbouring hole. (Task 5.)
2. A wrong `flip_x`/`flip_y`: the offset grows, autocenter fails as runaway within its iteration cap, the stage returns to the start and nothing is saved. (Task 5.)
3. Corrections after the world changed: a recalibrated tray, an edited tray map, a correction file hand-edited to a far position, or one for another device; none is used. (Tasks 3, 5.)
4. A camera that fails or stalls mid-autocenter (grab error, frames that never get newer): failure with the stage back at the start, no endless loop, laser never fired when `on_failure = "fail"`. (Tasks 5, 6.)
5. `stop()`, a new move, or the run ending while an autocenter is in progress: the stage stops or goes where the new move says, nothing is saved, and the next hole move starts clean. (Task 5.)

---

### Task 1: `vision::AutocenterReason`

**Files:**
- Modify: `libs/vision/include/pychron/vision/autocenter.hpp`, `libs/vision/src/autocenter.cpp`
- Test: `tests/vision/test_autocenter.cpp` (existing string expectations become enum ones)

**Interfaces:**
- Produces:

```cpp
enum class AutocenterReason { None, NoTarget, MaxIterations, Runaway, MaxTotal, Invalid, Clipped, StaleFrame, Camera };
std::string_view to_string(AutocenterReason) noexcept;  // "", "no_target", "max_iterations", "runaway", "max_total", "invalid", "clipped", "stale_frame", "camera"
// AutocenterStep::reason is an AutocenterReason
```

- [ ] **Step 1:** Change the existing tests to compare against the enum; add `AutocenterReason.NamesAreTheOldWords`.
- [ ] **Step 2: Run** `cmake --build build/dev -j8` — Expected: FAIL to compile.
- [ ] **Step 3:** Implement; fix any other user of `reason` (grep).
- [ ] **Step 4: Run** `ctest --test-dir build/dev -R Autocenter` — Expected: PASS.
- [ ] **Step 5: Commit** `vision: autocenter failure reasons are an enum`.

---

### Task 2: `CameraConfig`, `CameraLibrary`

**Files:**
- Create: `libs/laser/include/pychron/laser/camera.hpp`, `libs/laser/src/camera.cpp`
- Modify: `libs/laser/CMakeLists.txt` (link `pychron::vision`)
- Test: `tests/laser/test_camera.cpp`

**Interfaces:**
- Produces:

```cpp
enum class CameraSource { Sim, Recorded };
enum class OnAutocenterFailure { Continue, Fail };
struct CameraConfig {
  std::string device;
  CameraSource source = CameraSource::Sim;
  double px_per_mm = 23.0;
  bool flip_x = false, flip_y = true;
  StageXY aim_offset_px{};
  Duration settle{std::chrono::milliseconds(200)};
  std::string frames;                       // recorded: case directory, relative to the lab
  StageXY sim_tray_error_mm{};              // sim
  double sim_noise = 0.01;
  int sim_width = 200, sim_height = 200;
  double tolerance_mm = 0.03, max_step_mm = 0.5;
  int max_iterations = 4, frames_per_step = 3;
  OnAutocenterFailure on_failure = OnAutocenterFailure::Continue;
  vision::CameraStageMap map() const;       // from_scale(px_per_mm, flip_x, flip_y)
};
class CameraLibrary {
 public:
  static CameraLibrary load(const std::filesystem::path& file);   // never fails; a missing file is empty
  static CameraLibrary parse(std::string_view toml, std::string file_name);
  const CameraConfig* find(std::string_view device) const;
  std::vector<std::string> devices() const;                        // sorted
  // "<file>: <device>.<key>: <what>"; a device whose table has a problem is not in find().
  const std::vector<std::string>& problems() const noexcept;
  // The problems of one device's table, for the queue check.
  std::vector<std::string> problems_of(std::string_view device) const;
};
```

Ranges: `px_per_mm` > 0 (≤ 10000); `settle_ms` 0–10000; `sim.noise` 0–1; `sim.width`/`height` 16–4096; `tolerance_mm` > 0 (≤ 10); `max_step_mm` > 0 (≤ 10); `max_iterations` 1–50; `frames_per_step` 1–15; `source = "recorded"` requires `frames`.

- [ ] **Step 1: Write the failing tests:** `CameraConfig.DefaultsWithAnEmptyTable`; `CameraConfig.ReadsEveryKey`; `CameraConfig.MapFollowsTheFlips` (a target right of centre needs a move whose sign follows `flip_x`; pinned against `vision::CameraStageMap::from_scale`); `CameraConfigBad.RefusesBadValues` (parameterised: each range end, wrong types, unknown key at each level, `source = "usb"`, recorded without `frames`, `on_failure = "retry"`), each naming device and key; `CameraLibrary.OneBadTableDoesNotHideTheOthers`; `CameraLibrary.MissingFileIsEmpty`; `CameraLibrary.NotTomlIsOneProblem`.
- [ ] **Step 2: Run** — Expected: FAIL to compile.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `ctest --test-dir build/dev -R Camera` — Expected: PASS.
- [ ] **Step 5: Commit** `laser: camera configuration per extraction device`.

---

### Task 3: `CorrectionStore`

**Files:**
- Create: `libs/laser/include/pychron/laser/correction_store.hpp`, `libs/laser/src/correction_store.cpp`
- Modify: `libs/laser/include/pychron/laser/calibration_store.hpp`, `libs/laser/src/calibration_store.cpp` (`CalibrationStatus::fingerprint`)
- Test: `tests/laser/test_correction_store.cpp`, `tests/laser/test_calibration_store.cpp`

**Interfaces:**
- Produces:

```cpp
// calibration_store.hpp: sha256 hex of the points as written; set when state is Ok
std::string CalibrationStatus::fingerprint;
std::string fingerprint(std::span<const CalibrationPoint> points);

struct HoleCorrection { double x = 0, y = 0; double residual_mm = 0; std::string found; };   // stage mm; found: UTC ISO-8601
class CorrectionStore {
 public:
  explicit CorrectionStore(std::filesystem::path dir);
  std::filesystem::path file(std::string_view device, std::string_view tray) const;
  // The corrections that apply to this map and calibration; empty when there is no file or it is for another.
  // Config error only for an unreadable file or an unsafe name.
  Result<std::map<std::string, HoleCorrection, std::less<>>> load(const TrayMap& map, std::string_view device,
                                                                 std::string_view calibration) const;
  // Adds or replaces one hole; a file for another map or calibration is started again.
  Result<void> put(const TrayMap& map, std::string_view device, std::string_view calibration,
                   std::string_view hole, const HoleCorrection& correction) const;
  Result<void> clear(std::string_view device, std::string_view tray) const;
  Result<void> clear_hole(const TrayMap& map, std::string_view device, std::string_view calibration,
                          std::string_view hole) const;
};
```

- [ ] **Step 1: Write the failing tests:** `Fingerprint.ChangesWithAnyPoint` and `CalibrationStore.StatusCarriesTheFingerprint`; `CorrectionStore.PutsAndLoads` (two holes, exact doubles, `found` kept); `.MissingIsEmpty`; `.AnotherMapIsIgnoredAndReplacedAtTheNextPut`; `.AnotherCalibrationIsIgnoredAndReplaced`; `.AHoleNotOnTheMapIsRefused` (put) and ignored (load of a hand-edited file); `.NonFiniteValuesInAFileAreIgnored`; `.RefusesUnsafeNames`; `.SaveGoesThroughATemporaryFile` (a directory in the temporary's place: Io error, old file whole); `.ClearHoleAndClear`; `.AnotherSchemaVersionIsAnError`; `.EachDeviceHasItsOwn`.
- [ ] **Step 2: Run** — Expected: FAIL to compile.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `ctest --test-dir build/dev -R 'CorrectionStore|Fingerprint|CalibrationStore'` — Expected: PASS.
- [ ] **Step 5: Commit** `laser: per-hole corrections stored per device and tray`.

---

### Task 4: `SimTrayCamera` and `make_frame_source`

**Files:**
- Create: `libs/laser/include/pychron/laser/tray_camera.hpp`, `libs/laser/src/tray_camera.cpp`
- Test: `tests/laser/test_tray_camera.cpp`

**Interfaces:**
- Consumes: `CameraConfig`, `vision::HoleScene`, `vision::render`, `vision::RecordedSource`, `vision::load_case`.
- Produces:

```cpp
// What a camera looking down the beam path is over: where the stage is and
// where the calibration says the current tray's holes are (stage mm).
struct TraySight { StageXY stage{}; std::vector<StageXY> holes; double hole_radius_mm = 0.5; };
using TraySightFn = std::function<TraySight()>;

class SimTrayCamera final : public vision::IFrameSource {
 public:
  SimTrayCamera(const CameraConfig& config, TraySightFn sight, const Clock& clock);
  Result<vision::Frame> grab() override;      // stamped from `clock`, seq from 1
  vision::FrameInfo info() const override;
  vision::Truth last_truth() const;           // of the hole rendered; visible = false with none
};

Result<std::unique_ptr<vision::IFrameSource>> make_frame_source(const CameraConfig& config,
    const std::filesystem::path& lab, TraySightFn sight, const Clock& clock);
```

The scene: the hole nearest the stage (true position = calibrated + `sim_tray_error_mm`), `neighbours = true` with `pitch_mm` the distance to its nearest other hole, `hole_radius_mm` from the sight, `px_per_mm`, size and noise from the config, seed advancing per grab. Image convention is the vision library's (image +x = stage +x, image +y = stage −y) composed with the config's flips relative to the default (`flip_x = false`, `flip_y = true`): a config with other flips mirrors the rendered frame, so that a *wrong* flip in the config really misleads autocenter. With no hole within the frame: the bare tray.

- [ ] **Step 1: Write the failing tests:** `SimTrayCamera.TheHoleIsWhereTheTruthSays` (stage on the calibrated hole, error (0.2, −0.1): `SimpleFinder` finds the centre within 1 px of centre + (0.2, +0.1)·px_per_mm, image y down); `.FollowsTheStage`; `.RendersTheNearestHole`; `.BareTrayWithNoHoleInView` (finder finds nothing; truth not visible); `.FramesGetNewerAndNumbered` (timestamps follow the clock, seq 1, 2, 3); `.AFlippedConfigMirrorsTheImage`; `.NoiseIsSeededPerGrab` (two cameras, same sequence); `MakeFrameSource.SimAndRecorded` (a recorded case written with `vision::FrameRecorder` replays; a missing directory is a Config error naming it).
- [ ] **Step 2: Run** — Expected: FAIL to compile.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `ctest --test-dir build/dev -R 'SimTrayCamera|MakeFrameSource'` — Expected: PASS.
- [ ] **Step 5: Commit** `laser: a simulated camera that sees the tray`.

---

### Task 5: Autocenter and corrections in `LaserSystem`

**Files:**
- Modify: `libs/laser/include/pychron/laser/laser_system.hpp`, `libs/laser/src/laser_system.cpp`
- Create: `libs/laser/src/autocenter_run.hpp`, `libs/laser/src/autocenter_run.cpp` (the state machine, private to the library)
- Modify: `tests/laser/laser_harness.hpp` (a harness with corrections and a sim camera)
- Test: `tests/laser/test_laser_autocenter.cpp`

**Interfaces:**
- Consumes: Tasks 1–4; `IStage::set_xy`, `moving`, `position`, `stop`.
- Produces, on `LaserSystem`:

```cpp
struct AutocenterOutcome {
  enum class Result { None, Converged, Failed, Stopped } result = Result::None;
  vision::AutocenterReason reason = vision::AutocenterReason::None;
  std::string hole, tray;
  int iterations = 0;
  StageXY found{}, moved_mm{};
  double residual_mm = 0;
};
// Both before the first move; what they point at must outlive the system.
void set_corrections(const CorrectionStore& corrections);
void attach_camera(CameraConfig config, std::unique_ptr<vision::IFrameSource> frames, const Clock& clock);
bool has_camera() const;
TraySightFn sight();                         // for the sim camera; valid while the system lives
AutocenterOutcome last_autocenter() const;
std::map<std::string, HoleCorrection, std::less<>> corrections() const;   // of the current tray
double guard_mm(std::string_view hole) const;                             // 0 for an unknown hole
```

Behaviour is spec sections 5 and 6. `moving()` order: the driver's `moving()` first (true → return true); then the phase's one action. Settle is measured on the attached clock from the poll that first saw the stage stopped. Frames for one Look are grabbed in that one `moving()` call. `on_failure = "fail"`: the `moving()` call that sees the stage back at the start returns the error; the next returns false.

- [ ] **Step 1: Write the failing tests** (harness: `ChromiumSim`, tray `small` — holes 5 mm apart — calibrated at c = (10, 20), sim camera with tray error (0.15, −0.10), settle 200 ms, a `CorrectionStore` in the scratch lab):
  - `Autocenter.ConvergesOnTheTrueHoleAndSaves` — hole 3: the simulated stage ends within 0.03 mm of (15.15, 19.90); outcome Converged, iterations ≥ 1, `moved_mm` ≈ (0.15, −0.10); the store has hole 3 at that position with a residual below tolerance and a `found` time.
  - `Autocenter.TheNextMoveStartsAtTheCorrection` — the first `Stage.MoveTo` of a second move is the corrected position; it converges in one look with no nudge.
  - `Autocenter.WithoutTheFlagTheCorrectionIsUsedAndNoFrameTaken`.
  - `Autocenter.WithNoCameraNothingChanges` (the 2a behaviour, flag true).
  - `Autocenter.SettleIsWaitedBeforeLooking` — no grab until 200 ms of clock after arrival.
  - `Autocenter.OnePollOneStageCommand`.
  - `Autocenter.NoHoleInViewReturnsToTheStart` — tray error (3, 3): outcome Failed/NoTarget; stage back at the calibrated position; `moving()` ends false (`continue`); nothing saved.
  - `Autocenter.FailingIsAnErrorWhenAskedFor` — the same with `on_failure = fail`: one `moving()` returns Config naming hole, tray and `no_target`; the next false.
  - `Autocenter.AWrongFlipIsCaughtAsRunaway` — config `flip_x = true` against the scene: Failed/Runaway (or MaxTotal), back at the start, nothing saved.
  - `Autocenter.NeverEndsOnTheNeighbour` — parameterised tray error along x: 2.0 (under the 2.25 guard) converges on the true hole; 2.4 and 2.6 fail; in every case the final stage position is within the guard of the calibrated position.
  - `Autocenter.ACorrectionOutsideTheGuardIsIgnored` — a hand-written correction 3 mm away: the move goes to the calibrated position.
  - `Autocenter.ANewCalibrationDropsTheCorrections`.
  - `Autocenter.ACameraThatFailsIsAFailure` — a frame source failing on its nth grab: Failed/Camera, back at the start.
  - `Autocenter.FramesThatNeverGetNewerEndIt` — a source repeating one timestamp: Failed (StaleFrame or MaxIterations) within a bounded number of polls.
  - `Autocenter.StopEndsItAndSavesNothing`; `Autocenter.ANewMoveAbandonsIt` (to another hole: that hole is centred, the first has no correction); `Autocenter.SetXyAbandonsIt`.
  - `Autocenter.GuardIsFromTheNearestNeighbour` — `guard_mm("3") == 2.25`; a one-hole tray gives 1.0.
  - The extraction, laser, stage and pattern conformance suites over the camera harness.
- [ ] **Step 2: Run** — Expected: FAIL to compile.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `ctest --test-dir build/dev -R 'Autocenter|LaserSystem|PatternRunner|Conformance'` then the whole suite — Expected: PASS.
- [ ] **Step 5: Commit** `laser: a hole move can centre the hole under the beam`.

---

### Task 6: `Lab`, queue check, `LabSession`, the example

**Files:**
- Modify: `libs/experiment/include/pychron/experiment/lab/lab.hpp`, `libs/experiment/src/lab/lab.cpp`, `libs/experiment/src/lab/session.cpp`
- Create: `configs/examples/cameras.toml`
- Modify: `.gitignore` (`configs/examples/**/stage_corrections/`)
- Test: `tests/experiment/test_lab_extraction.cpp`, `tests/integration/test_lab_session.cpp`

**Interfaces:**
- Produces: `laser::CameraLibrary Lab::cameras;` (`<lab>/cameras.toml`), `std::unique_ptr<laser::CorrectionStore> Lab::corrections;` (`<lab>/stage_corrections`, never null). Diagnostics: field `"extraction"`.
- `LabSession::Services`: for each laser system, `set_corrections(*lab.corrections)`; with a camera table: `make_frame_source(config, lab.paths.dir, system.sight(), line clock)` and `attach_camera`. A frame source that cannot be made is noted in the session's problems (`LabSession::problems()`, new: `std::vector<std::string>`) and the device runs without a camera.

- [ ] **Step 1: Write the failing tests:**
  - `LabExtractionTest.LoadsCameras`; `.ACameraThatDidNotLoadStopsRunsOnItsDevice` (message is the table's problem); `.ADeviceWithoutACameraIsFine`.
  - `LabSessionTest.ALaserQueueMovesFiresAndLeavesTheLaserOff` — updated: with the example's sim camera (tray error (0.15, −0.10)) the beam-on stage positions are about the *true* holes: hole 3 ends at (30.15, 29.90) ± 0.03, the hexagon is about (20.15, 19.90); `stage_corrections/co2.example-9.toml` in the scratch lab has holes 3 and 7.
  - `LabSessionTest.AHiddenHoleFailsTheRunBeforeTheLaserFires` — `on_failure = "fail"`, tray error (3, 3): run 1 Failed, error names hole 3 and `no_target`; no `Laser.Fire` in the simulator's log.
  - `LabSessionTest.AHiddenHoleIsCarriedOnPastByDefault` — the same with `continue`: runs succeed at the calibrated positions.
  - The patterns out-of-travel test and the two-laser test still pass (their fixtures adjusted for the camera where needed, never weakened).
- [ ] **Step 2: Run** — Expected: FAIL to compile, then FAIL.
- [ ] **Step 3: Implement**; write `configs/examples/cameras.toml` (commented, as spec section 3).
- [ ] **Step 4: Run** both suites — Expected: `build/dev` all pass; `build/dev-ui` only the four known failures.
- [ ] **Step 5: Commit** `experiment: laser sessions centre holes with the lab's cameras`.

---

### Task 7: `elctl laser autocenter | corrections | look`

**Files:**
- Modify: `apps/elctl/src/laser.cpp`, `apps/elctl/src/laser.hpp`, `apps/elctl/src/cli.cpp`
- Test: `apps/elctl/tests/test_laser_commands.cpp`

**Interfaces:**
- Consumes: `Lab::cameras`, `Lab::corrections`, `LaserSystem` with a camera, `vision::SimpleFinder`.

```
elctl ... laser autocenter <device> <tray> <hole> [--timeout <s>]
elctl ... laser corrections <device> <tray> [clear [<hole>]]
elctl ... laser look <device> [--tray <tray>]
```

- [ ] **Step 1: Write the failing tests:**
  - `LaserCmd.AutocenterCentresAndSaves` (`--sim`, the tray calibrated near the corner as the goto test does): exit 0; output has `converged`, the moved distance `0.150, -0.100` (± printed rounding), the residual; `corrections` then lists the hole with its distance from calibrated.
  - `LaserCmd.AutocenterFailureIsExitOneWhateverTheConfigSays` (tray error (3, 3), `on_failure = continue`): exit 1, stderr has `no_target`; nothing saved.
  - `LaserCmd.AutocenterNeedsACamera` (no table for the device): exit 1 naming `cameras.toml`.
  - `LaserCmd.CorrectionsClearOneAndAll`; `LaserCmd.CorrectionsWithNoneSaysSo`.
  - `LaserCmd.LookSaysWhatTheFinderSees` (`--sim`: at the stage's rest position with a tray calibrated so a hole is in view): centre px, offset px and mm, radius px.
  - `LaserCmd.LookOnRecordedFrames` (a case written in the test with `vision::FrameRecorder` from `vision::render`; `source = "recorded"`; no `--sim`, laser unplugged): prints the offset; opens no hardware.
  - `LaserCmd.LookSeesNothingIsExitOne`.
  - Usage errors.
- [ ] **Step 2: Run** — Expected: FAIL (unknown commands).
- [ ] **Step 3: Implement.** `autocenter` polls `moving()` every 50 ms; Ctrl-C or timeout → `stop()`. Each completed step is printed from `last_autocenter()` deltas (iteration count change).
- [ ] **Step 4: Run** `ctest --test-dir build/dev -R LaserCmd` — Expected: PASS.
- [ ] **Step 5: Commit** `elctl: laser autocenter, corrections and look`.

---

### Task 8: Documentation

**Files:**
- Modify: `docs/dev_setup.md` (cameras.toml, what autocenter does and when, corrections and when they are dropped, the wrong-hole guard, `on_failure`, the three commands, that live frames wait for screen capture)
- Modify: `docs/superpowers/specs/2026-10-04-laser-autocenter-design.md` (Status; "As built")
- Modify: `docs/superpowers/specs/2026-10-03-vision-design.md` §20 (which leftovers are done)
- Modify: `libs/experiment/include/pychron/experiment/lab/lab.hpp` header comment

- [ ] **Step 1:** Write the changes.
- [ ] **Step 2: Run** both suites and the documented example command — Expected: as Task 6.
- [ ] **Step 3: Commit** `docs: laser autocenter and hole corrections`.
