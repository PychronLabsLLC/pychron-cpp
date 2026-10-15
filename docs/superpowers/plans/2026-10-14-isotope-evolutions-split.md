# Isotope Evolutions Split Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The Evolutions dock draws each series of the shown kind on a panel of its own, stacked on one time axis, each scaled to its own data, scrolling when there are too many.

**Architecture:** `EvolutionsView` keeps its one `QCustomPlot` and gives it one `QCPAxisRect` per shown series, laid out by a pure `evolution_panels()` function from the keys in their order of arrival. The rects share a left/right `QCPMarginGroup` and one x range; only the bottom rect labels time. The plot sits in a `QScrollArea` with a minimum height of 80 px a panel.

**Tech Stack:** C++20, Qt 6 Widgets, QCustomPlot 2.1.1, QtTest (`tests/ui`).

**Spec:** `docs/superpowers/specs/2026-10-14-isotope-evolutions-split-design.md` (approved by the owner 2026-10-14). Section numbers below are its.

## Global Constraints

- One panel a series of the shown kind; no series drawn on another's y axis; no series scaled (spec 5).
- Titles: signal `Ar40 H1`; baseline `H1` when the key's isotope is empty, else `Ar40 H1`; sniff `Ar40` (spec 4.2).
- Order: arrival order of each series' first point, per kind; emptied when a run starts (spec 4.2).
- `kPanelMinHeight = 80` px a panel; no horizontal scroll (spec 4.1).
- y per panel: its points, curve and intercept, 4% margin; a zero-height range padded by 1.0, as `refresh()` does today. x: one range for every panel, earliest to latest x over all shown series, time zero included, 4% margin (spec 4.3).
- Tick labels and the title `Time (s)` on the bottom panel only (spec 4.3).
- Focus box and legend removed: `set_focus`, `focus`, `focus_choices` gone (spec 4.1, 4.5).
- Redraw at most every 50 ms (`kRefreshMs`), as today (spec 4.4). No interaction of any kind (spec 8).
- Qt code: a parent owns its child. `const` by default, `[[nodiscard]]` on results that must not be dropped.
- After `rebuild()` removes the default axis rect, `plot_->xAxis` and `plot_->yAxis` are null: nothing reads them; tests use the panel accessors.
- Before each commit: `python3 tools/quality_check.py --build-dir build/dev-ui` (exit 0), then `cmake --build --preset dev-ui --target pychron_ui_test_evolutions_view && ctest --preset dev-ui -R '^ui\.test_evolutions_view$'`. Before the branch lands: full `cmake --build --preset dev-ui`, `ctest --preset dev-ui`, `python3 tools/compiler_sweep.py --build-dir build/dev-ui`.
- Commits: Conventional Commits, scope `ui` (`docs` for the guide alone).

## Review Focus

1. A series whose readings are all equal (a flat baseline): its panel still has a y range of non-zero height, not a collapsed axis. Task 2, `aFlatSeriesStillHasARange`.
2. A kind with no series yet (Sniff before any sniff): no panels, no crash, and switching back draws the others again. Task 2, `aKindWithoutSeriesShowsNoPanels`.
3. Series of two kinds arriving interleaved (a baseline block before the signals): each kind keeps its own arrival order. Task 1, `eachKindKeepsItsOwnOrder`.
4. A reading that is no number (NaN) in a series: the panel's y range stays finite and holds the other points. Task 2, `aReadingThatIsNoNumberLeavesTheRangeFinite`.
5. A new series arriving while another kind is shown: it appears in its place when its kind is selected, and the shown panels do not move. Task 2, `aSeriesOfAnotherKindLeavesTheShownPanels`.

---

### Task 1: `evolution_panels`

**Files:**
- Modify: `apps/pychron-ui/src/evolutions_view.hpp`, `apps/pychron-ui/src/evolutions_view.cpp`
- Test: `tests/ui/test_evolutions_view.cpp`

**Interfaces:**
- Produces, in `evolutions_view.hpp`, namespace `pychron::ui`:

```cpp
struct EvolutionPanel {
  experiment::collect::SeriesKey key;
  QString title;
};
// Panels of `kind` in arrival order; `arrived` holds every key in the order its first point came.
[[nodiscard]] std::vector<EvolutionPanel> evolution_panels(
    const std::vector<experiment::collect::SeriesKey>& arrived, experiment::collect::SeriesKind kind);
```

- [ ] **Step 1: Write the failing tests** (private slots of `TestEvolutionsView`):
  - `panelsComeInArrivalOrder`: `arrived = {Ar40 H1, Ar36 CDD, Ar39 AX}` (Signal) gives titles `{"Ar40 H1", "Ar36 CDD", "Ar39 AX"}`, keys in that order.
  - `anIsotopeOnTwoDetectorsHasTwoPanels`: `{Ar40 H1, Ar40 H2}` Signal gives `{"Ar40 H1", "Ar40 H2"}`.
  - `baselinePanelsAreTitledByDetector`: `{"", "H1", Baseline}` gives `"H1"`; `{"Ar40", "H1", Baseline}` gives `"Ar40 H1"`.
  - `sniffPanelsAreTitledByIsotope`: `{"Ar40", "H1", Sniff}` gives `"Ar40"`.
  - `eachKindKeepsItsOwnOrder`: `arrived = {H1 Baseline, Ar40 H1 Signal, AX Baseline, Ar39 AX Signal}`: Signal gives the two signals in that order, Baseline the two baselines in that order, Sniff gives none.
- [ ] **Step 2:** `cmake --build --preset dev-ui --target pychron_ui_test_evolutions_view`: fails to compile (`evolution_panels` undeclared).
- [ ] **Step 3: Implement** `evolution_panels` in `evolutions_view.cpp`: a filter of `arrived` by `kind`, the title by the rules of Global Constraints (the signal and baseline title is today's `series_name`, `evolutions_view.cpp:33-37`).
- [ ] **Step 4:** `ctest --preset dev-ui -R '^ui\.test_evolutions_view$'`: passes.
- [ ] **Step 5: Commit** `feat(ui): the Evolutions dock knows its panels and their order`.

### Task 2: A panel a series

**Files:**
- Modify: `apps/pychron-ui/src/evolutions_view.hpp`, `apps/pychron-ui/src/evolutions_view.cpp`
- Test: `tests/ui/test_evolutions_view.cpp`

**Interfaces:**
- Consumes: `evolution_panels` (Task 1).
- Produces on `EvolutionsView` (ranges as `(lower, upper)` pairs: no QCustomPlot type in the header):

```cpp
[[nodiscard]] QStringList panel_titles() const;                 // top to bottom, shown kind
[[nodiscard]] std::pair<double, double> panel_x_range(int i) const;  // 0 = top
[[nodiscard]] std::pair<double, double> panel_y_range(int i) const;
[[nodiscard]] bool panel_has_time_labels(int i) const;          // tick labels and "Time (s)" shown
```

  `graph_count()` stays and means the panels shown. Removed: `set_focus`, `focus`, `focus_choices`, `focus_box_`, `focus_`, `fill_focus`, the legend.

- [ ] **Step 1: Write the failing tests:**
  - `onePanelPerSeriesOfTheShownKind`: three signal keys and one baseline key fed: `graph_count() == 3`, `panel_titles().size() == 3`; `set_kind(Baseline)`: `graph_count() == 1`.
  - `aNewSeriesIsAddedAtTheBottom`: after two signal series, a third: `panel_titles()` is the first two then the new one.
  - `eachPanelScalesItsOwnY`: Ar40 H1 points near 1000 (999, 1000, 1001) and Ar36 CDD near 1 (0.9, 1.0, 1.1): `QTRY_VERIFY` that panel 1's y range holds 0.9 and 1.1 and its upper is under 10; panel 0's holds 999 and 1001.
  - `everyPanelSharesTheTimeAxis`: Ar40 at t 10..20, Ar36 at t 15..30: every `panel_x_range(i)` equal, lower ≤ 10 and upper ≥ 30.
  - `onlyTheBottomPanelHasTimeLabels`: three panels: `panel_has_time_labels` false, false, true.
  - `aRunStartingEmptiesThePanels`: `on_run_started` after series: `graph_count() == 0`, `panel_titles()` empty; a new series then is panel 0.
  - `aFlatSeriesStillHasARange`: three equal readings: `panel_y_range(0).second > panel_y_range(0).first`.
  - `aKindWithoutSeriesShowsNoPanels`: signal series fed; `set_kind(Sniff)`: `graph_count() == 0`; `set_kind(Signal)`: `graph_count()` as before.
  - `aReadingThatIsNoNumberLeavesTheRangeFinite`: readings 1.0, NaN, 3.0: both ends of `panel_y_range(0)` finite, range holds 1 and 3.
  - `aSeriesOfAnotherKindLeavesTheShownPanels`: Signal shown with two panels; a baseline series arrives: `panel_titles()` unchanged; `set_kind(Baseline)`: one panel, its title.
  - In `drawsFitsToTimeZeroAndListsIntercepts`: `view.plot()->xAxis->range().lower <= 10.0` becomes `view.panel_x_range(0).first <= 10.0`; the block "One series alone, scaled to it" (`focus_choices`, `set_focus`) is replaced by: after `ar39` arrives, `graph_count() == 2` and `panel_titles() == {"Ar40 H1", "Ar39 H2"}`.
- [ ] **Step 2:** build the test target: fails to compile (the new accessors are undeclared).
- [ ] **Step 3: Implement.**
  - Keep `std::vector<SeriesKey> arrived_`, appended in `on_series` when a key is first seen, cleared in `clear()`.
  - `Graphs` gains `QCPAxisRect* rect` and its own `QCPGraph* excluded`; the shared `excluded_` goes.
  - `rebuild()`: `plot_->plotLayout()->clear()`, then for each `evolution_panels(arrived_, kind_)` entry a `new QCPAxisRect(plot_)` in row `i`, column 0, background `theme().plot_bg`, a left/right `QCPMarginGroup` shared by all (one owned by the view, made in the constructor), graphs added with `plot_->addGraph(rect->axis(QCPAxis::atBottom), rect->axis(QCPAxis::atLeft))` and styled as today (`evolutions_view.cpp:221-252`), y title the panel's title, x tick labels and `Time (s)` only on the last rect.
  - `refresh()`: as today for data, curves, intercepts and the table; then each panel's value axis `rescale()` plus the 4% pad (1.0 when the size is 0); the x range is the union of the panels' key ranges (`QCPGraph::getKeyRange`) plus the 4% pad, set on every rect's bottom axis.
  - Remove the focus box, its connection and `fill_focus`; remove the legend calls.
- [ ] **Step 4:** `ctest --preset dev-ui -R '^ui\.test_evolutions_view$'`: passes.
- [ ] **Step 5:** `python3 tools/quality_check.py --build-dir build/dev-ui`: exit 0.
- [ ] **Step 6: Commit** `feat(ui): the Evolutions dock draws each series on a panel of its own`.

### Task 3: The panels scroll

**Files:**
- Modify: `apps/pychron-ui/src/evolutions_view.hpp`, `apps/pychron-ui/src/evolutions_view.cpp`
- Test: `tests/ui/test_evolutions_view.cpp`

**Interfaces:**
- Consumes: the panels of Task 2.
- Produces on `EvolutionsView`:

```cpp
static constexpr int kPanelMinHeight = 80;
[[nodiscard]] QScrollArea* scroll_area() const;
```

- [ ] **Step 1: Write the failing test** `manyPanelsScroll`: a view `resize(400, 300)` and `show()`n (`QVERIFY(QTest::qWaitForWindowExposed(&view))`), ten signal series fed: `QTRY_VERIFY(view.scroll_area()->verticalScrollBar()->maximum() > 0)`; `view.plot()->height() >= 10 * EvolutionsView::kPanelMinHeight`; `view.scroll_area()->horizontalScrollBar()->maximum() == 0`. With two series: `verticalScrollBar()->maximum() == 0`.
- [ ] **Step 2:** fails to compile (`scroll_area`, `kPanelMinHeight` undeclared).
- [ ] **Step 3: Implement:** `plot_` goes into a `QScrollArea` (`setWidgetResizable(true)`, `setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff)`, frameless) where the plot was in the layout; `rebuild()` sets `plot_->setMinimumHeight(n * kPanelMinHeight + bottom)`, where `bottom` is the bottom rect's x axis and label height (a constant of 40 px is enough). The old `plot_->setMinimumHeight(200)` goes.
- [ ] **Step 4:** the test target passes.
- [ ] **Step 5: Commit** `feat(ui): the Evolutions dock scrolls when its panels would be too short`.

### Task 4: The window, the guide

**Files:**
- Modify: `tests/ui/test_experiment_window.cpp:236-268`, `docs/user/04-experiments.md:215`

**Interfaces:**
- Consumes: `panel_titles()` (Task 2).

- [ ] **Step 1: Test** in `runsTheExampleQueue` beside the checks at `:236-243`: `evo->panel_titles().size() == evo->graph_count()` and `evo->panel_titles().contains("Ar40 H1")`; after `set_kind(Baseline)` at `:267`, `evo->graph_count() > 0`.
- [ ] **Step 2:** `ctest --preset dev-ui -R '^ui\.test_experiment_window$'`: passes (it exercises Task 2's code; if it fails, the fault is in Tasks 1 to 3, not here).
- [ ] **Step 3: Guide:** the Evolutions row of the dock table (`04-experiments.md:215`) says each series of the shown kind has a panel of its own on one time axis, each scaled to its own data, scrolling when there are many; signals and baselines by isotope and detector, sniffs by isotope; no zoom or saving yet.
- [ ] **Step 4:** full `cmake --build --preset dev-ui`, `ctest --preset dev-ui`, `python3 tools/compiler_sweep.py --build-dir build/dev-ui` (0 warnings; `evolutions_view.cpp` and its test are among the QCustomPlot files gcc cannot parse, so read them by hand against AGENTS.md's CI list).
- [ ] **Step 5: Commit** `test(ui): the experiment window's evolutions are a panel a series` with the guide in the same commit (`docs` alone would hide the test).
