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
- `RunContext` is made inside `Runner::run` (`libs/processing/src/units.cpp:299`); a node is run again only when its fingerprint changes, and that holds its options, its inputs and, for a unit that reads the source, the generation (`units.cpp:269-282`). Time is in neither. The bridge keeps one `Runner`, with its cache, per channel.
- The only thing the time-series unit does with "now" is the x axis when `x.kind = relative` and `x.origin = now` (`time_series.cpp:58`, `:263`). The default is `x.kind = time`, and neither the `Blanks` nor the `Air monitor` preset changes it: by default the dock draws against absolute time and "now" does not enter.
- `select` pages 500 rows at a time whatever its limit (`units_builtin.cpp:86`), and passes `exclude_tags = {"invalid"}` by default, which adds the tag joins to the statement (`sql/browse.hpp:34-36`).
- `IAnalysisSource::refresh()` is on the interface (`source.hpp:84`) and safe against a concurrent `browse` or `load` in both sources (`StoreSource`: `refresh_mutex`, cache under `cache_mutex`, atomic generation; `RecordDirectorySource`: its mutex). `StoreSource::refresh` empties the whole analysis cache when anything changed (`store_source.cpp:641-643`): after each saved run both tabs load their N analyses again.
- `RunFinished` may come before the store has the run: `SavePipeline` can post the hand-off, and `Saved::pending` is then empty with the outcome not yet known (`persister.hpp:98-135`). The data browser's refresh has the same gap today.
- `saves_to_database()` is true whenever the session has a persister, the `no_database` one included (`main.cpp:660-671`): it does not mean there is a store to read.
- The experiment window is made lazily (`MainWindow::ensure_experiment_window()`, `main_window.cpp:353-365`), after its constructor has run `default_layout()` and `restore_last()` (`experiment_window.cpp:177-179`). `DataWorkspace::set_source` destroys the bridge (`data_workspace.cpp:125-130`). Docks are listed under Window > Panels, read when the menu is built (`dock_layouts.cpp:60-74`).
- The example queues have no "blank, air, unknown, blank": `experiment.toml` is `bu`, `66001`, `66001`; `experiment.sim-air.toml` is blanks and airs only.
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
| the file under `tests/processing` that tests the `select` unit (find it: `grep -l '"select"' tests/processing`), `tests/processing/test_store_source.cpp` | The query's result, on `MemorySource` and on a store. |
| `libs/processing/include/pychron/processing/units.hpp`, `src/units.cpp`, `src/units_builtin.cpp`, `src/time_series.cpp` | `RunContext::now`, given to `Runner::run`; a unit that uses it says so; `select` asks for no more rows than its limit. |
| `apps/pychron-ui/src/processing_bridge.hpp`, `.cpp` | Refresh on the worker; time handed to the run. |
| `apps/pychron-ui/src/reference_series.hpp`, `.cpp` (new) | `reference_pipeline()`: the pipeline of one tab, a pure function. |
| `apps/pychron-ui/src/reference_series_dock.hpp`, `.cpp` (new) | The dock's widget: header, two tabs, their options. |
| `apps/pychron-ui/src/experiment_window.hpp`, `.cpp`, `main_window.cpp` | The dock, its place, its wiring, settings. |
| `apps/pychron-ui/CMakeLists.txt` | Four new files. |
| `tests/ui/test_reference_series.cpp` (new), `tests/ui/test_experiment_window.cpp`, `tests/ui/test_data_windows.cpp` (where the bridge is exercised today) | The pipeline, the dock, the window. |
| `docs/user/04-experiments.md` | The References dock. |

---

### Task 1: The query is one the store can answer from an index

**Files:** `tests/persistence/test_schema.cpp`, `tests/processing/test_store_source.cpp`, `tests/processing/flux_seed.hpp` (or a new seed helper beside it)

Nothing shipped changes in this task unless the plan is bad. It comes first because everything after it assumes no migration.

- [x] **Step 1:** `TEST(SchemaIndexes, TheReferenceSeriesBrowseUsesAnIndexAndDoesNotSortInATempTree)`. Compose the browse statement as `browse.cpp:150-177` does for the dock's real filter: five analysis types (Blanks) and one (`air`, a second case), one spectrometer name, a `from` time, `exclude_tags = {"invalid"}` and a limit (include `sql/browse.hpp`; the file's helper `column(*db, "EXPLAIN QUERY PLAN " + sql, "detail", bindings)` is at `:38`). The index `(mass_spectrometer_uuid, analysis_type, timestamp_utc)` cannot give time order across five types, so two shapes are good and are what the test accepts: a search on that index (one type), or a walk of `analysis_ts_ix` backwards that stops at the limit (`SCAN a USING INDEX analysis_ts_ix`, no temp tree). What it refuses: a `SCAN` of `analysis` with no index, and `USE TEMP B-TREE FOR ORDER BY` over every match. Do the same for the `COUNT(*)` form (`browse.cpp:155-158`).
- [x] **Step 2:** run it: `cmake --build --preset dev --target pychron_persistence_tests && build/dev/tests/persistence/pychron_persistence_tests --gtest_filter='SchemaIndexes.*'`. Three outcomes:
  - It passes: go on.
  - The select sorts in a temp tree or scans: the filter by spectrometer name does not reach `analysis_ms_type_ts_ix`. Do not add a migration. Report the plan's text and stop the task: the choice (resolve the name to its uuid first and filter on the column; or accept a sort of at most the matches since the reset) is the owner's, with the timing of step 4 beside it.
  - Only the count scans: the dock does not need the count. Go on, and in Task 4 make the read not ask for it (a `BrowseQuery` flag that `select` sets, or a page with `after` semantics): name what was done.
- [x] **Step 3:** `StoreSourceTest.TheNewestReferencesOfOneSpectrometerSinceATime` in `tests/processing/test_store_source.cpp`: seed, through a helper that takes the analysis type and the spectrometer (the existing `seed_ingest_monitor`, `flux_seed.hpp:100-110`, writes `"unknown"`: give it a sibling, do not change it), blanks of three kinds, airs, unknowns and a cocktail on one spectrometer and a blank on another, at known times. A `BrowseQuery` of the five blank kinds, that spectrometer, `from` in the middle, `limit` 3 returns the three newest blanks at or after `from`, newest first; none of the others. The same for `air`.
- [x] **Step 4 (done 2026-10-14 by `StoreLoadTiming.ReferenceSeries`, numbers in its commit; the store had no airs, so the air tab was timed empty and unknowns stood in for size):** timing, as `libs/persistence/AGENTS.md:152-168` asks for a query paid per run: the browse and the whole `select` + `reduce` for N = 50, with the source's cache empty (a refresh after a saved run empties it: that is the dock's case), for one tab and for both, on stores of 24, 400 and 2000 analyses, the way `StoreLoadTiming` does it (`tests/processing/test_store_load_timing.cpp:29`, `PYCHRON_BENCH_DB`). The numbers go in the commit message; CI gets the plan test, not a clock.
- [x] **Step 5:** commit `test(persistence): the newest references of a spectrometer are read from an index`.

  Found while timing: `select` given an empty `uuids` list runs its query instead, and with no query options that is the newest 5000 analyses of the store (12.6 s on the 8716-analysis copy). Task 4's dock gives `select` the tab's query (types, spectrometer, `from`, `limit`), never the uuids of a browse that may be empty.

### Task 2: A run can be told what time it is, and `select` reads no more than it is asked for

**Files:** `libs/processing/include/pychron/processing/units.hpp`, `libs/processing/src/units.cpp`, `src/units_builtin.cpp`, `src/time_series.cpp`, `tests/processing/test_time_series.cpp`, the test file of the pipeline runner and of `select`

By default the dock draws against absolute time and this changes nothing it shows; it is for the operator who chooses `x.kind = relative`, `x.origin = now`, and it removes a read of the system clock from a path a simulated session uses.

- [x] **Step 1: failing tests:**
  - `TimeSeriesUnit.UsesTheRunsTimeWhenGiven`: `x.kind = relative`, `x.origin = now`, analyses at 400 and 700, the run's time 1000: the points are at the offsets 1000 gives. `...UsesTheSystemClockWhenNot`: with none, the scene equals `build_time_series(d, o, <system now>)` to within a second.
  - `Runner.ALaterTimeRunsATimeDependentNodeAgain`: the same pipeline, source and options run twice with times 1000 and 2000: the second scene is drawn at 2000, not taken from the cache. `Runner.ALaterTimeDoesNotRunTheOthersAgain`: `select` and `reduce` are not run again (a counting source), and neither is a `time_series` node whose options do not use now.
  - `SelectUnit.AsksForNoMoreRowsThanItsLimit`: limit 5 on a source that records each `BrowseQuery`: one page, `limit` 5.
- [x] **Step 2:** `std::optional<double> now;` in `RunContext` (UTC epoch seconds). `Runner::run` takes it (a defaulted parameter or a setter, as fits `units.hpp:121`) and puts it in the context (`units.cpp:299`). `Unit` gains `virtual bool uses_time(const Options&) const { return false; }`; the time-series unit answers from its `x.kind` and `x.origin`; the fingerprint (`units.cpp:269-282`) includes `now` for a node whose unit says yes. `time_series.cpp:458`: `ctx.now.value_or(<system clock>)`. `select`: `q.limit = std::min<std::size_t>(limit, 500)`.
- [x] **Step 3:** grep the other units for `system_clock::now()`; one that uses it for something drawn gets the same treatment, with a test.
- [x] **Step 4:** `build/dev/tests/processing/pychron_processing_tests`; commit `feat(processing): a pipeline run can be given the time it is run at`.

  Built: `Runner::run` takes `now` as a defaulted fourth parameter, after `cancel`. Step 3 found one other read of the system clock, the publication report's "generated (UTC)" stamp (`report.cpp`), which is not a unit and draws nothing: left as it is.

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

- [ ] **Step 1:** `refresh()` is safe against a concurrent `browse()`/`load()` by reading (see "What the code says"); confirm it under TSan with a test that refreshes from one thread while another loads, for both sources, before the bridge changes.
- [ ] **Step 2: failing tests** with a counting fake source: `refreshIsCalledOnTheWorkerBeforeTheRun` (the fake records the thread id and the order of `refresh` and `browse`; the thread is not the GUI thread); `withoutTheOptionRefreshIsNotCalled` (figure windows are unchanged); `aFailedRefreshIsDeliveredAsTheRunsError`; `theRunsTimeReachesTheUnits` (two submits with a later time and a time-dependent figure: the second scene is the later one); `anUnchangedSourceIsNotReadAgain`: two submits with `refresh_source` and the same pipeline, generation not moved: `load` is called for the first only (the channel's `Runner` keeps its cache between submits).
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
- [ ] **Step 2:** implement, following `figure_window.cpp:56-88` for how a pipeline is put together (`pipeline.add(registry, kind, id, inputs)`, then options set on the node; `UnitRegistry::builtin()`). The chain is `select`, `reduce`, `group` (key `analysis_type`), `time_series`: no graph-level `group` and no `edits` unit. The figure node's id is a named constant the dock submits as its target. `select`'s `limit` is `last_n`; its `remove_tags` and `omit_tags` are left at their defaults (an invalid blank is not drawn).
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
  - `showingItReadsOnceAndShowingItAgainDoesNot`.
  - `theRefreshButtonReads` and `theResetButtonResets` (clicks, found by object name).
  - `aReadThatFindsNothingNewDrawsNothingAgain`: generation not moved: the view's scene pointer is the one it was.
  - `theOptionsButtonOpensTheEditorAndAChangeDrawsThatTabAgain`: the dialog has a `PresetBar` and an `OptionsEditor`; a changed option gives that tab a new scene and leaves the other's alone.
  - `theResetPointIsNotKept`: a second dock on the same settings shows what was before the reset.
  - `aRunTheStoreDoesNotHaveYetIsReadForAgain`: see step 2; a source that gains the analysis only after the first read: the point appears without a further call from the test.
  - `destroyingItWithAReadInFlightIsSafe`: delete the dock while the source blocks in `load`, release, process events: no callback runs.
- [ ] **Step 2: implement.** A `QTabWidget` with two pages, each a `SceneView`; a header row: a `QSpinBox` "Last" (5 to 500), `Reset` ("from now"), `Refresh`, the status label; on each tab's corner an options button that opens a small dialog holding a `PresetBar` and an `OptionsEditor` for `time_series_schema()`, as `figure_window.cpp:126-140` builds them for a dock; a change of options submits that tab again. Each tab has its own channel (`bridge.new_channel()`), submits with `SubmitOptions{.refresh_source = true, .now = wall_now()}` (spell every field: clang 18 warns on a designated initializer that leaves one out). The first `read()` is on the first `showEvent`. The destructor releases its channels (`bridge.release(channel)`) and each callback is guarded (a `QPointer` to the dock). After a saved run the dock reads, and if that read's generation was not newer than the one before, it reads again after 2 s and after 10 s of the session's time, then stops: `RunFinished` can come before the store has the run. The wait is a single-shot started from the session clock's pace (`Deps` gains `std::function<void(std::chrono::seconds, std::function<void()>)> later`, which the window fills from what the timeline already uses to tick on the session's clock, and tests call by hand); it is for a redraw, never for correctness, and `Refresh` is always there. Reset takes `wall_now()` when pressed: a run that began before it and is saved after it is left out, since an analysis's time is its start; say so in the user guide.
- [ ] **Step 3:** `ctest --preset dev-ui -R '^ui\.test_reference_series$'`; `python3 tools/quality_check.py`; commit `feat(ui): a References pane of recent blanks and airs, with its options and presets`.

### Task 6: The dock in the experiment window

**Files:** `apps/pychron-ui/src/experiment_window.hpp`, `.cpp`, `apps/pychron-ui/src/main_window.cpp`, `tests/ui/test_experiment_window.cpp`

- [ ] **Step 1: failing tests:**
  - `aWindowGivenReferencesHasTheDock`: `window.set_references(&processing_bridge, &presets)`: a dock with object name `ExperimentReferencesDock`, tabbed with the Evolutions dock, Evolutions in front, and listed under Window > Panels.
  - `aWindowNotGivenThemHasNone`: `findChild` finds no such dock, and Window > Panels has no entry for it.
  - `MainWindow` gives them only when the data source is the store the session saves to: with the records folder as the data source (no database, or one that could not be opened) the window gets none. In `tests/ui/test_main_window.cpp` or where `set_data` is tested.
  - `aSavedRunIsFollowedByARead`, `aRunThatWasNotSavedIsNot` (`RunSummary::save_error`), through the bridge's `runFinished`.
  - `theQueueOfBlankAirUnknownBlank`, in `tests/ui/test_reference_series.cpp` (not in `test_experiment_window`, which already takes 155 to 170 s of its 300 s on CI's macOS runner): `SimLab` with a `QueueSpec` built by hand, identifiers `bu`, `a`, `66001`, `bu` (no example queue has that order), its record files read by a `RecordDirectorySource` on the folder the session writes (`<data>/records`) under a `ProcessingBridge` given to the window: after the queue, with no call from the test, Blanks has two points and Airs one, and the unknown's uuid is in neither scene; a fifth run, a blank, adds a point the same way. This is the spec's window test (section 8); the source is the record folder because no UI fixture has a store, and what differs with a store (the SQL) is Task 1's. Without embedded Python the `sim_air` script is dropped (`experiment_fixture.hpp:49-52`): the air run is still typed `air` by its identifier, which is all the dock reads.
  - `aLayoutSavedWithTheDockPlacesItAgain`: float or hide the dock, save the layout, make a second window on the same settings and give it references: the dock is as it was left.
  - `aLayoutSavedBeforeTheDockStillPlacesIt`: restore a dock layout captured without the dock (`DockLayouts::restore_last`, `experiment_window.cpp:177-179`): the dock is visible and tabbed with Evolutions, not floating or lost.
  - `closingTheDataFirstTakesTheDockAway`: with a read in flight, `set_references(nullptr, nullptr)` removes the dock and nothing of it calls the bridge after (teardown order in `main.cpp:690-691` is data first, then the experiment).
- [ ] **Step 2: implement.** `void ExperimentWindow::set_references(ProcessingBridge*, processing::PresetStore*)`; the dock is made when both are there. The window's constructor has already run `default_layout()` and `restore_last()` by then, so `set_references` places the dock itself (`tabifyDockWidget(evolutions_dock_, references_dock_)`, Evolutions raised), then restores the last layout again so that a layout saved with the dock applies, and rebuilds Window > Panels (`dock_layouts.cpp:60-74` reads the docks when the menu is built). `default_layout()` (`:187-198`) does the same tabbing when the dock exists, so Reset Layout works. `Deps::wall_now` from `bridge_.session().wall_now()` (a `WallTime`: `std::chrono::duration<double>(t.time_since_epoch()).count()`; `executor_pane.cpp:330` does the like in milliseconds), `mass_spectrometer` from the queue the window holds (`QueueTableModel::queue()`). `runFinished` with `!summary.save_error` calls `read()`, queued, so the store has the run. `MainWindow` calls `set_references(processing_bridge(), presets)` in `ensure_experiment_window()` (`main_window.cpp:353-365`, where the window is made) and in `set_data` for a window that exists; `set_data` calls `set_references(nullptr, nullptr)` before `data_->set_source`, which destroys the bridge (`data_workspace.cpp:125-130`). It passes them only when the data source is the session's store: `main.cpp` knows (`store_source` at `:470-484` against the records-folder fallback at `:581`) and tells `MainWindow` with a flag on `set_data`; `saves_to_database()` is not that (true for the `no_database` persister too).
- [ ] **Step 3:** `ctest --preset dev-ui -R '^ui\.test_(experiment_window|reference_series|main_window)$'`.
- [ ] **Step 4:** commit `feat(ui): the experiment window shows recent blanks and airs beside the evolutions`.

### Task 7: User guide, and the branch as a whole

**Files:** `docs/user/04-experiments.md` (section 3, the table of docks at `:215`), the spec's status line

- [ ] The References dock: what each tab shows, which analyses (the queue's spectrometer, the newest N, since Reset), when it reads (when shown, after each saved run, on Refresh), the options button and presets, that a queue that writes files has no such dock.
- [ ] Full `cmake --build --preset dev-ui`, `ctest --preset dev-ui`, `python3 tools/quality_check.py`, `python3 tools/compiler_sweep.py --build-dir build/dev-ui`; the TSan run of Task 3 again on the finished branch. Report the counts.
- [ ] Commit `docs: the References dock of the experiment window`.

## Not in this plan

Spec section 7: legacy's autoplot; unknowns, cocktails and other reference types in the dock; figure windows that follow the store; alarms on a drifting reference.

## Decided by the owner, 2026-10-13

1. Blanks holds five kinds: `blank_unknown`, `blank_air`, `blank_cocktail`, `blank_extractionline` and the bare `blank` of imported data. (The blank fit window's own list, `reference_fit.cpp:748`, has four: no `blank_extractionline`.)
2. A run saved a moment after its `RunFinished` is caught by reading again 2 s and 10 s later (Task 5). When the executor hardening work lands its `SpoolStatus`, that event replaces the two re-reads, here and for the data browser.
3. The dock exists only when the data source is the store the session saves to; otherwise there is none (Task 6).
4. A queue that names no spectrometer shows every spectrometer's references.

Kept as planned, with no objection: the dock is tabbed behind Evolutions; the reads share the one worker of the data windows' figures; the window test reads the simulated queue's record files, the store's part being tested in `tests/persistence` and `tests/processing`.

Still to watch, not a decision: after each saved run both tabs load their N analyses again, since a refresh empties the source's cache; the cost is Task 1's numbers. If it is too much at N = 500, the cure is in `StoreSource::refresh` (drop only what changed), not in the dock.
