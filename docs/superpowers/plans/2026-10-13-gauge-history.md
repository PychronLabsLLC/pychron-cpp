# Gauge History Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** From the canvas, resting the pointer on a gauge for 3 s shows a small chart of its last 10 minutes, and clicking it opens a window for that gauge with a chart whose range the operator chooses (10 min to 24 h).

**Architecture:** A `ReadingHistory` keeps a time-bounded ring of readings for each named series, fed from the samples the main window's bridge already relays. A `HistoryChart` widget draws one such series over a range. Neither knows what a gauge is, so heaters and the cryostat can use them later. The canvas learns to say that a gauge was clicked or rested on; the main window answers with a `GaugePopup` or a `GaugeWindow`, both of which read the history and hold a `HistoryChart`.

**Tech Stack:** C++20, Qt 6 Widgets, QCustomPlot, `pychron::TimeSeriesRing`, QtTest.

**Spec:** `docs/superpowers/specs/2026-10-13-gauge-history-design.md`, agreed 2026-10-13. Read it first; section numbers below refer to it. Four statements of it are corrected in this branch from the code: see below.

## What the code says

Read from `develop` `3e0f9021`.

- `GaugeLabelItem` (`apps/pychron-ui/src/canvas_items.hpp:218-244`, `.cpp:580-649`) is a `QGraphicsSimpleTextItem` at z 3 with no flags, no hover and no mouse handling. `wired()` says whether the line defines the gauge (set in `canvas_view.cpp:136-144`); a gauge that is only drawn is not wired.
- `GaugeLabelItem` inherits `QGraphicsSimpleTextItem::shape()`, the text's rectangle only, and hover and press are hit-tested against `shape()`, not `boundingRect()`: without a `shape()` of its own the dial and the chip's padding would take neither. Its `refresh()` sets a centring transform (`canvas_items.cpp:658-668`), so `chip_rect()` is in item coordinates: `mapRectToScene` for anything compared with another item.
- A line started by the UI test fixture does not scan (`make_example_line` sets `run_scheduler = false`, `ui_fixture.hpp:24-48`), and the snapshot a start publishes fills the bridge's state without a `pressureSample` (`core_bridge.cpp:242-244`): a test puts its own `PressureSample`s on the bus, with `ts` from `bridge.now()` onward (the existing pressure test's `{}` is the clock's epoch).
- `MainWindow::spectrometer_settings()` makes a new `QSettings` each call (`main_window.cpp:315-317`) from a factory only `set_spectrometer` sets; without it, it is the user's own settings.
- A valve takes its click in the item: `ValveItem::mousePressEvent` (`canvas_items.cpp:213-220`) calls a callback the view set (`canvas_view.cpp:104`), and its hover the same way (`:93-96`, `setAcceptHoverEvents(true)` at `:49`). `CanvasView` has no `signals:` block and overrides no mouse or wheel event; it does not zoom or pan, a wheel scrolls it. It has no edit or read-only mode. (Spec 4.2 and 5 spoke of zoom and of modes: corrected.)
- Gauges are above valves (z 3 over z 2). An item that ignores a press lets it through to what is below; one that accepts it does not. A wired gauge drawn over a valve would take that valve's click: Task 3 tests the example and NMGRL canvases for such an overlap.
- `CoreBridge::pressureSample(const PressureSample&)` (`core_bridge.hpp:110`) arrives on the GUI thread (queued relay, `core_bridge.cpp:40-53`). `PressureSample{gauge, value, units, ts}`; `ts` is a `TimePoint` of the line's clock.
- The scan interval is the line's, not a gauge's: `config().system.scan_interval_ms`, default 1000 (`system_config.hpp:22`, used for every gauge at `extraction_line.cpp:382`). Spec 4.1's "3 scan intervals of that gauge" is 3 of the line's: corrected.
- `GaugeConfig` (`system_config.hpp:138-146`): `name`, `units`, `alarm_high`, `alarm_low`. The line's name is `config().system.name`.
- There is no conversion from a `TimePoint` to wall time. `CoreBridge::wall_now()` (a `QDateTime`) and `now()` (`core_bridge.hpp:75-76`) give it: the wall time of `ts` is `wall_now()` less `now() - ts`. That holds on both clocks (`virtual_clock.cpp:48-50`, `clock.cpp:39`).
- `TimeSeriesRing` (`libs/core/include/pychron/core/time_series_ring.hpp`): `Point{t, value}`, `push(t, value)` drops a point earlier than the last without saying so, stores NaN, `range(t0, t1)` gives min and max skipping NaN, `set_span`, at most 200 000 points; `back()` and `operator[]` do not check. It trims what is older than its span before the newest, and silently drops the oldest beyond 200 000 points. 24 h at 1 s is 86 400 points; a line scanned faster than every 0.43 s holds less than a day (at 100 ms about 5.5 h). The window's "since" is read from what is held, so it stays true.
- The only time-of-day axis in the UI is `scene_view.cpp:241-247` (`QCPAxisTickerDateTime`, `Qt::UTC`, seconds since the epoch). The gauge window shows the operator's local time, as the alarms dock and log do with `wall_now()`.
- `Qt::Popup` takes the click that closes it; the command palette uses it (`command_palette.cpp:88`). The hover popup must not eat a click meant for the canvas, so it is a `Qt::ToolTip` window: no focus, no grab. Such a window still gets mouse events when the pointer is on it, does not hide itself, and on Wayland cannot be placed without a parent window: Task 4 sets `WA_TransparentForMouseEvents` and `WA_ShowWithoutActivating`, parents it to the main window, and hides it when the application or the window is deactivated. A `Qt::Tool` window vanishes on macOS when the application is not active; the laser windows are `Qt::Window` (`main_window.cpp:464-487`) and the gauge window is too.
- No UI timer has an injected delay today; tests wait with `QTRY_*`. The 3 s rest is too long for a test, so the delay is settable.
- The main window opens the spectrometer window on demand and keeps it (`main_window.cpp:251-263`), and keeps laser windows in a map (`:464-487`); `closeEvent` (`:603-614`) closes them; `~MainWindow` (`:506-509`) clears what holds pointers into members. Window settings come from `spectrometer_settings()` (`:315-317`), keyed `spectrometer_window/<name>`.
- `docs/user/02-extraction-line.md:158` says "there is no separate gauge window".
- `apps/pychron-ui/CMakeLists.txt` lists sources and headers by hand (`:18-19`, `:92-93`); `tests/ui/CMakeLists.txt` globs `test_*.cpp`.

## Global Constraints

- Nothing here touches a device or waits on one. History is filled and read on the GUI thread.
- No clock is read for data. A point's time is its sample's `ts`; a wall time shown is `wall_now()` less the age of the point on the line's clock. No `QDateTime::currentDateTime()`, no `steady_clock::now()`.
- The hover delay is the one real-time wait (a `QTimer`): it waits on the operator's hand. It is added to the virtual clock spec's list of what stays on real time.
- A click or a rest on a gauge does nothing else: no actuation, no change of selection. Valve clicks are unchanged.
- Memory is bounded: 24 h a series, whatever the session's length.
- Qt code: a parent owns its child. `const` by default, `[[nodiscard]]` on results that must not be dropped.
- Each task is one commit, Conventional Commits with scope `ui`. Before each: `python3 tools/quality_check.py --build-dir build/dev-ui` (exit 0) and the tests the task names, built with the `dev-ui` preset. Before the branch lands: full `cmake --build --preset dev-ui`, `ctest --preset dev-ui`, `python3 tools/compiler_sweep.py --build-dir build/dev-ui`.

## File Structure

| File | Responsibility |
|---|---|
| `apps/pychron-ui/src/reading_history.hpp`, `.cpp` (new) | `ReadingHistory`: a ring a series, gaps, time going back, units. |
| `apps/pychron-ui/src/history_chart.hpp`, `.cpp` (new) | `HistoryChart`: one series over a range; linear or log; guide lines. |
| `apps/pychron-ui/src/canvas_items.hpp`, `.cpp` | `GaugeLabelItem` takes a click and a rest. |
| `apps/pychron-ui/src/canvas_view.hpp`, `.cpp` | Signals `gauge_clicked`, `gauge_rested`, `gauge_left`; the delay. |
| `apps/pychron-ui/src/gauge_popup.hpp`, `.cpp` (new) | The hover popup. |
| `apps/pychron-ui/src/gauge_window.hpp`, `.cpp` (new) | The window a gauge. |
| `apps/pychron-ui/src/main_window.hpp`, `.cpp` | Owns the history, the popup and the windows. |
| `apps/pychron-ui/CMakeLists.txt` | Eight new files. |
| `tests/ui/test_reading_history.cpp`, `test_history_chart.cpp`, `test_gauge_window.cpp` (new), `test_canvas_view.cpp`, `test_main_window.cpp` (or where the main window is tested) | Tests. |
| `docs/user/02-extraction-line.md`, `docs/superpowers/specs/2026-10-06-virtual-clock-design.md` | The guide; the real-time list. |

---

### Task 1: A history of readings

**Files:** `apps/pychron-ui/src/reading_history.hpp`, `.cpp`, `apps/pychron-ui/CMakeLists.txt`, `tests/ui/test_reading_history.cpp`

```cpp
class ReadingHistory {
 public:
  static constexpr double kSpanSeconds = 24.0 * 3600.0;

  // `gap` is the longest two readings may be apart and still be joined by a line.
  // Only `names` are kept: a reading of any other is ignored.
  ReadingHistory(std::vector<std::string> names, Duration gap);

  void add(const std::string& name, double value, const std::string& units, TimePoint ts);

  struct Series {
    std::vector<TimeSeriesRing::Point> points;  // t: seconds before `newest` (<= 0); NaN value: a break
    QString units;
    std::optional<TimePoint> newest;            // of the series; nullopt when it has none
  };
  // The readings of `name` no older than `range` before its newest one.
  [[nodiscard]] Series series(const std::string& name, Duration range) const;
  [[nodiscard]] std::optional<TimePoint> oldest(const std::string& name) const;
  [[nodiscard]] unsigned revision(const std::string& name) const;  // moves with every change to that series
  [[nodiscard]] bool has(const std::string& name) const;
};
```

Time is kept per series as seconds from that series' first reading, and handed out relative to its newest: a caller never needs the history's origin. Emptying a series (time going back, a change of units) forgets its origin too. A break is a NaN point at the midpoint in time of the two readings it parts. `series()` finds the start of the range by binary search on `t`, not from the front.

- [x] **Failing tests** (QtTest, `QTEST_APPLESS_MAIN`), times built as `TimePoint{} + seconds`:
  - `aSeriesIsKeptForEachName`: two names, readings interleaved; each has its own.
  - `timesAreSecondsBeforeTheNewest`: readings at 100, 160, 220 s: `t` is -120, -60, 0; `newest` is 220 s.
  - `aRangeLeavesOutWhatIsOlder`: range 90 s gives the last two; the reading exactly at the edge is in.
  - `aLongSilenceIsABreak`: gap 3 s; readings at 0, 1, 2, 10, 11: a NaN point between 2 and 10, none elsewhere; readings exactly 3 s apart are joined.
  - `timeGoingBackEmptiesThatSeries`: after 100, 160 a reading at 5: the series is the one reading; the other series is untouched; `revision` moved.
  - `aChangeOfUnitsEmptiesThatSeries`: torr then mbar: one reading, units mbar.
  - `nothingOlderThanADayIsKept`: readings an hour apart over 30 h: `oldest` is no more than 24 h before the newest.
  - `anUnknownNameIsAnEmptySeries`: no points, no `newest`, `has` false, `revision` 0; and `aReadingOfANameNotGivenIsIgnored`: `add` of it changes nothing.
  - `aDayOfSecondsIsReadQuickly`: 86 400 readings; `series(name, 10 min)` and `series(name, 24 h)` each a thousand times; print the time (no assertion on it: the number goes in the commit message, and decides whether the 24 h chart needs thinning).
  - `aValueThatIsNoNumberIsABreak`: a NaN or infinite value is stored as a break, not drawn.
  - `revisionMovesWithEachReading`.
- [x] **Implement** over `TimeSeriesRing(kSpanSeconds)`; check `size()` before `back()`.
- [x] `cmake --build --preset dev-ui --target pychron_ui_test_reading_history && ctest --preset dev-ui -R '^ui\.test_reading_history$'`; commit `feat(ui): a history of timed readings, a day long, a series a name`.

### Task 2: A chart of one series

**Files:** `apps/pychron-ui/src/history_chart.hpp`, `.cpp`, `apps/pychron-ui/CMakeLists.txt`, `tests/ui/test_history_chart.cpp`

```cpp
class HistoryChart : public QWidget {
 public:
  explicit HistoryChart(QWidget* parent = nullptr);

  // `newest_wall`: the wall time of t = 0, for the axis labels; invalid: the
  // axis is labelled in minutes before the newest reading instead.
  void set_series(const ReadingHistory::Series& readings, std::chrono::seconds range, const QDateTime& newest_wall);
  void set_log(bool log);
  void set_guides(std::vector<double> y);   // horizontal rules (alarm limits); drawn only when inside the view
  void set_compact(bool compact);           // the popup: no axis titles, fewer ticks

  struct Summary { int points = 0; double latest = 0, lowest = 0, highest = 0; };
  [[nodiscard]] std::optional<Summary> summary() const;   // nullopt with no reading in the range

  // For tests.
  [[nodiscard]] int point_count() const;        // drawn, breaks not counted
  [[nodiscard]] AxisRange shown_x() const;      // seconds since the epoch when given a wall time, else seconds before newest
  [[nodiscard]] AxisRange shown_y() const;
  [[nodiscard]] bool log() const;
  [[nodiscard]] int guide_count() const;        // drawn
  [[nodiscard]] QString x_label_at(double x) const;
};
```

- [x] **Failing tests** (`QTEST_MAIN`):
  - `drawsThePointsOfTheRange`: three points and a break: `point_count()` 3, the line broken (two segments: test through the graph's data having a NaN at the break).
  - `theXAxisIsTheRangeEndingAtTheNewest`: range 600 s, newest wall 2026-10-13 08:30:00 local: `shown_x()` is that instant less 600 s to that instant, and `x_label_at` of the right edge reads `08:30`. A range of 6 h or more labels with the date too.
  - `withoutAWallTimeTheAxisIsMinutesBeforeNow`: `shown_x()` is -600 to 0.
  - `yFitsWhatIsShownWithAMargin`: values 2 and 4: the range holds both with room; one value alone still gives a range (not zero height).
  - `logScaleNeverTakesANonPositiveRange`: values 1e-9 and 1e-7 on log; then a zero among them: the zero is a break on log and a point on linear (as `StripChartView` does where it fills its graphs, `log && !(value > 0)`). `HistoryChart` has no floor of its own on a log axis (`StripChartView`'s `kLogFloor` of 1e-6 would hide a pressure), and its margin there is a factor, not a difference.
  - `linearIsTheDefault`.
  - `guidesInsideTheViewAreDrawnAndOthersAreNot`.
  - `anEmptySeriesHasNoSummaryAndSaysSo`: a centred text "No readings" in place of the plot; `summary()` nullopt.
  - `theSummaryIsOfTheRangeShown`: latest, lowest, highest, points.
- [x] **Implement** as `StripChartView` sets its plot up (`strip_chart_view.cpp:39-62`: `theme().plot_bg`, `phFastPolylines`), one graph in `theme().series[0]`, `QCPAxisTickerDateTime` with `Qt::LocalTime` when given a wall time (`hh:mm` under 6 h, `MM-dd hh:mm` from 6 h), guides as `QCPItemStraightLine` in `theme().error`.
- [x] `ctest --preset dev-ui -R '^ui\.test_history_chart$'`; commit `feat(ui): a chart of one series of readings over a chosen range`.

### Task 3: The canvas says when a gauge is clicked or rested on

**Files:** `apps/pychron-ui/src/canvas_items.hpp`, `.cpp`, `apps/pychron-ui/src/canvas_view.hpp`, `.cpp`, `tests/ui/test_canvas_view.cpp`

```cpp
// CanvasView
 signals:
  void gauge_clicked(const QString& name);
  void gauge_rested(const QString& name, const QRect& global_chip);  // the pointer stayed on it for the delay
  void gauge_left();                                                 // it moved off, the view scrolled, or a button went down
 public:
  static constexpr int kGaugeRestMs = 3000;
  void set_gauge_rest_ms(int ms);   // tests: 0
```

- [x] **Failing tests**, in the fixture of `test_canvas_view.cpp` (`:58-65`), clicking as `clickingAValveActuatesIt` does (`:122-133`: `view_->mapFromScene(item->scenePos())`, `QTest::mouseClick(view_->viewport(), ...)`):
  - `clickingAGaugeSaysWhich`: `gauge_clicked("IG1")` once (`QSignalSpy`); no valve changed state; nothing was sent to the line. Also with the click on the chip's dial and on its padding, not only on the text.
  - `clickingAGaugeTheLineDoesNotDefineSaysNothing` (the decoration gauge of `:836-852`).
  - `aRightClickOnAGaugeSaysNothing`.
  - `restingOnAGaugeSaysSoAfterTheDelay`: delay 0: the view shown and exposed (as the click test does), `QTest::mouseMove` to a point off the chip and then onto it, `QTRY_COMPARE(rested.count(), 1)` with the gauge's name and the chip's rectangle on the screen. No test here uses `QTest::mouseMove` for a hover yet (the valve's hover test sends the scene event itself, `:200-203`): try it on the offscreen platform first, and if the move does not reach the item, send `QGraphicsSceneHoverEvent`s as that test does and say so in the commit.
  - `leavingBeforeTheDelayCancelsIt`: delay 200 ms: move on, move off at once, wait 400 ms: no `gauge_rested`. (200 and 400: the one real wait here; widen both if CI shows it close.)
  - `leavingAfterARestSaysSo`: `gauge_left` once; pressing a button while rested, a wheel over the view (whether or not the canvas has anything to scroll) and a change of a scroll bar's value say it too.
  - `restingOnADecorationGaugeSaysNothing`.
  - `aValveUnderAGaugeChipStillTakesItsClick`: for the example canvas and `configs/examples/nmgrl/canvas.toml`, no wired gauge's chip (`mapRectToScene(chip_rect())`) covers a valve's centre. If one does, that is reported to the owner as a fault of the canvas file, not worked round.
  - `clickingAValveIsAsBefore` (the existing test passes unchanged).
- [x] **Implement:** `GaugeLabelItem` gets `set_on_click`, `set_on_hover(std::function<void(bool entered)>)`, `setAcceptHoverEvents(true)`, a pointing-hand cursor when wired, a `shape()` that is the chip (dial and padding included), `mousePressEvent` (left button, wired: callback and accept; else ignore), `hoverEnterEvent` / `hoverLeaveEvent`. `CanvasView` gets the `signals:` block, a single-shot `QTimer` started on enter and stopped on leave, and emits `gauge_left` from leave, from `scrollContentsBy`, and from a press or a wheel anywhere on the viewport (an event filter that consumes nothing).
- [x] `ctest --preset dev-ui -R '^ui\.test_canvas_view$'`; commit `feat(ui): the canvas says when a gauge is clicked or rested on`.

  Built: `QTest::mouseMove` reaches the item on the offscreen platform, so the hover tests move the pointer. `aValveUnderAGaugeChipStillTakesItsClick` reads each canvas and its own `extraction_line.toml` and measures a chip showing a reading, instead of loading a line: the example's line cannot load NMGRL's canvas. NMGRL's five gauges are drawn for illustration and none is wired, so there it holds with nothing to check; an unwired chip lets the click through. `gauge_left` is said only after a `gauge_rested`. The second press of a double click on a wired gauge is a click too.

### Task 4: The hover popup

**Files:** `apps/pychron-ui/src/gauge_popup.hpp`, `.cpp`, `apps/pychron-ui/CMakeLists.txt`, `tests/ui/test_gauge_window.cpp`

```cpp
class GaugePopup : public QFrame {      // Qt::ToolTip: no focus, takes no click
 public:
  static constexpr std::chrono::seconds kRange{600};
  GaugePopup(const ReadingHistory& history, std::function<QDateTime(TimePoint)> wall_of, QWidget* parent);  // parent: the main window
  void show_for(const QString& name, const QRect& global_chip);    // beside the chip, clear of it, inside the screen
  void refresh();                                                   // called when that gauge has a new reading
  [[nodiscard]] QString gauge() const;
  [[nodiscard]] QString heading() const;       // "IG1  1.20e-08 torr"
  [[nodiscard]] QString message() const;       // empty when the chart is shown
  [[nodiscard]] HistoryChart* chart() const;
};
```

- [x] **Failing tests:** with two or more readings in the last 10 minutes: heading has name, latest value as the chip writes it (`'e', 2`) and units; the chart has the points; `message()` empty. With one reading: no chart, `message()` "No readings yet". With readings, all older than 10 minutes: "No readings in the last 10 minutes". `refresh()` after a new reading adds a point; `refresh()` when hidden does nothing. Size about 320 x 180. The window flags are `Qt::ToolTip` and it does not take focus when shown (`QApplication::focusWidget()` unchanged). `show_for` near the right or bottom edge of the screen keeps the popup inside it, and it never covers the chip it is for. It has `WA_TransparentForMouseEvents` and `WA_ShowWithoutActivating`. It hides on `hide()` from its owner and by itself when the application is deactivated.
- [x] **Implement.** The chart is compact and linear (spec 4.4). `wall_of` is called once a refresh, for the newest reading.
- [x] `cmake --build --preset dev-ui --target pychron_ui_test_gauge_window && ctest --preset dev-ui -R '^ui\.test_gauge_window$'`.
- [x] Commit `feat(ui): a popup of a gauge's last ten minutes`.

  Built: the constructor also takes `std::function<TimePoint()> now`, the line's time (`CoreBridge::now()`; Task 6 passes it): "all older than 10 minutes" is measured from now, and `ReadingHistory::series` measures from the series' newest reading, so without it a gauge that stopped reporting would be drawn as current. `gauge()` returns a `const QString&`. Placement is a static `placed(chip, size, screen)`, tested on its own: right of the chip, else left, else below, else above. `wall_of` is called once a refresh when the chart is shown, and not at all when a message is.

### Task 5: The gauge window

**Files:** `apps/pychron-ui/src/gauge_window.hpp`, `.cpp`, `apps/pychron-ui/CMakeLists.txt`, `tests/ui/test_gauge_window.cpp`

```cpp
class GaugeWindow : public QWidget {    // a `Qt::Window` of its own; hidden on close, kept by the main window
  Q_OBJECT
 public:
  struct Deps {
    const ReadingHistory* history = nullptr;
    std::function<QDateTime(TimePoint)> wall_of;     // null: no wall labels
    std::optional<double> alarm_high, alarm_low;
    std::function<std::unique_ptr<QSettings>()> settings;   // null: nothing kept; opened for each read and write
    QString line;                                    // settings key
  };
  GaugeWindow(QString name, Deps deps, QWidget* parent = nullptr);

  void refresh();                         // new reading: at most 4 redraws a second
  static const std::vector<std::chrono::seconds>& ranges();   // 600, 1800, 3600, 21600, 86400
  // For tests.
  [[nodiscard]] QComboBox* range_box() const;
  [[nodiscard]] QCheckBox* log_box() const;
  [[nodiscard]] HistoryChart* chart() const;
  [[nodiscard]] QString header() const;   // "1.20e-08 torr   min 1.1e-08   max 1.4e-08   since 14:02"
  [[nodiscard]] QString copy_text() const;
};
```

- [x] **Failing tests:**
  - `opensOnTenMinutesAndLinear`.
  - `theRangeChosenIsKeptForThatGauge`: choose 1 h on IG1; a new `GaugeWindow("IG1")` on the same settings opens on 1 h; `GaugeWindow("CG1")` on 10 min. Key `gauge_window/<line>/<gauge>/range_s`; a value that is none of the ranges falls back to 10 min. The same for `log` (`.../log`) and the window's geometry (`.../geometry`).
  - `pointsOutsideTheRangeAreNotDrawn`.
  - `theHeaderGivesLatestMinAndMaxOfTheRange`, numbers as the chip writes them.
  - `aRangeLongerThanTheHistorySaysSinceWhen`: 20 minutes of history, range 1 h: header ends `since hh:mm` of the oldest reading's wall time; with the range inside the history there is no `since`.
  - `alarmLimitsAreGuides`: both set and inside the view: two guides; neither set: none.
  - `logAndLinear`: the box switches the chart.
  - `copyGivesTimeAndValue`: a header line and a line a point, tab separated, time as `yyyy-MM-dd HH:mm:ss`, value in full precision; breaks left out. `wall_of` is called once, for the newest reading, and each point's time is that plus its `t`: two calls can straddle a jump of a simulated clock, and the lines would disagree.
  - `aNewReadingIsDrawn`: `refresh()` after `add`: one more point, at once when the window has not drawn in the last 250 ms. Many `refresh()` calls in one event-loop turn redraw once more, 250 ms later (`QTRY_*`) (count through the history's `revision` the window last drew, and a redraw counter).
  - `withoutAWallClockTheAxisIsMinutesBeforeNow` and Copy's time column is seconds before the newest.
  - `theTitleIsTheGaugesName`.
- [x] **Implement.** Redraws are throttled as `StripChartView` throttles its own (draw at once when the last was long enough ago, else a single-shot timer for the rest), 250 ms. `Deps` is filled field by field where it is made, not with a designated initializer that leaves fields out (clang 18).
- [x] `ctest --preset dev-ui -R '^ui\.test_gauge_window$'`; commit `feat(ui): a window of a gauge's history, with its range and scale`.

  Built: `range()`, `gauge()`, and for tests `redraw_count()` and `drawn_revision()`, `kMinRedrawMs` (250) and `kDefaultRange`. A `refresh()` while the window is hidden draws nothing and marks it stale; it draws when shown again (a closed window is kept by the main window, and redrawing a day of points four times a second for nobody is waste). The redraw cap is measured from the end of a drawing. Geometry is saved on every hide, which a close is too. Header numbers are the chip's (`'e', 2`): `1.20e-08 torr   min 1.10e-08   max 1.40e-08   since 09:40`; with no reading, "No readings". The `since` time is the newest reading's wall time less the age of the oldest held, so `wall_of` is still asked once a drawing; without a wall clock it reads `since 20 min before newest`. Copy's header line is `time` (or `seconds before newest`) and `value (<units>)`; a value is the shortest text that reads back as the same double (`QLocale::FloatingPointShortest`), and every finite reading of the range is in it, a non-positive one on a log scale too.

### Task 6: The main window ties them together

**Files:** `apps/pychron-ui/src/main_window.hpp`, `.cpp`, `tests/ui/test_docks.cpp` (it makes a `MainWindow` and gives it a settings factory on a temporary file, `:539`)

- [x] **Failing tests** on the example line (`ui::test::make_example_line`, `ui_fixture.hpp:24`), each giving the window a settings factory on a temporary file first (no test may write the user's settings), and each publishing its own `PressureSample`s with `ts` from `bridge.now()` on:
  - `readingsGoIntoTheHistory`: a `PressureSample` for IG1 on the bus: `QTRY_VERIFY(window.reading_history().has("IG1"))`; one for a gauge the line does not define is ignored. (The values a start's snapshot carries are not put in the history: they have no time.)
  - `clickingAGaugeOpensItsWindow`: emit the canvas's `gauge_clicked("IG1")` (or click the chip): `window.gauge_window("IG1")` is visible, titled IG1, with the gauge's alarm limits as guides; a second click raises the same window (same pointer); another gauge has its own.
  - `aGaugeWindowFollowsNewReadings`: publish `PressureSample`s from a thread as `gaugeLabelTurnsRedOnAlarmAndClearsInLimits` does (`test_canvas_view.cpp:854-866`), but with rising `ts`: `QTRY_VERIFY` the window's point count grows.
  - `restingOnAGaugeShowsThePopupAndLeavingHidesIt` (canvas delay 0); a click on a gauge hides the popup and opens the window.
  - `wallTimesAreTheLines`: on a line made with a `VirtualClock` (the fixture's `clock` argument), the window's header `since` and Copy's times are the virtual clock's wall times, not today's.
  - `closingTheMainWindowClosesGaugeWindows`, and destroying it with a gauge window open and a popup shown is clean under ASan.
  - `gaugeWindowsKeepTheirRangeAcrossASession`: settings from the window's settings factory.
- [x] **Implement:** members `ReadingHistory history_` (the configured gauges' names; gap: 3 × `config().system.scan_interval_ms`), declared after `bridge_`, `GaugePopup* gauge_popup_`, `std::map<QString, GaugeWindow*> gauge_windows_`. The popup and the windows are the main window's children, which Qt would delete after `history_` is gone: `~MainWindow` deletes the popup and every gauge window itself, first. `connect(&bridge_, &CoreBridge::pressureSample, ...)`: add to the history, then refresh the popup if it shows that gauge and that gauge's window if it exists. `wall_of(ts)` is `bridge_.wall_now().addMSecs(-ms(bridge_.now() - ts))`. Gauge windows are made as the laser windows are (`main_window.cpp:464-487`: `WA_DeleteOnClose` false, `Qt::Window`), given the main window's settings factory, closed in `closeEvent` (`:603-614`) and deleted in `~MainWindow` (`:506-509`). Accessors for tests: `reading_history()`, `gauge_window(name)`, `gauge_popup()`.
- [x] Commit `feat(ui): a gauge on the canvas shows its history on a rest and a click`.

  Built: the settings factory is the main window's own `set_gauge_settings` (the one `spectrometer_settings()` reads is set only with a spectrometer); without it, the application's QSettings, as there. `ReadingHistory::has` says whether a name is kept, not whether it has readings (Task 1), so `readingsGoIntoTheHistory` tests through `oldest()` and `revision()`. Besides the accessors: `open_gauge_window(name)`, null for a name the line does not define. The gauge tests other than the first do not start the line, so what they count is only what they publish. The suite is clean under ASan and UBSan.

### Task 7: The guide, the real-time list, and the branch as a whole

**Files:** `docs/user/02-extraction-line.md`, `docs/superpowers/specs/2026-10-06-virtual-clock-design.md`, the spec's status line

- [ ] User guide: the gauge chip rows of the table at `:46-47` (rest for 3 s: a chart of the last 10 minutes; click: the gauge's window; a gauge that is only drawn does neither); "Gauges" (`:155-168`): replace "there is no separate gauge window" with what the window is: the ranges, linear and log, the alarm limits drawn, Copy, that the history begins when the program starts and holds a day, that longer records are the observability dashboards' (`docs/observability.md`).
- [ ] Virtual clock spec, section 4.4 (`:386-424`), the entry "the UI: its timers" (`:410`) covers the canvas's rest delay and the charts' redraw throttle already; name the rest delay in it, since it is the one the operator feels.
- [ ] Full `cmake --build --preset dev-ui`, `ctest --preset dev-ui`, `python3 tools/quality_check.py --build-dir build/dev-ui`, `python3 tools/compiler_sweep.py --build-dir build/dev-ui`. Report the counts.
- [ ] Spec status: built, with what its section 6 leaves out.
- [ ] Commit `docs: a gauge's history from the canvas`.

## Not in this plan

Spec section 6: history across restarts or older than a day; heaters, cryostat temperatures and valves over time (the history and the chart are ready for them); several gauges on one chart; alarms on a rate of change; `elctl`.
