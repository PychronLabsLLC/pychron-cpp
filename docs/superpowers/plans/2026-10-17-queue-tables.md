# Experiment window: Executed and Queue tables: implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Split the experiment window's one queue table into an Executed table and a Queue table over the one
`QueueTableModel`, add Reset, Clear executed and Repeat last run, and offer to resume a queue from
`executor_state.json` (row T024).

**Architecture:** The model gains one number, `executed_rows()`: rows below it are shown in Executed, rows from it
on in Queue. Rows never move in the model; two `QSortFilterProxyModel` subclasses split it for two `QTableView`s
in a vertical `QSplitter`. The library gains a JSON reader of the executor's state file (`Executor::read_state`);
the window turns what it reads into an offer (`resume_offer`, a pure function) and, if taken, sets the boundary
and the earlier statuses and starts.

**Tech Stack:** C++20, Qt 6 Widgets (6.4 floor), Qt Test (`tests/ui`), GoogleTest (`tests/experiment`),
nlohmann_json (linked privately by `libs/experiment` from Task 1 on; before it only `experiment_store` linked it).

**Spec:** `docs/superpowers/specs/2026-10-13-queue-tables-design.md` (survey and legacy analysis:
`docs/superpowers/specs/2026-10-13-queue-tables-survey.md`).

## Global Constraints

- One model: no second list, no row moved between views by the model; the boundary is a number (spec 6).
- Executor, `LabSession`, edit versioning and the queue file format unchanged; `executor_state.json` format
  unchanged (spec 5, 6). Only a reader is added.
- Only rows of the Queue view are edited or acted on by row actions (spec 6).
- Every action refused while running says so; nothing is silently ignored (spec 6). Menu text of the refusal:
  `A queue is running.`; `QString* error` text of the window's methods: `a queue is running` (as `load_queue`).
- Qt: nothing newer than 6.4 without `QT_VERSION_CHECK`; refilter with `invalidateFilter()`, not
  `beginFilterChange` (6.9) (spec 3).
- QSettings keys exactly `experiment_window/executed_collapsed` and `experiment_window/tables_splitter`; the
  tables are not part of `DockLayouts` (spec 3).
- `row_editable(row) = !locked_ && row >= max(frozen_rows(), executed_rows())` (spec 2).
- Errors are `pychron::Result<T>`; no exceptions out of `read_state` (parse with
  `nlohmann::json::parse(text, nullptr, false)` and check `is_discarded()`; gcc warns on a dropped `json::parse`
  result, so keep it).
- Existing tests reading the one table switch to the Queue view; none dropped (spec 7).
- Inner loop per `AGENTS.md`: build one target, run its binary filtered. UI targets build under preset `dev-ui`
  (`build/dev-ui/tests/ui/pychron_ui_<test>`), `tests/experiment` under `dev`
  (`build/dev/tests/experiment/pychron_experiment_tests`). After each C++ task: `python3 tools/quality_check.py`
  and `python3 tools/compiler_sweep.py --changed` (`--build-dir build/dev-ui` for `apps/pychron-ui`, `tests/ui`).

## Decisions this plan takes where the spec is silent or cannot hold as written

Each is written back into the spec by the task that makes it, so the two never disagree.

1. **Identifier check on resume (spec 5).** The file's `runs[]` lists only runs that started, so it has no
   identifier for row `next_row` itself. The check compares every `runs[]` entry's `identifier` with the open
   queue's identifier at that entry's `row` (a row past the queue's end counts as a mismatch). First mismatch
   disables Resume with the line `Row %1 is %2 now; the file has %3.` (1-based row). An empty `runs[]` has nothing
   to compare and is resumable. Task 6.
2. **Row numbers in the dialog** are the `#` column's (1-based): "stopped at row N" shows `next_row + 1`. Task 6.
3. **Where the offer is made.** `load_queue()` does not offer (tests and programs call it). A public
   `offer_resume()` does; it is called by `open_dialog()` after a successful load, by `MainWindow` after the
   `--queue` load, and by Queue > Resume.... It offers nothing while running or when `executed_rows() > 0`: that
   is the window that just stopped (spec 5, "not offered right after a Stop"). Task 6.
4. **Statuses from the file.** For a row listed twice in `runs[]` (a held run's failed attempt, then the run) the
   last entry wins. A state string `run::run_state_from` does not know leaves the row without a status. Task 6.
5. **Nothing left to start.** `QueueTableModel::runnable()` also needs a row at or past `executed_rows()`. After
   a queue runs to its end Start is disabled until Reset; `runsTheExampleQueue`'s last `start_enabled()`
   assertion becomes `!start_enabled()`, then true after `reset_executed()`. Task 2 (model), Task 4 (window test).
6. **Row operations on executed rows.** `adopt()` refuses any edit that changes a row below
   `max(frozen_, executed_)`, live or not (today: below `frozen_` while live), with the existing message
   `rows the executor has reached cannot change`. `clear_executed()` is the one operation past that guard.
   Task 2.
7. **Start's statuses.** Start clears only statuses of rows `>= executed_rows()` (`clear_status(from)`), so
   Executed keeps what it showed after a Stop. Task 2 (model), Task 4 (window).
8. **Repeat** lives in the model (`repeat(rows)`), returning the inserted rows; copies keep source order and are
   inserted at `insert_position(executed_rows())`. With no executed run it is refused: menu text
   `No run has been executed.`. Task 3 (model), Task 5 (window).
9. **Names.** `ExperimentWindow::table()` becomes `queue_table()`; `executed_table()` is added (no test uses
   `table()` today). `cancelAsksFirstAndStartsFromTheSelectedRow` is renamed
   `cancelAsksFirstAndStartsAtTheFirstNotExecutedRow` (its start half changes meaning, spec 7). Task 4.
10. **Resume starts the queue** at once ("then Start as usual", spec 5): the window calls `start()` after setting
    the boundary. Task 6.

## Review Focus

1. The executor's own queue edit (`QueueEdited`: model reset with `keep_status`) while live must leave both
   views split at the same boundary, not show every row in Queue until the next frontier event. Test in Task 3.
2. Repeat while live against a stale version: the committer refuses, `editRefused` says why, nothing is
   inserted. Test in Task 3.
3. A state file that is not JSON, or has no `next_row`, at Open: no dialog, no error box, nothing changes. Test in
   Task 6.
4. A queue shortened since the crash (rows deleted, so `next_row >= ` row count): not offered. Test in Task 6.
5. Repeat with nothing executed: refused with a message, never a silent no-op or a copy of a Queue row. Test in
   Task 5.

---

### Task 1: `Executor::read_state` and `run::run_state_from`

**Files:**
- Modify: `libs/experiment/include/pychron/experiment/executor/executor.hpp` (beside `resume_row`, line 241)
- Modify: `libs/experiment/src/executor/executor.cpp` (`resume_row`, line 672: regex reader replaced)
- Modify: `libs/experiment/CMakeLists.txt` (`nlohmann_json` linked `PRIVATE` to `pychron_experiment`)
- Modify: `libs/experiment/include/pychron/experiment/run/state.hpp`, `libs/experiment/src/run/state.cpp`
- Test: `tests/experiment/test_executor.cpp`, `tests/experiment/test_run.cpp`

**Interfaces:**
- Produces (namespace `pychron::experiment::executor`):
  ```cpp
  struct ExecutorStateRun {
    std::size_t row = 0;
    std::string identifier, run_id, state;  // state: run::to_string of the run's state
  };
  struct ExecutorStateFile {
    std::string queue;  // "" when absent
    std::string state;  // to_string(ExecutorState) as written; "" when absent
    std::size_t next_row = 0;
    std::vector<ExecutorStateRun> runs;
  };
  // in class Executor:
  static Result<ExecutorStateFile> read_state(const std::filesystem::path& state_file);
  static Result<std::size_t> resume_row(const std::filesystem::path& state_file);  // = read_state(...)->next_row
  ```
- Produces (namespace `pychron::experiment::run`):
  `std::optional<RunState> run_state_from(std::string_view text) noexcept;` (inverse of `to_string(RunState)`).

- [x] **Step 1: Write the failing tests**

  In `test_executor.cpp`:
  - `ExecutorTest.ReadStateGivesWhatWriteStateWrote`: the abort scenario of the existing resume test (three
    unknowns, abort on the third reading). Then `auto s = Executor::read_state(lab_.dir() / "executor_state.json")`;
    `ASSERT_TRUE(s)`; `EXPECT_EQ(s->queue, q.spec().name)`; `EXPECT_EQ(s->state, "finalizing")`;
    `EXPECT_EQ(s->next_row, *Executor::resume_row(...))`; `ASSERT_EQ(s->runs.size(), r.runs.size())`; for each
    `i`: `row`, `identifier`, `run_id` equal `r.runs[i]`'s and `state == run::to_string(r.runs[i].state)`.
  - `ExecutorTest.ReadStateOfAnOlderFileGivesItsNextRow`: file `{"next_row": 4}` gives `next_row == 4`,
    `queue == ""`, `state == ""`, `runs.empty()`.
  - `ExecutorTest.ReadStateRefusesWhatIsNotAStateFile`: missing file → error kind `ErrorKind::Io`; `not json` and
    `{"queue": "q"}` and `{"next_row": -1}` → `ErrorKind::Config`, the second's text contains `no next_row`.

  In `test_run.cpp`: `RunState.FromStringIsToStringsInverse`: for every `RunState` from `Pending` to `Aborted`,
  `run_state_from(to_string(s)) == s`; `run_state_from("nope")` is `std::nullopt`.

- [x] **Step 2: Run them to see them fail**

  `cmake --build --preset dev --target pychron_experiment_tests` fails to compile (`read_state`, `run_state_from`
  undeclared).

- [x] **Step 3: Implement**

  `read_state`: read the file (Io error if it cannot be opened), `nlohmann::json::parse(text, nullptr, false)`;
  not an object or no unsigned integer `next_row` → Config error `<path>: no next_row`. `queue`, `state` read when
  strings; `runs` when an array, each entry's fields read when of the right type. `resume_row` returns
  `read_state(state_file)` mapped to `next_row`; drop the `<regex>` include if nothing else uses it.
  `run_state_from`: loop over the enum comparing with `to_string`.

- [x] **Step 4: Run them to see them pass, and the existing readers**

  `build/dev/tests/experiment/pychron_experiment_tests --gtest_filter='ExecutorTest.*:ExecutorHolding_.*:RunState.*'`
  all pass (`TheStateFileCarriesTheHoldAndTheLastFailedRow` still reads `1` from both files). Then
  `elctl exp run --resume` is unchanged: `ctest --preset dev-quick -R elctl`.

- [x] **Step 5: Checks and commit**

  `python3 tools/quality_check.py`; `python3 tools/compiler_sweep.py --changed`.
  `git commit -m "feat(experiment): Executor::read_state reads the executor's state file as JSON"`

---

### Task 2: the executed boundary in `QueueTableModel`

**Files:**
- Modify: `apps/pychron-ui/src/queue_table_model.hpp`, `apps/pychron-ui/src/queue_table_model.cpp`
- Modify: `docs/superpowers/specs/2026-10-13-queue-tables-design.md` (section 2: decisions 5, 6, 7)
- Test: `tests/ui/test_queue_table_model.cpp`

**Interfaces:**
- Produces (class `QueueTableModel`):
  ```cpp
  std::size_t executed_rows() const noexcept;
  bool row_editable(std::size_t row) const noexcept;   // !locked_ && row >= max(frozen_rows(), executed_rows())
  std::size_t insert_position(std::size_t at) const noexcept;  // max(at, frozen_rows(), executed_rows())
  bool runnable() const;              // check_.ok() && queue_.size() > executed_rows()
  void clear_status(std::size_t from = 0);  // statuses of rows >= from, and their run_rows_ entries
  signals: void executedChanged(int rows);  // emitted only when the number changes
  ```
  Member `std::size_t executed_ = 0;`. `set_live(from, …)` sets it to `from`; `set_frozen(n)` (when it advances)
  sets it to `n`; `end_live()` leaves it; `set_queue(spec, false)` sets it to 0, `set_queue(spec, true)` keeps it.
  `executedChanged` is emitted after `frozenChanged` and after the model's state is updated.

- [x] **Step 1: Write the failing tests**

  - `executedFollowsTheFrontierAndOutlivesTheQueue`: `set_live(0, accept-all committer)`; `QSignalSpy` on
    `executedChanged`; `set_frozen(1)` → `executed_rows() == 1`, spy count 1, last argument 1; `set_frozen(1)`
    again → spy count still 1; `set_frozen(2)`; `end_live()` → `frozen_rows() == 0`, `executed_rows() == 2`;
    `flags(index(1, Identifier))` lacks `ItemIsEditable`, `flags(index(2, Identifier))` has it;
    `insert_position(0) == 2`; `move_up({2})` false; `remove({0})` false; `toggle_skip({1})` false and
    `editRefused` said `reached`; `set_queue(example_queue)` → `executed_rows() == 0`, spy's last argument 0.
  - `aQueueEditKeepsTheBoundary`: live, `set_frozen(1)`, `on_queue_edited({queue, version 5, frozen 1})` →
    `executed_rows() == 1`.
  - `clearStatusFromKeepsTheExecutedRows`: feed `on_run_finished` for rows 0 and 1, `clear_status(1)` →
    `status(0)` is `Success`, `status(1)` empty.
  - `runnableNeedsARowNotExecuted`: live, `set_frozen(3)` (all rows), `end_live()` → `!runnable()`.

- [x] **Step 2: Run them to see them fail**

  `cmake --build --preset dev-ui --target pychron_ui_test_queue_table_model` fails (`executed_rows`,
  `executedChanged` undeclared).

- [x] **Step 3: Implement as the Interfaces block says**

  `adopt()`'s kept-rows check runs over `max(frozen_rows(), executed_)` rows, live or not (decision 6), with the
  existing `editRefused` text. Write decisions 5, 6, 7 into spec section 2 (compressed, as the spec is).

- [x] **Step 4: Run the model's tests**

  `build/dev-ui/tests/ui/pychron_ui_test_queue_table_model` passes, old cases included
  (`liveEditsGoThroughTheCommitter`, `setConditionalsFollowsTheFrozenRows`, `statusFollowsRunEvents`).

- [x] **Step 5: Checks and commit**

  `python3 tools/quality_check.py`; `python3 tools/compiler_sweep.py --changed --build-dir build/dev-ui`.
  `git commit -m "feat(ui): the queue model keeps the rows the executor reached as executed"`

---

### Task 3: model operations (Reset, Clear executed, Repeat, resume) and the two proxies

**Files:**
- Modify: `apps/pychron-ui/src/queue_table_model.hpp`, `apps/pychron-ui/src/queue_table_model.cpp`
- Create: `apps/pychron-ui/src/queue_views.hpp`, `apps/pychron-ui/src/queue_views.cpp`
- Modify: `apps/pychron-ui/CMakeLists.txt` (both source lists, beside `queue_table_model`)
- Test: `tests/ui/test_queue_table_model.cpp`; Create: `tests/ui/test_queue_views.cpp` (globbed: target
  `pychron_ui_test_queue_views`)

**Interfaces:**
- Consumes: Task 2's `executed_rows()`, `executedChanged(int)`, `insert_position`.
- Produces (class `QueueTableModel`):
  ```cpp
  // All runs back in Queue: executed 0, statuses cleared; the queue itself unchanged, edited() not emitted.
  // False while live.
  bool reset_executed();
  // Removes rows < executed_rows() as one reset edit (revalidated, edited() emitted), then executed 0 and
  // statuses cleared. False while live or with nothing executed.
  bool clear_executed();
  // Copies of `rows` (each < executed_rows()), aliquot and step cleared, in source order, inserted at
  // insert_position(executed_rows()). The inserted rows, or nullopt (empty, a row not executed, refused edit).
  std::optional<std::vector<std::size_t>> repeat(std::vector<std::size_t> rows);
  // After a crash or a Stop: executed = next_row, each (row, state) a finished status. False while live or
  // when next_row > rowCount().
  bool resume_from(std::size_t next_row, const std::vector<std::pair<std::size_t, experiment::run::RunState>>& statuses);
  ```
- Produces (`queue_views.hpp`, namespace `pychron::ui`):
  ```cpp
  class ExecutedProxy : public QSortFilterProxyModel { /* accepts source row < executed_rows(); flags without ItemIsEditable */ };
  class QueueProxy : public QSortFilterProxyModel { /* accepts source row >= executed_rows() */ };
  // each: explicit X(QueueTableModel& source, QObject* parent = nullptr); sets the source, connects
  // executedChanged to invalidateFilter(); no sorting (setDynamicSortFilter(false)).
  ```

- [ ] **Step 1: Write the failing tests**

  In `test_queue_table_model.cpp`:
  - `resetExecutedPutsEveryRowBack`: refused while live; after `end_live()` with `executed_rows() == 2` and
    statuses: true, `executed_rows() == 0`, no status, queue equal to before, `edited` spy 0.
  - `clearExecutedRemovesThem`: refused while live; after end with 2 executed of 3: true, `rowCount() == 1`, that
    row is the old row 2's identifier, `executed_rows() == 0`, `edited` spy 1; again → false.
  - `repeatCopiesWithoutAliquotOrStep`: row 0 given `id.aliquot = 7`, `id.step = "A"`; `resume_from(2, {})`;
    `repeat({0})` returns `{2}`; row 2 has row 0's identifier, no aliquot, empty step; `repeat({2})` (not
    executed) and `repeat({})` → nullopt.
  - `repeatWhileLiveGoesThroughTheCommitter`: live with a committer that refuses (`fail(ErrorKind::Config,
    "stale")`), `set_frozen(1)`: `repeat({0})` nullopt, `editRefused` said `stale`, row count unchanged.
  - `resumeFromSetsTheBoundaryAndStatuses`: `resume_from(2, {{0, Success}, {1, Failed}})` → true,
    `executed_rows() == 2`, `status(0) == Success`, `status(1) == Failed`; `resume_from(4, {})` false; while live
    false.

  In `test_queue_views.cpp` (fixture as `test_queue_table_model.cpp`'s, example queue, 3 rows):
  - `theBoundarySplitsTheRows`: live, `set_frozen(1)` → `ExecutedProxy::rowCount() == 1`, `QueueProxy::rowCount()
    == 2`, `queue.mapToSource(queue.index(0, 0)).row() == 1`; `set_frozen(3)` → 3 and 0.
  - `executedRowsAreNotEditable`: `resume_from(1, {})` (not live); executed proxy's `flags(index(0, Identifier))`
    lacks `ItemIsEditable`.
  - `aStatusChangeDoesNotRefilter`: spies on `layoutChanged`, `rowsInserted`, `rowsRemoved`, `modelReset` of both
    proxies; `on_run_started` / `on_run_finished` for row 0 → those spies 0, a `dataChanged` spy on the executed
    proxy > 0.
  - `aQueueEditKeepsBothViewsSplit` (Review Focus 1): live, `set_frozen(2)`, `on_queue_edited({queue, 9, 2})` →
    executed proxy 2 rows, queue proxy 1.

- [ ] **Step 2: Run them to see them fail**

  `cmake --build --preset dev-ui --target pychron_ui_test_queue_table_model pychron_ui_test_queue_views` fails to
  compile.

- [ ] **Step 3: Implement as the Interfaces block says**

  `clear_executed()` sets the guard aside for its own edit (decision 6): it removes through `ExperimentQueue::remove`
  and `adopt(…, true)` with `executed_` already 0, restoring it if `adopt` refuses. `repeat()` goes through
  `insert_runs`, so a live repeat is an ordinary live edit.

- [ ] **Step 4: Run both binaries**

  `build/dev-ui/tests/ui/pychron_ui_test_queue_table_model` and `build/dev-ui/tests/ui/pychron_ui_test_queue_views`
  pass.

- [ ] **Step 5: Checks and commit**

  `python3 tools/quality_check.py`; `python3 tools/compiler_sweep.py --changed --build-dir build/dev-ui`.
  `git commit -m "feat(ui): executed and queue views over the one queue model"`

---

### Task 4: the window's two tables

**Files:**
- Modify: `apps/pychron-ui/src/experiment_window.hpp`, `apps/pychron-ui/src/experiment_window.cpp`
  (constructor 70–209, `selected_rows` 395, `select_rows` 402, `start` 669, `update_state` 702)
- Modify: `docs/superpowers/specs/2026-10-13-queue-tables-design.md` (section 3: decision 9)
- Test: `tests/ui/test_experiment_window.cpp`

**Interfaces:**
- Consumes: Task 3's `ExecutedProxy`, `QueueProxy`; Task 2's `executed_rows()`, `clear_status(from)`.
- Produces (class `ExperimentWindow`):
  ```cpp
  QTableView* queue_table() const noexcept;     // was table()
  QTableView* executed_table() const noexcept;
  QWidget* executed_section() const noexcept;   // header row (fold button, "Executed (N)") and executed_table()
  QSplitter* tables_splitter() const noexcept;
  bool executed_collapsed() const noexcept;
  void set_executed_collapsed(bool collapsed);  // section shows its header only; kept in QSettings
  void start();                                 // from model().executed_rows(); the selection is not read
  void select_row(int row);                     // source row; selects it in the Queue view if it is there
  ```
  Fold button `QToolButton` named `executed_fold`; count label named `executed_count` with text
  `Executed (%1)`. Splitter state saved with `saveState()` on `splitterMoved` and on close under
  `experiment_window/tables_splitter`; fold state under `experiment_window/executed_collapsed` when changed.

- [ ] **Step 1: Write the failing tests, and move the existing ones to the Queue view**

  - `runsTheExampleQueue`: after `request_start()`, `QTRY_VERIFY(window.model().status(0).has_value())` then
    `QTRY_VERIFY(window.executed_table()->model()->rowCount() >= 1)` and `executed_section()->isVisible()`; at the
    end `executed_table()->model()->rowCount() == 3`, `queue_table()->model()->rowCount() == 0`,
    `!pane->start_enabled()` (decision 5); then `window.model().reset_executed()` → `pane->start_enabled()`.
  - `theExecutedSectionShowsOnlyWithExecutedRows`: after `load_queue`, `!executed_section()->isVisible()`;
    `model().resume_from(1, {})` → visible, `executed_count` text `Executed (1)`.
  - `theTablesKeepTheirFoldAndSplitAcrossAReopen`: window 1: `resume_from(1, {})`, `set_executed_collapsed(true)`,
    `tables_splitter()->setSizes({100, 400})`, close; window 2 on the same settings: `executed_collapsed()`, and
    `tables_splitter()->saveState()` equals window 1's.
  - `bothTablesShareTheirColumnWidths`: `queue_table()->setColumnWidth(QueueTableModel::Comment, 222)` →
    `executed_table()->columnWidth(QueueTableModel::Comment) == 222`.
  - `rowActionsUseTheQueueSelection`: `resume_from(1, {})`; `select_row(0)` selects nothing (row 0 is executed),
    `select_row(1)` → Delete action removes source row 1 and row 0 stays; `queue_table()->model()->index(0, 0)`
    maps to source row 1 before the delete.
  - `startBeginsAtTheFirstNotExecutedRow`: `resume_from(1, {})`, `select_row(2)`, `start()` →
    `QTRY_VERIFY(model().status(1).has_value())`, `!model().status(0)` (resume gave no status), then let it end.
  - `cancelAsksFirstAndStartsFromTheSelectedRow` renamed `cancelAsksFirstAndStartsAtTheFirstNotExecutedRow`:
    `select_row(1)` kept; waits for `status(0)` instead of `status(1)`; asserts `status(0) == Cancelled` and
    `!status(1)` at the end. Cancel half unchanged.
  - `editsTheQueueWhileItRuns`, `rowsOpenTheirScriptsInTheEditor`: unchanged unless they fail; any use of the old
    table goes through `queue_table()`.

- [ ] **Step 2: Run them to see them fail**

  `cmake --build --preset dev-ui --target pychron_ui_test_experiment_window` fails (`queue_table`,
  `executed_table` undeclared).

- [ ] **Step 3: Implement**

  Central widget: diagnostics, conditionals header, then a vertical `QSplitter`: the executed section
  (header row, `executed_table_`) above `queue_table_` (today's table, edit triggers, context menu, row actions
  unchanged). `executed_table_`: `NoEditTriggers`, `SelectRows`, hidden vertical header, column widths from
  `queue_table_`'s header (`sectionResized` → `resizeSection` on the executed header). Section hidden while
  `executed_rows() == 0`; on `executedChanged` update the count, visibility and, if the executed view's scroll
  bar was at its maximum before, `scrollToBottom()`, then `update_state()` (Start follows the boundary; the
  pane's run count `runnable_rows` stays every row not skipped, so `progress_text` is as today). `selected_rows()` maps the Queue view's selection through
  `QueueProxy::mapToSource`; `select_rows()` and the factory's `inserted` handler's `scrollTo` map source rows
  with `mapFromSource` (an invalid index is skipped). The measurement panel follows `queue_table_`'s selection.
  `start()`: `from = model_.executed_rows()`; `model_.clear_status(from)`; rest unchanged. Write decision 9 into
  spec section 3.

- [ ] **Step 4: Run the window's tests**

  `build/dev-ui/tests/ui/pychron_ui_test_experiment_window` passes, every case.

- [ ] **Step 5: Checks and commit**

  `python3 tools/quality_check.py`; `python3 tools/compiler_sweep.py --changed --build-dir build/dev-ui`.
  `git commit -m "feat(ui): the experiment window shows executed runs apart from the queue"`

---

### Task 5: Queue > Reset, Clear Executed, Repeat Last Run, and the Executed context menu

**Files:**
- Modify: `apps/pychron-ui/src/experiment_window.hpp`, `apps/pychron-ui/src/experiment_window.cpp`
  (`build_actions`, 307)
- Modify: `docs/superpowers/specs/2026-10-13-queue-tables-design.md` (section 4: decision 8)
- Test: `tests/ui/test_experiment_window.cpp`

**Interfaces:**
- Consumes: Task 3's `reset_executed()`, `clear_executed()`, `repeat(rows)`; Task 4's `executed_table()`.
- Produces (class `ExperimentWindow`):
  ```cpp
  bool reset_executed(QString* error = nullptr);  // "a queue is running" while running
  bool clear_executed(QString* error = nullptr);  // asks first; "a queue is running", "cancelled"
  // The selected rows of Executed, else its last row; "no run has been executed" with none.
  bool repeat_last(QString* error = nullptr);
  void set_ask_clear_executed(std::function<bool(int runs)> ask);  // default: QMessageBox question
  ```
  Menu `MenuHub::Menu::Queue` (Experiment > Queue) gains `Reset`, `Clear Executed...`, `Repeat Last Run`; each
  shows its refusal in a `QMessageBox::information` (`A queue is running.`, `No run has been executed.`). The
  question's text: `Remove %1 executed runs from the queue?`. `executed_table()`'s `ActionsContextMenu`: `Repeat`
  (same as Repeat Last Run), `Open Extraction Script`, `Open Post-Measurement Script` (the selected executed row's
  script in the script editor, as `edit_row_script` does for a Queue row).

- [ ] **Step 1: Write the failing tests**

  - `resetAndClearExecutedAreRefusedWhileRunning`: start the example queue; while `bridge.running()`:
    `reset_executed(&e)` false with `e == "a queue is running"`, same for `clear_executed`, ask hook not called.
  - `clearExecutedAsksRemovesAndMarksModified`: `load_queue`, `resume_from(2, {})`, ask hook answering false →
    false, `e == "cancelled"`, 3 rows; answering true (hook got `2`) → true, 1 row, `executed_rows() == 0`,
    `modified()`.
  - `resetPutsTheRunsBackWithoutModifying`: `resume_from(2, {{0, Success}})`, `reset_executed()` → true,
    `executed_rows() == 0`, `!modified()`, `queue_table()->model()->rowCount() == 3`.
  - `repeatInsertsAtTheFrontOfTheQueue`: not live: `resume_from(2, {})`, `repeat_last()` → 4 rows, row 2 has row
    1's identifier, `queue_table()->model()->index(0, Identifier)` shows it. Executed selection of row 0 → the
    copy of row 0 at row 2. Live: start, `QTRY_VERIFY(model().executed_rows() >= 1)`, `repeat_last()` true, queue
    has 4 rows, wait for the end: `queue completed` among the events.
  - `repeatWithNothingExecutedSaysSo` (Review Focus 5): `load_queue`, `repeat_last(&e)` false,
    `e == "no run has been executed"`, 3 rows.

- [ ] **Step 2: Run them to see them fail**

  `cmake --build --preset dev-ui --target pychron_ui_test_experiment_window` fails (`repeat_last` undeclared).

- [ ] **Step 3: Implement as the Interfaces block says; write decision 8 into spec section 4.3**

- [ ] **Step 4: Run the window's tests**

  `build/dev-ui/tests/ui/pychron_ui_test_experiment_window` passes.

- [ ] **Step 5: Checks and commit**

  `python3 tools/quality_check.py`; `python3 tools/compiler_sweep.py --changed --build-dir build/dev-ui`.
  `git commit -m "feat(ui): reset, clear executed and repeat the last run from the Queue menu"`

---

### Task 6: resume after a crash or a Stop

**Files:**
- Create: `apps/pychron-ui/src/resume_offer.hpp`, `apps/pychron-ui/src/resume_offer.cpp`
- Modify: `apps/pychron-ui/CMakeLists.txt`
- Modify: `apps/pychron-ui/src/experiment_window.hpp`, `apps/pychron-ui/src/experiment_window.cpp`
  (`open_dialog` 648, `build_actions`)
- Modify: `apps/pychron-ui/src/main_window.cpp` (`ensure_experiment_window`, after the `--queue` load, line 408)
- Modify: `docs/superpowers/specs/2026-10-13-queue-tables-design.md` (section 5: decisions 1–4, 10)
- Test: Create `tests/ui/test_resume_offer.cpp`; `tests/ui/test_experiment_window.cpp`

**Interfaces:**
- Consumes: Task 1's `Executor::read_state`, `ExecutorStateFile`, `run::run_state_from`; Task 3's
  `resume_from`; Task 4's `start()`.
- Produces (`resume_offer.hpp`, namespace `pychron::ui`):
  ```cpp
  struct ResumeOffer {
    std::size_t next_row = 0;
    QString question;   // "The queue %1 stopped at row %2 (%3) at %4. Resume from row %2?"
    QString how;        // "It was stopped." (state "finalizing" or "idle") or
                        // "It did not finish (the program ended while it ran)."
    QString mismatch;   // "" when resumable, else "Row %1 is %2 now; the file has %3."
    std::vector<std::pair<std::size_t, experiment::run::RunState>> statuses;  // last entry per row wins
    bool resumable() const noexcept { return mismatch.isEmpty(); }
  };
  // nullopt unless file.queue == queue.name and 0 < file.next_row < queue.runs.size().
  // %1 queue name, %2 next_row + 1, %3 that row's identifier now, %4 `written` as "yyyy-MM-dd HH:mm".
  std::optional<ResumeOffer> resume_offer(const experiment::executor::ExecutorStateFile& file,
                                          const experiment::QueueSpec& queue, const QDateTime& written);
  ```
- Produces (class `ExperimentWindow`):
  ```cpp
  // Reads <session data>/executor_state.json; asks when resume_offer() gives one; on yes resume_from() and
  // start(). False (nothing asked) while running, with *error "a queue is running"; false with *error left
  // empty with executed_rows() > 0, or with no file, a file that does not read, or no offer.
  bool offer_resume(QString* error = nullptr);
  void set_ask_resume(std::function<bool(const ResumeOffer&)> ask);  // default: dialog, Resume / Not now
  // How a Queue menu action shows its refusal; default QMessageBox::information(this, ...).
  void set_inform(std::function<void(const QString&)> inform);
  QAction* resume_action() const;  // Queue > Resume...
  ```
  State file path: `bridge_.session().data() / "executor_state.json"` (as `LabSession::resume_row`); time from
  `QFileInfo::lastModified()`. The default dialog shows `question`, `how` beneath it and, when not resumable,
  `mismatch` with Resume disabled. Queue menu gains `Resume...` calling `offer_resume(&e)`; when it returns
  false the menu informs `A queue is running.` if `e == "a queue is running"`, else `Nothing to resume.`
  (Global Constraints: a refusal while running says so). This task also moves Task 5's three menu actions
  onto the `inform` hook (their text unchanged).

- [ ] **Step 1: Write the failing tests**

  `test_resume_offer.cpp` (pure function, a 3-row `QueueSpec` named `q` built in the test, identifiers
  `a b c`):
  - `offeredForAWorkingState`: `{queue "q", state "running", next_row 1, runs [{0, "a", "r1", "success"}]}` →
    offer, `next_row == 1`, question contains `row 2 (b)`, `how == "It did not finish (the program ended while it
    ran)."`, `resumable()`, `statuses == {{0, Success}}`.
  - `offeredAfterAStop`: same with `state "finalizing"` → `how == "It was stopped."`.
  - `notOfferedForAnotherQueueOrRow`: name `other`; `next_row` 0; 3; 4 → nullopt each.
  - `anIdentifierThatChangedDisablesIt`: `runs [{0, "x", …}]` → offer, `!resumable()`,
    `mismatch == "Row 1 is a now; the file has x."`.
  - `theLastEntryForARowWins`: runs `[{0, "a", "r1", "failed"}, {0, "a", "r2", "success"}]` →
    `statuses == {{0, Success}}`; an unknown state string leaves its row out.

  `test_experiment_window.cpp` (state file written by hand into `sim.dir / "data" / "executor_state.json"` with
  the example queue's `name` and identifiers):
  - `resumeStartsAtTheFilesNextRowWithEarlierStatuses`: `next_row 1`, row 0 `success`; ask hook returns true;
    `offer_resume()` true, `status(0) == Success` at once, `executed_rows() >= 1`; when it ends `status(1)` and
    `status(2)` are `Success`, `status(0)` still `Success`.
  - `notNowChangesNothing`: hook returns false → `offer_resume()` false, `executed_rows() == 0`, no status.
  - `noOfferWithoutAGoodFile` (Review Focus 3 and 4): no file; `not json`; `{"queue": …}` without `next_row`;
    `next_row` 3 (the row count) → hook never called, `offer_resume()` false.
  - `noOfferInTheWindowThatJustStopped`: start, `bridge.stop()` once `status(0)` is set, wait for the end;
    hook counting calls; `offer_resume()` false, count 0; `load_queue` of the same file then `offer_resume()` →
    hook called once.
  - `anOfferWithAnotherIdentifierCannotResume`: row 0 `identifier "zzz"`; hook sees `!resumable()`, returns
    true; nothing starts (`!bridge.running()`, `executed_rows() == 0`).
  - `resumeIsRefusedWhileRunningAndSaysSo`: a good state file (`next_row 1`), ask hook counting calls, inform
    hook recording its text; start the example queue; while `bridge.running()`: `offer_resume(&e)` false with
    `e == "a queue is running"`; `resume_action()->trigger()` → inform got exactly `A queue is running.`; ask
    hook count 0. After the queue ends and `reset_executed()`, with no state file left to offer from (file
    removed): `resume_action()->trigger()` → inform got `Nothing to resume.`.

- [ ] **Step 2: Run them to see them fail**

  `cmake --build --preset dev-ui --target pychron_ui_test_resume_offer pychron_ui_test_experiment_window` fails to
  compile.

- [ ] **Step 3: Implement as the Interfaces block says**

  `offer_resume()` checks `bridge_.running()` first and sets `*error = "a queue is running"` before reading
  any file. It does nothing when the hook says yes to an offer that is not resumable. `open_dialog()` calls
  it after a successful `load_queue`; `MainWindow::ensure_experiment_window` after its `--queue` load succeeds.
  Write decisions 1–4 and 10 into spec section 5.

- [ ] **Step 4: Run both binaries**

  `build/dev-ui/tests/ui/pychron_ui_test_resume_offer` and `build/dev-ui/tests/ui/pychron_ui_test_experiment_window`
  pass; `build/dev-ui/tests/ui/pychron_ui_test_menu_hub` still passes (new Queue menu actions).

- [ ] **Step 5: Checks and commit**

  `python3 tools/quality_check.py`; `python3 tools/compiler_sweep.py --changed --build-dir build/dev-ui`.
  `git commit -m "feat(ui): the experiment window offers to resume a queue that stopped"`

---

### Task 7: user docs, spec pointers, and the whole-tree run

**Files:**
- Modify: `docs/user/04-experiments.md` (section 3 window table, 3.2 row operations, 3.3 editing while running,
  8.1 Start)
- Modify: `docs/superpowers/specs/2026-10-02-experiment-window-design.md` (5.2, 5.5: a pointer to
  `2026-10-13-queue-tables-design.md`)

- [ ] **Step 1: Write the docs**

  Section 3: the centre is two tables, Executed (foldable, read-only, "Executed (N)") above Queue. 3.2: row
  operations act on Queue only; Executed's context menu (Repeat, Open script). 3.3: a run moves to Executed when it
  starts; after the queue ends executed rows stay read-only until Queue > Reset. New subsection: Reset, Clear
  Executed, Repeat Last Run, Resume... and the offer at Open and at start-up with `--queue`. 8.1: Start begins at the
  first row of Queue, not at the selected row; to run executed rows again, Reset then Start.

- [ ] **Step 2: Whole-tree build and tests, once**

  `cmake --build --preset dev-ui` and `ctest --preset dev-ui`: all pass. `python3 tools/compiler_sweep.py
  --build-dir build/dev-ui`: no warning. `python3 tools/quality_check.py`: exit 0.

- [ ] **Step 3: Commit**

  `git commit -m "docs(ui): the Executed and Queue tables, Reset, Repeat and resume"`

## Open questions

None.
