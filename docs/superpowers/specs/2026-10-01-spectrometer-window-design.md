# Spectrometer window (live strip chart)

Date: 2026-10-01
Status: Draft
Owner: Jake Ross
Depends on: `2026-09-29-spectrometer-control-design.md` (Spectrometer facade,
AcquisitionEngine, `IntensityReading`), `2026-09-29-instrument-control-design.md`
(Qt-free core, CoreBridge pattern), `2026-09-30-implementation-priorities.md`
(item 6, QCustomPlot decision).
Reference: legacy pychron `pychron/spectrometer/scan_manager.py`,
`managers/stream_graph_manager.py`, `spectrometer/tasks/spectrometer_panes.py`.

## 1. Goal

Give `pychron-ui` the legacy pychron "Spectrometer" window: a live scrolling
plot of every detector's intensity against time, an intensities table, and
the controls an operator uses while watching the beam (integration time,
graph settings, which detectors are shown, and positioning the magnet on an
isotope).

Success: with `pychron-ui --sim`, opening Window > Spectrometer shows lines
moving for each sim detector within two integration periods; choosing a
detector and isotope and pressing Apply moves the peak onto that detector and
the trace steps accordingly.

## 2. Scope

In scope (version 1):

- Strip chart with scan width, linear/log Y, autoscale or manual Y limits,
  Clear.
- Intensities table with a rolling standard deviation.
- Controls: integration time, graph group, per-detector show checkbox and
  colour, Magnet (detector + isotope + Apply, current position, large-move
  confirmation).
- Stall banner with Restart.
- Per-user persistence of the window's settings.
- The core pieces the window needs: a continuous scan service, a time-series
  ring buffer, sim bring-up, detector colours.

Out of scope: Record Scan to CSV, markers and event annotations, rise rate,
source panel, readout comparison, mass/DAC scanners, peak-centre and
coincidence tabs, DAC/mass sliders, deflection editing, coordination with
running jobs beyond the `pause`/`resume` hooks, `elctl` commands.

Relationship to planned units: this delivers part of `spec_ui_readout` (live
intensities, integration selector, position by isotope) ahead of its
dependency chain, and the time strip chart is new scope not in the
spectrometer control spec, whose "scan view" is a sweep plot. When this lands,
`spec_ui_readout`'s goal in `tools/spec_router/spec_router/units.toml` is
trimmed to what remains (protect badges, table selector, position by
mass/native, active toggles on hardware).

## 3. Decisions

| Decision | Choice | Reason |
|---|---|---|
| Plotting library | QCustomPlot 2.1.1, `apps/pychron-ui` only | Accepted in the priorities plan (GPLv3-compatible). Two source files, no extra Qt modules beyond Widgets and PrintSupport. Fast enough for 10 Hz x 6 series. This table is the ADR `spec_ui_readout` asks for. |
| Where the scan loop lives | New `ScanService` in core | Nothing manages a free-running `AcquisitionEngine` today; the UI must not own hardware lifecycle. |
| History buffer | `TimeSeriesRing` in `libs/core`, bounded by time | Priority 6 asks for a core ring store; time bounding fixes pychron's point-count limit, which shrinks history at fast integration. |
| Detector checkbox | Hides the line only | Pychron's `active` is a display flag. Hidden series keep buffering; nothing is sent to hardware. |
| Bridge | New `SpectrometerBridge`, separate from `CoreBridge` | `CoreBridge` is bound to `ExtractionLine`; the spectrometer is optional. Same Gate/executor pattern. |
| Window | Separate top-level `QMainWindow` | Matches pychron's separate task window; the canvas stays visible beside it. |
| Settings store | `QSettings` (per user) | Per-operator view state, unlike the lab-wide `canvas.toml` preferences. |

## 4. Core design (Qt-free)

### 4.1 `TimeSeriesRing` (`libs/core`)

`pychron/core/time_series_ring.hpp`. Not thread-safe; owned by one thread.

```cpp
class TimeSeriesRing {
 public:
  struct Point { double t; double value; };
  explicit TimeSeriesRing(double span_s);     // keeps points with t >= newest.t - span_s
  void set_span(double span_s);               // trims immediately
  void push(double t, double value);          // t must be non-decreasing; older t is ignored
  void clear();
  std::size_t size() const;
  const Point& operator[](std::size_t i) const;   // 0 = oldest
  // min/max of values with t in [t0, t1]; nullopt if none (NaN values skipped).
  std::optional<std::pair<double, double>> range(double t0, double t1) const;
};
```

A hard cap of 200 000 points guards against a runaway producer; beyond it the
oldest points are dropped.

### 4.2 `ScanService` (`libs/systems/spectrometer`)

`pychron/systems/spectrometer/scan_service.hpp`. Owns the free-running
lifecycle of `Spectrometer::acquisition()`.

```cpp
struct ScanStatus {             // bus event
  std::string spectrometer;
  bool running = false;
  bool paused = false;
  Duration integration{};       // engine.integration(): snapped once a frame arrives
  std::string error;            // empty when healthy
  TimePoint ts{};
};

class ScanService {
 public:
  ScanService(Spectrometer&, SignalBus&, const Clock&);
  ~ScanService();                                   // stops if it started the engine
  Result<void> start(Duration integration);         // no-op if running at that integration
  void stop();
  Result<void> set_integration(Duration);           // stop + start; publishes ScanStatus
  void pause();                                     // stop the engine, remember integration
  Result<void> resume();
  bool running() const;
  bool paused() const;
  Duration integration() const;
  ScanStatus status() const;
};
```

Behaviour:

- `start` when the engine is already running outside the service (a blocking
  `acquire()` in progress) returns the engine's error unchanged; the service
  stays stopped and publishes a `ScanStatus` carrying the error.
- Every state change publishes `ScanStatus`. The first `IntensityReading`
  after a start publishes one more with the snapped integration.
- The service subscribes to `Alarm` from the engine's stall detector (alarm
  source `"acquisition"`) and republishes a
  `ScanStatus` with `error` set and `running` still true; the engine itself
  is left alone. `start` or `set_integration` clears the error.
- All methods are thread-safe (one mutex) and block only as long as
  `engine.start/stop` do; callers use a worker thread, never the GUI thread.
- Reading data is unchanged: consumers subscribe to `IntensityReading`.

### 4.3 Detector colour

The config-layer `cfg::DetectorConfig` (what `Spectrometer::config()` exposes) gains
`std::string color` (empty = unset). The spectrometer
config loader accepts an optional `color = "#rrggbb"` per `[[detectors]]`
entry and reports a diagnostic for anything that is not `#` plus six hex
digits. The UI falls back to a fixed eight-colour palette by detector index.
The example sim configs get colours.

### 4.4 Sim bring-up helper

`pychron/systems/spectrometer/bringup.hpp` (in `libs/systems`, which already
links `pychron::sim` for the extraction line's sim transports):

```cpp
struct SpectrometerBringup {
  bool sim_beam_from_table = false;   // register a BeamModel that follows the config's field table
};
Result<std::unique_ptr<Spectrometer>> load_spectrometer_for_app(
    const std::filesystem::path& config, SpectrometerContext ctx, SpectrometerBringup options = {});
```

With `sim_beam_from_table`, it builds `BeamSettings::table_value` from the
loaded field table and registers it as the `default` beam model before
assembling, exactly as `tests/integration/test_spectrometer_sim.cpp` does by
hand; that test is refactored to use the helper.

## 5. UI design (`apps/pychron-ui`)

### 5.1 Build

- `cmake/PychronDependencies.cmake`: when `BUILD_UI`, fetch
  `https://www.qcustomplot.com/release/2.1.1/QCustomPlot-source.tar.gz` with
  `URL_HASH SHA256=5e2d22dec779db8f01f357cbdb25e54fbcf971adaee75eae8d7ad2444487182f`
  (the build fails closed on mismatch) and build it as a static `qcustomplot` target with warnings off.
  If the download host is unavailable, the two files are vendored under
  `third_party/qcustomplot/` with their GPL header instead; either way the
  target name is the same.
- `find_package(Qt6 ... Widgets PrintSupport)`; `vcpkg.json` `ui` feature
  gains the print-support feature of `qtbase`.
- `pychron_link_drivers(pychron-ui pychron_sim)` and the same for the UI test
  executables that create a sim spectrometer.

### 5.2 `SpectrometerBridge`

A `QObject` built from `Spectrometer&`, `ScanService&` and the shared
`SignalBus&`. Same rules as `CoreBridge`: bus handlers reach it only through a
Gate and post queued calls; blocking commands run on one executor `QThread`;
no core object holds a `QObject*`.

- Signals: `reading(const IntensityReading&)`, `magnetMoved(const MagnetMoved&)`,
  `detectorChanged(const DetectorState&)`, `scanStatus(const ScanStatus&)`,
  `commandFinished(const QString& what, const Result<void>&)`.
- State mirror (main thread): detector configs and colours, per-detector
  isotope, last `ScanStatus`, last magnet position and mass on reference.
- Commands (non-blocking, result via `commandFinished`): `start_scan()`,
  `stop_scan()`, `set_integration(double seconds)`,
  `position(QString isotope, QString detector)`.
- Readings are coalesced: if several arrive before the GUI thread runs, all
  are delivered in order in one batch, so a slow repaint never backs up the
  bus.

### 5.3 `StripChartModel` (no widgets)

Holds one `TimeSeriesRing` per detector, the time origin, and the view
settings. Plain C++ plus Qt core types so it is testable without a window.

- `append(const IntensityReading&)`: x = seconds from the first reading's
  `ts` since the last clear, shared by every detector in the row. A
  `nullopt` value appends nothing for that detector (the line shows a gap,
  not a zero). Values are used as delivered: the engine has already applied
  `software_gain`.
- Scan width `w` in seconds (UI shows minutes; default 1 min; minimum 1 s).
  Ring span = `1.8 * w`.
- X range: `[0, 1.05 w]` while `x < w`, else `[x - w, x + 0.05 w]`.
- Y range:
  - Autoscale: min/max of visible series over the visible X range, padded by
    10 % of the span; a zero span pads by 1 (or by 10 % of the value). Empty
    data keeps the previous range. Recomputed at most every 0.5 s.
  - Manual: `ymin < ymax` enforced; an invalid edit is rejected and the field
    reverts.
  - Log scale: non-positive values are not plotted; the lower limit is clamped
    to the smallest positive visible value (or 1e-6 if none).
- `clear()`: empties every ring and resets the time origin to the next
  reading.

### 5.4 `SpectrometerWindow`

- Title "Spectrometer" plus " (Simulation)" in sim mode.
- Centre: `StripChartView`, a thin QCustomPlot wrapper that draws the model:
  one graph per detector in its colour, axes labelled "Time (s)" and "Signal",
  no legend (the intensities table is the legend), light-yellow plot
  background as in pychron. Repaints are capped at 20 Hz.
- Left dock "Controls" (not closable):
  - Integration time: combo of presets `0.1, 0.2, 0.5, 1, 2, 5, 10` s, and a
    read-only label with the snapped actual value from `ScanStatus`.
  - Graph: Scan Width (mins), Scale (linear/log), Autoscale Y, Max, Min,
    Clear. Min/Max track the live range while autoscale is on.
  - Detectors: one row per detector with a show checkbox, colour swatch and
    name.
  - Magnet: detector combo, isotope combo (the isotopes the active field
    table defines), Apply button, and read-only "Position" (native value) and
    "Mass on <reference>" from `MagnetMoved`. Changing a combo moves nothing;
    Apply calls `position(isotope, detector)`. While a move is pending Apply
    is disabled.
  - Large-move confirmation: if the mass change on the reference detector
    exceeds `confirm_move_amu` (setting, default 5.0; 0 disables), a Yes/No
    dialog (default No) asks first. Unknown current mass counts as large.
- Right dock "Intensities": table with Colour, Name, Isotope, Intensity
  (`%.5f`), ±1σ (`%.5f`), Units. σ is the population standard deviation of
  the last 10 readings plus the current one, as in pychron. A saturated
  reading shows the intensity cell with a red background and a "saturated"
  tooltip; a detector with no data in the latest row shows an em dash.
- Stall banner: when `ScanStatus.error` is non-empty, a bar across the top
  shows the message and a Restart button (`stop_scan` then `start_scan`).
  A failed command shows its error in the same bar until the next success.
- Lifecycle: opening the window starts the scan at the saved integration (or
  the config's `integration_time_s`); closing it stops the scan and saves
  settings. The window is created once and shown/hidden.

### 5.5 App wiring

- `pychron-ui [extraction_line.toml [canvas.toml]] [--sim] [--spectrometer <file>]`.
  With `--sim` and no `--spectrometer`, the example
  `spectrometer.sim-integrated.toml` is used with `sim_beam_from_table`.
  Without either, no spectrometer is loaded.
- The spectrometer shares the extraction line's clock, scheduler and bus.
  Teardown order: window, bridge, scan service, spectrometer, then the line.
- A spectrometer that fails to load is not fatal: the error goes to the log
  dock and the menu item stays disabled.
- `MainWindow` gains a "Window" menu with "Spectrometer" (Ctrl+Shift+S),
  enabled only when a spectrometer was loaded.

### 5.6 Persistence

`QSettings` group `spectrometer_window/<spectrometer name>`: geometry, dock
state, scan width, scale, autoscale, ymin, ymax, hidden detectors, selected
detector and isotope, integration time, `confirm_move_amu`. Restored on open;
saved on close. Unknown detector or isotope names in saved settings are
ignored. Tests use a temporary settings file.

## 6. Error handling

- Engine stall or start failure: banner, scan keeps its state, Restart.
- `position` failure: banner with `Error.what`; combos keep their selection.
- Reading with all detectors `nullopt`: time still advances; lines show gaps.
- Readings whose `ts` goes backwards (clock reset, new run): the model clears
  and starts a new origin.
- Bridge destruction with commands in flight follows `CoreBridge`: the command
  finishes on the executor; its result is dropped.

## 7. Files

| Path | Change |
|---|---|
| `libs/core/include/pychron/core/time_series_ring.hpp`, `src/time_series_ring.cpp` | New |
| `libs/systems/.../spectrometer/scan_service.{hpp,cpp}` | New |
| `libs/systems/.../spectrometer/config.hpp`, `config_loader.cpp` | `color` field |
| `libs/systems/.../spectrometer/bringup.{hpp,cpp}` | New; integration test refactored onto it |
| `cmake/PychronDependencies.cmake`, `apps/pychron-ui/CMakeLists.txt`, `tests/ui/CMakeLists.txt`, `vcpkg.json` | QCustomPlot, PrintSupport, sim drivers |
| `apps/pychron-ui/src/spectrometer_bridge.*`, `strip_chart_model.*`, `strip_chart_view.*`, `intensities_model.*`, `spectrometer_window.*` | New |
| `apps/pychron-ui/src/main.cpp`, `main_window.*` | `--spectrometer`, Window menu |
| `configs/examples/spectrometer.sim-*.toml` | detector colours |
| `tools/spec_router/spec_router/units.toml` | trim `spec_ui_readout` goal |
| `docs/dev_setup.md` | how to open the window in sim |

## 8. Testing

Core:

- `TimeSeriesRing`: span trimming, `set_span` shrink, out-of-order push
  ignored, `range` over a sub-window, NaN skipped, hard cap.
- `ScanService` with fake acquirers and a `ManualClock` (pattern of
  `tests/systems/test_acquisition.cpp`): start publishes status and readings
  flow; `set_integration` restarts and reports the snapped value; stop;
  pause/resume keeps the integration; start while a blocking acquire holds the
  engine returns the error and publishes it; a stall alarm sets `error`
  without stopping; destructor stops only what it started.
- Detector `color`: parsed, default empty, bad value is a diagnostic with a
  line number.
- Bring-up helper: both sim configs load and a positioned isotope produces
  signal on the chosen detector (the refactored integration test).

UI (headless, sim spectrometer, real clock at 0.1 s integration):

- `StripChartModel`: X range before and after the window fills; ring span
  follows scan width; autoscale padding and throttle; manual limit
  validation; log clamp; hidden series excluded from autoscale but still
  buffered; gap on `nullopt`; clear resets the origin; backwards `ts` resets.
- Intensities model: σ over 11 values matches a hand calculation; saturated
  flag; em dash on missing data.
- `SpectrometerBridge`: readings arrive on the GUI thread in order; a burst
  is delivered as one batch; `set_integration` and `position` report through
  `commandFinished`; destruction with a command in flight is clean.
- `SpectrometerWindow`: opening starts the scan and lines gain points;
  unchecking a detector hides its graph; Apply positions and the Position
  label updates; a large move asks and declining sends nothing; a stall shows
  the banner and Restart clears it; settings round-trip through a temporary
  `QSettings` file; closing stops the scan.
- `MainWindow`: the menu item is disabled without a spectrometer and opens
  the window with one.

Manual check: `pychron-ui --sim`, open the window, confirm moving traces and
a visible step when positioning Ar40 then Ar36 on H1.

## 9. Rollout

1. Core: `TimeSeriesRing`, detector `color`, `ScanService`, bring-up helper.
2. Build: QCustomPlot, PrintSupport, sim driver linking.
3. UI models and bridge (`StripChartModel`, intensities model,
   `SpectrometerBridge`).
4. `StripChartView`, `SpectrometerWindow`, persistence.
5. App wiring (`--spectrometer`, Window menu), docs, units.toml trim.

Each step builds and passes `ctest --preset dev` and `dev-ui` on its own.
