# Laser System (part 2a) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A queue that names an extraction device, a tray and a hole drives a real or simulated laser to that hole and fires it.

**Architecture:** A new `libs/laser` holds tray maps, the stage calibration and a `LaserSystem` facade that wraps a driver's `IExtractionDevice` and turns hole names into stage millimetres. `Lab` loads the files, `check_lab_queue` refuses what cannot run, `LabSession` builds one facade per extraction driver and each run binds its own. `elctl laser` makes and checks calibrations.

**Tech Stack:** C++20, toml++, GoogleTest, the existing `Result`/`Error` types. No Qt, no OpenCV.

**Spec:** `docs/superpowers/specs/2026-10-04-laser-system-design.md`

## Global Constraints

- AGENTS.md: no pull requests; never skip or disable a failing test; warnings are errors on the CI compilers.
- `libs/laser` depends on `core` and `devices` only. Qt appears in no `libs/` public header.
- Everything returns `Result`; no exceptions across a public function.
- Numbers from text are parsed with `codec::parse_decimal`-style locale-free parsing, never `std::from_chars(double)` (not on the macOS CI libc++) and never `std::stod`.
- Stage and map coordinates are millimetres. Transform: `stage = s R(theta) map + c`.
- Scale rule: a fitted scale more than 2% from 1 is a Config error.
- Calibration file: `<lab>/stage_calibrations/<device>.<tray>.toml`, `schema_version = 1`. Tray maps: `<lab>/tray_maps/*.txt`.
- Tests never write beside `configs/examples`; they copy to a temporary directory with a random suffix.
- Lifetime: fakes and stores are declared before the object that refers to them.
- Build and test: `cmake --build build/dev -j8 && ctest --test-dir build/dev -j8`.

## Review Focus

1. A tray map saved by Windows tools (CRLF line ends, a UTF-8 BOM, trailing spaces): it loads the same holes as the LF file. (Task 1 test.)
2. A calibration whose points are nearly collinear or two holes swapped: the rms shows it and nothing moves to a wild position; a swapped pair trips the scale or rms, not a silent mirror. (Task 2 test.)
3. A run on device `co2` followed by a run on device `diode` in one queue: each run ends its own device and the first is off before the second starts. (Task 6 test.)
4. A calibration file edited while a session is open (`elctl laser calibrate` during a UI session): the next `set_tray` uses the new one, and a half-written file is never read (atomic rename). (Tasks 3 and 4 tests.)
5. A hole whose calibrated position is outside the driver's travel: the driver's refusal reaches the script as an error naming the hole, and the stage has not moved. (Task 4 test.)

---

### Task 1: `libs/laser`, `TrayMap`, `TrayLibrary`

**Files:**
- Create: `libs/laser/CMakeLists.txt` (copy the shape of `libs/codecs/CMakeLists.txt`; target `pychron_laser`, alias `pychron::laser`, links `pychron::devices pychron::core`)
- Create: `libs/laser/include/pychron/laser/tray_map.hpp`, `libs/laser/src/tray_map.cpp`
- Create: `tests/laser/CMakeLists.txt` (glob `test_*.cpp`, target `pychron_laser_tests`, define `PYCHRON_TEST_DATA_DIR`), `tests/laser/test_tray_map.cpp`
- Create: `tests/data/tray_maps/221-hole.txt` (a real legacy map; see Step 1), `tests/data/tray_maps/small.txt`
- Modify: `CMakeLists.txt:27` — add `laser` to `PYCHRON_LIBS` after `devices`

**Interfaces:**
- Produces (namespace `pychron::laser`):

```cpp
struct Hole { std::string id; double x = 0, y = 0, dimension = 0; };
enum class HoleShape { Circle, Square };
class TrayMap {
 public:
  static Result<TrayMap> parse(std::string_view text, std::string name);
  static Result<TrayMap> load(const std::filesystem::path& file);   // name = stem
  const std::string& name() const noexcept;
  HoleShape shape() const noexcept;
  double dimension() const noexcept;
  const std::vector<Hole>& holes() const noexcept;                  // file order
  const Hole* find(std::string_view id) const;
  std::optional<std::string> center_hole() const;                   // 5th calibration hole
  std::optional<std::string> right_hole() const;                    // 2nd (east)
  const std::string& sha256() const noexcept;                       // hex, of the bytes given
};
class TrayLibrary {
 public:
  static TrayLibrary load(const std::filesystem::path& dir);        // never fails
  const TrayMap* find(std::string_view name) const;
  std::vector<std::string> names() const;                           // sorted
  const std::vector<std::string>& problems() const noexcept;        // "<file>:<line>: <what>"
};
```

- [ ] **Step 1: Fixtures.** `small.txt`: header `circle,1.0`, an empty valid line, calibration line `2,3,4,5,1`; holes `1,0,0` `2,0,5` `3,5,0` `4,0,-5` `5,-5,0` and one row of each other form (`7,7` → id `6`; `8,8,(1)`; `9,9,r2.5`; `A,1,2,(3)`). `221-hole.txt`: fetch the legacy file (legacy Pychron `setupfiles/tray_maps/221-hole.txt`, from the lab's Drive setupfiles folder in memory `drive-setupfiles-survey`, else the NMGRL pychron GitHub support files). If neither is reachable, ledger a ruling and generate a hexagonal 221-hole map in the legacy format with a script committed under `tools/`.

- [ ] **Step 2: Write the failing tests** in `tests/laser/test_tray_map.cpp`:
  - `TrayMap.ParsesEveryRowForm` — `small.txt`: 9 holes; ids `1..5,6,7,8,A` in order (rows without an id numbered by position); hole `8` has `dimension == 2.5`, the others `1.0`; `shape() == Circle`.
  - `TrayMap.CalibrationHolesComeFromTheHeader` — `center_hole() == "1"`, `right_hole() == "3"`; both `nullopt` with an empty third header line.
  - `TrayMap.CommentsAndBlankLinesAreSkipped`.
  - `TrayMap.WindowsFilesLoadTheSame` — the same text with `\r\n`, a BOM and trailing spaces gives equal holes (ids, x, y); the sha differs.
  - `TrayMap.ABadRowNamesItsLine` — parameterised: `1,x,2`, `1,2`+`1,3` duplicate id, `nan,1`, `1,2,3,4,5`, a file with two header lines; each a Config error whose `what` contains `:<line>`.
  - `TrayMap.ShaIsOfTheBytes` — equals `pychron::sha256(text)` in hex.
  - `TrayMap.TheLegacy221MapLoads` — 221 holes, `find("1")` and `find("221")` non-null, centre and right holes present.
  - `TrayLibrary.LoadsADirectoryAndReportsWhatDidNot` — a temp dir with one good and one bad `.txt` and one `.md`: `names() == {good}`, one problem naming the bad file.

- [ ] **Step 3: Run** `cmake --build build/dev -j8` — Expected: FAIL to compile (`tray_map.hpp` not found).
- [ ] **Step 4: Implement** `TrayMap` and `TrayLibrary`. Lines split on `\n`, a trailing `\r` and surrounding blanks trimmed, a leading BOM dropped; `#` to end of line is a comment only at line start.
- [ ] **Step 5: Run** `ctest --test-dir build/dev -R 'TrayMap|TrayLibrary'` — Expected: PASS.
- [ ] **Step 6: Commit** `laser: tray maps in the legacy format`.

---

### Task 2: `solve` and `Transform`

**Files:**
- Create: `libs/laser/include/pychron/laser/calibration.hpp`, `libs/laser/src/calibration.cpp`
- Test: `tests/laser/test_calibration.cpp`

**Interfaces:**
- Consumes: `TrayMap` (Task 1).
- Produces:

```cpp
struct StageXY { double x = 0, y = 0; };
struct CalibrationPoint { std::string hole; double x = 0, y = 0;
                          friend bool operator==(const CalibrationPoint&, const CalibrationPoint&) = default; };
struct Transform {
  double cx = 0, cy = 0, rotation = 0, scale = 1;        // rotation in radians
  StageXY to_stage(double mx, double my) const noexcept;
  StageXY to_map(double sx, double sy) const noexcept;
};
struct Solution { Transform transform; double rms_mm = 0; std::size_t points = 0; };
inline constexpr double kMaxScaleError = 0.02;
Result<Solution> solve(const TrayMap& map, std::span<const CalibrationPoint> points);
```

- [ ] **Step 1: Write the failing tests** (map = `small.txt`; a helper `at(Transform, hole)` makes exact points):
  - `Solve.NoPointsIsNotCalibrated` — Config error containing "not calibrated".
  - `Solve.OnePointIsAShift` — point hole `1` at (12, 8): `cx==12, cy==8, rotation==0, scale==1, rms==0`.
  - `Solve.TwoPointsGiveShiftAndRotation` — points made with rotation 3° and c=(12,8) on holes `1`,`3`: rotation within 1e-9 rad, scale exactly 1, every hole within 1e-9 mm.
  - `Solve.TwoPointsAtTheWrongDistanceShowInRms` — second point 0.2 mm long: `rms_mm` ≈ 0.2, scale still 1.
  - `Solve.ManyPointsAreFittedWithScale` — 5 points made with rotation −7°, scale 1.01, c=(3,−4), each jittered ≤ 0.01: rotation within 1e-3, scale within 1e-3, `rms_mm < 0.02`, `points == 5`.
  - `Solve.RoundTrips` — `to_map(to_stage(p)) == p` within 1e-12 for rotation 30°, scale 1.015.
  - `Solve.RefusesDegenerateSets` — parameterised: unknown hole; the same hole twice; two points at one stage position; three points whose fitted scale is 1.05 (message contains "scale"); a non-finite coordinate.
  - `Solve.SwappedHolesDoNotPassSilently` — holes `2` and `4` exchanged in a 5-point set: either an error or `rms_mm > 1`.

- [ ] **Step 2: Run** — Expected: FAIL to compile.
- [ ] **Step 3: Implement.** N ≥ 3 is the closed-form 2-D similarity fit: with centred map points `p` and stage points `q`, `a = Σ(p·q)`, `b = Σ(p×q)`, `rotation = atan2(b, a)`, `scale = hypot(a, b) / Σ|p|²`, `c = q̄ − s R p̄`. Two points: the same rotation, `scale = 1`, `c` from the first point.
- [ ] **Step 4: Run** `ctest --test-dir build/dev -R Solve` — Expected: PASS.
- [ ] **Step 5: Commit** `laser: stage calibration from hole points`.

---

### Task 3: `CalibrationStore`

**Files:**
- Create: `libs/laser/include/pychron/laser/calibration_store.hpp`, `libs/laser/src/calibration_store.cpp`
- Test: `tests/laser/test_calibration_store.cpp`

**Interfaces:**
- Consumes: `CalibrationPoint`, `Solution`, `solve`, `TrayMap`.
- Produces:

```cpp
struct StoredCalibration { std::string device, tray, tray_sha256; std::vector<CalibrationPoint> points; };
enum class CalibrationState { Missing, Stale, Unsolvable, Ok };
struct CalibrationStatus { CalibrationState state = CalibrationState::Missing;
                           std::optional<Solution> solution; std::string why; };   // why: empty when Ok
std::string_view to_string(CalibrationState) noexcept;   // "not calibrated", "stale", "unsolvable", "ok"
class CalibrationStore {
 public:
  explicit CalibrationStore(std::filesystem::path dir);
  std::filesystem::path file(std::string_view device, std::string_view tray) const;
  Result<std::optional<StoredCalibration>> load(std::string_view device, std::string_view tray) const;
  Result<void> save(const TrayMap& map, std::string_view device, std::span<const CalibrationPoint> points) const;
  Result<void> clear(std::string_view device, std::string_view tray) const;
  // Never an error: what a move may rely on.
  CalibrationStatus status(const TrayMap& map, std::string_view device) const;
};
```

- [ ] **Step 1: Write the failing tests:**
  - `CalibrationStore.SavesAndLoadsPoints` — round trip of 3 points; the file has `schema_version = 1`, `center`, `rotation_deg`, `scale`, `rms_mm`.
  - `CalibrationStore.MissingIsNotAnError` — `load` → `nullopt`; `status().state == Missing`.
  - `CalibrationStore.AChangedMapMakesItStale` — save against map A, `status` against the same file with one hole moved: `Stale`, no solution, `why` names the tray.
  - `CalibrationStore.TheSolvedValuesInTheFileAreNotTrusted` — hand-edit `rotation_deg` in the file: `status().solution` is unchanged.
  - `CalibrationStore.RefusesUnsafeNames` — device or tray `"../x"`, `"a/b"`, `""`, `"a\\b"`: Config error, nothing written.
  - `CalibrationStore.SaveRefusesPointsThatDoNotSolve` — the file from the earlier save is unchanged byte for byte.
  - `CalibrationStore.SaveIsAtomic` — after `save`, the directory holds exactly the one `.toml` (no temporary left); a pre-existing file is replaced, never truncated in place (write, then `std::filesystem::rename`).
  - `CalibrationStore.ClearDeletesAndIsIdempotent`.
  - `CalibrationStore.AnotherSchemaVersionIsAnError` — `schema_version = 2` → Config error; `status` → `Unsolvable` with the message.

- [ ] **Step 2: Run** — Expected: FAIL to compile.
- [ ] **Step 3: Implement.** File name `<device>.<tray>.toml`; a safe name is non-empty, has no `/`, `\\`, NUL, and is not `.` or `..`. The directory is created on `save`.
- [ ] **Step 4: Run** `ctest --test-dir build/dev -R CalibrationStore` — Expected: PASS.
- [ ] **Step 5: Commit** `laser: calibrations stored per device and tray`.

---

### Task 4: `LaserSystem`; the Chromium driver drops `TrayLookup`

**Files:**
- Create: `libs/laser/include/pychron/laser/laser_system.hpp`, `libs/laser/src/laser_system.cpp`
- Test: `tests/laser/test_laser_system.cpp` (uses `tests/devices/extraction/conformance.hpp`; add `${PROJECT_SOURCE_DIR}/tests/devices` to the test target's include path)
- Modify: `libs/devices/include/pychron/devices/extraction/chromium.hpp`, `libs/devices/src/extraction/chromium.cpp` — remove `TrayLookup`, `set_tray_lookup`, `lookup_`
- Modify: `libs/devices/include/pychron/devices/driver_registry.hpp` — `DriverSchema::extraction_device`
- Modify: `tests/devices/extraction/test_chromium.cpp`, `tests/devices/test_driver_registry.cpp`

**Interfaces:**
- Consumes: `TrayLibrary`, `CalibrationStore::status`, `Transform::to_stage`.
- Produces:

```cpp
// driver_registry.hpp
struct DriverSchema { /* ... */ bool extraction_device = false; };

// laser_system.hpp
class LaserSystem final : public extraction::IExtractionDevice, public extraction::IStage {
 public:
  LaserSystem(std::string name, extraction::IExtractionDevice& driver,
              const TrayLibrary& trays, const CalibrationStore& calibrations);
  std::string tray() const;
  CalibrationStatus calibration() const;       // of the current tray; Missing with no tray
  // IExtractionDevice and IStage as declared in interfaces.hpp
};
```

Chromium after this task: `move_to_position` accepts `s<n>` only (anything else is a Config error naming the position), `set_tray` accepts any name, `positions()` is empty. `ChromiumLaser::schema().extraction_device == true`.

- [ ] **Step 1: Write the failing driver tests.** In `test_chromium.cpp` delete `lookup()` and the hole-by-lookup tests; add `Chromium.AHoleNameIsNotTheDriversToResolve` (`move_to_position("12", false)` → Config error, nothing written to the wire) and `Chromium.SchemaMarksAnExtractionDevice`. In `test_driver_registry.cpp`: `DriverRegistry.OtherKindsAreNotExtractionDevices` (every schema but `chromium` has the flag false).
- [ ] **Step 2: Run** — Expected: FAIL to compile (`extraction_device`).
- [ ] **Step 3: Change the driver and the schema.** Run the devices tests — Expected: PASS.
- [ ] **Step 4: Write the failing facade tests** (harness: `ManualClock`, `ChromiumSim`, hooked `SimTransport`, `ChromiumLaser` with limits ±50, a temp lab dir with `small.txt` and a calibration of rotation 0, c=(10,20)):
  - Instantiate the `ExtractionDevice` and `Stage` conformance suites over a `LaserSystem` (as `test_chromium.cpp` does for the driver).
  - `LaserSystem.AHoleLandsOnItsCalibratedPosition` — `set_tray("small")`, `move_to_position("3", false)`, poll `moving()` to false: sim position (15000, 20000) µm; z as it was.
  - `LaserSystem.SignsAreTheDrivers` — driver `signs = {-1,1,1}`: the wire target x is −15000.
  - `LaserSystem.ScanNamesGoToTheDriver` — `add_scan`, `move_to_position("s1", false)` → the log has `Scans.MoveTo 1`.
  - `LaserSystem.RefusalsSendNothing` — parameterised: no tray set; unknown hole; tray without a calibration; stale calibration (map edited after save). Each: Config error whose `what` names device and tray; `sim.log()` unchanged.
  - `LaserSystem.UnknownTrayIsRefusedAndTheOldOneKept`.
  - `LaserSystem.AnEmptyTrayNameClearsIt`.
  - `LaserSystem.PositionsAreTheTraysHoles` — file order; empty with no tray.
  - `LaserSystem.ACalibrationSavedLaterIsSeenAtTheNextSetTray` — refused, then `store.save`, `set_tray` again, moves.
  - `LaserSystem.AHoleOutsideTravelIsTheDriversRefusal` — calibration c=(49,0), hole `3`: an error whose `what` contains the hole id; `Stage.Pos?` unchanged.
  - `LaserSystem.FeaturesAreTheDrivers` — `laser() == driver.laser()`, `stage()` is the system; over a fake driver with no stage (`tests/devices/extraction/fake.hpp`) `stage() == nullptr`.
  - `LaserSystem.AutocenterIsAcceptedAndChangesNothing`.
- [ ] **Step 5: Run** — Expected: FAIL to compile (`laser_system.hpp`).
- [ ] **Step 6: Implement** `LaserSystem`. The mutex guards `tray_` and the cached status only; the transform is copied out before the driver call. An error from the driver's `set_xy` is returned with `"hole <id> on <tray>: "` prefixed to `what`.
- [ ] **Step 7: Run** `ctest --test-dir build/dev -R 'LaserSystem|Chromium|DriverRegistry'` — Expected: PASS.
- [ ] **Step 8: Commit** `laser: LaserSystem resolves holes; the Chromium driver no longer does`.

---

### Task 5: `Lab` and the queue check

**Files:**
- Modify: `libs/experiment/CMakeLists.txt` (link `pychron::laser`), `libs/experiment/include/pychron/experiment/lab/lab.hpp`, `libs/experiment/src/lab/lab.cpp`
- Test: `tests/experiment/test_lab.cpp`; link the Chromium registration into the test target with `pychron_link_drivers(pychron_experiment_tests pychron_devices)` if the registry lacks `chromium` there

**Interfaces:**
- Consumes: `TrayLibrary`, `CalibrationStore`, `DriverRegistry::instance().schema(kind)->extraction_device`.
- Produces, in `struct Lab`:

```cpp
laser::TrayLibrary trays;                                 // <lab>/tray_maps
std::unique_ptr<laser::CalibrationStore> calibrations;    // <lab>/stage_calibrations; never null
std::vector<std::string> extract_devices;                 // sorted driver names
```

Diagnostics use `field = "extraction"` and the run's row.

- [ ] **Step 1: Write the failing tests** (a temp lab with a line config holding `[drivers.co2] kind = "chromium"` on a sim transport, `tray_maps/small.txt`, a saved calibration for `co2`/`small`):
  - `Lab.ListsExtractionDevicesAndTrays` — `extract_devices == {"co2"}`, `trays.names() == {"small"}`.
  - `Lab.ABadTrayMapIsALabProblem`.
  - `LabCheck.ALaserRunThatCanRunHasNoErrors` — queue `extract_device = "co2"`, `tray = "small"`, a run with hole 3.
  - `LabCheck.UnknownDevice` — message contains `'diode'` and `co2`.
  - `LabCheck.UnknownTray`.
  - `LabCheck.HolesNeedATray` — run with a hole, queue without `tray`.
  - `LabCheck.AHoleNotOnTheTray` — hole 99: message names `99` and `small`.
  - `LabCheck.TrayNotCalibratedForTheDevice` — after `clear`: "not calibrated"; after editing the map: "stale".
  - `LabCheck.TheRunsOwnDeviceWins` — queue device `co2`, run device `diode` → the unknown-device error on that row only.
  - `LabCheck.EachMessageOnce` — three runs with the same fault give one diagnostic.
  - `LabCheck.ALabWithoutDevicesChecksAsBefore` — the example lab's queue with `extract_device = "anything"` and a hole: no new diagnostics.
  - `LabCheck.SkippedRunsAreNotChecked`.
- [ ] **Step 2: Run** — Expected: FAIL to compile (`Lab::trays`).
- [ ] **Step 3: Implement** in `load_lab` and a new static `check_extraction(const Lab&, const QueueSpec&, std::vector<Diagnostic>&)` called from `check_lab_queue`. A run "has an extraction" when `has_extraction` (as `queue_file.cpp` decides it) is true; its device is `extraction.device` or the queue's.
- [ ] **Step 4: Run** `ctest --test-dir build/dev -R 'Lab'` — Expected: PASS.
- [ ] **Step 5: Commit** `experiment: the lab knows its trays, calibrations and extraction devices`.

---

### Task 6: runs bind their device; `LabSession` builds the systems; the example lab

**Files:**
- Modify: `libs/experiment/include/pychron/experiment/run/run.hpp`, `libs/experiment/src/run/run.cpp`
- Modify: `libs/experiment/src/lab/session.cpp`
- Modify: `configs/examples/extraction_line.toml` (add transport `laser_pc` kind `sim` and `[drivers.co2] kind = "chromium"`)
- Create: `configs/examples/tray_maps/example-9.txt`, `configs/examples/stage_calibrations/co2.example-9.toml`, `configs/examples/scripts/extraction/laser_extract.py`, `configs/examples/experiment.laser.toml`
- Test: `tests/experiment/test_run.cpp`, `tests/integration/test_lab_session.cpp`

**Interfaces:**
- Consumes: `LaserSystem`, `Lab::trays`, `Lab::calibrations`, `ExtractionLine::device(name)`, `ExtractionLine::config().drivers`.
- Produces, in `RunServices`:

```cpp
// Extraction device by name; null result: none. line.device, when set, wins.
std::function<extraction::IExtractionDevice*(std::string_view name)> devices;
```

`Run` resolves once in `prepare()` into a member `extraction::IExtractionDevice* device_`, uses it for `env.line.device` and `end_extraction()`, and calls `device_->stage()->set_tray(queue_.tray)` there when both exist and the tray is not empty; a failure of that call fails the run before any script.

`laser_extract.py`:

```python
#! pychron: eqtime=20
def main():
    move_to_position(position)
    enable()
    extract(extract_value, extract_units)
    fire_laser()
    sleep(duration)
    end_extract()
    disable()
```

(If the script vocabulary names differ, use the bound verbs; ledger the ruling.)

- [ ] **Step 1: Write the failing run tests** in `test_run.cpp` with two `FakeExtractionDevice`s from `run_fakes.hpp` (or `tests/devices/extraction/fake.hpp`):
  - `Run.BindsTheDeviceItsSpecNames` — `devices` returns A for "co2", B for "diode"; a run with `extraction.device = "diode"` under a queue device "co2": the script's `extract` reaches B; A is untouched.
  - `Run.SetsTheQueuesTrayBeforeTheScript` — the fake's stage saw `set_tray("small")` before the first script call.
  - `Run.EndsOnlyItsOwnDevice` — after the run B saw `end_extract` and `disable`; A saw nothing.
  - `Run.AnUnknownTrayFailsTheRunBeforeAnyScript`.
  - `Run.ADeviceSetDirectlyWins` — `line.device` set and `devices` set: `line.device` is used.
  - `Run.NoResolverAndNoDeviceIsAsBefore`.
- [ ] **Step 2: Run** — Expected: FAIL to compile (`RunServices::devices`).
- [ ] **Step 3: Implement** the run changes. Run `ctest --test-dir build/dev -R 'Run\.'` — Expected: PASS.
- [ ] **Step 4: Write the example files** and the failing session tests in `test_lab_session.cpp` (the fixture copies `configs/examples` to a temp dir):
  - `LabSessionTest.ALaserQueueMovesFiresAndLeavesTheLaserOff` — runs `experiment.laser.toml` (two unknowns on holes 3 and 7, `extract_device = "co2"`, `tray = "example-9"`, 20 %): both runs succeed; the Chromium simulator (reached through `line.sim()`; add an accessor `sim::SimSystem::chromium(std::string_view transport)` if none exists) logged two stage moves to the calibrated microns, `Laser.Output 20`, `Laser.Fire`; at the end `firing() == false`, `enabled() == false`, `output() == 0`.
  - `LabSessionTest.TwoDevicesInOneQueueEachEndTheirOwn` — a temp line config with drivers `co2` and `diode` (both Chromium on sim transports), run 1 on `co2`, run 2 on `diode`: `co2`'s simulator is off before `diode`'s first command (compare log timestamps or sequence), and both are off at the end.
  - `LabSessionTest.AnUncalibratedTrayIsRefusedAtStart` — `start()` is a Config error naming the tray; nothing reached the simulator.
  - `LabSessionTest.TheExampleQueueStillRuns` (the existing example test, unchanged, still green with the new driver in the line config).
- [ ] **Step 5: Run** — Expected: FAIL (no device bound: the script's `move_to_position` raises "not supported").
- [ ] **Step 6: Implement** in `LabSession::Services`: a `std::map<std::string, std::unique_ptr<laser::LaserSystem>, std::less<>>` built from every `hw.line.config().drivers` name whose `hw.line.device(name)` casts to `extraction::IExtractionDevice`; `s.devices` looks a name up in it. Declared after the things it refers to.
- [ ] **Step 7: Run** the whole suite: `cmake --build build/dev -j8 && ctest --test-dir build/dev -j8` — Expected: PASS, including every existing test that loads `configs/examples/extraction_line.toml` (a count of drivers or transports asserted elsewhere is updated to the new truth, never skipped).
- [ ] **Step 8: Commit** `experiment: runs drive the lab's extraction devices`.

---

### Task 7: `elctl laser`

**Files:**
- Create: `apps/elctl/src/laser.hpp`, `apps/elctl/src/laser.cpp`
- Modify: `apps/elctl/src/cli.cpp` (dispatch `laser`, help text), `apps/elctl/CMakeLists.txt` (link `pychron::laser`)
- Test: `apps/elctl/tests/test_laser_commands.cpp`

**Interfaces:**
- Consumes: `load_lab`, `Lab::trays`, `Lab::calibrations`, `Lab::extract_devices`, `ExtractionLine` (as `exp.cpp` opens it, honouring `--sim`), `LaserSystem`.
- Produces: `int laser_command(const std::vector<std::string>& args, const ExpGlobals& globals, Io io);` Exit codes as the other commands: 0 ok, 1 failed, 2 usage.

```
elctl [-c line.toml] [--sim] laser trays [--lab <dir>]
elctl ... laser calibrate <device> <tray> point <hole> [--x X --y Y] [--lab <dir>]
elctl ... laser calibrate <device> <tray> center|right [--x X --y Y]
elctl ... laser calibrate <device> <tray> show|clear
elctl ... laser goto <device> <tray> <hole> [--timeout <s>]
```

- [ ] **Step 1: Write the failing tests** (`ElctlTest` scratch copy of the examples):
  - `LaserCmd.TraysListsMapsAndCalibrationState` — output has `example-9`, `9 holes`, `co2: calibrated (2 points, rms`.
  - `LaserCmd.PointWithNumbersOpensNoHardware` — without `--sim` and with a line config whose laser transport is an unreachable TCP host: `point 1 --x 10 --y 20` exits 0 and the file has the point.
  - `LaserCmd.PointReadsTheLiveStage` — with `--sim`: the stored point equals the simulator's position (0,0 at start).
  - `LaserCmd.PointReplacesAnEarlierPointOnTheSameHole`.
  - `LaserCmd.CenterAndRightUseTheMapsHoles`; `LaserCmd.CenterWithoutCalibrationHolesIsAnError`.
  - `LaserCmd.APointThatDoesNotSolveLeavesTheFile` — a second point at the first one's stage position: exit 1, file unchanged, stderr says why.
  - `LaserCmd.OnlyOneOfXAndYIsAUsageError` — exit 2.
  - `LaserCmd.ShowPrintsPointsAndSolution`; `LaserCmd.ClearRemovesTheFile`.
  - `LaserCmd.GotoMovesAndReportsTheMiss` — `--sim`: exit 0, output has the hole, the position and `miss 0.000 mm`.
  - `LaserCmd.GotoUncalibratedIsRefused` — exit 1.
  - `LaserCmd.UnknownDeviceNamesTheKnownOnes`.
- [ ] **Step 2: Run** — Expected: FAIL to compile / link (`laser_command`).
- [ ] **Step 3: Implement.** `goto` polls `moving()` every 100 ms of the line's clock until false, `--timeout` (default 60 s) or `interrupt_count() > 0`; on interrupt or timeout it calls the stage's stop where the device offers one (Chromium: a `set_xy` to the current position is not a stop — ledger what the driver offers) and exits 1.
- [ ] **Step 4: Run** `ctest --test-dir build/dev -R LaserCmd` — Expected: PASS.
- [ ] **Step 5: Commit** `elctl: laser trays, calibrate and goto`.

---

### Task 8: Documentation

**Files:**
- Modify: `docs/dev_setup.md` ("A Chromium laser": holes, trays, calibration, the example queue; drop "Nothing uses the device in a queue yet")
- Modify: `configs/examples/laser.chromium.toml.example` (the driver name is the `extract_device`)
- Modify: `docs/superpowers/specs/2026-10-04-laser-system-design.md` (Status: Implemented; anything decided differently, with the reason)
- Modify: `docs/superpowers/specs/2026-10-04-chromium-protocol-survey.md` §7 row `move_to_position(hole)` → the laser system's
- Modify: `libs/experiment/include/pychron/experiment/lab/lab.hpp` header comment (the two new directories)

- [ ] **Step 1:** Write the doc changes: how to calibrate a tray on a real Chromium (jog to the centre hole, `elctl laser calibrate co2 <tray> center`, the same for `right`, `goto` to check), and what is still missing (patterns, autocenter, UI).
- [ ] **Step 2: Run** the full suite — Expected: PASS; `build/dev-ui`: `cmake --build build/dev-ui -j8 && ctest --test-dir build/dev-ui -j8` — Expected: only the four known macOS offscreen failures (`ui.test_theme`, `test_menu_hub`, `test_command_palette`, `test_brand`).
- [ ] **Step 3: Commit** `docs: trays, calibration and the laser queue`.
