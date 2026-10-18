# Experiment window: Executed and Queue tables (option A)

Status: agreed 2026-10-15 (owner answered section 9). Written by unattended
run for T024 on 2026-10-13.
Builds on `2026-10-13-queue-tables-survey.md` and its owner's decisions
(section 6): option A, one `QueueTableModel` through two views; a run
moves to Executed when it starts; Executed shows every executed run and is
collapsible; no `rem`/`ex` files; window offers resume after a crash (and,
owner 2026-10-15, after a deliberate Stop) from `executor_state.json`;
Reset, Clear executed and Repeat last run as menu actions. Nothing built.

## 1. Now

- One `QTableView` (`apps/pychron-ui/src/experiment_window.cpp:60`) over
  one `QueueTableModel`; runs stay in place, state is row colour.
- Frozen rows (`QueueTableModel::frozen_rows()`, `queue_table_model.hpp:106`
  to `:113`) mirror executor's frontier (`QueueFrontier`, published at
  `libs/experiment/src/executor/executor.cpp:584`–`593`: a row is frozen
  once the executor acts on it: launched, skipped, a pause; a row whose
  delay still counts down is not). Frontier only grows; model reports it as
  whole-table `dataChanged` and `frozenChanged`; `end_live()` sets it back
  to 0 when the queue ends (`:401`–`406`).
- Start from first selected row (`experiment_window.cpp:560`–`575`):
  `clear_status`, `set_live(from, …)`.
- `executor_state.json` (data dir, `libs/experiment/src/lab/session.cpp:95`):
  `queue`, `state`, `next_row`, `runs[]` of row, identifier, run_id, state
  (`executor.cpp:463`–`482`). `Executor::resume_row` reads `next_row` only
  (`:485`); `LabSession::resume_row` (`session.cpp:311`); used by `elctl exp
  run --resume` only (`apps/elctl/src/exp.cpp:162`).
- No Reset, Clear executed or Repeat action.

## 2. Model: the executed boundary

`QueueTableModel` gains `executed_rows()`: number of leading rows shown in
Executed.

- While live: `executed_rows() == frozen_rows()`. A run is frozen before
  `RunStarted` (frontier advances before launch), so it moves when it
  starts, as owner decided. A skipped row passed by the executor moves too
  (shown faint, as now).
- When the queue ends (`end_live()`), `executed_rows()` keeps its value:
  finished runs stay in Executed. `frozen_rows()` goes to 0 as now (rows
  editable again).
- Set to 0 by: Reset (4.1), Clear executed (4.2, after removing) and
  `set_queue` (Open, New). Start never moves it backwards: a queue starts
  at `executed_rows()`, the first row of the Queue view (3). No start from
  an arbitrary row N in this round (owner, 2026-10-15), so no row is ever
  in Executed without having been reached by the executor.
- Signal `executedChanged(int)`. Rows never move in the model: numbers
  (`#` column) stay the queue's, every operation that addresses rows by
  index (edits, versions, `insert_position`, selection → start row) is
  unchanged.
- Rule: rows `< executed_rows()` are never editable (they are frozen while
  live; after the queue ends, they stay read-only until Reset, owner
  2026-10-15).
  `row_editable(row) = !locked_ && row >= max(frozen_rows(), executed_rows())`.
  `insert_position(at) = max(at, frozen_rows(), executed_rows())`.
- Row operations on executed rows (plan decision 6): `adopt()` refuses any
  edit that changes a row below `max(frozen_rows(), executed_rows())`, live
  or not, with existing `editRefused` text `rows the executor has reached
  cannot change`. `clear_executed()` (4.2) is the one operation past that
  guard.
- Nothing left to start (plan decision 5): `runnable()` is `check_.ok() &&
  queue_.size() > executed_rows()`. After a queue runs to its end Start is
  disabled until Reset.
- Start's statuses (plan decision 7): `clear_status(from)` forgets statuses
  of rows `>= from` and their run ids; Start clears from `executed_rows()`,
  so Executed keeps what it showed after a Stop.
- Who moves it: `set_live(from, …)` sets it to `from`; `set_frozen(n)`, when
  it advances, to `n`; `end_live()` leaves it; `set_queue(spec, false)` sets
  0, `set_queue(spec, true)` (executor's `QueueEdited`) keeps it.
  `executedChanged` emitted after `frozenChanged`, once model state updated.

## 3. Views

Two `QSortFilterProxyModel` subclasses over the one model, no sorting:

- `ExecutedProxy`: accepts `row < executed_rows()`; flags without
  `ItemIsEditable`.
- `QueueProxy`: accepts `row >= executed_rows()`.
- Both refilter on `executedChanged` (`invalidateFilter()`; Qt 6.4 on
  Ubuntu: no `beginFilterChange`, which is 6.9). Only boundary changes
  refilter: a status change stays a `dataChanged`.

Window (`experiment_window.cpp:60`–`128`):

- `QSplitter` (vertical): top "Executed (N)" section, a header row with a
  fold button and the count, then the Executed `QTableView`; bottom the
  Queue `QTableView` (today's table: same edit triggers, context menu, row
  actions).
- Executed: read-only, `SelectRows`, follows newest row
  (`scrollToBottom` on `executedChanged` unless the operator scrolled up,
  as the timeline's follow toggle does). Hidden when `executed_rows()` is 0.
- Collapsed: section shows header only. Fold state and splitter sizes kept
  in `QSettings` under `experiment_window/executed_collapsed` and
  `experiment_window/tables_splitter`. Not part of `DockLayouts` (the
  tables are the central widget).
- Column widths: one set, applied to both views; Queue view's header drives
  Executed's (resize one, both follow).
- `selected_rows()` (`experiment_window.cpp:290`) maps the Queue view's
  selection to source rows. Row actions (Move, Duplicate, Delete, Skip, End
  After, Set Conditionals, edit scripts) act on the Queue view only.
  Executed view's context menu: Repeat (4.3), Open script (read-only, as
  now for frozen rows).
- Start: always from `executed_rows()`, the Queue view's first row; the
  selection is not read (owner, 2026-10-15: no start from row N this
  round). Before: first selected row, else 0. To run executed rows again:
  Reset, then Start. `elctl exp run --from` and `--resume` unchanged.
- Run factory (`run_factory_panel.cpp:453`) inserts after the Queue view's
  selection; with none, appends (unchanged: `insert_position` clamps).
- `select_rows` and `scrollTo` after an insert or move map source rows to
  the Queue view; a source row not in it (executed) skipped.
- Names (plan decision 9): `ExperimentWindow::table()` becomes
  `queue_table()`; added `executed_table()`, `executed_section()` (header
  row and Executed table), `tables_splitter()`, `executed_collapsed()`,
  `set_executed_collapsed(bool)`. Fold button `QToolButton` named
  `executed_fold`, count `QLabel` named `executed_count`, text
  `Executed (%1)`. Splitter state (`saveState()`) written on
  `splitterMoved` and on close; fold state written when changed. Fold
  state applied before splitter state restored: a collapsed section's
  minimum is its header's, and restored sizes are not held to the table's.
  Test `cancelAsksFirstAndStartsFromTheSelectedRow` renamed
  `cancelAsksFirstAndStartsAtTheFirstNotExecutedRow` (start half changes
  meaning, 7).

## 4. Menu actions (Experiment > Queue)

### 4.1 Reset

All runs back in Queue: `executed_rows()` to 0, statuses cleared. Queue
file unchanged (rows never moved). Refused while running ("A queue is
running."). Legacy equivalent: `experiment_queue.py:425`.

### 4.2 Clear executed

Removes rows `< executed_rows()` from the queue (one model operation,
`reset=true` path, revalidated, marks queue modified), then
`executed_rows()` 0. Asks first ("Remove N executed runs from the
queue?"). Refused while running: executor and live edits address rows by
index from frozen (`executor.cpp:432`–`447`, `replace_from`); removing rows
before the frontier would shift them (owner, 2026-10-15: refused).

### 4.3 Repeat last run

Copy of the last executed run (or the selected ones in Executed), aliquot
and step cleared, inserted at the front of Queue
(`insert_position(executed_rows())`). Allowed while running: an insert at
the frontier is an ordinary live edit, refused like any other when stale.
Legacy repeated `executed_runs[0]` (oldest, a defect: survey section 1);
here the newest.

- Repeat in model (plan decision 8): `repeat(rows)` returns inserted rows;
  copies keep source order, inserted at
  `insert_position(executed_rows())`. With no executed run refused, menu
  text `No run has been executed.`: never a silent no-op, never a copy of a
  Queue row (selection in Queue not read).
- Window (`ExperimentWindow`): `reset_executed(QString*)`,
  `clear_executed(QString*)`, `repeat_last(QString*)`; error texts
  `a queue is running`, `no run has been executed`, `cancelled` (Clear
  Executed's question answered no). Question hook
  `set_ask_clear_executed(std::function<bool(int runs)>)`, default
  `QMessageBox` question `Remove %1 executed runs from the queue?`. Queue
  menu: `Reset`, `Clear Executed...`, `Repeat Last Run`; refusal shown in
  `QMessageBox::information` (`A queue is running.`,
  `No run has been executed.`); `cancelled` shows nothing.
- Executed table's `ActionsContextMenu`: `Repeat` (as Repeat Last Run),
  `Open Extraction Script`, `Open Post-Measurement Script` (selected
  executed row's script in script editor, as `edit_row_script` for a Queue
  row; nothing with other than one row selected).

## 5. Resume after a crash or a Stop

- When the window has a queue and is not running (after Open, at start-up
  with `--queue`, and on the Experiment menu's Resume...), it reads the
  data dir's `executor_state.json`.
- Offered when: file's `queue` equals the open queue's `name` and
  `0 < next_row <` row count, whatever its `state` (owner, 2026-10-15:
  after a deliberate Stop too, not only after a crash). A crash leaves a
  working state (`preparing`, `running`, `stopping`, `cancelling`,
  `aborting`); every end, clean or stopped, writes `finalizing` last
  (`executor.cpp:679`–`681`, then `Idle` is set but not written). A queue
  that ran to its end has `next_row` equal to the row count and is not
  offered.
  Dialog: "The queue *name* stopped at row N (*identifier*) at
  *time of file*. Resume from row N?" Resume / Not now. Line under it says
  how it stopped: "It was stopped." for `finalizing`, "It did not finish
  (the program ended while it ran)." for a working state.
- Not offered right after a Stop in the same window: there the executed
  rows are still in Executed and Start continues from `executed_rows()`
  (2). Offered on Open, at start-up with `--queue`, and from Resume....
- Resume: `executed_rows()` set to `next_row`, statuses of rows in the
  file's `runs[]` filled from it (success, failed, …) so Executed shows what
  ran before, then Start as usual (it starts at `executed_rows()`). This is
  the one way rows reach Executed other than by the executor; they are
  rows it did reach, in the run the file records. Not now: nothing changes; Resume... stays in menu.
- Library: `Executor::read_state(path)` → `Result<ExecutorStateFile>`
  {queue, state, next_row, runs{row, identifier, run_id, state}}, parsing
  the file with nlohmann_json (already linked by `libs/experiment`), not a
  regex;
  `resume_row` becomes `read_state(...)->next_row` so `elctl --resume` reads
  it the same way. File format unchanged.
- A queue changed since the crash (rows inserted or removed before
  `next_row`) is not detected beyond name and row identifier: dialog shows
  the row's identifier now and in the file; when they differ, Resume is
  disabled with that reason. No path or hash of runs compared (owner,
  2026-10-15).

## 6. Rules

- One model: no second list, no row ever moved between views by the
  model; boundary is a number.
- Executor, `LabSession`, edit versioning and the queue file format
  unchanged (5 adds a reader, no writer change).
- Only rows of the Queue view are edited or acted on by row actions.
- Every action refused while running says so; nothing is silently ignored.
- Qt: nothing newer than 6.4 without `QT_VERSION_CHECK`.

## 7. Tests (`tests/ui`, `tests/experiment`)

- Model: `executed_rows` follows frontier while live, survives
  `end_live`, is 0 after Reset and `set_queue`; rows below it not editable
  after end.
- Proxies: row counts and source rows on boundary change; status change
  does not refilter.
- Window: a run moves to Executed at `RunStarted` in `runsTheExampleQueue`;
  fold state and splitter kept across a reopen; row actions use Queue
  selection; Start starts at first not executed whatever is selected; Reset
  and Clear executed refused while running; Clear executed removes and
  marks modified; Repeat inserts at the front of Queue, live and not.
- Resume: dialog offered for a matching state file in a working state and
  in `finalizing` (each with its line), not for another queue name, or
  `next_row` 0, equal to the row count or past it; not offered in the
  window that just stopped; Resume starts at
  `next_row` with earlier statuses filled; identifier mismatch disables it.
- `tests/experiment`: `read_state` round trip of a file written by
  `write_state`; a file with only `next_row` (older) still gives it.
- Existing tests that read the one table (`editsTheQueueWhileItRuns`,
  `cancelAsksFirstAndStartsFromTheSelectedRow`, `rowsOpenTheirScriptsInTheEditor`,
  `ExperimentWindow::table()` users) switch to the Queue view; none dropped.
  `cancelAsksFirstAndStartsFromTheSelectedRow` keeps its cancel half; its
  start half asserts the first not executed row instead of the selection.

## 8. Docs

`docs/user/04-experiments.md` section 3 (window, 204), 3.2 row operations
(243), 3.3 editing while running (260–273), 8.1 Start (654): two tables,
Reset, Clear executed, Repeat, resume; Start no longer starts from the
selected row. Experiment window spec
(`2026-10-02-experiment-window-design.md` 5.2, 5.5) gets a pointer here.

## 9. Owner, 2026-10-15

1. Clear executed while a queue runs: refused (4.2).
2. No start from row N this round: Start always begins at the first not
   executed row (2, 3).
3. After a queue ends, executed rows stay read-only until Reset (2).
4. Resume match: queue name plus row identifier only (5).
5. Resume offered after a deliberate Stop too, not only after a crash (5).

## 10. Out of scope

- Queue file format and `queues/` directory (T041,
  `2026-10-13-queue-directory-design.md`).
- `rem`/`ex` files, autosave (owner: no).
- Seeding aliquots on resume (T091): a resumed step heat with no fixed
  aliquot gives its remaining steps a new aliquot until that lands.
- Start from an arbitrary row (UI); selection-based start returns, if
  wanted, as its own change.
- Changing row colours or the status column (survey's spec/code gap on
  skipped rows stays its own item).

## Open questions

None.
