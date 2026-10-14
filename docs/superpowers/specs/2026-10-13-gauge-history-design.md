# Gauge history: a time series from a gauge on the canvas

Status: agreed 2026-10-13 (owner's answers, section 9). Plan:
`docs/superpowers/plans/2026-10-13-gauge-history.md`; 4.1, 4.2, 4.3 and 5
corrected from code when it was written. Drafted by an
unattended run from task T077 (owner's words, section 1). Nothing built.

## 1. Asked for

"Clicking on a gauge should bring up a timeseries of the last 10 minutes.
The user can configure a longer time range but default to 10 minutes. When
the user hovers over a gauge for more than 3 seconds then a popup chart of
the last 10 minutes is shown."

## 2. What exists

Read from `develop` `4098d1e5`.

- Gauge on canvas: `GaugeLabelItem` (`apps/pychron-ui/src/canvas_items.hpp:218`),
  a chip with name and latest reading; red text outside `alarm_high` /
  `alarm_low`. `CanvasView::on_pressure` (`canvas_view.cpp:454`) sets its
  value from `PressureSample{gauge, value, units, ts}` (`events.hpp:36`),
  relayed by `CoreBridge::pressureSample`. `ts` is on the line's clock.
- `GaugeScanner` reads each gauge at its configured interval; a failed read
  is one warning, no sample.
- Nothing keeps a reading once the next has come: no history in UI, none in
  `libs/`. Metrics exporter (`libs/metrics`) publishes the latest value for
  an outside Prometheus; that is where history lives today, outside the app.
- `StripChartModel` / `StripChartView` (spectrometer window) draw rings of
  `(t, value)` with linear or log y; `TimeSeriesRing` (`libs/core`) is a
  time-bounded ring.
- Gauge chip takes no click today; valves do (`CanvasView`'s actuation).
- A gauge drawn on the canvas but not in `extraction_line.toml` is
  decoration: no reading ever.

## 3. Goal

From the canvas, without leaving it: see how a gauge's pressure went over
the last minutes, by hovering (a glance) or clicking (a window that stays,
with a range the operator chooses).

## 4. Design

### 4.1 History (`ReadingHistory`, `apps/pychron-ui/src/reading_history.{hpp,cpp}`)

- One `TimeSeriesRing` per configured gauge, fed from
  `CoreBridge::pressureSample` from the moment the main window exists. Owned
  by the main window; canvas, popup and gauge windows read it.
- Span kept: 24 h (longest range offered, 4.3). At a 1 s scan that is
  86 400 points a gauge, 16 bytes each: 1.4 MB a gauge. Rings are bounded by
  time, so a slower gauge costs less.
- Point: `(seconds since history's first sample, value)`; units kept beside
  (last seen; a change of units empties that gauge's ring).
- A gap (read failed, no samples) is a gap: when two samples are further
  apart than 3 scan intervals (the line's `scan_interval_ms`: there is no
  interval a gauge), a NaN point is put between, so
  the line breaks instead of bridging.
- Sample whose `ts` is before the last (scan restarted, simulated clock
  reset): that gauge's ring is emptied.
- History starts when the app starts. Nothing is read from disk, nothing
  written.

### 4.2 Hover popup

- Pointer rests on a gauge chip for 3 s (not moved off it): a frameless
  popup beside the chip, about 320 x 180 px: gauge name, latest reading, a
  small chart of the last 10 minutes. Updates while shown. Goes when the
  pointer leaves the chip, on a button press anywhere, on a scroll of the
  canvas (it has no zoom or pan).
- Fewer than 2 points in the last 10 minutes: popup says "no readings yet"
  (or "no readings in the last 10 minutes"), no empty chart.
- Decoration gauge (not configured): no popup.
- The 3 s is real time (a wait on the operator's hand): a `QTimer`. Added
  to the virtual clock spec's list of what stays on real time (its 4.5).
- Popup takes no focus and no clicks meant for the canvas: a `Qt::ToolTip`
  window, not `Qt::Popup` (which takes the click that closes it).

### 4.3 Click: gauge window

- Click on a gauge chip opens a small tool window "IG1" (one a gauge;
  a second click raises it). Chart of that gauge, range box: 10 min
  (default), 30 min, 1 h, 6 h, 24 h.
- Range chosen is kept per gauge in settings
  (`gauge_window/<line>/<gauge>/range_s`); window geometry too. Default
  stays 10 min for a gauge never opened.
- Y: linear by default, box for log (pressures span decades); choice kept
  per gauge with the range; autoscale to the range shown, with alarm limits drawn as horizontal rules when set and
  inside the view.
- X: time before the newest sample, labelled as local time of day from the
  line's wall clock (wall time of a sample: `CoreBridge::wall_now()` less
  `now() - ts`), so a simulated session reads simulated time. No `QDateTime::currentDateTime()`.
- Header: latest value, min and max over the range shown.
- "Copy": the points shown as tab-separated `time, value`.
- Window follows new samples (redraw at most 4 times a second).
- Range longer than what history holds: chart shows what there is; header
  says since when ("since 14:02").

### 4.4 Chart

One small widget (`HistoryChart`) used by popup and window: QCustomPlot
directly, one graph, theme's plot background. Fed plain points; tested
without a line. Popup's y is linear. Neither it nor the history of 4.1
knows what a gauge is (a named series of timed values with units): heaters
and cryostat temperatures are to use both later (section 6).

## 5. Rules

- Reading history never touches a device and never waits: UI thread reads
  rings filled by the bridge's queued signal on the same thread.
- No clock read for data: x from sample `ts`; wall labels from
  `CoreBridge::wall_now`. Hover delay is the one real-time wait.
- Click on a gauge does nothing else (no actuation, no selection change).
  Valve clicks unchanged.
- A gauge chip drawn over a valve must not take the valve's click: press
  accepted only inside the chip; plan tests the shipped canvases.
- Memory bounded by 4.1's span whatever the session's length.

## 6. Out of scope

- History across restarts, or older than 24 h: Prometheus and Grafana have
  it (`docs/observability.md`). A link from the gauge window to the
  dashboard could come later.
- Heaters, cryostat temperatures, valves' state over time: same idea, own
  task once this one is agreed (their docks exist).
- Several gauges on one chart.
- Alarms on rate of change.
- `elctl` access to history.

## 7. Files (expected)

- `apps/pychron-ui/src/reading_history.{hpp,cpp}`: 4.1.
- `apps/pychron-ui/src/history_chart.{hpp,cpp}`: 4.4.
- `apps/pychron-ui/src/gauge_popup.{hpp,cpp}`, `gauge_window.{hpp,cpp}`:
  4.2, 4.3.
- `apps/pychron-ui/src/canvas_view.{hpp,cpp}`, `canvas_items.{hpp,cpp}`:
  hover and click on `GaugeLabelItem`, signals `gauge_hovered`,
  `gauge_clicked`.
- `apps/pychron-ui/src/main_window.{hpp,cpp}`: owns history, opens windows.
- `docs/user/02-extraction-line.md`; virtual clock spec 4.5.

## 8. Tests

- `tests/ui/test_reading_history.cpp`: ring per gauge; span bound; gap
  becomes a break; time going back empties; unknown gauge ignored; units
  change empties.
- `tests/ui/test_canvas_view.cpp`: click on a gauge chip emits
  `gauge_clicked(name)`, on a decoration gauge nothing; hover emits after
  the delay (delay injected, 0 in test) and not when pointer left first;
  valve click unchanged.
- `tests/ui/test_gauge_window.cpp`: default range 10 min; range kept per
  gauge; points outside range not drawn; min / max header; log and linear;
  alarm rules; "since" text when history is shorter; Copy text; wall time
  labels from injected clock.
- Popup: text when no readings; chart when there are; closes on leave.
- Changes `apps/pychron-ui`: final run `dev-ui`.

## 9. Owner, 2026-10-13

1. Click opens a window of its own a gauge (4.3).
2. Ranges 10 min to 24 h, 24 h kept in memory: as drafted.
3. Y linear by default, with a toggle for log.
4. History beginning at app start is accepted.
5. Gauges only now; heaters and cryostat later, on the same history and
   chart.
