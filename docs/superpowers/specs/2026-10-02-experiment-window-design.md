# Experiment window (queue, executor, evolutions)

Date: 2026-10-02
Status: Implemented (v1)
Owner: Jake Ross
Depends on: `2026-09-29-experiment-system-design.md` (section 10.4, stage E7),
`2026-10-01-spectrometer-window-design.md` (bridge pattern, QCustomPlot,
`ScanService` pause/resume), `2026-09-29-instrument-control-design.md`
(Qt-free core, CoreBridge pattern).
Reference: legacy pychron `pychron/experiment/tasks/experiment_panes.py`,
`experiment/queue/experiment_queue.py`, `experiment/executor.py`,
`experiment/automated_run/plot_panel.py`.

## 1. Goal

Run an experiment queue from `pychron-ui` the way `elctl exp run` does, and
watch it: open a queue, see it validated row by row, edit it, start it, follow
each run's state, block and counts, see why the executor is waiting, stop,
cancel, abort or truncate with a confirmation, and watch the isotope
evolutions of the run being measured.

Success: `pychron-ui --sim --sim-speed 400 --lab configs/examples
configs/examples/experiment.toml`, Window > Experiment, Start: the three
example runs go green one after another, the evolutions plot fills for each,
and the records land under the data directory, exactly as with `elctl`.

## 2. Scope

Version 1 (this spec):

- Core: lab loading and queue checking moved out of `elctl` into
  `libs/experiment` (`lab`), a `LabSession` that assembles the run services
  and runs the executor on its own thread, a queue snapshot event from the
  executor, and a `ClockPump` for simulated time shared by `elctl` and the UI.
- UI: `ExperimentBridge`, queue table model with inline validation and row
  operations, executor pane, evolutions view, `ExperimentWindow`, app wiring.

Added after v1: the run factory side panel (section 5.6), with frequency
insert and per-type field enabling.

Deferred to later versions (from spec 10.4): measurement panel (template picker, parameter form, override diff
badges), script editor, phase timeline and overlap lanes, fit overlay and
intercept display in the evolutions, notifications, editing a queue while it
runs.

## 3. Decisions

| Decision | Choice | Reason |
|---|---|---|
| Where lab loading lives | `libs/experiment` `lab/` (Qt-free) | `elctl` and the UI must agree on what a lab directory is and on what makes a queue runnable; one implementation. |
| Who owns the executor thread | `LabSession` (core) | The UI must not own hardware lifecycle (same rule as `ScanService`). `elctl` uses it too. |
| Queue edits during a run | Not allowed in v1; the table is read-only while running | The executor mutates the queue (conditional queue actions) on its thread. Editing both sides needs an executor-side edit API; deferred. |
| Seeing executor-side queue edits | Executor publishes `QueueEdited{QueueSpec}` (a copy) after applying them | A copy published on the bus is race-free; the UI replaces its table from it. |
| Spectrometer window scanning while a queue runs | `LabSession` pauses the `ScanService` for the queue and resumes it after | Measurement needs the acquisition engine; `start_acquisition` fails while a free-running scan holds it. |
| Simulated time in the UI | `--sim-speed` with the same `ClockPump` as `elctl` | Example runs take minutes of instrument time; the pump runs the scheduler inline so polling keeps pace with the clock. |
| Evolution data | `collect::SeriesUpdated` on the bus, coalesced like readings | Already published per reading; no new core API. |
| Plot widget | QCustomPlot, as the strip chart | Already a UI dependency. |
| Confirmations | Injectable callbacks, default `QMessageBox` (default button No) | Testable headless, as the spectrometer window's large-move confirmation. |

## 4. Core design (Qt-free)

### 4.1 `lab` (`libs/experiment`, `pychron/experiment/lab/lab.hpp`)

Moved from `apps/elctl/src/exp.cpp`, behaviour unchanged:

```cpp
struct LabPaths {
  std::filesystem::path dir;              // plans/, scripts/, conditionals/, identifiers.toml, peak_center.toml
  std::filesystem::path line_config;      // extraction_line.toml; empty: none
  std::filesystem::path spectrometer;     // spectrometer config; empty: none
};

struct Lab { ... };                       // as elctl's Lab, minus the data dir
Lab load_lab(const LabPaths&);            // never fails; problems are collected in lab.problems

struct LabCheck {
  QueueReport report;                     // check_queue against the lab's resolvers
  std::vector<Diagnostic> extra;          // lab problems (run -1), peak-center configs, conditionals
  bool ok() const;                        // no Error in either
  std::vector<Diagnostic> all() const;    // report.diagnostics then extra
};
LabCheck check_lab_queue(const Lab&, const QueueSpec&);
```

`check_lab_queue` is `elctl`'s `report()` without the printing. Per-run
conditional diagnostics carry `run = i` and `field = "conditionals"`, each
distinct message once. `elctl exp validate/run` print from it.

### 4.2 `LabSession` (`pychron/experiment/lab/session.hpp`)

```cpp
struct SessionHardware {
  systems::ExtractionLine& line;                  // started; shares clock, scheduler, bus
  spectrometer::Spectrometer* spectrometer = nullptr;
  spectrometer::ScanService* scan = nullptr;      // paused while a queue runs
};
struct SessionOptions {
  std::filesystem::path data;                     // records/, spool/, executor_state.json
  executor::ExecutorOptions executor;             // state_file defaults to data/executor_state.json
};

class LabSession {
 public:
  LabSession(const Lab&, SessionHardware, SessionOptions);   // lab and hardware must outlive it
  ~LabSession();                                             // aborts a running queue and joins
  Result<void> start(QueueSpec queue, std::size_t from_row);  // Config error if running or the queue does not check
  void stop(); void cancel(); void abort(); void truncate(bool quick = false);
  bool running() const;
  executor::ExecutorState state() const;
  std::optional<executor::QueueResult> wait();                // joins; nullopt if never started
  std::size_t pending_saves() const;                         // records still in the spool
  static Result<std::size_t> resume_row(const std::filesystem::path& data);
};
```

- Services are assembled once, in the constructor, exactly as `elctl`
  assembles them today (script host, valve services, spectrometer port,
  peak-center port, instrument metrics, persisters, blank factory).
- `start` runs `Executor::execute` on a session-owned thread on a private
  copy of the queue. Before it, a running `ScanService` is paused; after the
  queue ends it is resumed (errors from resume are published as a `Log`).
- A `QueueEnded{QueueResult}` event is published on the bus when the
  executor returns, after the scan is resumed.
- All controls are thread-safe and non-blocking.

### 4.3 Executor: `QueueEdited`

```cpp
struct QueueEdited {
  QueueSpec queue;                       // the whole queue after the edit
  std::vector<std::string> changes;      // as RunSummary::queue_changes
};
```

Published by `Executor::finish` after a run's queue actions or post-run
conditionals changed the queue, before `RunFinished`.

### 4.4 `ClockPump` (`libs/core`, `pychron/core/clock_pump.hpp`)

```cpp
class ClockPump {
 public:
  ClockPump(ManualClock&, double speed);          // advances speed ms per real ms
  ~ClockPump();                                   // stop()
  void drive(Scheduler*);                         // run_pending() after each step; null stops driving
  void stop();                                    // joins the pump thread
};
```

What `elctl` does inline today, moved so the UI can use it. The driven
scheduler must have `threads = 0` and no dispatcher, and must outlive the
pump or be cleared with `drive(nullptr)` first (which waits for a step in
progress).

## 5. UI design (`apps/pychron-ui`)

### 5.1 `ExperimentBridge`

A `QObject` built from `LabSession&`, `const Lab&` and the shared `SignalBus&`.
Same rules as `SpectrometerBridge`: bus handlers reach it through a Gate and
post queued calls; no core object holds a `QObject*`.

- Relayed events (each a signal of the same name): `ExecutorStateChanged`,
  `RunStarted`, `RunFinished`, `ExecutorWaiting`, `run::RunStateChanged`,
  `measurement::BlockStarted`, `BlockFinished`, `CountsProgress`,
  `ConditionalTripped`, `QueueEdited`, `QueueEnded`,
  `jobs::PeakCenterDone`.
- `collect::SeriesUpdated` is coalesced into one batch per queued call, in
  order (`seriesUpdated(batch)`), as the spectrometer bridge does readings.
- Commands forward to the session (all non-blocking): `start(queue, row)`
  returns the `Result` at once, `stop`, `cancel`, `abort`, `truncate`.
- `check(queue)` runs `check_lab_queue` on the calling thread (no hardware,
  milliseconds).

### 5.2 `QueueTableModel` (`QAbstractTableModel`)

Columns: `#`, Status, Identifier, Aliquot, Step, Type, Position, Extract,
Script, Plan, Comment, Est. (Status sits next to `#` so it stays in view
beside the Evolutions dock).

- Rows mirror a `QueueSpec`. `set_queue(spec)` replaces it (from a file or a
  `QueueEdited`) and revalidates.
- Revalidation runs `ExperimentBridge::check` after every edit. Rows with an
  Error are drawn with a red row header and an error icon; the tooltip lists
  every diagnostic for that row (`field: message`). Warnings are yellow.
  Queue-level diagnostics are exposed separately (`queue_diagnostics()`).
- Editable columns (when not locked): Identifier, Position, Extract,
  Script, Plan, Comment. Identifier changes re-derive the analysis type
  through the lab's identifier rules; Type is read-only. An edit that does
  not parse (e.g. a non-number in Extract) is rejected and the cell keeps
  its value.
- Row operations through `ExperimentQueue`: move up/down, duplicate (copy
  below), delete, toggle skip, toggle end-after. Each emits `edited()`.
- Status column: blank until a run starts; then the run state from
  `RunStateChanged` (`extracting`, `measuring` ...), and the final state with
  its error from `RunFinished`. Colours: running blue, success green, failed
  red, cancelled/aborted orange, skipped rows grey. A truncated run shows
  "success (truncated)".
- Est. shows the per-run estimate (`h:mm:ss`) from the last check.
- `set_locked(bool)` makes every cell and operation read-only (while
  running).

### 5.3 `ExecutorPane`

A widget, docked at the bottom of the experiment window:

- State label (`ExecutorState`), queue progress `n/N` runs and a progress bar
  over the non-skipped rows, ETA of the remaining rows.
- Current run: identifier, run state, block, and a counts bar
  (`CountsProgress` i/n).
- Wait line: `ExecutorWaiting` reason and its duration (simulated time
  makes a wall-clock countdown wrong), cleared when a run starts, its state
  changes or a block starts.
- Buttons: Start (from the selected row, or row 0), Stop, Cancel, Abort,
  Truncate. Enabled by state: Start only when idle and the queue checks;
  the others only while running. Cancel and Abort ask first (default No);
  Stop and Truncate do not.
- Conditionals list: each `ConditionalTripped` as `time  run  name: check
  -> action`.
- Event list: one line per run start/finish, queue edit, peak-center result
  and queue end (newest last).
- Pending saves: "n record(s) in spool" when non-zero after the queue ends.

### 5.4 `EvolutionsView`

A QCustomPlot widget showing the run being measured:

- Tabs Signal, Baseline, Sniff (the series kinds); one graph per series key
  `isotope:detector` in the detector's colour (from the spectrometer config,
  palette fallback), legend on, x = seconds since the measurement epoch.
- Cleared on `RunStarted`; filled from `seriesUpdated` batches; repaints
  capped at 20 Hz.
- A title line: run identifier, block label and `count/target`.
- Peak-center results of the current run below the plot: detector, isotope,
  center or failure message.

### 5.5 `ExperimentWindow`

- Separate top-level `QMainWindow` titled "Experiment", plus
  " (Simulation)" when the session's line is simulated.
- Centre: the queue table (row selection, context menu with the row
  operations), queue-level diagnostics in a strip above it.
- Bottom dock "Executor" (`ExecutorPane`), right dock "Evolutions".
- Menus: Queue (Open..., Save, Save As..., Revalidate), Rows (Move Up,
  Move Down, Duplicate, Delete, Toggle Skip, End After; also the table's
  context menu), Executor (Start (F5), Stop, Cancel..., Abort...,
  Truncate). Opening while running is refused. Unsaved edits mark the title
  with `*`; closing or opening another queue with unsaved edits asks (Save /
  Discard / Cancel; Cancel keeps the window open, and quitting the app with
  it). Closing while a queue runs asks whether to stop it after the current
  run; the queue keeps running if the window just hides.
- Settings (`QSettings`, group `experiment_window`): geometry, dock state,
  last queue path, column widths.

### 5.6 Run factory panel

A left dock "Run Factory" over `experiment::FactoryForm`
(`pychron/experiment/factory/form.hpp`, Qt-free and unit-tested):

- Run: type (unknown or a special; picking a special fills its identifier),
  identifier, aliquot (blank: assigned at run start), step.
- Extraction: device, position (`4`, `1-6`, `1,3,5`; "one run per hole" with
  an optional identifier step per run), value and units, duration, cleanup,
  script (the lab's extraction scripts), step heat (`5, 10, 15` or
  `start:increment:count`; one run per value, steps A, B, C ...).
- Measurement: plan (the lab's plans), post-equilibration and
  post-measurement scripts, comment. Overrides come along from defaults or a
  row; the form does not edit them.
- Fields the analysis type does not use (`rules_for`) are disabled, and what
  they hold is stripped from the runs. A change of type applies the lab's
  `defaults.toml` entry for (type, device) when there is one; Defaults applies
  it on demand.
- A preview line says what Add inserts ("Adds 4 run(s): 20001 @1 … 20001 @4")
  or why it cannot (bad input, or the new runs failing the lab check: unknown
  plan or script, ...); Add is disabled then. The preview and Add stay in view
  below the scrolling form.
- Add (Ctrl+Return) inserts after the selected rows (or appends) and selects
  the new rows; then the identifier and/or position advance by the
  auto-increment settings, past what was just added. From Row fills the form
  from the first selected row.
- Frequency: a special run (from the lab's defaults) after every N unknowns,
  and/or before the first and after the last; with several rows selected,
  only that range counts.
- Block: one of `<lab>/blocks/*.toml`, repeated N times, inserted like Add.
- Locked (everything disabled) while a queue runs.

### 5.7 App wiring

- `pychron-ui [extraction_line.toml [canvas.toml]] [--sim] [--spectrometer
  <file>] [--lab <dir>] [--data <dir>] [--queue <file>] [--sim-speed <x>]`.
- `--lab` defaults to the extraction line config's directory; `--data`
  defaults to `<lab>/data`. `--queue` opens that queue when the window
  first shows. `--sim-speed` needs `--sim` (usage error otherwise) and puts
  the whole app on a `ManualClock` driven by a `ClockPump`.
- The session is built after the line starts and before the windows are
  shown; a session that cannot be built (no line, bad lab) is not fatal:
  the error goes to the log dock and the menu item stays disabled.
- `MainWindow` gains Window > Experiment (Ctrl+Shift+E).
- Teardown: experiment window, main window, experiment bridge, session
  (aborts and joins a running queue), spectrometer bridge, scan service,
  pump (`drive(nullptr)`), line stop, spectrometer, beam registry, line.

## 6. Error handling

- A queue file that does not parse: message box, nothing replaced.
- `start` refused (errors, already running): banner in the executor pane.
- A run failure: red row with the error in the status tooltip; the queue
  end reason in the executor pane.
- Session destroyed while running: abort, join; records in flight go to the
  spool.

## 7. Testing

Core:

- `lab`: the elctl validation tests keep passing through the moved code;
  `check_lab_queue` reports a missing peak-center config and a bad
  conditional with row numbers.
- `LabSession`: runs the example queue on the sim lab with a `ClockPump`
  (pattern of `MeasurementSim.ExecutorRunsAQueueOnTheSimLab`); start while
  running is refused; a failing check is refused; a running `ScanService` is
  paused and resumed; `QueueEnded` is published; destruction mid-run aborts.
- Executor: a modification trip publishes `QueueEdited` with the new rows.
- `ClockPump`: time advances; a driven scheduler's job runs once per step;
  `drive(nullptr)` stops calls.

UI (headless, offscreen):

- `QueueTableModel`: rows and columns from a spec; an error row is flagged
  with its tooltip; editing Plan to an unknown plan flags the row and
  editing it back clears it; non-numeric Extract is rejected; move,
  duplicate, delete, skip, end-after; status colours from run events;
  locked refuses edits.
- `ExperimentBridge`: events arrive on the GUI thread in order; a burst of
  `SeriesUpdated` arrives as one batch.
- `ExperimentWindow` end to end on the sim lab with a `ClockPump`: open the
  example queue, Start, all rows reach success, the evolutions view gained
  points, `QueueEnded` shows "completed"; Cancel asks and declining sends
  nothing; save round-trips through `parse_queue`.
- `MainWindow`: Window > Experiment disabled without a session.

## 8. Rollout

1. Core: `lab` move (elctl onto it), `QueueEdited`, `ClockPump` (elctl onto
   it), `LabSession` (elctl onto it).
2. UI: `ExperimentBridge`, `QueueTableModel`.
3. UI: `ExecutorPane`, `EvolutionsView`, `ExperimentWindow`.
4. App wiring, docs.

Each step builds and passes the full test suite with and without `BUILD_UI`.
