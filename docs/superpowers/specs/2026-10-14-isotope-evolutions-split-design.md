# Isotope evolutions: one panel a series

Status: draft 2026-10-14, from brainstorm with owner (task T018). Owner's
decisions in section 9. Nothing built.

## 1. Asked for

"The isotope evolutions in the experiment window should be separated by
isotope:det. Examine legacy. Before any implementation brainstorm on
alternatives. The only alternative I can think of is using one graph but
scaling the signals to a reference signal. I don't super like this because
it requires choosing a reference signal. Legacy plotted each isotope on a
separate graph."

## 2. What exists

Read from `develop` `9ea52b97`.

- `EvolutionsView` (`apps/pychron-ui/src/evolutions_view.{hpp,cpp}`), Evolutions
  dock of experiment window: tab bar of kinds (`Signal`, `Baseline`,
  `Sniff`; `kKinds`, `evolutions_view.cpp:31`), focus box, title (run,
  block count/target), one `QCustomPlot` with every series of shown kind
  overlaid, intercepts table, peak-center list.
- A series is `experiment::collect::SeriesKey{isotope, detector, kind}`
  (`collector.hpp:43`); `isotope` empty for a before/after baseline block.
  Points from `SeriesUpdated` batches, live fits from `FitsUpdated`.
- Per series today: points (detector colour, else palette), fit curve from
  time zero to last point (48 samples), intercept diamond at time zero, grey
  crosses on points the fit filtered (one shared graph). Legend shown.
  `refresh()` capped at 20 Hz (`kRefreshMs = 50`); x and y rescaled to all
  shown series with 4% margin.
- Isotopes differ by orders of magnitude on one y axis: focus box
  (`set_focus`) shows one series, axes scaled to it.
- Data side draws evolutions as stacked panels sharing x
  (`processing::make_evolution_scene`, `recall.cpp:179`, in a `SceneView`).
- Legacy (`pychron/experiment/plot_panel.py`, `graph/stacked_graph.py`):
  one plot an isotope, stacked vertically, shared time axis
  (`stacked_graph.py:126-130`), x axis on bottom plot only (`:143`); isotope
  on another detector its own plot titled `iso+det`
  (`automated_run.py:762-766`); tabs `Ar`, `Equil.` (sniff), `Baselines` (a
  plot a detector), `Peak Center` (`plot_panel.py:388-417`); per plot
  points, fit line, error envelope, stats box; y per plot, anchored at 0
  for signal and sniff when min <= 50% of range
  (`data_collector.py:489-545`); intercepts in a table; no cap, no scroll,
  order = detector or hop order; right-click save, export, rescale, fit.
- T048 (held): live plots lack zoom, pan, save, tooltip; to be decided with
  this task.

## 3. Goal

Every series of shown kind readable at once while a run measures: a panel
a series, its own y scale, one time axis. No reference signal, no scaling
of one series onto another.

## 4. Design

### 4.1 Layout

- Kind tabs, title line, intercepts table, peak-center list stay.
- Focus box removed; legend removed (panel's y title names its series).
- Plot inside a `QScrollArea` (widget resizable, no horizontal scroll).
  One `QCPAxisRect` a series of shown kind, stacked top to bottom in the one
  `QCustomPlot`.
- Panels share available height equally; minimum `kPanelMinHeight = 80` px
  each. Plot's minimum height = panels x 80 + bottom axis: more panels than
  fit, area scrolls.
- No panels (no series of shown kind yet): plot shows nothing, as an empty
  plot does today.

### 4.2 Panels

- Signal: a panel an isotope:detector key. Isotope peak-hopped onto a
  second detector: own panel. Title `Ar40 H1`.
- Baseline: a panel a key. Title `H1` (block baseline, isotope empty), else
  `Ar40 H1`.
- Sniff: a panel an isotope. Title `Ar40`. Sniffs are always multicollection,
  never peak hops (owner): one detector an isotope.
- Order: arrival order in the run (first point of each series), the
  measurement plan's order. Kept per kind; emptied at run start with the
  series.
- Panel order and titles from one pure function of the held keys (in
  arrival order) and the shown kind: tested alone.

### 4.3 Each panel

- What one series has today: points, fit curve, intercept diamond, grey
  crosses on its filtered points; each panel its own crosses graph.
- y: autoscaled to that panel's points, curve and intercept, 4% margin (as
  today). Legacy's anchoring at 0 not copied.
- x: one range for all panels, from earliest to latest x over every shown
  series (time zero included), 4% margin. Tick labels and title `Time (s)`
  on bottom panel only; other panels' x axes keep ticks, no labels.
- y axes aligned down the stack by one `QCPMarginGroup` (left side).
- Colours as today: detector colour unless another series of kind has it,
  else first free palette colour.

### 4.4 Updates

- `rebuild()` (new series, kind change, clear): lays out rects and graphs.
- `refresh()` (dirty, at most every 50 ms, as today): data of each panel,
  per-panel y, shared x, intercepts table, one queued replot.
- Nothing new on GUI thread beyond the per-panel loop; no measurement owed
  (not a change made for speed).

### 4.5 Interface

`EvolutionsView` keeps `on_run_started`, `on_series`, `on_fits`,
`on_peak_center`, `clear`, `set_kind`, `kind`, `plot`, `graph_count`,
`point_count`, `title_text`, `peak_centers`, `intercept_columns`,
`intercept_rows`, `fit_of`, `fit_curve_count`, `series_color`. Removed:
`set_focus`, `focus`, `focus_choices`. Added, for tests:

```cpp
static constexpr int kPanelMinHeight = 80;
[[nodiscard]] QStringList panel_titles() const;     // top to bottom, shown kind
[[nodiscard]] QCPRange panel_x_range(int i) const;  // 0 = top
[[nodiscard]] QCPRange panel_y_range(int i) const;
[[nodiscard]] QScrollArea* scroll_area() const;
```

Pure helper (`evolutions_view.hpp`, namespace `pychron::ui`):

```cpp
struct EvolutionPanel {
  experiment::collect::SeriesKey key;
  QString title;
};
// Panels of `kind` in arrival order; `arrived` holds every key in the order its first point came.
[[nodiscard]] std::vector<EvolutionPanel> evolution_panels(
    const std::vector<experiment::collect::SeriesKey>& arrived, experiment::collect::SeriesKind kind);
```

### 4.6 With T042 (Peak center tab)

T042 plan task 5 puts a `QStackedWidget` in the dock: kind page and
`PeakCenterView`. Kind page holds title, this scroll area, intercepts
label and table. Either may land first; second rebases. Its plan's mention
of the focus box holds only while the box exists.

## 5. Rules that must hold

- No series is drawn on another's y axis; no series is scaled.
- Every panel shows the same x range.
- A panel's y range holds all of its points, its curve and its intercept.
- Order of panels does not change during a run except by a new series
  being added at the end of its kind.
- Redraw stays at most every 50 ms; points and fits arrive as today.

## 6. Tests

`tests/ui/test_evolutions_view.cpp`:

- `onePanelPerSeriesOfTheShownKind`: three signal keys, one baseline:
  Signal shows 3 panels, Baseline 1.
- `anIsotopeOnTwoDetectorsHasTwoPanels`: `Ar40 H1`, `Ar40 H2`.
- `sniffPanelsAreTitledByIsotope`.
- `panelsComeInArrivalOrder`; `aNewSeriesIsAddedAtTheBottom`.
- `eachPanelScalesItsOwnY`: Ar40 near 1000 fA, Ar36 near 1 fA: Ar36's
  y range holds its points and is under 10; Ar40's holds 1000.
- `everyPanelSharesTheTimeAxis`: equal `panel_x_range` for all.
- `onlyTheBottomPanelHasTimeLabels`.
- `manyPanelsScroll`: 10 series in a 300 px tall view: vertical scroll bar
  maximum > 0; each panel at least `kPanelMinHeight`.
- `aRunStartingEmptiesThePanels`.
- `evolution_panels` alone: order, titles, kind filter.
- Existing `drawsFitsToTimeZeroAndListsIntercepts`, `aFitBeforeItsPointsIsIgnored`
  kept, read per panel; focus tests removed.

`tests/ui/test_experiment_window.cpp`: simulated queue's checks of the
dock follow (panel count = series of Signal).

## 7. Order of work

1. `evolution_panels` and its test.
2. Stacked panels in `EvolutionsView`, focus box and legend removed, tests.
3. Scroll area and minimum height, test.
4. Experiment window test; user guide (`docs/user/04-experiments.md`,
   Evolutions dock).

## 8. Out of scope

- Any interaction: zoom, pan, save or copy image, right-click menu,
  tooltip (T048).
- Ratio plots; legacy's y anchored at 0; error envelope and stats box on
  panels (fits stay in intercepts table).
- Choosing which series to show, or their order.
- A Whiff tab (not shown today).
- Drawing through `processing::Scene` / `SceneView` (considered, section 9).

## 9. Decided by owner, 2026-10-14

1. Layout: all panels stacked, each small, scrolling when they would be too
   short (legacy's arrangement). No enlarge-one, no grid.
2. No interaction in phase 1.
3. Sniffs split by isotope; always multicollection, never peak hops.
4. Approach: stacked axis rects in the existing `EvolutionsView` and its
   one `QCustomPlot`. Rejected: a `processing::Scene` drawn by `SceneView`
   (rebuilds every graph each update, no scroll for tall stacks;
   reconsidered with T048); one `QCustomPlot` a series (y axes cannot
   align, x kept in step by hand).
5. Focus box dropped.
