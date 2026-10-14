# Experiment window: Executed and Queue tables (option A)

Status: draft 2026-10-13, not agreed. Written by unattended run for T024.
Builds on `2026-10-13-queue-tables-survey.md` and its owner's decisions
(section 6): option A, one `QueueTableModel` through two views; a run
moves to Executed when it starts; Executed shows every executed run and is
collapsible; no `rem`/`ex` files; window offers resume after a crash from
`executor_state.json`; Reset, Clear executed and Repeat last run as menu
actions. Nothing built. Questions: section 9.

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
- Set to 0 by: Reset (4.1), Clear executed (4.2, after removing), `set_queue`
  (Open, New), and Start from row 0. Start from row N sets it to N
  (`set_live(from)`): rows before N move to Executed with no status, shown
  "not run" (question 2).
- Signal `executedChanged(int)`. Rows never move in the model: numbers
  (`#` column) stay the queue's, every operation that addresses rows by
  index (edits, versions, `insert_position`, selection → start row) is
  unchanged.
- Rule: rows `< executed_rows()` are never editable (they are frozen while
  live; after the queue ends, editing them means Reset first).
  `row_editable(row) = !locked_ && row >= max(frozen_rows(), executed_rows())`.
  (Question 3.)

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
- Start from selected row: Queue view's first selected row, else
  `executed_rows()` (first not executed), else 0. (Before: first selected,
  else 0.)
- Run factory (`run_factory_panel.cpp:453`) inserts after the Queue view's
  selection; with none, appends (unchanged: `insert_position` clamps).
- `select_rows` and `scrollTo` after an insert or move map source rows to
  the Queue view.

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
before the frontier would shift them. (Question 1.)

### 4.3 Repeat last run

Copy of the last executed run (or the selected ones in Executed), aliquot
and step cleared, inserted at the front of Queue
(`insert_position(executed_rows())`). Allowed while running: an insert at
the frontier is an ordinary live edit, refused like any other when stale.
Legacy repeated `executed_runs[0]` (oldest, a defect: survey section 1);
here the newest.

## 5. Resume after a crash

- When the window has a queue and is not running (after Open, at start-up
  with `--queue`, and on the Experiment menu's Resume...), it reads the
  data dir's `executor_state.json`.
- Offered when: file's `queue` equals the open queue's `name`, its `state`
  is one the executor writes only while working (`preparing`, `running`,
  `stopping`, `cancelling`, `aborting`: it did not reach the end), and
  `0 < next_row <` row count. Every end, clean or stopped, writes
  `finalizing` last (`executor.cpp:679`–`681`, then `Idle` is set but not
  written), so a queue the operator stopped is not offered. (Question 5.)
  Dialog: "The queue *name* stopped at row N (*identifier*) at
  *time of file*. Resume from row N?" Resume / Not now.
- Resume: `start(next_row)` as Start does, and statuses of rows in the
  file's `runs[]` filled from it (success, failed, …) so Executed shows what
  ran before the crash. Not now: nothing changes; Resume... stays in menu.
- Library: `Executor::read_state(path)` → `Result<ExecutorStateFile>`
  {queue, state, next_row, runs{row, identifier, run_id, state}}, parsing
  the file with nlohmann_json (already linked by `libs/experiment`), not a
  regex;
  `resume_row` becomes `read_state(...)->next_row` so `elctl --resume` reads
  it the same way. File format unchanged.
- A queue changed since the crash (rows inserted or removed before
  `next_row`) is not detected beyond name and row identifier: dialog shows
  the row's identifier now and in the file; when they differ, Resume is
  disabled with that reason. (Question 4.)

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
  `end_live`, is 0 after Reset, `set_queue`, Start from 0, N after Start
  from N; rows below it not editable after end.
- Proxies: row counts and source rows on boundary change; status change
  does not refilter.
- Window: a run moves to Executed at `RunStarted` in `runsTheExampleQueue`;
  fold state and splitter kept across a reopen; row actions use Queue
  selection; Start with nothing selected starts at first not executed; Reset
  and Clear executed refused while running; Clear executed removes and
  marks modified; Repeat inserts at the front of Queue, live and not.
- Resume: dialog offered for a matching non-idle state file, not for idle,
  another queue name, or `next_row` 0 or past the end; Resume starts at
  `next_row` with earlier statuses filled; identifier mismatch disables it.
- `tests/experiment`: `read_state` round trip of a file written by
  `write_state`; a file with only `next_row` (older) still gives it.
- Existing tests that read the one table (`editsTheQueueWhileItRuns`,
  `cancelAsksFirstAndStartsFromTheSelectedRow`, `rowsOpenTheirScriptsInTheEditor`,
  `ExperimentWindow::table()` users) switch to the Queue view; none dropped.

## 8. Docs

`docs/user/04-experiments.md` section 3 (window, 204), 3.2 row operations
(243), 3.3 editing while running (260–273), 8.1 Start (654): two tables,
Reset, Clear executed, Repeat, resume. Experiment window spec
(`2026-10-02-experiment-window-design.md` 5.2, 5.5) gets a pointer here.

## 9. Questions for owner

1. Clear executed while a queue runs: refused (proposed, rows are
   addressed by index), or wanted (needs an executor edit that drops rows
   before the frontier)?
2. Start from row N: rows before N shown in Executed as "not run"
   (proposed: they are behind the frontier), or left in Queue?
3. After a queue ends, executed rows stay read-only until Reset
   (proposed), or editable in place?
4. Resume match: queue name plus row identifier (proposed), or also the
   queue file's path or a hash of its runs?
5. Offer resume after a deliberate Stop too (file says `finalizing`,
   `next_row` short of the end), or only after a crash (proposed)?

## 10. Out of scope

- Queue file format and `queues/` directory (T041,
  `2026-10-13-queue-directory-design.md`).
- `rem`/`ex` files, autosave (owner: no).
- Seeding aliquots on resume (T091): a resumed step heat with no fixed
  aliquot gives its remaining steps a new aliquot until that lands.
- Changing row colours or the status column (survey's spec/code gap on
  skipped rows stays its own item).
