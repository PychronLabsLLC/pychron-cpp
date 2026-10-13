# Experiment Reference Series Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A References dock in the experiment window that shows the recent blanks and airs of the queue's spectrometer as time series, a tab for each, and draws them again when a run is saved.

**Architecture:** Nothing new is computed and no new SQL is written. Each tab is the pipeline a time-series figure window already runs (`select` → `reduce` → `group` → `time_series`), with `select` given a query (analysis types, spectrometer, from, limit) in place of a list of uuids and `group` keyed on the analysis type. It runs on the data workspace's `ProcessingBridge`, on a channel of its own, so a newer request replaces an older one. The bridge learns to bring the source up to date on its worker before a job that asks for it, and to carry the session's time to the units.

**Tech Stack:** C++20, Qt 6 Widgets, `libs/processing` pipeline units, GoogleTest (`tests/processing`, `tests/persistence`), QtTest (`tests/ui`).

**Spec:** `docs/superpowers/specs/2026-10-13-experiment-reference-series-design.md`, agreed in outline by the owner 2026-10-13 (blanks and airs, a live pane). Read it first. Its sections 4, 5.1, 5.2 and 5.4 are corrected in this branch from the code: see below.

## What the code says

Read from `develop` `4098d1e5`.

- The `select` unit already takes a query: `analysis_types`, `mass_spectrometers`, `from`, `limit` and the rest (`libs/processing/src/units_builtin.cpp:40-53`), pages `IAnalysisSource::browse` newest first and stops at `limit` (`:72-93`). So "the newest N blanks of this spectrometer since T" is options, not code.
- The `group` unit has a key `analysis_type`; each group gets a palette colour and a legend entry (`figure_common.cpp:107-120`). Three blank kinds in one figure are three groups with no new code.
- A figure window runs its pipeline on `ProcessingBridge` (`apps/pychron-ui/src/processing_bridge.hpp:33-82`): one worker thread, `submit(channel, pipeline, targets, done)`; a new submit on a channel drops that channel's queued job and cancels its running one (`processing_bridge.cpp:30-49`); the result is delivered on the GUI thread only if it is still the latest (`:109-136`). That is the spec's "one more read, not a queue of them".
- The bridge belongs to the data workspace (`data_workspace.cpp:130`, made when there is a source and a preset store; `MainWindow::processing_bridge()`, `main_window.hpp:133`). The experiment window has neither it nor a source today: `ExperimentWindow(ExperimentBridge&, bool, unique_ptr<QSettings>, QWidget*)`.
- `StoreSource::generation()` moves only in `StoreSource::refresh()` (`store_source.cpp:628-647`), and the one caller today is the data browser, on the GUI thread (`data_browser_window.cpp:418-426`). The dock may not read on the GUI thread, so the refresh has to happen on the bridge's worker.
- The time-series unit takes "now" from `std::chrono::system_clock` (`time_series.cpp:458`); `RunContext` (`units.hpp:41-46`) has no time. A simulated session needs the session's wall clock there (AGENTS.md, "Time").
- The session's wall clock is `LabSession::wall_now()`, reached as `bridge.session().wall_now()` (`experiment_bridge.hpp:48`). The spec named `CoreBridge::wall_now()`, which the experiment window cannot reach.
- The queue names its spectrometer: `QueueSpec::mass_spectrometer` (`run_spec.hpp:61`), copied into each record (`session.cpp:159`); the store filters by that name (`sql/browse.hpp:41`).
- Blank kinds in the tree are five strings, not three: `blank_unknown`, `blank_air`, `blank_cocktail`, `blank_extractionline` (`identifiers.cpp:23-26`) and a bare `blank` that imported data carries (`reference_fit.cpp:748`).
- The Evolutions dock is alone on the right today (`experiment_window.cpp:190`); nothing is tabbed with it.
- `ExperimentBridge::saves_to_database()` (`experiment_bridge.hpp:66-67`, set in `main.cpp:671`) is what tells a queue that saves to a store from one that writes files.
- Indexes over `analysis`: `analysis_ts_ix (timestamp_utc)` and `analysis_ms_type_ts_ix (mass_spectrometer_uuid, analysis_type, timestamp_utc)` (`migrations/sqlite/0001_init.sql:274-275`). The browse statement orders by `a.timestamp_utc DESC, a.uuid DESC LIMIT ?` (`sql/browse.hpp:68`) and filters the spectrometer by name through a join. Whether that plan uses an index for this filter has not been run: Task 1.
- `StoreSource::browse` counts every match on a first page (`store_source.cpp:654`, `count_total = !query.after`).
- No UI test has a store. `SimLab` (`tests/ui/experiment_fixture.hpp:57`) writes record files unless given a persister; `RecordDirectorySource` reads such files as an `IAnalysisSource`.

## Global Constraints

- The dock computes nothing: the scene comes from `libs/processing` and is drawn by `SceneView`.
- No read of the source on the GUI thread, and none on the executor's.
- A failed read leaves the last figure and says so in the header. Nothing here can stop or delay a queue: the dock only listens to `runFinished`.
- A session that does not save to a store has no dock, not an empty one.
- Time shown or compared comes from `bridge.session().wall_now()`. No `QDateTime::currentDateTime()`, no `system_clock::now()` in what this plan adds.
- Read `libs/persistence/AGENTS.md` ("Schema and queries"), `libs/processing/AGENTS.md` and `apps/pychron-ui/AGENTS.md` before the tasks under them. No migration is planned; if Task 1 shows one is needed, stop and report: a migration costs every lab a step.
- A thread is not added. The bridge's worker gains one call (`refresh`), which shares state with the GUI thread: Task 3 is run under `-DPYCHRON_SANITIZE=thread`.
- Commits: Conventional Commits with the component as scope. Before each that changes C++: `python3 tools/quality_check.py` (exit 0), then the tests the task names. UI tasks build with `dev-ui`; before the branch lands, full `cmake --build --preset dev-ui`, `ctest --preset dev-ui`, `python3 tools/compiler_sweep.py --build-dir build/dev-ui`.

## File Structure

| File | Responsibility |
|---|---|
| `tests/persistence/test_schema.cpp` | The plan of the reference query. |
| `tests/processing/test_units.cpp` (or the file that tests `select`), `tests/processing/test_store_source.cpp` | The query's result, on `MemorySource` and on a store. |
| `libs/processing/include/pychron/processing/units.hpp`, `src/time_series.cpp` | `RunContext::now`. |
| `apps/pychron-ui/src/processing_bridge.hpp`, `.cpp` | Refresh on the worker; time handed to the run. |
| `apps/pychron-ui/src/reference_series.hpp`, `.cpp` (new) | `reference_pipeline()`: the pipeline of one tab, a pure function. |
| `apps/pychron-ui/src/reference_series_dock.hpp`, `.cpp` (new) | The dock's widget: header, two tabs, their options. |
| `apps/pychron-ui/src/experiment_window.hpp`, `.cpp`, `main_window.cpp` | The dock, its place, its wiring, settings. |
| `apps/pychron-ui/CMakeLists.txt` | Four new files. |
| `tests/ui/test_reference_series.cpp` (new), `tests/ui/test_experiment_window.cpp`, `tests/ui/test_processing_bridge.cpp` (or where the bridge is tested) | The pipeline, the dock, the window. |
| `docs/user/04-experiments.md` | The References dock. |

---

### Task 1: The query is one the store can answer from an index

**Files:** `tests/persistence/test_schema.cpp`, `tests/processing/test_store_source.cpp`, `tests/processing/flux_seed.hpp` (or a new seed helper beside it)

Nothing shipped changes in this task unless the plan is bad. It comes first because everything after it assumes no migration.

- [ ] **Step 1:** `TEST(SchemaIndexes, TheReferenceSeriesBrowseUsesAnIndexAndDoesNotSortInATempTree)`. Compose the browse statement as `browse.cpp:150-177` does for a filter of five analysis types, one spectrometer name, a `from` time and a limit (include `sql/browse.hpp`; the file's helper `column(*db, "EXPLAIN QUERY PLAN " + sql, "detail", bindings)` is at `:38`). Assert: no `SCAN` of `analysis` (it is `a` in the statement), no `USE TEMP B-TREE FOR ORDER BY`. Do the same for the `COUNT(*)` form (`browse.cpp:155-158`).
- [ ] **Step 2:** run it: `cmake --build --preset dev --target pychron_persistence_tests && build/dev/tests/persistence/pychron_persistence_tests --gtest_filter='SchemaIndexes.*'`. Three outcomes:
  - It passes: go on.
  - The select sorts in a temp tree or scans: the filter by spectrometer name does not reach `analysis_ms_type_ts_ix`. Do not add a migration. Report the plan's text and stop the task: the choice (resolve the name to its uuid first and filter on the column; or accept a sort of at most the matches since the reset) is the owner's, with the timing of step 4 beside it.
  - Only the count scans: the dock does not need the count. Go on, and in Task 4 make the read not ask for it (a `BrowseQuery` flag that `select` sets, or a page with `after` semantics): name what was done.
- [ ] **Step 3:** `StoreSourceTest.TheNewestReferencesOfOneSpectrometerSinceATime` in `tests/processing/test_store_source.cpp`: seed, through a helper that takes the analysis type and the spectrometer (the existing `seed_ingest_monitor`, `flux_seed.hpp:100-110`, writes `"unknown"`: give it a sibling, do not change it), blanks of three kinds, airs, unknowns and a cocktail on one spectrometer and a blank on another, at known times. A `BrowseQuery` of the five blank kinds, that spectrometer, `from` in the middle, `limit` 3 returns the three newest blanks at or after `from`, newest first; none of the others. The same for `air`.
- [ ] **Step 4:** timing, as `libs/persistence/AGENTS.md:152-168` asks for a query paid per run: the browse and the whole `select` + `reduce` for N = 50 on stores of 24, 400 and 2000 analyses, the way `StoreLoadTiming` does it (`tests/processing/test_store_load_timing.cpp:29`, `PYCHRON_BENCH_DB`). The numbers go in the commit message; CI gets the plan test, not a clock.
- [ ] **Step 5:** commit `test(persistence): the newest references of a spectrometer are read from an index`.

### Task 2: A unit can be told what time it is

**Files:** `libs/processing/include/pychron/processing/units.hpp`, `libs/processing/src/time_series.cpp`, `tests/processing/test_time_series.cpp`

- [ ] **Step 1: failing test** `TimeSeriesUnit.UsesTheRunsTimeWhenGiven`: run the unit through the registry with `RunContext::now = 1000.0` on a dataset whose analyses are at 400 and 700 and an x axis relative to now (find the option that makes it so in `time_series_schema()`): the points are at the offsets 1000 gives, not the system clock's. And `...UsesTheSystemClockWhenNot`: with no `now`, the scene equals `build_time_series(d, o, <system now>)` to within a second.
- [ ] **Step 2:** `std::optional<double> now;` in `RunContext` (UTC epoch seconds, as `build_time_series` takes it). `time_series.cpp:458`: `ctx.now.value_or(<system clock>)`; the parameter gets its name back.
- [ ] **Step 3:** grep the other units for `system_clock::now()`; one that uses it for something drawn gets the same line, with a test. One that does not is left.
- [ ] **Step 4:** `build/dev/tests/processing/pychron_processing_tests --gtest_filter='TimeSeries*'`; commit `feat(processing): a pipeline run can be given the time it is run at`.

### Task 3: The bridge refreshes the source on its worker, and carries the time

**Files:** `apps/pychron-ui/src/processing_bridge.hpp`, `.cpp`, the bridge's test in `tests/ui`

```cpp
struct SubmitOptions {
  bool refresh_source = false;         // IAnalysisSource::refresh() on the worker, before the run
  std::optional<double> now;           // RunContext::now
};
void submit(int channel, processing::Pipeline, std::vector<std::string> targets, Callback done,
            SubmitOptions options = {});
```

- [ ] **Step 1:** read how `IAnalysisSource::refresh()` is declared (`source.hpp:79-93`) and what `StoreSource` and `RecordDirectorySource` guard it with. The data browser calls `refresh()` and `browse()` on the GUI thread while the worker calls `browse()` and `load()`: if `refresh()` is not safe against a concurrent `browse()`/`load()` from another thread today, that is a bug this task meets first: write the failing test under TSan and fix it in the source, before the bridge changes.
- [ ] **Step 2: failing tests** with a counting fake source: `refreshIsCalledOnTheWorkerBeforeTheRun` (the fake records the thread id and the order of `refresh` and `browse`; the thread is not the GUI thread); `withoutTheOptionRefreshIsNotCalled` (figure windows are unchanged); `aFailedRefreshIsDeliveredAsTheRunsError`; `theRunsTimeReachesTheUnits`; `anUnchangedSourceIsNotReadAgain`: two submits with `refresh_source` and the same pipeline, generation not moved: `load` is called for the first only (the pipeline's cache keys on the generation, `units.hpp:56`: confirm that the bridge keeps a pipeline's cache between submits of a channel; if it does not, the dock keeps the last generation it drew and does not submit when `refresh` on the worker leaves it unchanged: then this test is of that).
- [ ] **Step 3:** implement; `-DPYCHRON_SANITIZE=thread` build of the bridge's test and of `ui.test_data_windows`.
- [ ] **Step 4:** commit `feat(ui): a processing job can bring the source up to date on the worker first`.

### Task 4: The pipeline of a tab

**Files:** `apps/pychron-ui/src/reference_series.hpp`, `.cpp`, `apps/pychron-ui/CMakeLists.txt`, `tests/ui/test_reference_series.cpp`

```cpp
enum class ReferenceKind { Blanks, Airs };

struct ReferenceQuery {
  ReferenceKind kind = ReferenceKind::Blanks;
  std::string mass_spectrometer;       // empty: any
  int last_n = 50;                     // 5 to 500
  std::optional<double> since;         // UTC epoch s; the reset point
};

[[nodiscard]] std::vector<std::string> analysis_types(ReferenceKind kind);
[[nodiscard]] std::string starting_preset(ReferenceKind kind);   // "Blanks", "Air monitor"
// select (the query) -> reduce -> group (key = analysis_type) -> time_series (figure options).
[[nodiscard]] processing::Pipeline reference_pipeline(const ReferenceQuery& query, const processing::Options& figure);
```

- [ ] **Step 1: failing tests** on a `MemorySource` filled by hand (a helper that makes an analysis of a type, spectrometer and time; `MemorySource` applies the same filter, `libs/processing/src/source.cpp:99`), the pipeline run directly, no bridge:
  - `blanksAreTheNewestNOfEveryBlankKind`: seven blanks of four kinds, `last_n` 5: the dataset has the five newest, oldest first; the scene has one group per kind present, each named by its type.
  - `airsAreAirsOnly`; `unknownsAndCocktailsAreInNeither`.
  - `anotherSpectrometersAreLeftOut`; `anEmptySpectrometerMeansAny`.
  - `sinceLeavesOutWhatIsOlder`: the analysis exactly at `since` is in.
  - `nothingFoundIsASceneThatSaysSo` (`no analyses` warning, not an error).
  - `theFigureOptionsReachTheFigure`: an option of the preset changes the scene.
  - `analysis_types(Blanks)` is the five strings; `analysis_types(Airs)` is `air`.
- [ ] **Step 2:** implement, following `figure_window.cpp:56-88` for the unit names, ports and the `edits` unit if a time-series figure needs it. `select`'s `limit` is `last_n`; its `remove_tags` and `omit_tags` are left at their defaults (an invalid blank is not drawn).
- [ ] **Step 3:** `ctest --preset dev-ui -R '^ui\.test_reference_series$'`; commit `feat(ui): the pipeline that draws the recent blanks or airs of a spectrometer`.

### Task 5: The dock's widget

**Files:** `apps/pychron-ui/src/reference_series_dock.hpp`, `.cpp`, `apps/pychron-ui/CMakeLists.txt`, `tests/ui/test_reference_series.cpp`

```cpp
class ReferenceSeriesDock : public QWidget {
  Q_OBJECT
 public:
  struct Deps {
    ProcessingBridge* bridge = nullptr;
    processing::PresetStore* presets = nullptr;
    std::function<double()> wall_now;                 // UTC epoch s, the session's
    std::function<std::string()> mass_spectrometer;   // the queue's
    QSettings* settings = nullptr;                    // the window's
  };
  explicit ReferenceSeriesDock(Deps deps, QWidget* parent = nullptr);

  void read();            // both tabs; called when first shown, on Refresh, after a saved run
  void reset();           // since = now, then read()
  void set_last_n(int n);

  [[nodiscard]] SceneView* view(ReferenceKind kind) const;
  [[nodiscard]] QString status() const;   // "read 14:02:11", or "could not read: <why>"
};
```

- [ ] **Step 1: failing tests** with a `ProcessingBridge` over a `MemorySource` and a `wall_now` the test sets (wait for a result with `QTRY_VERIFY` on the view's scene, as the figure window's tests do):
  - `showsTheBlanksAndTheAirs`: two blanks and an air in the source: after `read()`, the Blanks view's scene has two points and the Airs view's one.
  - `aFurtherBlankShowsAfterTheNextRead`.
  - `resetEmptiesTheTabsAndTheNextReferenceReturns`: `reset()` at `wall_now` 5000: both scenes say `no analyses`; a blank at 5001 and `read()`: one point.
  - `lastNOneShowsTheNewestOnly`; `lastNIsKeptInSettings` (`experiment_window/reference_series/last_n`) and clamped to 5..500 when read back.
  - `thePresetOfEachTabIsKept`: Blanks starts on `Blanks`, Airs on `Air monitor`; choosing another on one tab writes `experiment_window/reference_series/<blanks|airs>/preset`; a second dock on the same settings starts with it; the other tab is untouched. A preset name that no longer exists falls back to the starting one.
  - `aFailedReadKeepsTheLastFigureAndSaysSo`: a source that fails `browse` after one good read: the scene pointer is the one it was, `status()` begins `could not read`.
  - `aReadWhileOneIsRunningIsFollowedByOneMore`: a source that blocks in `load` until released; three `read()` calls: after release the source saw two runs, not three, and the scene is of the last.
  - `theStatusGivesTheSessionsTime`, not the system's.
  - `nothingIsReadOnTheGuiThread`: the fake source records thread ids.
- [ ] **Step 2: implement.** A `QTabWidget` with two pages, each a `SceneView`; a header row: a `QSpinBox` "Last" (5 to 500), `Reset` ("from now"), `Refresh`, the status label; on each tab's corner an options button that opens a small dialog holding a `PresetBar` and an `OptionsEditor` for `time_series_schema()`, as `figure_window.cpp:126-140` builds them for a dock; a change of options submits that tab again. Each tab has its own channel (`bridge.new_channel()`), submits with `SubmitOptions{.refresh_source = true, .now = wall_now()}` (spell every field: clang 18 warns on a designated initializer that leaves one out). The first `read()` is on the first `showEvent`.
- [ ] **Step 3:** `ctest --preset dev-ui -R '^ui\.test_reference_series$'`; `python3 tools/quality_check.py`; commit `feat(ui): a References pane of recent blanks and airs, with its options and presets`.

### Task 6: The dock in the experiment window

**Files:** `apps/pychron-ui/src/experiment_window.hpp`, `.cpp`, `apps/pychron-ui/src/main_window.cpp`, `tests/ui/test_experiment_window.cpp`

- [ ] **Step 1: failing tests:**
  - `aSessionThatSavesToAStoreHasAReferencesDock`: `bridge.set_saves_to_database(true)` (the existing test at `:421-429` shows how) and `window.set_references(&processing_bridge, &presets)`: a dock with object name `ExperimentReferencesDock`, tabbed with the Evolutions dock, Evolutions in front.
  - `aSessionThatWritesFilesHasNone`: with either missing, `findChild` finds no such dock, and the View menu has no entry for it.
  - `aSavedRunIsFollowedByARead`, `aRunThatWasNotSavedIsNot` (`RunSummary::save_error`), through the bridge's `runFinished`.
  - `theQueueOfBlankAirUnknownBlank`: `SimLab` with a queue of blank, air, unknown, blank, its record files read by a `RecordDirectorySource` under a `ProcessingBridge`, `set_saves_to_database(true)` so that the dock exists: after the queue, Blanks has two points and Airs one. This is the spec's window test (section 8); the source is the record folder because no UI fixture has a store, and what differs with a store (the SQL) is Task 1's.
  - `aLayoutSavedBeforeTheDockStillPlacesIt`: restore a dock layout captured without the dock (`DockLayouts::restore_last`, `experiment_window.cpp:177-179`): the dock is visible and tabbed with Evolutions, not floating or lost.
  - `closingTheDataFirstTakesTheDockAway`: `set_references(nullptr, nullptr)` removes the dock and nothing of it calls the bridge after (teardown order in `main.cpp:690-691` is data first, then the experiment).
- [ ] **Step 2: implement.** `void ExperimentWindow::set_references(ProcessingBridge*, processing::PresetStore*)`; the dock is made when both are there and `bridge_.saves_to_database()`. `default_layout()` (`:187-198`): `tabifyDockWidget(evolutions_dock_, references_dock_)` and raise Evolutions. `Deps::wall_now` from `bridge_.session().wall_now()` (convert as the window already does for the timeline), `mass_spectrometer` from the queue the window holds (`QueueTableModel::queue()`). `runFinished` with `!summary.save_error` calls `read()`, queued, so the store has the run. `MainWindow` calls `set_references(processing_bridge(), presets)` where it makes the experiment window (`set_experiment`, `main_window.cpp:331-352`) and where the data changes (`set_data`).
- [ ] **Step 3:** `ctest --preset dev-ui -R '^ui\.test_(experiment_window|reference_series|main_window)$'`.
- [ ] **Step 4:** commit `feat(ui): the experiment window shows recent blanks and airs beside the evolutions`.

### Task 7: User guide, and the branch as a whole

**Files:** `docs/user/04-experiments.md` (section 3, the table of docks at `:215`), the spec's status line

- [ ] The References dock: what each tab shows, which analyses (the queue's spectrometer, the newest N, since Reset), when it reads (when shown, after each saved run, on Refresh), the options button and presets, that a queue that writes files has no such dock.
- [ ] Full `cmake --build --preset dev-ui`, `ctest --preset dev-ui`, `python3 tools/quality_check.py`, `python3 tools/compiler_sweep.py --build-dir build/dev-ui`; the TSan run of Task 3 again on the finished branch. Report the counts.
- [ ] Commit `docs: the References dock of the experiment window`.

## Not in this plan

Spec section 7: legacy's autoplot; unknowns, cocktails and other reference types in the dock; figure windows that follow the store; alarms on a drifting reference.

## Questions for the owner

None blocks Tasks 1 to 5. Choices made here that the owner may want otherwise:

1. Blanks holds five kinds, not the spec's three: `blank_extractionline` and the bare `blank` of imported data are blanks too.
2. The dock is tabbed behind Evolutions (which sits alone on the right today).
3. The reads share the one worker of the data windows' figures: a long figure job delays the dock's redraw, never the queue. A worker of its own is a second connection to the store; not taken.
4. A queue that names no spectrometer shows every spectrometer's references.
5. The window test reads the simulated queue's record files, not a store: no UI fixture has a store, and building one is larger than this feature. The store's part is tested in `tests/persistence` and `tests/processing`.
