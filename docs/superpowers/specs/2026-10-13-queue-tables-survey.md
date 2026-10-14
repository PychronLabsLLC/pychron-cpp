# Split queue tables: legacy vs pychron-cpp (survey)

Survey only, for T024 ("use the split queue tables that legacy used; when
editing one table, when executing, in-progress and executed runs move to
another table; brainstorm alternatives before implementing; examine legacy
fully"). Input to brainstorm with owner. No design agreed; nothing built.

## 1. Legacy (Python pychron)

One `ExperimentQueue` (`pychron/experiment/queue/experiment_queue.py:76`)
holds two lists; one `ExperimentEditor` per queue tab
(`pychron/experiment/tasks/experiment_editor.py:127`) shows them as two grids.

- Lists: `automated_runs` (pending, `base_queue.py:108`), `executed_runs`
  (`experiment_queue.py:82`). `cleaned_automated_runs` = pending, not skipped,
  state `not run` (`base_queue.py:629`–`632`).
- View (`experiment_editor.py:208`–`261`): executed grid above
  (`visible_when="executed"`, read-only, adapter
  `ExecutedAutomatedRunSpecAdapter`, `tabular_adapter.py:153`), pending grid
  below (editable only if `automated_runs_editable`, `:212`–`213`; adapter
  `AutomatedRunSpecAdapter`, `tabular_adapter.py:445`).
- Executed grid shows **last 5 only** (`n_executed_display = 5`,
  `experiment_queue.py:81`, `display_executed_runs` `:556`); progress ratio
  counts all (`:558`–`561`).
- **Move happens at run start, not end.** `set_run_inprogress`
  (`experiment_queue.py:449`–`468`): removes run from `automated_runs`,
  appends to `executed_runs`, scrolls executed grid to it. Called after
  `run.start()` succeeds (`experiment_executor.py:2043`–`2052`). A run whose
  start fails stays in pending, state `fail`. `queue.executed = True` at
  start (`experiment_executor.py:1081`).
- Run order: executor takes a snapshot generator of `cleaned_automated_runs`
  (`experiment_executor.py:1325`, `experiment_queue.py:508`–`512`); no pop.
  Edits set `queue_modified`; generator rebuilt at next loop turn
  (`experiment_executor.py:1349`–`1353`).
- Edits while running: **not blocked**. Pending grid stays editable; a dirty
  editor sets `executor.executable = False` (`experiment_task.py:1027`–`1029`);
  info refresh suppressed while executor alive (`experimentor.py:312`–`313`).
- Reset (`experiment_queue.py:425`–`447`): executed runs go back to front of
  pending, `executed = False`. `reset_queues` over every tab
  (`experiment_task.py:157`–`166`).
- Files: after each run `<queue>.rem.txt` (remaining) and `<queue>.ex.txt`
  (executed) under `experiments/rem/`
  (`experiment_executor.py:1824`–`1831`).
- Repeat action reinserts `executed_runs[0]` (`experiment_executor.py:3382`–`3389`).
- Row colours (`tabular_adapter.py:72`–`82`, `278`–`314`): not executable,
  then skip, then state (extraction, measurement, success, truncated,
  canceled, failed, aborted, end_after, invalid), then analysis-type colour,
  then stripe. Icon ball for success / truncated (`:352`–`356`). Legend pane
  `experiment_panes.py:715`–`727`.
- Run blocks: `RunBlock` view, make / repeat block (`run_block.py:147`–`166`).

Legacy defects seen (not to copy):

- `move(step)` (`experiment_queue.py:313`–`320`):
  `idx = max(min(0, idx + step), len - 1)` is always `len - 1`; Move Up / Down
  does not move one row.
- `_find_run` matches by runid (`:455`–`457`, `:535`–`541`): duplicate
  identifier/aliquot/step finds first pending row.
- Repeat uses oldest executed run, same object in both lists.
- Executed grid hides all but 5; `_no_update` toggled by hand, no
  try/finally (`:449`–`452`).
- Edits mid-run take effect only at next run boundary, with no word to the
  operator beyond `executable = False`.

## 2. pychron-cpp now

One table, `QueueTableModel` (`apps/pychron-ui/src/queue_table_model.hpp`,
`.cpp`). Runs **stay in place**; state shown by row colour.

- Columns (`hpp:44`): `#`, Status, Identifier, Aliquot, Step, Type,
  Position, Extract, Script, Plan, Conditionals, Comment, Est. Editable:
  Identifier, Position, Extract, Script, Plan, Comment
  (`cpp:144`–`156`), only when `row_editable` (`hpp:113`:
  `!locked_ && row >= frozen_rows()`).
- Colours: `state_color` (`cpp:485`), whole-row background: success, failed
  (error), cancelled/aborted (stopped), running states (progress). Skipped:
  faint text. Diagnostics error / warning tint when no run status.
- Live editing (experiment window spec
  `docs/superpowers/specs/2026-10-02-experiment-window-design.md` 4.3, 5.2):
  rows past the frontier editable while running; each change goes through
  `ExperimentBridge::edit` → `LabSession::edit` → `Executor::edit`
  (`libs/experiment/src/executor/executor.cpp:431`), refused on stale
  version or frozen row, refusal shown as "Edit refused". Frontier
  (`QueueFrontier{frozen, version}`) freezes a row once started, waited on,
  or skipped past. Queue-level fields and Open / New refused while running.
  Strictly safer than legacy.
- Events `RunStarted`, `RunStateChanged`, `RunFinished` relayed on the main
  thread (`experiment_bridge.cpp:39`–`45`). Status cleared only on Start
  (`clear_status`), not persisted.
- `executor_state.json` written at run consumption and end
  (`executor.cpp:463`, `:643`); `resume_row` exists (`executor.cpp:485`,
  `session.cpp:311`) but has **no UI caller** (tests only).
- History: timeline in the Executor dock (`timeline.hpp`, follows the
  active run); no executed-runs table, no `rem` / `ex` files.

## 3. What legacy's split gives, and what it costs

Gives: pending list short and always at the top of its grid; executed runs
out of the way; edit target obvious (whatever is in the pending grid).

Costs: a run leaves the editable grid the moment it starts, so its row
number changes under the operator; executed history truncated to 5; two
selection models, two scroll positions; queue order no longer visible in one
place (needed to read a step-heat sequence).

pychron-cpp already has the safety the split gave legacy (frozen rows, edits
refused past the frontier); what it lacks is the *visual* separation: with
long queues the editable part scrolls away below finished rows.

## 4. Options for brainstorm (author's suggestion first)

A. **One model, two views.** Keep `QueueTableModel`; show it through two
   `QSortFilterProxyModel`s: "Executed" (rows `< frozen_rows()`, read-only,
   collapsible, all of them, auto-scroll to newest) above "Queue" (rows
   `>= frozen_rows()`, editable). Row numbers stay the queue's. No change to
   executor, edit path or file. Running row: in Executed (legacy) or pinned
   at top of Queue: owner's choice.
B. **One table, frozen part collapsible.** A "Hide executed" toggle (or a
   fold row "12 runs done") and auto-scroll to the frontier. Smallest change;
   no second view.
C. **Legacy literal.** Two lists in the model, rows moved at start, `rem` /
   `ex` files. Copies legacy's costs (renumbering, two selections) and needs
   changes in edit versioning, which addresses rows by index.

Author leans A: legacy's look, current edit safety, nothing below the model
changes.

## 5. Questions for owner

1. A, B or C?
2. Does the running run sit with executed (legacy: moves at start) or with
   the queue until it finishes?
3. Executed list: all runs or last N (legacy 5)? Collapsible?
4. Write `rem` / `ex` queue files after each run (legacy), or is
   `executor_state.json` enough? Related: wire `resume_row` into the window
   (resume after a crash), now library-only.
5. Reset / "clear executed" / repeat last run: wanted, and where (menu,
   executed grid's context menu)?

## Related, found in passing

- Spec 5.2 says skipped rows grey and failure on the row header; code tints
  whole row and greys text. Small spec/code gap.
- Crash resume (`resume_row`) is library-only.

## Out of scope

Queue file format (T041, `2026-10-13-queue-format-survey.md`); any code.

## 6. Owner's decisions (2026-10-13)

1. Option A: one `QueueTableModel`, two views.
2. A run moves to Executed as soon as it starts.
3. Executed shows every executed run, and is collapsible.
4. No `rem` / `ex` files (T041). Crash resume in the window: not answered yet.
5. Reset, clear executed and repeat last run: wanted, as menu actions.

Next: spec for option A.
