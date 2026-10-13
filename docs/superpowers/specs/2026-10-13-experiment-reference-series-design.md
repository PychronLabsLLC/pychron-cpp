# Reference series in the experiment window

Status: agreed in outline 2026-10-13 (owner: blanks and airs as a live time
series pane; section 9). Drafted by an unattended run. Nothing built. Task T028 (`pychron usage notes.md`):
"implement legacy's auto time series plotter in the experiment".

## 1. Problem

While a queue runs, operator judges the instrument by its references: are
blanks falling, is air 40/36 steady, did the cocktail move. Experiment window
shows only the run being measured (`EvolutionsView`, cleared at each run
start) and the queue table. To see the last blanks operator opens the data
window, selects them, opens a time series; that figure holds a fixed list of
analyses (`figure_window.hpp:41-45`) and does not follow the queue. Only the
browser list follows (`main_window.cpp:346`, `data_browser_window.cpp:405`).

## 2. Legacy (examined 2026-10-13, `~/Programming/pychron`)

Two things answer to the task's words.

- **Time series pane**, alive: dock `pychron.experiment.timeseries`
  (`experiment/tasks/experiment_panes.py:502-523`). After each run that
  passes the post-run check (`experiment_executor.py:1745-1752`), and on a
  refresh button, it asks the database for the last N analyses that are not
  unknowns, of the queue's mass spectrometer, since a reset point
  (`get_last_n_analyses(timeseries_n_recall, exclude_types=("unknown",),
  low_post=...)`, `:3540-3548`). N default 50, a spin box. Grouped by
  analysis type into tabs, fixed in code: `air`, `cocktail`,
  `blank_unknown`, `blank_air`, `blank_cocktail` (`:3652-3655`). Each tab a
  series figure with the usual series options (Ar40 to Ar36, F). Reset
  button moves the reset point to now. Refreshes in place.
- **Autoplot**, dead: `use_autoplot` was to open, after each saved run, an
  ideogram, spectrum or series of that run's identifier
  (`experiment/tasks/experiment_task.py:969-1000`). Its handler returns at
  line 925 ("suppressed for stability"); no preference shows the switch.

This spec is the first. The second is out of scope (section 7).

## 3. Goal

A dock in the experiment window that always shows the recent blanks and airs
of this spectrometer as time series, a tab for each, and redraws itself when
a run is saved.

## 4. What exists to build on

- `processing::build_time_series(const Dataset&, const Options&, double now)`
  (`libs/processing` `time_series.hpp:21`): any quantity (`Ar40`,
  `Ar40/Ar36`, `Ar40.bs_corrected`, `.baseline`, `.intercept`, `age`,
  `extract_value`, ...), fits none to exponential, panels.
- Factory presets of `figure.time_series` (`time_series.cpp:75`): Default,
  Air monitor, Blanks, Unknowns, Spectrometer.
- `SceneView`: zoom, pan, save, copy, tooltip, context menu.
- `ExperimentBridge::runFinished`; the data source's `generation()`, which
  changes when the store has something new.

## 5. Design

### 5.1 Dock

New dock "References" in `ExperimentWindow`, tabbed with Evolutions by
default (`default_layout`). Hidden when the session has no store (a queue
saved to files only): nothing to read.

- Tab bar: two tabs, Blanks and Airs (owner, 2026-10-13). Blanks holds every
  blank kind the store has (`blank_unknown`, `blank_air`, `blank_cocktail`),
  each kind a group of the figure with its own colour and legend entry.
  Airs holds `air`. Other reference types (cocktail, detector_ic) are not
  shown: out of scope, section 7.
- Each tab: one `SceneView` drawing `build_time_series` of that type's
  analyses.
- Header: "Last N" spin box (default 50, 5 to 500), Reset button ("from
  now": only analyses after this moment), Refresh button, a note of when it
  was last read.

### 5.2 What is read

One query when the dock is first shown, after each `runFinished` whose save
succeeded, and on Refresh: the newest N analyses whose type is a blank kind
or `air`, of the session's spectrometer, with run time at or after the
reset point (none: no lower bound). Then loaded and reduced as a figure
window loads its selection.

- Query and load run off the UI thread and off the executor's, the way a
  figure window loads its selection (not read for this draft: the plan
  names the mechanism). A read still running when the next run finishes is
  followed by one more read, not a queue of them.
- Query is by spectrometer and time, ordered by time descending with a
  limit: must use an index in that order (persistence rules, "Queries":
  `ORDER BY ... LIMIT` over `analysis`). Plan test in `SchemaIndexes` if a
  new statement is needed; no migration expected, to be checked against
  the browse statement the data window already uses.
- Unchanged `generation()` since the last read: nothing is read.

### 5.3 Options

- Each tab is a time-series figure with options and presets like any other:
  a small options button on the tab opens the generated editor for
  `figure.time_series`; `PresetBar` as in figure windows.
- Starting preset: Blanks tab "Blanks", Airs tab "Air monitor". Operator's choice per type is kept in the window's
  settings (`experiment_window/reference_series/<type>/preset`).
- N and the reset point: N kept in settings; reset point is the session's
  (gone when the window closes).

### 5.4 Time

"Now" for the figure's x axis and for Reset comes from the line's wall
clock (`CoreBridge::wall_now()`): a simulated session resets and plots in
simulated time (AGENTS.md, Time).

## 6. Rules

- The dock computes nothing: scene from `libs/processing`, drawn by
  `SceneView`.
- No read on the UI thread; no read per analysis beyond what a figure
  window's load does for the same N.
- A failed read leaves the last figure and says so in the header; it never
  stops or delays the queue.
- A queue not saving to a store shows no dock, not an empty one.

## 7. Out of scope

- Legacy's autoplot (a figure of the current sample after each run).
  Nearest thing here would be a figure window that follows a selection;
  separate task.
- Unknowns in the dock (an "Unknowns" tab of ages against time), and
  cocktails or other reference types.
- Making every figure window follow the store.
- Alarms on a reference drifting (conditionals on a series).

## 8. Tests

- `libs/processing` or the workspace, no UI: the reference query returns
  the newest N blanks and airs of one spectrometer after a reset point,
  newest first; unknowns, cocktails and another spectrometer's are left out.
- UI (`tests/ui/test_experiment_window.cpp`, simulated lab with a store):
  after a queue of blank, air, unknown, blank: Blanks has two points and
  Airs one; the unknown is in neither; a further blank adds a point without
  touching the dock; Reset empties the tabs and the next reference returns
  one; N = 1 shows the newest only; a session without a store has no dock.
- Preset kept per type across two windows.
- Changes `apps/pychron-ui`: final run is `dev-ui`. A query added: numbers
  from `StoreLoadTiming`-style timing on a lab's store copy in the commit
  message.

## 9. Owner, 2026-10-13, and what is kept as drafted

Decided: the live time series pane, not the dead autoplot; blanks and airs.

Kept as drafted, not asked again (say if wrong):

- Blank kinds share one Blanks tab, as groups.
- Spectrometer: the session's only, no chooser.
- N defaults to 50; no lower bound in time until Reset is pressed.
- A dock in the experiment window, not a data window that follows.
