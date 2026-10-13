# Rise rate calculator in the spectrometer window

Status: owner approved spec, plan and implementation ahead, in task's own
words (T083, inbox 2026-10-13: "this task should do spec, plan and
implementation. I give my approval"). Written and built by an unattended run
2026-10-13 (branch `feat/rise-rate`); choices owner did not make are in
section 9. Built as written, with: detector box follows the Magnet group's
detector whenever that changes and no run is on (4.3 said "starts on").

## 1. Problem

Operator isolates the spectrometer (or a line volume open to it) and wants
to know how fast a signal climbs: static rise of Ar40 for a leak or outgassing
check, before and after a bake, after a filament change. Spectrometer window
shows the strip chart and latest intensities; rate has to be read off the
chart by eye.

## 2. Legacy (examined 2026-10-13, `~/Programming/pychron`)

`spectrometer/jobs/rise_rate.py`, a tab of the scan pane's controls beside
Magnet and Source (`tasks/spectrometer_panes.py:357-386`).

- Start: remembers chart's latest x and reads detector's intensity once;
  black vertical rule on chart.
- Stop: reads intensity again; red rule. Two numbers, both "fA/min":
  - endpoints: `(I_stop - I_start) / minutes`.
  - linear: `polyfit(linspace(0, minutes, n), ys, 1)[0]` over the chart's
    points since start.
- Table of results (N, Endpoints, Linear, Duration (m), Timestamp), Clear
  (the rules), Clear Results.
- Each stop appends every result of the session to `rise_rates.json` in
  appdata.
- Detector: the scan pane's current detector.

Faults, kept out of this design:

- Linear fit assumes evenly spaced samples (`linspace`): wrong when a
  reading is late or missing.
- Fit reads the chart's buffer: a run longer than the chart keeps loses its
  start.
- Endpoints come from two extra instantaneous reads, not from the series
  the fit uses.
- Each save writes the whole session again: results repeated in the file.
- No uncertainty; nothing shown until Stop.

## 3. Goal

In the spectrometer window: pick a detector, Start, watch the rate settle,
Stop; get rise rate by endpoints and by straight-line fit with its
uncertainty, marked on the chart, kept in a table for the session.

## 4. Design

### 4.1 Calculation (pure, `apps/pychron-ui/src/rise_rate.hpp`)

```cpp
struct RisePoint { double t; double v; };            // seconds, detector units
struct RiseRate {
  int n = 0;                       // readings used
  double duration_min = 0;         // last t - first t
  double rise = 0;                 // last v - first v
  double endpoints = 0;            // rise / duration_min
  double linear = 0;               // least-squares slope, units per minute
  std::optional<double> linear_se; // its standard error; n >= 3
};
std::optional<RiseRate> rise_rate(std::span<const RisePoint> points);
```

- Points with non-finite `v` are left out. Fewer than 2 left, or zero
  duration: nullopt.
- `linear`: ordinary least squares of v on the readings' own times (minutes
  from first point). `linear_se = sqrt(SSR / (n - 2) / Sxx)`; absent for
  n = 2.
- `endpoints` from first and last points of the same series.
- Units: detector's units per minute (`fA/min`, `cps/min`).

### 4.2 A run (`RiseRun`, same header)

Accumulates one detector's readings between Start and Stop in its own
buffer (not the chart's ring: a run may outlast the chart's span).

- `start(detector)`; `add(const IntensityReading&)` takes that detector's
  value at the reading's `ts`; `current()` is `rise_rate` of what it holds.
- Reading without that detector, or with no value: skipped.
- Saturated reading: kept, run marked `saturated` (result says so).
- Reading whose `ts` is not after the last kept: run is void (`restarted`):
  the scan began again or time went back. Stop then gives no result and
  says why.
- Time is the readings' `ts` only (the line's clock): nothing reads a clock
  here. Simulated session: rate in simulated minutes.

### 4.3 Panel (`RiseRatePanel`, `apps/pychron-ui/src/rise_rate_panel.{hpp,cpp}`)

Group "Rise rate" in the Controls dock, under Magnet.

- Detector box: every detector; starts on the magnet group's target
  detector. Disabled while a run is on.
- Start / Stop: one button.
- While running, after each reading: `n`, duration, endpoints and linear
  (± 1 se) so far. Operator sees the rate settle and stops when it has.
- Stop: result appended to table: N, Detector (isotope on it at start when
  known), Endpoints, Linear, ± (se), Minutes, n, Time (wall time of stop,
  when the window was given a clock; else blank), note (`saturated`).
  Fewer than 2 readings or a void run: no row, status line says why.
- Table kept for the session (window's lifetime), newest last. Buttons:
  Copy (table as tab-separated text, with header, to clipboard), Clear
  results.
- Numbers shown with 4 significant figures; se with 2.

### 4.4 Chart marks

`StripChartModel::x_of(TimePoint)`: chart x of a reading time (nullopt when
chart is empty). `StripChartView::set_marks(std::vector<ChartMark>)`:
vertical rules at chart x, drawn across the plot; black-ish (theme text)
at start, theme error at stop, as legacy. Marks follow the last run only;
Start clears the previous run's marks; Clear chart or a chart reset clears
them (their x means nothing after). Chart cleared under a run: run goes
on, its stop is not drawn alone.

### 4.5 Wall time

`SpectrometerWindow::set_wall_clock(std::function<QDateTime()>)`; main
window passes `CoreBridge::wall_now`. Not set (tests, a window made
alone): Time column blank. No `QDateTime::currentDateTime()`.

## 5. Rules

- Calculation and run are plain data in, plain data out: tested without a
  window.
- Nothing is sent to the spectrometer; nothing moves; no valve. Panel only
  listens to readings the window already gets.
- No clock read in panel or run; durations from reading timestamps.
- Hidden detector (line not drawn) can still be measured.
- Closing the window, or the banner's Restart, abandons a run; no result,
  status says why.
- Changing integration time, magnet position or scan restart during a run
  is not prevented: operator's business. A restart that sends time back
  voids the run (4.2).

## 6. Out of scope

- Writing results to a file (legacy's `rise_rates.json`). Copy covers
  taking them elsewhere; a store for instrument health readings is its own
  task.
- Rise rate from scripts (`extraction_line_pyscript.py:296-331`, heating
  check by `min_rise_rate`): extraction scripts, not this window.
- Fits other than a straight line; outlier rejection.
- Rise of several detectors at once.
- Leak rate in pressure or mol units (needs sensitivity and volume).

## 7. Files

- `apps/pychron-ui/src/rise_rate.{hpp,cpp}`: 4.1, 4.2.
- `apps/pychron-ui/src/rise_rate_panel.{hpp,cpp}`: 4.3.
- `apps/pychron-ui/src/strip_chart_model.{hpp,cpp}`,
  `strip_chart_view.{hpp,cpp}`: 4.4.
- `apps/pychron-ui/src/spectrometer_window.{hpp,cpp}`, `main_window.cpp`:
  panel in Controls, readings forwarded, marks, wall clock.
- `apps/pychron-ui/CMakeLists.txt`: four new files.
- `docs/user/03-spectrometer.md`: "Rise rate".

## 8. Tests

- `tests/ui/test_rise_rate.cpp`: hand numbers for endpoints, slope and se;
  uneven spacing gives the true slope (legacy's would not); NaN left out;
  1 point and zero duration nullopt; n = 2 has no se. Run: skips other
  detectors and missing values; saturated marked; time going back voids.
  Panel: Start/Stop makes a row with the numbers; live labels move while
  running; too few readings makes no row and says so; void run same; Copy
  text; Clear results; detector box disabled while running; Time blank
  without a clock and the given time with one.
- `tests/ui/test_strip_chart_model.cpp`: `x_of`. `tests/ui`
  (`test_spectrometer_window.cpp`): group is in Controls; a run on the
  simulated spectrometer gives a row and two marks; Clear chart removes
  marks; Start again replaces them.
- Changes `apps/pychron-ui`: final run `dev-ui`.

## 9. Choices made without the owner

1. Results are not written to a file; Copy instead (6).
2. Both rates kept, as legacy: endpoints is what a hand calculation gives,
   linear is steadier.
3. Live numbers while running.
4. One detector a run.
5. Place: a group in Controls, not a dock of its own.
