# Peak Center Plot Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** While and after a peak center in a run, the Evolutions dock shows the sweep of the reference detector and of the additional detectors on one graph, the others scaled onto the reference, with low, centre and high marked and the outcome in the title.

**Architecture:** The peak center job publishes two more events on the bus it already has, one when a try starts and one per sweep step. `ExperimentBridge` relays them to the GUI thread. A new `PeakCenterView` keeps the points of each try, scales the additional detectors with a pure function, and draws with `QCustomPlot`. `EvolutionsView` gains a fourth tab that shows that view in place of its own plot and selects it when a try starts.

**Tech Stack:** C++20, Qt 6 Widgets, QCustomPlot, GoogleTest (`libs/systems`), QtTest (`tests/ui`).

**Spec:** `docs/superpowers/specs/2026-10-12-peak-center-plot-design.md`, agreed 2026-10-13. Read it first; section numbers below refer to it. Three points of it are corrected in this branch from the code (its sections 1, 4.1 and 4.4): see "What the code says" below.

## What the code says

Read from `develop` `4098d1e5`.

- `PeakCenterOptions::bus` exists (`peak_center.hpp:144`) and the session sets it (`libs/experiment/src/lab/session.cpp:45-46`). `PeakCenterDone` is published at `peak_center.cpp:299`.
- Sweep points already reach the bus, as `JobProgress` (`job_runner.cpp:56-62`), but that event has no try index and is keyed by job id, and the bridge does not relay it. The two new events are still what the view needs. Spec section 1 said the points do not reach the bus: corrected.
- `Sweep::run` takes a `Progress&` (`sweep.hpp:74`) and a `Progress` gets its sink only in its constructor (`progress.hpp:47`). So `Runner::attempt` makes a `Progress` of its own for the sweep whose sink publishes `PeakCenterPoint` and then calls `progress_.report(u)`: the job's progress and `last()` are what they were. Spec 4.1 corrected to say so.
- `Runner::attempt(double start, double end)` (`peak_center.cpp:335`) has no try index; the loop that calls it is at `:283`. First move at `:340`; the sweep at `:370-372`.
- `EvolutionsView`'s tab bar is one tab per series kind, mapped by index (`evolutions_view.cpp:31`, `:52-56`, `:95-97`, `:217`). A fourth tab needs that mapping guarded; the spec's "a tab beside the kind tabs" stands, and 4.4 now says how.
- A peak center skipped for low signal (`engine.cpp:259-275`) publishes nothing: the view does not change, and the log line is all there is. Unchanged here.
- `tests/ui/CMakeLists.txt:8` globs `test_*.cpp`: a new test file is a new ctest `ui.<name>` with no CMake edit. `apps/pychron-ui/CMakeLists.txt` lists sources and headers by hand (`:12` onward).
- The UI fixture's spectrometer (`configs/examples/spectrometer.sim-integrated.toml`): reference `H1`; detectors `H2`, `H1`, `AX`, `L1`, `L2`, `CDD`. In the simulator none of the others has a peak inside the example's sweep window when `Ar40` is centred on `H1` (window 4.941 to 5.061; `Ar40` falls at 5.123 on `H2` and 4.874 on `AX`, further off on the rest; `H2` sees `Ar39` at 4.995, which air does not hold). Task 6 is about that.
- `EvolutionsView::clear()` runs at every run start (`on_run_started`, `evolutions_view.cpp:115-118`), not at queue start, and the peak center list is per run (`test_experiment_window.cpp:265` expects one line after three runs). The graph's kept results are cleared at queue start, as the spec says (4.4), through `ExperimentBridge::queueStarted`.
- When the job is cancelled or its hardware fails, `Runner::run` returns before `PeakCenterDone` (`peak_center.cpp:284`, `:292`): the view is left with a try and no outcome.
- An additional detector whose fit fails is kept as `DetectorPeak{d, nullopt, error}` (`conclude()`, `peak_center.cpp:390-393`) and does not fail the peak center.

## Global Constraints

- Scaling is for display. The fit, the field table and the record read raw values; nothing in `libs/` changes what it computes.
- With no bus, the runner publishes nothing and behaves as today.
- The two events are published on the job's thread, as `PeakCenterDone` is. No thread, no wait, no timer for correctness; nothing in the view reads a clock.
- The view shows what the events say. It never reads the spectrometer.
- `PeakCenterDone` alone, with no live events before it, draws the whole graph.
- Qt code: a parent owns its child. `const` by default, `[[nodiscard]]` on results that must not be dropped.
- Read `apps/pychron-ui/AGENTS.md` before the UI tasks and `libs/setup/AGENTS.md` before Task 6.
- Commits: Conventional Commits with the component as scope. Before each that changes C++: `python3 tools/quality_check.py` (exit 0), then the tests the task names.
- UI tasks build with the `dev-ui` preset: `cmake --build --preset dev-ui --target pychron_ui_<test>` and `ctest --preset dev-ui -R '^ui\.<test>$'`. Before the branch lands: full `cmake --build --preset dev-ui`, `ctest --preset dev-ui`, `python3 tools/compiler_sweep.py --build-dir build/dev-ui`.

## File Structure

| File | Responsibility |
|---|---|
| `libs/systems/include/pychron/systems/jobs/peak_center.hpp`, `src/jobs/peak_center.cpp` | `PeakCenterTryStarted`, `PeakCenterPoint`; published. |
| `apps/pychron-ui/src/experiment_bridge.hpp`, `.cpp` | Two relays, two signals. |
| `apps/pychron-ui/src/peak_center_scale.hpp`, `.cpp` (new) | `scale_to_reference`. |
| `apps/pychron-ui/src/peak_center_view.hpp`, `.cpp` (new) | The graph: tries, series, markers, title, the 20 kept results. |
| `apps/pychron-ui/src/evolutions_view.hpp`, `.cpp` | Fourth tab, the stack, the list click. |
| `apps/pychron-ui/src/experiment_window.cpp` | Wiring of the two new signals. |
| `apps/pychron-ui/CMakeLists.txt` | Four new files in the source list. |
| `configs/examples/peak_center.toml` | `additional_detectors`. |
| `docs/user/03-spectrometer.md`, `docs/user/04-experiments.md` | What the graph shows; how to choose the detectors. |
| `tests/systems/test_jobs_peak_center.cpp` | Event order and content. |
| `tests/ui/test_peak_center_scale.cpp`, `test_peak_center_view.cpp` (new) | Scaling; the view. |
| `tests/ui/test_experiment_bridge.cpp`, `test_evolutions_view.cpp`, `test_experiment_window.cpp` | Relays; the tab; a simulated queue. |

---

### Task 1: The job says when a try starts and gives each point

**Files:** `libs/systems/include/pychron/systems/jobs/peak_center.hpp`, `libs/systems/src/jobs/peak_center.cpp`, `tests/systems/test_jobs_peak_center.cpp`

- [x] **Step 1: failing tests** in the `PeakCenterRun` fixture (`:157`), capturing as `AsAJobPublishesTheResult` (`:304`) does, into one `std::vector<std::string>` of tags so that order is asserted:

```cpp
TEST_F(PeakCenterRun, PublishesEachTryAndEachPointInOrder) {
  true_center_ = initial_ + 0.018;  // two tries, as RetriesAroundTheMaximum...
  std::vector<std::string> order;
  std::vector<PeakCenterTryStarted> started;
  std::vector<std::vector<SweepPoint>> points(2);
  auto s1 = rig_.bus.subscribe<PeakCenterTryStarted>([&](const PeakCenterTryStarted& e) {
    order.push_back("try" + std::to_string(e.try_index));
    started.push_back(e);
  });
  auto s2 = rig_.bus.subscribe<PeakCenterPoint>([&](const PeakCenterPoint& e) {
    if (order.empty() || order.back() != "points") order.push_back("points");
    points.at(static_cast<std::size_t>(e.try_index)).push_back(e.point);
  });
  auto s3 = rig_.bus.subscribe<PeakCenterDone>([&](const PeakCenterDone&) { order.push_back("done"); });
  PeakCenterOptions options;
  options.sweep.sleep = rig_.sweep_sleep();
  options.bus = &rig_.bus;
  const auto r = run(config(), options);
  ASSERT_TRUE(r);
  ASSERT_EQ(r->tries.size(), 2U);
  EXPECT_EQ(order, (std::vector<std::string>{"try0", "points", "try1", "points", "done"}));
  for (std::size_t i = 0; i < 2; ++i) {
    EXPECT_EQ(started[i].n_tries, 2);
    EXPECT_EQ(started[i].start, r->tries[i].start);
    EXPECT_EQ(started[i].end, r->tries[i].end);
    ASSERT_EQ(points[i].size(), r->tries[i].points.size());
    for (std::size_t k = 0; k < points[i].size(); ++k) {
      EXPECT_EQ(points[i][k].x, r->tries[i].points[k].x);
      EXPECT_EQ(points[i][k].y, r->tries[i].points[k].y);
    }
  }
  EXPECT_EQ(started[0].detector, r->detector);
  EXPECT_EQ(started[0].isotope, r->isotope);
  EXPECT_EQ(started[0].initial_center, r->initial_center);
}
```

  Also `TryStartedNamesTheAdditionalDetectors` (config with `additional_detectors`; the event lists them in config order), `WithoutABusNothingIsPublished` (subscribe to all three on `rig_.bus`, leave `options.bus` null: nothing arrives), `TheJobsOwnProgressIsUnchanged` and `AnAdditionalDetectorWithoutAPeakDoesNotFailThePeakCenter` (pins `conclude()`: `ok`, with that detector's `DetectorPeak::error` set). The two that compare a run with the bus against one without set `cfg.update_table = false` (the first run would otherwise move the field table under the second) and compare fields, since `PeakCenterResult` has no `operator==`. The fixture's `run()` always passes its own sinkless `progress_`: the progress case calls `run_peak_center` itself with a `Progress` whose sink counts, and compares the count and the last update.

- [x] **Step 2:** `cmake --build --preset dev --target pychron_systems_tests && build/dev/tests/systems/pychron_systems_tests --gtest_filter='PeakCenterRun.*'`: the new cases do not compile (no such types).

- [x] **Step 3: implement.** In `peak_center.hpp`, beside `PeakCenterDone` (`:138`), the two structs exactly as spec 4.1 gives them. In `peak_center.cpp`: `attempt(double start, double end, int try_index)`, called with the loop's `i` (`:283`, `:291`). Before the first move (`:340`), when `options_.bus != nullptr`, publish `PeakCenterTryStarted{r_.detector, r_.isotope, cfg_.additional_detectors, try_index, cfg_.n_tries, start, end, r_.initial_center}`. At the sweep (`:370`):

```cpp
Progress stepped([this, try_index](const ProgressUpdate& u) {
  if (options_.bus != nullptr && u.point) options_.bus->publish(PeakCenterPoint{try_index, *u.point});
  progress_.report(u);
});
auto points = runner.run(spec_, sweep, stepped, cancel_);
```

  Check what `Progress::report` and `last()` do with a forwarded update (`progress.hpp:42-50`) so that `progress_` ends as it did.

- [x] **Step 4:** the filter above passes; `ctest --preset dev-quick`.
- [x] **Step 5:** `python3 tools/quality_check.py`; commit `feat(systems): a peak center publishes each try's start and each sweep point`.

### Task 2: The bridge relays them

**Files:** `apps/pychron-ui/src/experiment_bridge.hpp`, `.cpp`, `tests/ui/test_experiment_bridge.cpp`

- [x] **Step 1: failing test.** In `runsTheExampleQueueAndRelaysEverything` (`:23`), beside the `peakCenterDone` connection (`:47-50`): count `peakCenterTryStarted` and `peakCenterPoint`, each calling `on_gui()`. After the queue: tries started is at least the peak centers done (`:91` has 3), points is greater than tries, and the last `try_index` seen in a point equals the last try started.
- [x] **Step 2:** signals `void peakCenterTryStarted(const pychron::jobs::PeakCenterTryStarted& event);` and `void peakCenterPoint(const pychron::jobs::PeakCenterPoint& event);` after `peakCenterDone` (`experiment_bridge.hpp:85`); `relay<jobs::PeakCenterTryStarted>(...)` and `relay<jobs::PeakCenterPoint>(...)` after `experiment_bridge.cpp:53`. The relay is a queued lambda: no metatype needed.
- [x] **Step 3:** `cmake --build --preset dev-ui --target pychron_ui_test_experiment_bridge && ctest --preset dev-ui -R '^ui\.test_experiment_bridge$'`.
- [x] **Step 4:** commit `feat(ui): the experiment bridge relays a peak center's tries and points`.

### Task 3: Scaling onto the reference

**Files:** `apps/pychron-ui/src/peak_center_scale.hpp`, `.cpp`, `apps/pychron-ui/CMakeLists.txt`, `tests/ui/test_peak_center_scale.cpp`

- [x] **Step 1: failing tests** (QtTest, `QTEST_APPLESS_MAIN`):
  - `mapsTheOthersRangeOntoTheReferences`: reference `{10, 110, 60}`, other `{1, 3, 2}` gives `{10, 110, 60}`.
  - `handNumbers`: reference `{2, 12}`, other `{100, 150, 200}` gives `{2, 7, 12}`.
  - `theScaledMaximumIsTheReferencesMaximum` and the same for the minimum, on uneven data (`qFuzzyCompare`: the arithmetic is not exact).
  - `aFlatOtherIsNotScaled`, `aFlatReferenceIsNotScaled`, `anEmptyInputIsNotScaled`: `std::nullopt`.
  - `theInputsMayDifferInLength`: an additional detector with a missing step has fewer values; the ranges are taken over what each has.
- [x] **Step 2: implement** the signature of spec 4.3, legacy's formula:

```cpp
std::optional<std::vector<double>> scale_to_reference(std::span<const double> reference,
                                                      std::span<const double> other) {
  if (reference.empty() || other.empty()) return std::nullopt;
  const auto [ref_lo, ref_hi] = std::ranges::minmax(reference);
  const auto [lo, hi] = std::ranges::minmax(other);
  const double R = ref_hi - ref_lo;
  const double r = hi - lo;
  if (R == 0.0 || r == 0.0) return std::nullopt;
  std::vector<double> out(other.size());
  std::ranges::transform(other, out.begin(), [&](double y) { return (y - lo) * R / r + ref_lo; });
  return out;
}
```

  (`minmax` on a range returns values, not references: no dangling. If gcc's `-Wdangling-reference` or clang-tidy objects to the structured binding, take the result into a named variable.)
- [x] **Step 3:** add both files to `apps/pychron-ui/CMakeLists.txt`'s list; build `pychron_ui_test_peak_center_scale`; `ctest --preset dev-ui -R '^ui\.test_peak_center_scale$'`.
- [x] **Step 4:** commit `feat(ui): an additional detector's sweep is scaled onto the reference's range`.

### Task 4: The view

**Files:** `apps/pychron-ui/src/peak_center_view.hpp`, `.cpp`, `apps/pychron-ui/CMakeLists.txt`, `tests/ui/test_peak_center_view.cpp`

```cpp
class PeakCenterView : public QWidget {
  Q_OBJECT
 public:
  explicit PeakCenterView(std::map<std::string, QColor> colors = {}, QWidget* parent = nullptr);

  void on_try_started(const jobs::PeakCenterTryStarted& e);
  void on_point(const jobs::PeakCenterPoint& e);
  void on_done(const jobs::PeakCenterDone& e);   // keeps the result; draws it whole
  void show_result(int index);                    // 0 = oldest kept
  void clear();                                   // queue start: graph and kept results
  void abandon();                                 // a run started with a try still open

  [[nodiscard]] int kept() const;                 // at most 20
  [[nodiscard]] QString title() const;
  [[nodiscard]] QCustomPlot* plot() const;

  // What is drawn, for tests: one entry per series, the current try's first.
  struct Series {
    QString name;            // "*H1" for the reference, the detector's name otherwise; empty for a faint try
    QVector<double> x, y;    // y as drawn (scaled for an additional detector)
    QVector<double> raw;     // y as measured: what the tooltip gives
    bool faint = false;
  };
  [[nodiscard]] std::vector<Series> series() const;
  [[nodiscard]] QVector<double> marker_x() const;  // low, centre, high, maximum; empty when none
  [[nodiscard]] QVector<double> marker_y() const;
  [[nodiscard]] QString tooltip_at(int series, int point) const;  // detector, x, raw intensity
  [[nodiscard]] QVector<double> rule_x() const;    // initial centre, fitted centre, centre without offset

 signals:
  void tryStarted();   // EvolutionsView selects the tab on it
};
```

  State is plain data: a `std::vector<Try>` for the peak center in hand (each `Try` the event's fields and its `SweepPoint`s), the done result when there is one, and a `std::deque<jobs::PeakCenterResult>` of the newest 20. One private `redraw()` builds every graph from that state, so that live events and `on_done` alone give the same picture.

- [ ] **Step 1: failing tests.** A helper `sweep_point(x, {{"H1", a}, {"AX", b}})` and a `try_started(index, n, additional)`:
  - `pointsFedOneByOneGrowTheReferenceSeries`: three points; `series()[0].name == "*H1"`, three x and y, y equal to raw.
  - `anAdditionalDetectorIsScaledOntoTheReference`: H1 `{0, 100, 0}`, AX `{1, 3, 1}`: the AX series' y is `{0, 100, 0}`, its raw `{1, 3, 1}`, its name `AX`, its pen colour the one given for `AX`.
  - `aFlatAdditionalDetectorIsDrawnRaw`.
  - `aStepWithoutADetectorsValueIsLeftOut`: the second point has no `AX`: the AX series has two points, not a zero.
  - `scalingIsRecomputedAtEachPoint`: after a point that raises the reference's maximum, the earlier scaled values have moved.
  - `theTitleNamesIsotopeDetectorAndTry`: `Ar40 on H1, try 1 of 2`.
  - `doneOkDrawsMarkersAndRules`: a `PeakCenterResult` with a `PeakShape`: `marker_x()` is low, centre, high, maximum x; `rule_x()` is `{initial_center, center}`; the title ends `center 5.123456, table updated` (the number formatted as `EvolutionsView::on_peak_center` formats it, `evolutions_view.cpp:160-170`).
  - `aDacOffsetAddsAThirdRule`: `config.dac_offset = 0.01`: `rule_x()` has a third value, `center - 0.01`.
  - `doneFailedKeepsThePointsAndDrawsNoMarkers`: the title is `Ar40 on H1: failed: <message>`.
  - `aSecondTryLeavesTheFirstFaint`: after `try_started(1, ...)` and one point, the first try's series are `faint`, unnamed and not in the legend; the current try's reference is first in `series()`.
  - `scalingIsWithinATry`: the faint try's scaled values do not change when the second try's points arrive.
  - `doneAloneDrawsEveryTry`: no live events; `on_done` with two tries gives the same `series()` as the live path did (compare with a second view fed live).
  - `doneReplacesWhatWasDrawnLive`: live points that differ from `result.tries.back().points` are gone after `on_done`.
  - `theNewestTwentyResultsAreKept`: 22 `on_done`; `kept() == 20`; `show_result(0)` shows the third.
  - `showResultRestoresAnEarlierOne`, and `aNewTryAfterShowingAnOldResultShowsTheNewOne`.
  - `clearEmptiesTheGraphAndTheKeptResults`.
  - `tryStartedIsEmittedOnce` per `on_try_started` (`QSignalSpy`).
  - `theTooltipGivesTheRawIntensity`: for a scaled series, `tooltip_at` has the raw value, not the scaled one.
  - `theInitialCentreIsRuledFromTheTrysStart`: after `on_try_started` and before any point, `rule_x()` is `{initial_center}`.
  - `theReferenceIsHeavierAndInTheLegend`: its graph's pen is wider than an additional detector's; the legend has `*H1` and each additional detector, and no faint try.
  - `markersSitOnTheReferencesValues`: `marker_y()` is the `PeakShape`'s low, centre, high and maximum y.
  - `aTryLeftWithoutAnOutcomeSaysSo`: `on_try_started`, two points, then `abandon()`: the points stay, the title is `Ar40 on H1, try 1 of 2: not finished`, nothing is added to the kept results.
- [ ] **Step 2: implement.** Plot set up as `EvolutionsView`'s is (`evolutions_view.cpp:47`, `:80-85`: minimum height 200, `theme().plot_bg`, `phFastPolylines`), with the legend on, upper left. Axis labels: x the magnet value in native units (take the wording the spectrometer window uses for the same quantity), y `Intensity`. Reference pen width 2.5, additional 1.5, faint 1.0 with alpha 70. Markers are a graph with no line and a scatter style (low, centre and high one colour, maximum another), their y the reference's value from `PeakShape`. Rules are `QCPItemStraightLine` or `QCPItemLine` across the axis rect: clear them with `plot_->clearItems()` in `redraw()`. A detector with no colour given takes `StripChartModel::palette_color(index)`, as `EvolutionsView` does. Tooltip: nearest point of the current try under the cursor, giving detector, x and raw intensity; if `EvolutionsView` has no tooltip to follow, a `QCPItemText` or `QToolTip` from `mouseMove` is enough, and its text comes from a function the test can call (`tooltip_at(series, index)`).
- [ ] **Step 3:** build `pychron_ui_test_peak_center_view`; `ctest --preset dev-ui -R '^ui\.test_peak_center_view$'`; `python3 tools/quality_check.py`.
- [ ] **Step 4:** commit `feat(ui): a view of a peak center's sweep, scaled, marked and titled`.

### Task 5: The tab in the Evolutions dock

**Files:** `apps/pychron-ui/src/evolutions_view.hpp`, `.cpp`, `apps/pychron-ui/src/experiment_window.cpp`, `tests/ui/test_evolutions_view.cpp`, `tests/ui/test_experiment_window.cpp`

- [ ] **Step 1: failing tests** in `test_evolutions_view.cpp` (it has none for peak centers yet):
  - `hasAPeakCenterTabAfterTheKinds`: four tabs, the last `Peak center`.
  - `thePeakCenterTabShowsTheGraphAndHidesTheKindControls`: selecting it shows `peak_center_view()` and hides the kind plot, the focus box and the intercepts table; selecting `Signal` brings them back, and the kind is still the one it was.
  - `aTryStartingSelectsTheTab` (decision 2), and `aSeriesArrivingDoesNotLeaveIt` (the spec keeps "no jump back to Signal"): `on_series` while on the peak center tab leaves the tab where it is, and the kind tabs still draw when selected.
  - `aPeakCenterStillAddsItsLine`: `peak_centers()` as before.
  - `clickingALineShowsThatPeakCenter`: two `on_peak_center`; clicking the first row selects the tab and the view's title is the first's.
  - `aRunStartingKeepsTheGraphAndItsKeptResults` (the list empties per run, as today; the graph of the last peak center stays until the next one starts) and `aRunStartingAbandonsAnOpenTry`.
  - `aRunStartingReturnsToTheSignalTab` (owner, 2026-10-13).
  - `theListOutlivesTheTwentyKept`: after 22, the two oldest rows are still in the list and clicking one changes nothing (and does not crash).
- [ ] **Step 2: implement.** A `QStackedWidget` where `plot_` is in the layout, holding the kind page (title, plot, the "Intercepts" label and its table) and the `PeakCenterView`. The tab handler shows the right page and the focus box before it calls `set_kind`, which returns early when the kind has not changed (`evolutions_view.cpp:213`). Tests read the tab bar with `findChild<QTabBar*>()`. `tabs_->addTab(tr("Peak center"))` after the kind loop (`:52-56`). The handler at `:95-97` already guards with `std::cmp_less(i, std::size(kKinds))`: add the other branch (show the view, hide `focus_box_`). The sync at `:217` (tab follows kind) must not pull the bar off the peak center tab: do it only when the current index is a kind's. New public members: `on_peak_center_try(const jobs::PeakCenterTryStarted&)`, `on_peak_center_point(const jobs::PeakCenterPoint&)`, `PeakCenterView* peak_center_view() const`; `on_peak_center` also calls the view's `on_done`; `on_run_started` calls the view's `abandon()` and selects the Signal tab; `clear()` (`:106-113`) does not touch the view. New `clear_peak_centers()` calls the view's `clear()`. Each list item carries its index among the kept results in `Qt::UserRole` (decremented as old ones fall out, or held as a running count: the test of 22 decides). The colours map goes to the view's constructor.
- [ ] **Step 3: wiring** in `experiment_window.cpp` after `:164`: `peakCenterTryStarted` and `peakCenterPoint` to the two new slots, and `ExperimentBridge::queueStarted` to `clear_peak_centers()` (test: a second queue in the same window starts with `kept() == 0`).
- [ ] **Step 4: failing test, then green,** in `test_experiment_window.cpp`'s `runsTheExampleQueue` (`:177`), beside the checks at `:235-266`: after the queue, `evo->peak_center_view()->series()` is not empty, its first name is `*H1`, its `marker_x()` has four values, and the tab bar has a `Peak center` tab. The shipped plan already runs a peak center after the run (`configs/examples/plans/sim_multicollect.toml:25-31`).
- [ ] **Step 5:** `ctest --preset dev-ui -R '^ui\.test_(evolutions_view|experiment_window)$'`; `python3 tools/quality_check.py`.
- [ ] **Step 6:** commit `feat(ui): the Evolutions dock has a Peak center tab that opens when one starts`.

### Task 6: The user guide says how to choose additional detectors

**Files:** `configs/examples/peak_center.toml`, `tests/ui/test_experiment_window.cpp`, `docs/user/03-spectrometer.md`

Spec 4.5 wants the example to name "the example spectrometer's other detectors that carry a peak at the reference position". In the shipped simulator none does (see "What the code says"). A detector named anyway would read baseline and noise, and the scaling would stretch that noise over the reference's whole range: a peak drawn where there is none. So the example is left as it is (owner, 2026-10-13): the view and its scaling are tested with data fed by hand, and the example shows the reference alone.

- [ ] `docs/user/03-spectrometer.md`, "Peak centring" (`:294`): the `additional_detectors` key, how to choose (detectors that sit on a peak when the reference is centred), and that a detector with no peak there is drawn as its noise scaled up.
- [ ] `profiles/instrument-common/peak_center.toml` is not changed: a new lab chooses its own (`tests/setup/test_profiles.cpp:127`, `:276` stay as they are).
- [ ] Commit `docs: how to choose a peak center's additional detectors`.

### Task 7: The user guide, and the branch as a whole

**Files:** `docs/user/04-experiments.md` (`:215`, the Evolutions row; `:699`, step 5 of "What a run does"), `docs/user/03-spectrometer.md`, the spec's status line

- [ ] The Evolutions row and a short paragraph: what the Peak center tab draws (the reference heavy and starred, the others scaled onto it so that their shapes can be compared, not their sizes; the markers; the rules; earlier tries faint), that it opens by itself when a peak center starts, that the dock returns to Signal when the next run starts, that a line of the list brings that run's peak center back, and that the last 20 of the queue are kept in memory. A peak center skipped for low signal draws nothing; one that was cancelled keeps its points and says it did not finish.
- [ ] Full `cmake --build --preset dev-ui`, `ctest --preset dev-ui`, `python3 tools/quality_check.py`, `python3 tools/compiler_sweep.py --build-dir build/dev-ui`. Report the counts.
- [ ] Spec status: built, with what section 6 leaves out.
- [ ] Commit `docs: the peak center graph in the experiment window`.

## Not in this plan

Spec section 6: sweep points in the run record and in recall; a peak center from the spectrometer window; an interpolated curve; mass on the x axis. Zoom and save for live plots are T048's.

## Decided by the owner, 2026-10-13

1. The example's `peak_center.toml` is left without `additional_detectors` (Task 6): the simulator gives no other detector a peak in the sweep. The user guide explains the key; the scaling is tested with data fed by hand. A richer example line is T076's.
2. The Evolutions dock returns to Signal when the next run starts (Task 5).
3. A detector is always scaled onto the reference, as legacy did, whatever its range.
