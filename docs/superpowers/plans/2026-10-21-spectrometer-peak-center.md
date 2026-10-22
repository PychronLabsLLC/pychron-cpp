# Peak Center from the Spectrometer Window Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let the operator configure, run, watch, cancel and apply a peak center from the spectrometer window, and save the configuration back to a named set in `peak_center.toml`.

**Architecture:** The core runner stays as it is; the window always runs with `update_table = false`, and an explicit Apply writes the table through a helper the runner now shares. `SpectrometerBridge` runs each peak center on its own clock-participant thread and relays the bus events of that run; a new `PeakCenterPanel` widget holds the form and result, docked in `SpectrometerWindow` beside a `PeakCenterView` dock. `LabSession::start` re-reads `peak_center.toml` so a saved set reaches the next queue.

**Tech Stack:** C++20, Qt 6 Widgets, toml++, GoogleTest (core), Qt Test (UI).

**Spec:** `docs/superpowers/specs/2026-10-21-spectrometer-peak-center-design.md`

## Global Constraints

- Errors are values: anything that can fail returns `pychron::Result<T>`; no throwing in `libs/`.
- A thread on a simulated path follows AGENTS.md "Time": `Clock::Hold` before `std::thread`, `Clock::Participant` first on the thread, a done flag set under the mutex and notified through the clock as its last act, and the joiner waits through `clock.wait` before `join()`. Never `Clock::Detached` around that join.
- A `jobs::CancelToken` is on the spectrometer's clock.
- Window runs always pass `update_table = false`; the window never writes the table except through Apply.
- Save goes through `config::set_local_table` (in place); comments and other sets stay byte for byte.
- A set name is a TOML bare key, `[A-Za-z0-9_-]+`.
- Exact UI strings: tooltip `a queue is running`; experiment refusal `peak center running in the spectrometer window`; close prompt `Cancel peak center?`; Apply confirmation `Write <table value> for <isotope> on <detector> to <table>? Propagate: yes/no.`
- Qt on Ubuntu 24.04 is 6.4: newer APIs go behind `QT_VERSION_CHECK`.
- Run `python3 tools/quality_check.py` and `python3 tools/compiler_sweep.py --changed` (`--build-dir build/dev-ui` for UI files) after every C++ task.
- New-thread code (Tasks 4 to 6) is run under `-DPYCHRON_SANITIZE=thread` before landing (Task 10).

## Review Focus

- A `peak_center.toml` with comments and several sets, saved from the window: only the saved set's table changes (Task 6 test `SaveKeepsCommentsAndOtherSets`).
- A set that gives `center` but no isotope: the run succeeds and moves the magnet, but Apply stays disabled because there's no isotope row to write (Task 8 test `applyDisabledWithoutIsotope`).
- The field-table editor saves a new table version between the result and Apply: Apply becomes disabled and doesn't write a stale value (Task 8 test `applyDisabledAfterTableChanged`).
- Cancel pressed while the live scan was running: the scan restarts at its old integration and no table is written (Task 5 test `cancelResumesScan`).
- `peak_center.toml` doesn't parse when the window opens: a banner names the error, the combo offers only the built-in `default`, and Save is disabled so the file isn't overwritten (Task 7 test `unparseableFileDisablesSave`).

---

### Task 1: Public validator and set writer

**Files:**
- Modify: `libs/systems/include/pychron/systems/jobs/peak_center.hpp`
- Modify: `libs/systems/src/jobs/peak_center.cpp` (`validate` in anonymous namespace, lines ~152-162; header comment at ~105 lists only some keys: list every file key)
- Test: `tests/systems/test_jobs_peak_center.cpp`

**Interfaces:**
- Produces:
  - `Result<void> jobs::validate_peak_center_config(const PeakCenterConfig&)`: the existing `validate`, renamed and declared. The parser and `Runner::run` call it.
  - `std::string jobs::format_peak_center_config(const PeakCenterConfig&)`: the table body, keys only, with no header.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST(PeakCenterConfig, FormatRoundTripsEveryKey) {
  PeakCenterConfig c;
  c.name = "tuned"; c.detector = "H1"; c.isotope = "Ar39";
  c.additional_detectors = {"AX", "L1"}; c.center = 5.25;
  c.window = 0.02; c.step = 0.0007; c.percent = 75; c.min_peak_height = 2.5;
  c.test_peak_flat = false; c.n_tries = 3; c.increase = false;
  c.integration = std::chrono::milliseconds(1500); c.settle = std::chrono::milliseconds(250);
  c.baseline_timeout = std::chrono::seconds(7); c.dac_offset = -0.0003;
  c.peak_shift_threshold = 0.01; c.update_table = false; c.propagate = false;
  auto parsed = parse_peak_center_configs("[tuned]\n" + format_peak_center_config(c));
  ASSERT_TRUE(parsed) << parsed.error().what;
  auto back = parsed->at("tuned");
  back.save_table = c.save_table;
  EXPECT_EQ(back, c);
}

TEST(PeakCenterConfig, FormatOfDefaultsRoundTripsAndOmitsCenter) {
  PeakCenterConfig c;
  const auto body = format_peak_center_config(c);
  EXPECT_EQ(body.find("center ="), std::string::npos);
  auto parsed = parse_peak_center_configs("[default]\n" + body);
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed->at("default"), c);
  EXPECT_FALSE(parsed->at("default").center.has_value());
}

TEST(PeakCenterConfig, ValidateIsPublic) {
  PeakCenterConfig c;
  c.step = c.window;
  auto v = validate_peak_center_config(c);
  ASSERT_FALSE(v);
  EXPECT_NE(v.error().what.find("step must be positive and smaller than window"), std::string::npos);
}
```

- [ ] **Step 2: Run, expect a compile failure (undeclared names)**

Run: `cmake --build --preset dev --target pychron_systems_tests`
Expected: error naming `format_peak_center_config` / `validate_peak_center_config`.

- [ ] **Step 3: Implement**

Build a `toml::table` and stream it with toml++ (`TOML_FLOAT_CHARCONV=0` already applies), so floats are the same bytes on every compiler. Durations go out as seconds through `std::chrono::duration<double>`, `direction` as `"increase"`/`"decrease"`, and `center` only when it's set. Integers stay integers (`n_tries`).

- [ ] **Step 4: Run, expect PASS**

Run: `build/dev/tests/systems/pychron_systems_tests --gtest_filter='PeakCenterConfig.*:PeakCenterRun.*'`

- [ ] **Step 5: Commit**

`feat(systems): format a peak center config as peak_center.toml`

---

### Task 2: Shared table write

**Files:**
- Modify: `libs/systems/include/pychron/systems/jobs/peak_center.hpp`, `libs/systems/src/jobs/peak_center.cpp` (`Runner::apply`, ~441-460)
- Test: `tests/systems/test_jobs_peak_center.cpp`

**Interfaces:**
- Produces: `Result<std::vector<std::string>> jobs::write_peak_center_to_table(spectrometer::Spectrometer& spec, const DetectorId& detector, const std::string& isotope, double table_value, bool propagate, bool save)`.
  - Calls `spec.update_table(...)`, then, when `save` is true, `spec.save_table(spec.clock().wall_now())`.
  - A failed save is not an error. It returns the note `field table not saved: <why>` and logs it as the runner does today.
  - The runner's `update_table` branch calls this helper, then `position` as before.

- [ ] **Step 1: Write the failing test**

```cpp
TEST_F(PeakCenterRun, ApplyAfterwardsGivesTheSameTableAsUpdateTable) {
  auto with = config();                      // update_table = true, propagate = true
  auto a = run(with);
  ASSERT_TRUE(a && a->ok && a->table_updated);
  const auto table_a = spec().active_table_copy();   // fixture accessor
  reset_spectrometer();                      // fresh fixture state, same sim sweep
  auto without = config(); without.update_table = false;
  auto b = run(without);
  ASSERT_TRUE(b && b->ok && !b->table_updated && b->table_value);
  auto w = write_peak_center_to_table(spec(), b->detector, b->isotope, *b->table_value, true, true);
  ASSERT_TRUE(w) << w.error().what;
  EXPECT_TRUE(w->empty());
  EXPECT_EQ(spec().active_table_copy(), table_a);
}
```

Use whatever table accessor and reset the `PeakCenterRun` fixture already has. If it has none, add `active_table_copy()` and `reset_spectrometer()` to the fixture class only.

- [ ] **Step 2: Run, expect a compile failure**
- [ ] **Step 3: Implement the helper and make `Runner::apply` call it**
- [ ] **Step 4: Run `--gtest_filter='PeakCenterRun.*'`, expect PASS (existing `CentersUpdatesTheTableAndRepositions` stays green)**
- [ ] **Step 5: Commit** `refactor(systems): one table write for the runner and the window`

---

### Task 3: Re-read sets at queue start

**Files:**
- Modify: `libs/experiment/include/pychron/experiment/lab/lab.hpp`, `libs/experiment/src/lab/lab.cpp` (`check_lab_queue`, peak center ref check ~328-334)
- Modify: `libs/experiment/include/pychron/experiment/lab/session.hpp`, `libs/experiment/src/lab/session.cpp` (`start` ~176, `check` ~239, construction ~61)
- Modify: `libs/experiment/include/pychron/experiment/measurement/adapters.hpp` (`SpectrometerPeakCenter`, ~154), `libs/experiment/src/measurement/adapters.cpp`
- Test: `tests/integration/test_lab_session.cpp`

**Interfaces:**
- Produces:
  - `Result<std::map<std::string, jobs::PeakCenterConfig>> lab::read_peak_center_file(const std::filesystem::path& file)`. A missing file gives an empty map; a parse error gives the parser's message. `load_lab` uses it too.
  - `check_lab_queue(const Lab&, const QueueSpec&, const std::map<std::string, jobs::PeakCenterConfig>* peak_centers = nullptr)`. A null map means `lab.peak_centers`.
  - `void SpectrometerPeakCenter::set_configs(std::map<std::string, jobs::PeakCenterConfig>)`, under `mutex_`. `config_for` reads `configs_` under the same mutex.
  - `LabSession` keeps `std::map<std::string, jobs::PeakCenterConfig> peak_centers_`, seeded from `lab.peak_centers`.
  - `start()` re-reads `lab_.paths.dir / "peak_center.toml"` before `check()`. It refuses when a queue is running, as today, before reading. On error it fails with `ErrorKind::Config` and the parser's message, which names the file. On success it replaces `peak_centers_` and calls `set_configs`.
  - `check()` and `edit()` pass `&peak_centers_`.

- [ ] **Step 1: Write the failing tests** (in `LabSessionTest`; the fixture's lab dir is a temp copy)

```cpp
TEST_F(LabSessionTest, ASetAddedAfterLoadIsUsedAtTheNextStart)
// write lab_dir/peak_center.toml with [tuned] after the session is built;
// queue whose plan's peak_center step names config "tuned";
// EXPECT_TRUE(session.start(queue)); wait(); the run's peak center report
// has config name "tuned" (or: run result's peak_centers[0] config window
// equals the file's window).

TEST_F(LabSessionTest, ABadPeakCenterFileRefusesTheStart)
// write "[x]\nwindow = -1\n"; auto r = session.start(queue);
// ASSERT_FALSE(r); EXPECT_EQ(r.error().kind, ErrorKind::Config);
// EXPECT_NE(r.error().what.find("peak_center.toml"), npos);
// EXPECT_FALSE(session.running());
```

If the example queue has no plan that uses a named peak-center config, write a small plan into the temp lab in the test, the same way neighbouring tests build theirs.

- [ ] **Step 2: Build `pychron_integration_tests`, run `--gtest_filter='LabSessionTest.*'`, expect the two new cases to FAIL**
- [ ] **Step 3: Implement as in Interfaces**
- [ ] **Step 4: Run `LabSessionTest.*` and `build/dev/tests/experiment/pychron_experiment_tests --gtest_filter='*Lab*'`, expect PASS**
- [ ] **Step 5: Commit** `feat(experiment): read peak_center.toml again when a queue starts`

---

### Task 4: Bridge runs a peak center on its own thread

**Files:**
- Modify: `apps/pychron-ui/src/spectrometer_bridge.hpp`, `apps/pychron-ui/src/spectrometer_bridge.cpp`
- Modify: `tests/ui/spectrometer_fixture.hpp` (`SimSpectrometer` takes the clock it runs on)
- Test: `tests/ui/test_spectrometer_bridge.cpp`

**Interfaces:**
- Consumes: `jobs::run_peak_center`, `jobs::PeakCenterOptions{.bus}`, `jobs::CancelToken`, `jobs::Progress`.
- Produces:
  - Methods (main thread):
    - `void peak_center(jobs::PeakCenterConfig config)`: forces `update_table = false`.
    - `void cancel_peak_center()`
    - `bool peak_center_running() const`
  - Signals:
    - `peakCenterTryStarted(const pychron::jobs::PeakCenterTryStarted&)`
    - `peakCenterPoint(const pychron::jobs::PeakCenterPoint&)`: points delivered in order. Batch them like readings when bursts lag.
    - `peakCenterFinished(const pychron::Result<pychron::jobs::PeakCenterResult>&)`
  - Q_DECLARE_METATYPE / `qRegisterMetaType` for the new types, next to the existing ones.
  - Only events published while the bridge's own run is going are relayed. A flag is set on the main thread before the thread starts and cleared when `peakCenterFinished` is delivered.
  - A start while running emits `peakCenterFinished` with `ErrorKind::Config` and the message `a peak center is running`.
  - `SimSpectrometer`: add a `make_sim_spectrometer(const Clock&, ...)`-style option, or a member `std::unique_ptr<Clock>`, so a test can put the spectrometer on a `VirtualClock`. The existing callers keep `SteadyClock`.

- [ ] **Step 1: Write the failing Qt Test slots**

```cpp
void peakCenterRunsAndRelaysItsEvents();
// VirtualClock sim; QSignalSpy on tryStarted/point/finished; bridge.peak_center(cfg)
// with cfg.isotope "Ar40", small window; QVERIFY(spy_finished.wait(10000));
// result ok, center set, table_updated false; tryStarted count >= 1;
// points count > 0 and try_index of points non-decreasing; peak_center_running() false after.

void peakCenterCancel();
// start, wait for first point, cancel_peak_center(); finished carries
// ErrorKind::Cancelled.

void secondPeakCenterRefused();
// start twice; second finished arrives first with Config "a peak center is running";
// the first completes ok.

void destroyDuringPeakCenterReturns();
// start, wait first point, delete bridge; returns within 5 s real time.

void planPeakCenterNotRelayed();
// publish a PeakCenterPoint on sim.bus with no window run going; no peakCenterPoint signal.
```

- [ ] **Step 2: Build `pychron_ui_test_spectrometer_bridge` (preset `dev-ui`), expect a compile failure**
- [ ] **Step 3: Implement**
  - Thread per run, owned by the bridge as `std::thread` plus a done flag, mutex and cv.
  - Start and join follow Global Constraints.
  - `Progress` sink discarded (the bus events carry the progress).
  - `CancelToken` on `spec_.clock()`.
  - Result posted through `Gate`.
  - The destructor cancels and joins before the existing teardown.
- [ ] **Step 4: Run `build/dev-ui/tests/ui/pychron_ui_test_spectrometer_bridge`, expect PASS for all slots (old and new)**
- [ ] **Step 5: Commit** `feat(ui): run a peak center from the spectrometer bridge`

---

### Task 5: Live scan paused around the run

**Files:**
- Modify: `apps/pychron-ui/src/spectrometer_bridge.cpp`
- Test: `tests/ui/test_spectrometer_bridge.cpp`

**Interfaces:**
- Consumes: Task 4's `peak_center`, the existing `start_scan`/`stop_scan`/`stop_requested_`.
- Produces:
  - When `state().scan` is running at `peak_center()`, the scan stops on the executor before the thread starts.
  - After the run, whatever its outcome, `start_scan(<integration it had>)` is issued unless `stop_scan` was called during the run.
  - A non-zero `config.integration` doesn't change the restored integration.

- [ ] **Step 1: Write the failing slots**

```cpp
void peakCenterPausesAndResumesScan();
// start_scan(0.5); wait scanStatus running; peak_center(cfg with integration 1s);
// during run: scanStatus not running; after finished: scanStatus running again,
// integration 0.5.

void cancelResumesScan();          // Review Focus
// as above but cancel mid-run; scan running again at 0.5; table unchanged
// (table_copy(active) equal before/after).

void stopDuringPeakCenterKeepsScanStopped();
// start_scan; peak_center; stop_scan() mid-run; after finished scan not running.
```

- [ ] **Step 2: Run, expect FAIL**
- [ ] **Step 3: Implement**
- [ ] **Step 4: Run the bridge suite, expect PASS**
- [ ] **Step 5: Commit** `feat(ui): pause the live scan while a peak center runs`

---

### Task 6: Bridge applies a result and saves a set

**Files:**
- Modify: `apps/pychron-ui/src/spectrometer_bridge.hpp`, `apps/pychron-ui/src/spectrometer_bridge.cpp`
- Test: `tests/ui/test_spectrometer_bridge.cpp`

**Interfaces:**
- Consumes: `jobs::write_peak_center_to_table` (Task 2), `jobs::format_peak_center_config` (Task 1), `config::set_local_table` (new, `libs/core`), `jobs::parse_peak_center_configs`.
- Produces:
  - Apply, on the executor:
    - `void apply_peak_center(QString detector, QString isotope, double table_value, bool propagate)`
    - Signal `peakCenterApplied(const pychron::Result<void>&)`. A save note isn't a failure: the signal is `Result<void>` OK, and the note is logged at warn and relayed as `peakCenterNote(QString)`.
  - Save, on the executor:
    - `void save_peak_center_set(std::filesystem::path file, QString name, jobs::PeakCenterConfig config)`
    - Signal `peakCenterSetSaved(const QString& name, const pychron::Result<void>&)`
  - A name that isn't `[A-Za-z0-9_-]+` is refused with Config `set name must be letters, digits, _ or -` before anything is written.
  - After the write, the whole file is re-parsed. A parse error reports `saved, but peak_center.toml does not load: <why>`.
  - `bool valid_set_name(const QString&)`: a free function in the bridge header, reused by the panel.

- [ ] **Step 1: Write the failing slots**

```cpp
void applyWritesTheTable();
// run ok without update; apply_peak_center(det, iso, *table_value, true);
// applied ok; fieldTableChanged emitted; table_copy(active) value for iso on det == table_value.

void SaveKeepsCommentsAndOtherSets();   // Review Focus
// temp file: "# keep me\n[default]\nisotope = \"Ar40\"\n\n# other\n[other]\nisotope = \"Ar36\"\nwindow = 0.01\n";
// save "default" with window 0.02; file text still contains "# keep me", "# other",
// and the [other] block unchanged byte for byte; parse gives default.window 0.02.

void saveNewSetAppends();
void saveRefusesBadName();      // "a b" -> Config, file untouched (mtime/text same)
```

- [ ] **Step 2: Run, expect FAIL**
- [ ] **Step 3: Implement**
- [ ] **Step 4: Run, expect PASS**
- [ ] **Step 5: Commit** `feat(ui): apply a peak center and save its set from the bridge`

---

### Task 7: Peak center panel: sets and form

**Files:**
- Create: `apps/pychron-ui/src/peak_center_panel.hpp`, `apps/pychron-ui/src/peak_center_panel.cpp` (add both to `apps/pychron-ui/CMakeLists.txt` next to `peak_center_view`)
- Test: `tests/ui/test_peak_center_panel.cpp`

**Interfaces:**
- Consumes: `SpectrometerBridge` (detectors, `isotopes_for`, `reference_detector`), `jobs::parse_peak_center_configs`, `jobs::validate_peak_center_config`.
- Produces: `class PeakCenterPanel : public QWidget`.
  - Constructor: `PeakCenterPanel(SpectrometerBridge& bridge, QWidget* parent = nullptr)`.
  - `void set_file(const std::filesystem::path&)`: reads the file and fills the combo.
  - `jobs::PeakCenterConfig config() const`: the form as a config.
  - `QString set_name() const`.
  - `bool edited() const`.
  - `void select_set(const QString&)`.
  - `void revert()`.
  - `void set_queue_running(std::function<bool()>)`.
  - Objects tests find by `objectName`:
    - Combo: `peakCenterSet`.
    - One field per key, named `pc_<file key>` (e.g. `pc_window`, `pc_n_tries`, `pc_direction`, `pc_additional_detectors`, `pc_center_enabled` + `pc_center`).
    - Buttons: `peakCenterStart`, `peakCenterCancel`, `peakCenterRevert`, `peakCenterSave`, `peakCenterApply`.
    - Labels: `peakCenterBanner`, `peakCenterReason`.
  - Signal `startRequested(jobs::PeakCenterConfig)` (Task 8 wires it).
  - The built-in `PeakCenterConfig{}` named `default` is offered when the file is missing or has no `[default]`.
  - `update_table` isn't a field; the file's value is kept in `config()`.

- [ ] **Step 1: Write the failing slots**

```cpp
void setLoadsForm();            // file with [tuned] window 0.02, n_tries 3 -> fields show them
void editMarksAndRevert();      // change pc_window -> edited() true, label bold; revert -> false, 0.02
void invalidFormDisablesStartAndSave();  // step >= window -> both disabled, peakCenterReason
                                         // contains "step must be positive and smaller than window"
void missingFileOffersBuiltInDefault();  // combo == {"default"}, config() == PeakCenterConfig{}
void unparseableFileDisablesSave();      // Review Focus: "[x]\nwindow = -1\n" -> banner has the
                                         // parser's message, combo {"default"}, Save disabled, Start enabled
void isotopesFollowDetector();  // pick H1 -> pc_isotope items == bridge.isotopes_for("H1")
void configRoundTrip();         // select set, config() == parsed set (update_table kept)
```

- [ ] **Step 2: Build `pychron_ui_test_peak_center_panel`, expect FAIL**
- [ ] **Step 3: Implement**
- [ ] **Step 4: Run, expect PASS**
- [ ] **Step 5: Commit** `feat(ui): a peak center panel with named sets and overrides`

---

### Task 8: Panel runs, shows the result, applies, saves; window docks

**Files:**
- Modify: `apps/pychron-ui/src/peak_center_panel.hpp/.cpp`
- Modify: `apps/pychron-ui/src/spectrometer_window.hpp`, `apps/pychron-ui/src/spectrometer_window.cpp`
- Test: `tests/ui/test_peak_center_panel.cpp`, `tests/ui/test_spectrometer_window.cpp`

**Interfaces:**
- Consumes: Tasks 4 to 7.
- Produces:
  - Panel behaviour:
    - Start calls `bridge.peak_center(config())`.
    - Cancel calls `cancel_peak_center()`.
    - The result area shows: initial center, fitted center, shift (fitted − initial), table value, additional detectors' centers, notes, and the status message. Objects are named `peakCenterResult*`.
  - Apply is enabled only when all of these hold:
    - The last result is `ok`.
    - It has `table_value`.
    - The isotope isn't empty.
    - No `fieldTableChanged` has arrived since the result.
    - No run is going.
    - No queue is running.
  - Apply asks the exact confirmation from Global Constraints (`QMessageBox`, made answerable in tests through a `set_ask(std::function<bool(QString)>)` like `ExperimentWindow::set_ask_stop`). It's disabled after a successful Apply.
  - Save asks for a name (default: the current set) and asks before overwriting an existing name. Both questions go through the same `set_ask` hook plus `set_ask_name(std::function<std::optional<QString>(QString)>)`. On success it re-reads the file and clears `edited()`.
  - `SpectrometerWindow` changes:
    - Docks: `peakCenterDock` (holds the panel) tabbed with the Controls dock, and `peakCenterPlotDock` (holds a `PeakCenterView`) beside the strip chart. Both appear in the View menu, and their state is saved like the existing docks.
    - The plot is cleared on each Start and fed from `peakCenterTryStarted`, `peakCenterPoint` and `peakCenterFinished`. Only an OK result value goes to `on_done`, wrapped as `PeakCenterDone`.
    - New methods `void set_peak_center_file(const std::filesystem::path&)`, `void set_queue_running(std::function<bool()>)` and `bool peak_center_running() const`.
    - `closeEvent`: with a run going, it asks `Cancel peak center?`. Yes cancels, waits for `peakCenterFinished`, then closes; no ignores the close.

- [ ] **Step 1: Write the failing slots**

```cpp
// test_peak_center_panel.cpp
void runShowsResult();              // start; finished ok; peakCenterResultShift text == fitted - initial (6 dp)
void applyEnabledOnlyAfterOk();     // before run disabled; after ok enabled; after apply disabled
void applyDisabledWithoutIsotope(); // Review Focus: config with center set, isotope "" -> ok result, Apply disabled
void applyDisabledAfterTableChanged(); // Review Focus: ok result, then bridge.replace_table(...) -> Apply disabled
void applyAsksAndWrites();          // ask hook receives the exact text; yes -> peakCenterApplied ok
void saveAsksBeforeOverwrite();     // name "tuned" exists -> ask called; no -> file unchanged
// test_spectrometer_window.cpp
void peakCenterDocksPresent();      // findChild<QDockWidget*>("peakCenterDock"/"peakCenterPlotDock") non-null, in View menu
void closeDuringRunAsks();          // ask returns false -> window still visible, run going; true -> closes after finished
```

- [ ] **Step 2: Run both suites, expect FAIL**
- [ ] **Step 3: Implement**
- [ ] **Step 4: Run both suites, expect PASS**
- [ ] **Step 5: Commit** `feat(ui): run and apply peak centers from the spectrometer window`

---

### Task 9: Exclusion with the queue

**Files:**
- Modify: `apps/pychron-ui/src/experiment_window.hpp`, `apps/pychron-ui/src/experiment_window.cpp` (`start()` ~973)
- Modify: `apps/pychron-ui/src/main_window.cpp` (spectrometer window creation ~309; experiment window creation)
- Test: `tests/ui/test_peak_center_panel.cpp`, `tests/ui/test_experiment_window.cpp`

**Interfaces:**
- Consumes: `SpectrometerWindow::set_queue_running`, `SpectrometerWindow::peak_center_running` (Task 8).
- Produces:
  - `ExperimentWindow::set_peak_center_running(std::function<bool()>)`. While it returns true, `start()` shows `pane_->show_error("peak center running in the spectrometer window")` and returns.
  - `MainWindow` wires:
    - The spectrometer window's `queue_running` to `experiment_ != nullptr && experiment_->running()`, the same lambda as `main_window.cpp:749`.
    - The experiment window's `set_peak_center_running` to `spectrometer_window_ != nullptr && spectrometer_window_->peak_center_running()`.
    - The order of window creation doesn't matter: the lambdas read the members when called.
  - Panel behaviour while `queue_running()`:
    - Start and Apply are disabled, with tooltip `a queue is running`. The state is re-checked on show, on Start, and on each queue start or end signal the window already gets (or a 1 s `QTimer` while visible if there is none).
    - Save stays enabled.

- [ ] **Step 1: Write the failing slots**

```cpp
void queueRunningDisablesStartAndApply();  // set_queue_running([]{return true;}) -> both disabled,
                                           // toolTip == "a queue is running"; Save enabled
void experimentStartRefusedWhilePeakCenterRuns();  // test_experiment_window: set_peak_center_running
                                                   // ([]{return true;}); start(); bridge not running;
                                                   // pane error text == "peak center running in the spectrometer window"
```

- [ ] **Step 2: Run, expect FAIL**
- [ ] **Step 3: Implement**
- [ ] **Step 4: Run both suites, expect PASS**
- [ ] **Step 5: Commit** `feat(ui): a queue and a window peak center never run together`

---

### Task 10: User guide, thread check, full run

**Files:**
- Modify: `docs/user/03-spectrometer.md` ("Peak centring")
- Modify: the spec's Status line (built, plus any departure found while building)

- [ ] **Step 1: Write the guide section**: running from the window, overrides versus sets, Apply and what it writes (Propagate), Save to set, and that plans see a saved set at the next queue start, while a bad file refuses that start.
- [ ] **Step 2: TSan**: configure a separate build directory with `-DPYCHRON_SANITIZE=thread` and UI on. Run `pychron_ui_test_spectrometer_bridge`, `pychron_ui_test_peak_center_panel` and `pychron_integration_tests --gtest_filter='LabSessionTest.*'`. Expected: no ThreadSanitizer report.
- [ ] **Step 3: Static analysis**: `python3 tools/quality_check.py` exits 0, and `python3 tools/compiler_sweep.py --build-dir build/dev-ui` is clean.
- [ ] **Step 4: Full build and tests**: `cmake --build --preset dev-ui` then `ctest --preset dev-ui`. Expected: 100% pass.
- [ ] **Step 5: Commit** `docs: peak center from the spectrometer window in the user guide`

## Open questions

None.
