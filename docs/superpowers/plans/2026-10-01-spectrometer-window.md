# Spectrometer Window Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A live spectrometer window in `pychron-ui`: scrolling strip chart of detector intensities, intensities table, and integration/graph/detector/magnet controls, modelled on legacy pychron's Spectrometer window.

**Architecture:** Core gains a time-bounded ring buffer, a `ScanService` that owns the free-running acquisition lifecycle, detector colours, and an app bring-up helper. The UI gains a `SpectrometerBridge` (same Gate/executor pattern as `CoreBridge`), widget-free models, a QCustomPlot view and a separate top-level `SpectrometerWindow` opened from a new Window menu.

**Tech Stack:** C++20, Qt6 Widgets + PrintSupport + Test, QCustomPlot 2.1.1, GoogleTest, existing `Spectrometer` / `AcquisitionEngine` / sim drivers.

**Spec:** `docs/superpowers/specs/2026-10-01-spectrometer-window-design.md`

## Global Constraints

- `libs/*` stays Qt-free. QCustomPlot and every Qt type live only in `apps/pychron-ui` and `tests/ui`.
- QCustomPlot pin: `https://www.qcustomplot.com/release/2.1.1/QCustomPlot-source.tar.gz`, `SHA256=5e2d22dec779db8f01f357cbdb25e54fbcf971adaee75eae8d7ad2444487182f`.
- No core object holds a `QObject*`; bus handlers reach Qt objects only through a Gate and queued calls; blocking spectrometer calls never run on the GUI thread (copy the pattern in `apps/pychron-ui/src/core_bridge.cpp`).
- `IntensityReading` values are already multiplied by `software_gain` (`acquisition.cpp:251`); nothing in this plan applies gain again.
- Spec values: scan width default 1 min, minimum 1 s; ring span `1.8 * w`; X range `[0, 1.05w]` then `[x - w, x + 0.05w]`; autoscale pad 10 %, throttle 0.5 s; σ over the last 10 readings plus the current; integration presets `0.1, 0.2, 0.5, 1, 2, 5, 10` s; `confirm_move_amu` default 5.0 (0 disables); repaint cap 20 Hz; ring hard cap 200 000 points.
- `PYCHRON_WARNINGS_AS_ERRORS=ON`: new targets use `pychron_set_warnings`; the `qcustomplot` target is built with warnings off and included as SYSTEM.
- Namespaces: core `pychron`, spectrometer `pychron::spectrometer`, UI `pychron::ui`. Match surrounding comment density and style.
- Verify with `cmake --build --preset dev --parallel && ctest --preset dev --output-on-failure` (core tasks) and the `dev-ui` preset (UI tasks). UI tests run headless (`QT_QPA_PLATFORM=offscreen`) with a 60 s timeout each, so sim integration is 0.1 s in tests.
- Commits: one per task, repo style (`feat(spectrometer): ...`, `feat(ui): ...`), ending with the session's attribution trailer.

## Review Focus

1. A reading whose detectors are all `nullopt`, or a detector that never reports: lines show gaps, time still advances, the table shows an em dash, nothing divides by zero (Task 6).
2. Log scale with zero or negative intensities (baseline noise goes negative): no crash, no NaN axis range, non-positive points simply not drawn (Tasks 6, 8).
3. Closing the window, or quitting the app, while a `position` or `set_integration` command is in flight: clean shutdown, no use-after-free, scan stopped (Tasks 7, 8, 9).
4. Changing integration time repeatedly and quickly: commands are serialized, the last one wins, the scan ends up running (Tasks 3, 7).
5. Saved settings naming a detector or isotope the current config no longer has, or a corrupt numeric value: ignored, defaults used (Task 8).

---

### Task 1: `TimeSeriesRing`

**Files:**
- Create: `libs/core/include/pychron/core/time_series_ring.hpp`, `libs/core/src/time_series_ring.cpp`
- Modify: `libs/core/CMakeLists.txt`, `tests/core/CMakeLists.txt`
- Test: `tests/core/test_time_series_ring.cpp`

**Interfaces:**
- Produces (namespace `pychron`):
  ```cpp
  class TimeSeriesRing {
   public:
    struct Point { double t; double value; };
    static constexpr std::size_t kMaxPoints = 200000;
    explicit TimeSeriesRing(double span_s);
    double span() const noexcept;
    void set_span(double span_s);            // trims immediately
    void push(double t, double value);       // t < newest t is ignored
    void clear();
    std::size_t size() const noexcept;
    bool empty() const noexcept;
    const Point& operator[](std::size_t i) const;   // 0 = oldest
    const Point& back() const;
    std::optional<std::pair<double, double>> range(double t0, double t1) const;  // min,max; NaN skipped
  };
  ```

- [ ] **Step 1: Write failing tests:** `KeepsPointsWithinSpan` (span 10, push t=0..20 step 1 → oldest t is 10, size 11); `SetSpanShrinkTrimsImmediately`; `OutOfOrderPushIgnored` (push 5 then 3 → size 1); `EqualTimestampsAccepted`; `RangeOverSubWindow` (values 1..10 at t=1..10, `range(3,5)` == {3,5}); `RangeSkipsNaNAndEmptyIsNullopt`; `HardCapDropsOldest` (span 1e12, push `kMaxPoints + 5` → size `kMaxPoints`, oldest t is 5); `ClearEmpties`; `NonPositiveSpanClampsToSmallPositive` (span 0 or negative keeps only the newest point, no crash).
- [ ] **Step 2: Run** `ctest --preset dev -R TimeSeriesRing` → FAIL (missing header).
- [ ] **Step 3: Implement** on `std::deque<Point>`.
- [ ] **Step 4: Run** `ctest --preset dev -R TimeSeriesRing` → PASS.
- [ ] **Step 5: Commit** `feat(core): add time-bounded TimeSeriesRing`.

### Task 2: Detector colour in the spectrometer config

**Files:**
- Modify: `libs/systems/include/pychron/systems/spectrometer/config.hpp` (`cfg::DetectorConfig` gains `std::string color;`, empty = unset), `libs/systems/src/spectrometer/config_loader.cpp` (accept `color` in the `[[detectors]]` key list at ~line 292, validate), `configs/examples/spectrometer.sim-integrated.toml`, `configs/examples/spectrometer.sim-legacy.toml` (one distinct `color = "#rrggbb"` per detector)
- Test: `tests/systems/test_spectrometer_config.cpp` (append)

**Interfaces:**
- Produces: `cfg::DetectorConfig::color` — `""` or exactly `#` + six hex digits (stored lower-case). Read by the UI through `Spectrometer::config().detectors`.
- Note: the spec names the devices-layer `DetectorConfig`; the colour belongs on the config-layer `cfg::DetectorConfig`, which is what `Spectrometer::config()` exposes. The devices struct is unchanged.

- [ ] **Step 1: Write failing tests** using the file's existing string-loading helper: `DetectorColorParsed` (`color = "#1E90ff"` → `"#1e90ff"`); `DetectorColorDefaultsEmpty`; `BadDetectorColorIsDiagnosticWithLine` for `"red"`, `"#12345"`, `"#12345g"`, and a non-string (diagnostic field ends in `.color`, line number of the key); `ExampleConfigsHaveDistinctColors` (load both example files via `cfg::load_spectrometer`, every detector colour non-empty and unique within the file).
- [ ] **Step 2: Run** `ctest --preset dev -R "SpectrometerConfig|DetectorColor"` → FAIL.
- [ ] **Step 3: Implement** with the loader's existing `r_.str` / `r_.require` helpers.
- [ ] **Step 4: Run** `ctest --preset dev` → PASS (all spectrometer tests still green with the edited example files).
- [ ] **Step 5: Commit** `feat(spectrometer): optional detector color in config`.

### Task 3: `ScanService`

**Files:**
- Create: `libs/systems/include/pychron/systems/spectrometer/scan_service.hpp`, `libs/systems/src/spectrometer/scan_service.cpp`
- Modify: `libs/systems/CMakeLists.txt` if sources are listed explicitly
- Test: `tests/systems/test_scan_service.cpp` (globbed)

**Interfaces:**
- Consumes: `Spectrometer::acquisition()` (`AcquisitionEngine&`: `start(Duration)`, `stop()`, `running()`, `integration()`), `Spectrometer::name()`, bus events `IntensityReading` and `Alarm` (engine stall alarms have `source == "acquisition"`).
- Produces (namespace `pychron::spectrometer`):
  ```cpp
  struct ScanStatus {
    std::string spectrometer;
    bool running = false;
    bool paused = false;
    Duration integration{};
    std::string error;       // empty when healthy
    TimePoint ts{};
  };
  class ScanService {
   public:
    ScanService(Spectrometer& spectrometer, SignalBus& bus, const Clock& clock);
    ~ScanService();
    Result<void> start(Duration integration);
    void stop();
    Result<void> set_integration(Duration integration);
    void pause();
    Result<void> resume();
    bool running() const;
    bool paused() const;
    Duration integration() const;
    ScanStatus status() const;
  };
  ```
- Behaviour (spec 4.2): every state change publishes `ScanStatus`; the first `IntensityReading` after a start publishes one more with `engine.integration()` (the snapped value); an `Alarm` with source `"acquisition"` while running publishes a status with `error` set and `running` still true; `start`/`set_integration`/`resume` clear the error; `start` at the current integration while running is a no-op returning success; if `engine.start` fails the service stays stopped, publishes a status with the error text, and returns the engine's error; `stop()` only stops an engine this service started; the destructor calls `stop()`. One mutex; bus publishes happen outside it. `set_integration` while stopped (or paused) records the value for the next `start`/`resume` and returns success; `resume()` without a prior `pause()` is a no-op success.

- [ ] **Step 1: Write failing tests** with the fakes in `tests/systems/spectrometer_fakes.hpp` and the `ManualClock` + `Scheduler{threads = 0}` pump pattern of `tests/systems/test_acquisition.cpp`: `StartPublishesStatusAndReadingsFlow`; `StartTwiceSameIntegrationIsNoop` (one status event); `SetIntegrationRestartsAndReportsSnapped` (fake snaps 0.15 s → 0.2 s; a later status carries 0.2 s); `StopPublishesNotRunning`; `PauseResumeKeepsIntegration`; `ResumeWithoutPauseIsNoop`; `StartWhileEngineBusyReturnsErrorAndPublishesIt` (engine already started directly → `ErrorKind::Config`, `status().running == false`, `error` non-empty); `StallAlarmSetsErrorButKeepsRunning` then `StartClearsError`; `UnrelatedAlarmIgnored` (source `"IG1"`); `DestructorStopsOnlyWhatItStarted` (engine started externally is still running after the service is destroyed); `RapidSetIntegrationLastWins` (Review Focus 4: five calls from two threads; afterwards `running()` and `integration()` equals one of the requested values and matches `engine.integration()`'s request).
- [ ] **Step 2: Run** `ctest --preset dev -R ScanService` → FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `ctest --preset dev -R "ScanService|Acquisition"` → PASS.
- [ ] **Step 5: Commit** `feat(spectrometer): add ScanService for continuous scans`.

### Task 4: App bring-up helper

**Files:**
- Create: `libs/systems/include/pychron/systems/spectrometer/bringup.hpp`, `libs/systems/src/spectrometer/bringup.cpp`
- Modify: `tests/integration/test_spectrometer_sim.cpp` (use the helper's beam-settings function in `SetUp`, keeping its deliberate H1 offset)
- Test: `tests/systems/test_spectrometer_bringup.cpp`

**Interfaces:**
- Consumes: `cfg::load_spectrometer`, `SpectrometerAssembler::assemble`, `sim::BeamModel`, `sim::BeamSettings`, `sim::BeamModelRegistry::global()`, `to_field_table` (as used in the integration test).
- Produces (namespace `pychron::spectrometer`):
  ```cpp
  struct SpectrometerBringup {
    bool sim_beam_from_table = false;
  };
  // BeamSettings whose table_value follows the config's active field table
  // (mass / 8 where the table has no value) and nominal_hv from the config.
  sim::BeamSettings beam_settings_from_config(const cfg::SpectrometerData& data);
  Result<std::unique_ptr<Spectrometer>> load_spectrometer_for_app(
      const std::filesystem::path& config, SpectrometerContext ctx, SpectrometerBringup options = {});
  ```
  With `sim_beam_from_table`, registers a `BeamModel` built from `beam_settings_from_config` as `"default"` before assembling. Errors from loading or assembling are returned unchanged.

- [ ] **Step 1: Write failing tests** (real `SteadyClock`, `Scheduler` with 2 threads started, 0.1 s integration; the test target already links sim drivers): `LoadsBothSimConfigs` (parameterised over `spectrometer.sim-integrated.toml` and `spectrometer.sim-legacy.toml`); `PositionedIsotopeGivesSignalOnChosenDetector` (with `sim_beam_from_table`, `position(Isotope{"Ar40"}, on H1)` then `acquire(3)` → mean on H1 well above the mean after positioning 0.5 native units away); `MissingFileIsError`; `WithoutSimFlagRegistryUntouched` (set a sentinel model as `"default"`, load without the flag, registry still returns the sentinel).
- [ ] **Step 2: Run** `ctest --preset dev -R SpectrometerBringup` → FAIL.
- [ ] **Step 3: Implement** and refactor the integration test's `SetUp` onto `beam_settings_from_config`.
- [ ] **Step 4: Run** `ctest --preset dev -R "SpectrometerBringup|SpectrometerSim"` → PASS.
- [ ] **Step 5: Commit** `feat(spectrometer): app bring-up helper with table-following sim beam`.

### Task 5: UI build dependencies

**Files:**
- Modify: `cmake/PychronDependencies.cmake` (inside `if(BUILD_UI)`: FetchContent the pinned tarball, `add_library(qcustomplot STATIC ...)` with `AUTOMOC ON`, SYSTEM include dir, warnings off, links `Qt6::Widgets Qt6::PrintSupport`), `apps/pychron-ui/CMakeLists.txt` (`find_package(Qt6 REQUIRED COMPONENTS Widgets PrintSupport)`, link `qcustomplot` to `pychron_ui_lib`, `pychron_link_drivers(pychron-ui pychron_sim)`), `tests/ui/CMakeLists.txt` (`pychron_link_drivers(${target} pychron_sim)` for each test), `vcpkg.json` (`ui` feature: add the print-support feature of `qtbase`), `docs/dev_setup.md` (note PrintSupport comes with `brew install qt`)
- Test: `tests/ui/test_plot_build.cpp`

**Interfaces:**
- Produces: CMake target `qcustomplot`; `#include <qcustomplot.h>` works in `apps/pychron-ui` and `tests/ui`; `DriverRegistry::global()` knows the sim spectrometer driver kinds in the app and in UI tests.
- Fallback if the download host is unreachable at configure time: vendor `qcustomplot.h`, `qcustomplot.cpp`, `GPL.txt` from the same tarball (verify the SHA256 first) under `third_party/qcustomplot/` and build the same target from there. Report which path was taken.

- [ ] **Step 1: Write the failing test** `tests/ui/test_plot_build.cpp` (`QTEST_MAIN`): `qcustomplotRenders` (create a `QCustomPlot`, add a graph with 3 points, `replot()`, `toPixmap(200,100)` is non-null); `simSpectrometerDriversRegistered` (`cfg::load_spectrometer(PYCHRON_EXAMPLE_CONFIGS_DIR "/spectrometer.sim-integrated.toml")` then `SpectrometerAssembler::assemble` with a local clock, scheduler and bus succeeds).
- [ ] **Step 2: Run** `cmake --preset dev-ui && cmake --build --preset dev-ui --parallel` → FAIL (no `qcustomplot.h`).
- [ ] **Step 3: Implement** the CMake changes.
- [ ] **Step 4: Run** `ctest --preset dev-ui -R "ui\."` → PASS, and `cmake --preset dev && cmake --build --preset dev --parallel` still configures without Qt or the download.
- [ ] **Step 5: Commit** `build(ui): add QCustomPlot and link sim spectrometer drivers`.

### Task 6: `StripChartModel` and `IntensitiesModel`

**Files:**
- Create: `apps/pychron-ui/src/strip_chart_model.hpp`, `strip_chart_model.cpp`, `intensities_model.hpp`, `intensities_model.cpp`
- Modify: `apps/pychron-ui/CMakeLists.txt` (add to `pychron_ui_lib`)
- Test: `tests/ui/test_strip_chart_model.cpp` (`QTEST_APPLESS_MAIN`)

**Interfaces:**
- Consumes: `TimeSeriesRing` (Task 1), `spectrometer::IntensityReading` / `Reading` / `Value`, `cfg::DetectorConfig` incl. `color` (Task 2).
- Produces (namespace `pychron::ui`):
  ```cpp
  struct DetectorSeries { std::string name; QColor color; QString units; QString isotope; bool visible = true; };
  enum class YScale { Linear, Log };
  struct AxisRange { double lo; double hi; };

  class StripChartModel {
   public:
    static QColor palette_color(std::size_t index);                 // 8 fixed colours, cycled
    explicit StripChartModel(std::vector<DetectorSeries> detectors);
    const std::vector<DetectorSeries>& detectors() const;
    const TimeSeriesRing& series(std::size_t i) const;
    void append(const spectrometer::IntensityReading& r);
    void clear();
    double latest_x() const;                                        // 0 when empty
    void set_scan_width(double seconds);  double scan_width() const;   // min 1.0, default 60.0
    void set_visible(std::size_t i, bool v);
    void set_scale(YScale s);             YScale scale() const;
    void set_autoscale(bool on);          bool autoscale() const;
    bool set_manual_y(double lo, double hi);                        // false and unchanged unless lo < hi (and lo > 0 on Log)
    AxisRange x_range() const;
    AxisRange y_range(TimePoint now);                               // autoscale recomputed at most every 0.5 s
  };

  class IntensitiesModel : public QAbstractTableModel {            // columns: Colour, Name, Isotope, Intensity, ±1σ, Units
   public:
    static constexpr int kWindow = 10;                              // previous readings kept
    explicit IntensitiesModel(std::vector<DetectorSeries> detectors, QObject* parent = nullptr);
    void update(const spectrometer::IntensityReading& r);
    void set_isotope(const std::string& detector, const QString& isotope);
  };
  ```
  `IntensitiesModel` roles: `DisplayRole` text (`%.5f`, em dash `—` when the latest row has no value), `BackgroundRole` = detector colour in column 0 and red in the Intensity cell when saturated, `ToolTipRole` "saturated" on that cell.

- [ ] **Step 1: Write failing tests:**
  - Model: `xIsSecondsSinceFirstReading`; `xRangeBeforeAndAfterWindowFills` (w=60: at x=30 → [0,63]; at x=100 → [40,103]); `ringSpanFollowsScanWidth` (w=10 → points older than 18 s gone); `scanWidthClampedToOneSecond`; `autoscalePadsTenPercent` (values 10..20 → [9,21]); `autoscaleFlatSeriesPads` (all 5.0 → lo < 5 < hi); `autoscaleThrottledToHalfSecond` (new extreme within 0.5 s not reflected, reflected after); `autoscaleIgnoresHiddenSeries`; `hiddenSeriesStillBuffers`; `nulloptLeavesGapAndAdvancesTime` (Review Focus 1); `allNulloptReadingKeepsPreviousYRange`; `manualLimitsRejectInverted`; `logScaleClampsLowerLimit` (values −1, 0, 2, 50 → lo == 2·(1−pad) > 0, no NaN; all non-positive → lo == 1e-6) (Review Focus 2); `clearResetsOrigin`; `backwardsTimestampResets`; `unknownDetectorInReadingIgnored`; `paletteUsedWhenColorUnset`.
  - Intensities: `sigmaOverElevenValuesMatchesHandCalc` (push 1..11 → population σ = 3.16228); `sigmaWithOneValueIsZero`; `saturatedCellRedWithTooltip`; `missingValueShowsEmDash`; `setIsotopeUpdatesColumn`.
- [ ] **Step 2: Run** `ctest --preset dev-ui -R test_strip_chart_model` → FAIL.
- [ ] **Step 3: Implement.** Autoscale range comes from `TimeSeriesRing::range` over `x_range()` across visible series; on Log use only positive values.
- [ ] **Step 4: Run** `ctest --preset dev-ui -R test_strip_chart_model` → PASS.
- [ ] **Step 5: Commit** `feat(ui): strip chart and intensities models`.

### Task 7: `SpectrometerBridge`

**Files:**
- Create: `apps/pychron-ui/src/spectrometer_bridge.hpp`, `spectrometer_bridge.cpp`, `tests/ui/spectrometer_fixture.hpp`
- Modify: `apps/pychron-ui/CMakeLists.txt`
- Test: `tests/ui/test_spectrometer_bridge.cpp`

**Interfaces:**
- Consumes: `ScanService`, `ScanStatus` (Task 3), `load_spectrometer_for_app` (Task 4), `Spectrometer::position(PositionTarget{Isotope{...}, detector}, PositionOptions)`, `Spectrometer::field_table()` (`points()` give `ControlPoint{isotope, mass, values}`), `Spectrometer::config()`, `reference_detector()`, events `IntensityReading`, `MagnetMoved`, `DetectorState`.
- Produces (namespace `pychron::ui`):
  ```cpp
  class SpectrometerBridge : public QObject {
    Q_OBJECT
   public:
    struct State {
      std::optional<double> magnet_native;
      std::optional<double> mass_on_reference;
      std::map<std::string, std::string> isotopes;     // detector -> isotope
      spectrometer::ScanStatus scan;
    };
    SpectrometerBridge(spectrometer::Spectrometer& spec, spectrometer::ScanService& scan, SignalBus& bus,
                       QObject* parent = nullptr);
    ~SpectrometerBridge() override;
    const State& state() const noexcept;
    QString name() const;
    std::vector<DetectorSeries> detectors() const;                  // config order, palette fallback
    QStringList isotopes_for(const QString& detector) const;        // active table points with a value on it
    std::optional<double> mass_of(const QString& isotope) const;
    QString reference_detector() const;
    double default_integration_s() const;                           // config integration_time_s
    void start_scan(double integration_s);
    void stop_scan();
    void set_integration(double seconds);
    void position(const QString& isotope, const QString& detector);
    void drain();                                                   // tests: block until queued commands finish
   signals:
    void readings(const std::vector<pychron::spectrometer::IntensityReading>& batch);
    void magnetMoved(const pychron::spectrometer::MagnetMoved& event);
    void detectorChanged(const pychron::spectrometer::DetectorState& event);
    void scanStatus(const pychron::spectrometer::ScanStatus& status);
    void commandFinished(const QString& what, const pychron::Result<void>& result);   // what: "start", "stop", "integration", "position"
  };
  ```
  Readings are queued under a mutex on the publishing thread and delivered by one pending queued call, so a burst arrives as a single in-order batch. The destructor closes the Gate, drops subscriptions, then quits and waits the executor (in-flight command finishes; its result is dropped).
- `tests/ui/spectrometer_fixture.hpp` produces:
  ```cpp
  struct SimSpectrometer {          // members destroyed in reverse order
    SteadyClock clock; SignalBus bus; Scheduler scheduler;
    std::unique_ptr<spectrometer::Spectrometer> spec;
    std::unique_ptr<spectrometer::ScanService> scan;
  };
  std::unique_ptr<SimSpectrometer> make_sim_spectrometer();   // sim-integrated example, sim_beam_from_table, scheduler started
  ```

- [ ] **Step 1: Write failing tests** (0.1 s integration, `QTRY_*` with 10 s limits): `readingsArriveOnGuiThreadInOrder` (timestamps strictly increasing; receiving thread is the test thread); `burstIsDeliveredAsOneBatch` (publish 50 `IntensityReading`s on the bus from a worker thread without spinning the event loop, then process events → one `readings` emission of size 50); `startAndStopReportThroughCommandFinished` and `scanStatus` mirrors `state().scan`; `setIntegrationReportsSnapped` (request 0.25 → status integration 0.2 or 0.3 per the sim's 100 ms quantum, `commandFinished("integration", ok)`); `rapidIntegrationChangesEndRunning` (Review Focus 4); `positionMovesAndUpdatesState` (`position("Ar40","H1")` → `magnetMoved` once, `state().magnet_native` set, `state().isotopes["H1"] == "Ar40"`); `positionUnknownIsotopeReportsError`; `isotopesForListsTableIsotopes` (contains `Ar40` and `Ar36` for `H1`; empty for an unknown detector); `detectorsCarryConfigColors`; `destroyWithCommandInFlightIsClean` (Review Focus 3: call `position` then destroy the bridge immediately; no crash, fixture teardown clean).
- [ ] **Step 2: Run** `ctest --preset dev-ui -R test_spectrometer_bridge` → FAIL.
- [ ] **Step 3: Implement** following `core_bridge.cpp` (Gate, `relay<E>`, worker `QObject` on `executor_`, `QPointer` for results). Register the signal argument types with `qRegisterMetaType` if the existing bridge does.
- [ ] **Step 4: Run** `ctest --preset dev-ui -R "test_spectrometer_bridge|test_core_bridge"` → PASS; repeat the new suite 5× (`--repeat until-fail:5`).
- [ ] **Step 5: Commit** `feat(ui): SpectrometerBridge`.

### Task 8: `StripChartView` and `SpectrometerWindow`

**Files:**
- Create: `apps/pychron-ui/src/strip_chart_view.hpp`, `strip_chart_view.cpp`, `spectrometer_window.hpp`, `spectrometer_window.cpp`
- Modify: `apps/pychron-ui/CMakeLists.txt`
- Test: `tests/ui/test_spectrometer_window.cpp`

**Interfaces:**
- Consumes: `StripChartModel`, `IntensitiesModel`, `DetectorSeries`, `YScale` (Task 6); `SpectrometerBridge` (Task 7); `qcustomplot` (Task 5).
- Produces (namespace `pychron::ui`):
  ```cpp
  class StripChartView : public QWidget {
   public:
    explicit StripChartView(StripChartModel& model, QWidget* parent = nullptr);
    void refresh();                         // push model data and ranges into the plot; replot capped at 20 Hz
    int graph_count() const;
    int point_count(int graph) const;
    bool graph_visible(int graph) const;
    AxisRange shown_x() const;  AxisRange shown_y() const;
  };

  class SpectrometerWindow : public QMainWindow {
    Q_OBJECT
   public:
    // `settings` defaults to the application's QSettings; tests pass a temp file.
    SpectrometerWindow(SpectrometerBridge& bridge, bool simulation, std::unique_ptr<QSettings> settings = nullptr,
                       QWidget* parent = nullptr);
    StripChartModel& chart_model();
    StripChartView* chart_view() const;
    IntensitiesModel* intensities() const;
    void set_confirm_move(std::function<bool(double delta_amu)> confirm);   // default: Yes/No dialog, default No
    // Programmatic equivalents of the controls, used by tests:
    void set_detector_shown(const QString& detector, bool shown);
    void select_target(const QString& detector, const QString& isotope);
    void apply_position();                  // the Apply button
    void choose_integration(double seconds);
    void set_scan_width_minutes(double minutes);
    void set_scale(YScale scale);  void set_autoscale(bool on);  bool set_manual_y(double lo, double hi);
    void clear_chart();
    QString banner_text() const;            // empty when hidden
    void restart_scan();                    // the banner's Restart button
    QString position_text() const;  QString mass_text() const;  QString actual_integration_text() const;
    bool apply_enabled() const;
   protected:
    void showEvent(QShowEvent*) override;   // start scan at saved or default integration
    void closeEvent(QCloseEvent*) override; // stop scan, save settings
  };
  ```
- Layout, labels and behaviour per spec 5.4; settings keys per spec 5.6 under group `spectrometer_window/<name>`. Plot background `#fafad2`, axis labels "Time (s)" and "Signal", no legend. Title "Spectrometer" or "Spectrometer (Simulation)". Large-move rule: `|mass_of(isotope) referred to the reference detector − state.mass_on_reference| > confirm_move_amu`, unknown current mass counts as large, threshold 0 disables.

- [ ] **Step 1: Write failing tests** (fixture from Task 7; temp-file `QSettings`): `openingStartsScanAndLinesGainPoints` (show → `QTRY_VERIFY(point_count(0) >= 3)`, `graph_count()` equals the detector count, title ends with "(Simulation)"); `uncheckingDetectorHidesGraphButKeepsBuffering`; `scanWidthAndRangesReachThePlot` (`shown_x()` equals the model's range); `logScaleWithNonPositiveDataDoesNotBreakAxis` (Review Focus 2: `shown_y().lo > 0`, finite); `clearEmptiesChart`; `integrationChangeShowsSnappedActual`; `applyPositionsAndUpdatesLabels` (`select_target("H1","Ar40")`, `apply_position()` → `position_text()` non-empty, intensities Isotope column for H1 is `Ar40`, Apply re-enabled); `changingCombosAloneMovesNothing` (no `magnetMoved`); `largeMoveAsksAndDecliningSendsNothing` (position Ar40, then target Ar36 with a confirm callback returning false → callback called once with |Δ| ≈ 4, no further `magnetMoved`); `smallMoveDoesNotAsk`; `thresholdZeroNeverAsks`; `stallShowsBannerAndRestartClearsIt` (publish `Alarm{source "acquisition"}` on the bus → `banner_text()` non-empty; `restart_scan()` → empty); `failedCommandShowsBanner` (unknown isotope); `closingStopsScanAndSavesSettings`; `settingsRoundTrip` (set width 2 min, log, manual Y, hide one detector, target, integration 0.5 → close → new window on the same file restores all); `staleOrCorruptSettingsIgnored` (Review Focus 5: file names detector `ZZ`, isotope `Xx99`, scan width `"abc"`, `ymin > ymax` → defaults, no crash); `closeWithCommandInFlightIsClean` (Review Focus 3).
- [ ] **Step 2: Run** `ctest --preset dev-ui -R test_spectrometer_window` → FAIL.
- [ ] **Step 3: Implement.** A 50 ms `QTimer` drives `StripChartView::refresh()` only when the model changed. Graph data is replaced from the ring on refresh (simple and adequate at these sizes); gaps are produced by inserting a NaN point where a detector missed a row.
- [ ] **Step 4: Run** `ctest --preset dev-ui` → PASS (whole suite), new suite 3× without flakes.
- [ ] **Step 5: Commit** `feat(ui): spectrometer window with live strip chart`.

### Task 9: App wiring, docs and unit trim

**Files:**
- Modify: `apps/pychron-ui/src/main.cpp` (`--spectrometer <file>`; with `--sim` and no file use `PYCHRON_EXAMPLE_CONFIGS_DIR/spectrometer.sim-integrated.toml` with `sim_beam_from_table`; share `line->clock()`, `line->scheduler()`, `line->bus()`; load failure → log dock line, not fatal; teardown order window, bridge, scan service, spectrometer, line), `apps/pychron-ui/src/main_window.hpp`, `main_window.cpp` (Window menu), `docs/dev_setup.md` (how to open the window in sim), `tools/spec_router/spec_router/units.toml` (`spec_ui_readout` goal: remove live intensities, integration selector and position-by-isotope; keep sigma-from-host display, active toggles on hardware, protect badges, stale indicator, position by mass/native, table selector; drop the plotting ADR clause and point at this spec), `docs/superpowers/plans/2026-09-30-implementation-priorities.md` (item 6 status: core ring and strip chart delivered; gauge plot dock remains)
- Test: `tests/ui/test_docks.cpp` (append)

**Interfaces:**
- Consumes: `SpectrometerWindow`, `SpectrometerBridge` (Tasks 7–8), `load_spectrometer_for_app` (Task 4), `ScanService` (Task 3).
- Produces: `void MainWindow::set_spectrometer(SpectrometerBridge* bridge, bool simulation);` (null disables), `QAction* MainWindow::spectrometer_action() const;` (text "Spectrometer", shortcut Ctrl+Shift+S, in menu "Window"), `SpectrometerWindow* MainWindow::spectrometer_window() const;` (created on first trigger, then shown/raised).

- [ ] **Step 1: Write failing tests:** `spectrometerActionDisabledWithoutSpectrometer`; `spectrometerActionOpensWindowOnce` (trigger twice → same window pointer, visible, scan running); `closingMainWindowClosesSpectrometerWindowAndStopsScan` (Review Focus 3).
- [ ] **Step 2: Run** `ctest --preset dev-ui -R test_docks` → FAIL.
- [ ] **Step 3: Implement** and update the docs and `units.toml`; run `tools/spec_router` tests if its venv exists (`tools/spec_router/.venv/bin/python -m pytest -q`), otherwise state that they were not run.
- [ ] **Step 4: Run** `ctest --preset dev --output-on-failure` and `ctest --preset dev-ui --output-on-failure` → PASS. Launch `build/dev-ui/apps/pychron-ui/pychron-ui --sim`, open Window > Spectrometer, confirm moving traces and a step when applying Ar40 then Ar36 on H1; capture a screenshot. Also launch without `--sim` against the example line and confirm the menu item is disabled and nothing else changed.
- [ ] **Step 5: Commit** `feat(ui): open the spectrometer window from pychron-ui`.

---

## Self-review notes

- **Spec coverage:** 4.1 → Task 1; 4.2 → Task 3; 4.3 → Task 2; 4.4 → Task 4; 5.1 → Task 5; 5.2 → Task 7; 5.3 → Task 6; 5.4, 5.6 → Task 8; 5.5 → Task 9; section 6 error cases → Tasks 6–8 tests; rollout order matches spec section 9.
- **Corrections to the spec made here (spec updated in the same commit):** intensities are already gain-corrected by the engine, so the model does not multiply by `software_gain`; the colour field goes on `cfg::DetectorConfig`; stall alarms are identified by `source == "acquisition"`.
- **Task order and dependencies:** 1–5 are independent of each other; 6 needs 1 and 2; 7 needs 3, 4, 5, 6; 8 needs 5–7; 9 needs 8.
