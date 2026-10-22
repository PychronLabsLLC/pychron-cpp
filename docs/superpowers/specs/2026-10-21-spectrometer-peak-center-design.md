# Peak center from the spectrometer window

Status: agreed 2026-10-21 (developer's answers, section 9); built 2026-10-21
on branch `feat/spectrometer-peak-center`; moved to own Peak Center window,
plot axes fixed to sweep width, 2026-10-21 on branch
`feat/peak-center-window`. Related:
`2026-10-01-spectrometer-window-design.md` (peak center left out there, line
42), `2026-10-12-peak-center-plot-design.md` (section 6 lists "peak center
from spectrometer window" as not built), `2026-09-29-spectrometer-control-design.md`
section 9.4.

## 1. Problem

Peak center runs only from plan (`peak_center` step, `PeakCenterStep`,
`libs/experiment` `plan/plan.hpp:179`) or script hook `peak_center()`
(`libs/scripting` `services.hpp:54`). Operator tuning between runs must write
plan or script. Spectrometer window (`apps/pychron-ui/src/spectrometer_window.hpp`)
has no peak-center control. Named configs in `<lab>/peak_center.toml` edited
only by hand; lab reads file once at UI start (`main.cpp:733`,
`lab.cpp:148-150`), `LabSession` copies map once (`session.cpp:61`): hand edit
reaches plans only after restart.

## 2. Goal

From spectrometer window, operator:

- picks named set from `peak_center.toml`, overrides any field for one run;
- starts, watches sweep and fit live, cancels;
- sees result (old centre, new centre, shift, table value);
- decides whether field table takes new value ("Apply to field table");
- optionally writes overrides back to set ("Save to set…"); next queue start
  uses it.

Plan path behaviour unchanged.

## 3. Core

### 3.1 Runner: unchanged

`run_peak_center` (`libs/systems` `jobs/peak_center.hpp:176`) used as is.
Window sends config with `update_table = false`. On ok fit runner then
`move_native(center)` (`peak_center.cpp` `Runner::apply`), fills
`table_value`, `table_updated = false`. Runner takes `hold_table("peak_center")`
for run's length, as today.

### 3.2 Writer

`libs/systems` `jobs/peak_center.hpp`:

```cpp
// The body of one named table of peak_center.toml (keys only, no header),
// such that parse_peak_center_configs("[" + name + "]\n" + body) gives back
// `config` (name and save_table aside: neither is a file key).
std::string format_peak_center_config(const PeakCenterConfig& config);
```

Rules:

- Every file key written, parser's names: `detector`, `isotope`,
  `additional_detectors`, `center` (only when set), `window`, `step`,
  `percent`, `min_peak_height`, `test_peak_flat`, `n_tries`, `direction`
  (`"increase"`/`"decrease"`), `integration_s`, `settle_s`,
  `baseline_timeout_s`, `dac_offset`, `peak_shift_threshold`, `update_table`,
  `propagate`.
- Durations as seconds, doubles written so they parse back equal
  (round-trip test over every key, `operator==` on config minus `name`,
  `save_table`).
- Goes through toml++ formatting, so `TOML_FLOAT_CHARCONV=0` rule holds: same
  bytes on every compiler.

### 3.3 Apply

Apply = exactly what runner does when `update_table` true, minus move:

```cpp
spec.update_table(detector, isotope, table_value, propagate);
spec.save_table(spec.clock().wall_now());
```

One helper does both, used by runner's `update_table` branch and by Apply:
`Result<std::vector<std::string>> write_peak_center_to_table(Spectrometer&,
const DetectorId&, const std::string& isotope, double table_value, bool
propagate, bool save)` (returns notes). Same calls, so table after Apply
equals table after `update_table = true` run with same fit. Save failure:
in-memory table kept, failure reported (same as runner's note "field table
not saved: <why>"). No `position` afterwards: magnet already at fitted
centre.

### 3.4 Sets re-read at queue start

`LabSession::start` reads `lab.paths.dir / "peak_center.toml"` (through new
`lab::read_peak_center_file`, also used by `load_lab`) before `check(queue)`;
`check_lab_queue` takes optional map, null = `lab.peak_centers`:

- File absent: empty map (plans fall back to `default`, as today).
- Parse or validate error: `start` fails, `ErrorKind::Config`, message names
  file and error (parser's message already does). Queue not started.
- Ok: fresh map replaces session's copy; `check` validates plan refs against
  it (today `lab.cpp:328-334` reads `lab.peak_centers`; check moves to
  session's map); `SpectrometerPeakCenter` gets it through new
  `set_configs(std::map<std::string, PeakCenterConfig>)`, under its mutex.

Load-time parse error of `peak_center.toml` (`Lab::peak_center_problem`) is
not counted by `check_lab_queue` when `start` supplies fresh map: file was
read again, load-time error stale.

UI check before Start same file: `LabCheck LabSession::check_queue(const
QueueSpec&) const`. No queue running: `peak_center.toml` read again each
call (small file; no mtime cache), map passed to `check_lab_queue`, so
load-time problem skipped as in `start`. Bad file: `Error` diagnostic, field
`lab`, text `start` refuses with, first of `extra`; plan refs then checked
against session's map (last start, else load). Queue running: session's map
(what `edit` checks against), file not read. `ExperimentBridge::check`
(queue table's checker, gates Start) calls it; run factory's probe goes
through `QueueTableModel::check_queue` (same checker), no reader of its own.
Set saved after load: Revalidate (or any edit) makes row runnable, no
restart.

`Lab::peak_centers` loaded at `load_lab` unchanged (problems list, elctl).
Map swapped only in `start`, never while queue runs: a run never changes
settings partway.

## 4. Bridge (`apps/pychron-ui/src/spectrometer_bridge.hpp`)

### 4.1 Interface

```cpp
// Non-blocking. Refused (peakCenterFinished with Config error) while one is
// running. Pauses the live scan when running, restarts it after at the
// integration it had.
void peak_center(jobs::PeakCenterConfig config);
// No-op when none running. Stopped: scan run stopped stays stopped (window
// closing would stop it at once).
enum class ScanAfter { Resume, Stopped };
void cancel_peak_center(ScanAfter after = ScanAfter::Resume);
// Teardown: cancels (ScanAfter::Stopped), waits, delivers finish at once.
void cancel_peak_center_and_wait();
bool peak_center_running() const;     // main thread

// While a run goes: refused, commandFinished("start" / "position") with
// Config "a peak center is running"; nothing changed (no scan intent). Scan
// beside sweep or magnet move would spoil fit. stop_scan not refused.
void start_scan(double integration_s);
void position(const QString& isotope, const QString& detector);

// Non-blocking, on the command executor; report once each.
void apply_peak_center(QString detector, QString isotope, double table_value, bool propagate);
void save_peak_center_set(std::filesystem::path file, QString name, jobs::PeakCenterConfig config);

signals:
void peakCenterTryStarted(const pychron::jobs::PeakCenterTryStarted& e);  // relayed from bus
void peakCenterPoint(const pychron::jobs::PeakCenterPoint& e);            // relayed, batched like readings
void peakCenterFinished(const pychron::Result<pychron::jobs::PeakCenterResult>& result);
void peakCenterApplied(const pychron::Result<void>& result);
void peakCenterSetSaved(const QString& name, const pychron::Result<void>& result);
void peakCenterNote(const QString& note);  // e.g. "field table not saved: <why>" after Apply

bool valid_set_name(const QString& name);  // free function: [A-Za-z0-9_-]+
```

`PeakCenterDone` on bus not relayed separately: `peakCenterFinished` carries
the result, including Cancelled and hardware errors that publish no
`PeakCenterDone`. Bridge passes `PeakCenterOptions{.bus = &bus}`, so bus
events of plan runs also seen; bridge relays only events of its own run
(between its start and finish), so a plan's peak center (refused in UI anyway,
section 5.4) never draws in window.

### 4.2 Thread

Run on own `std::thread`, not command executor (executor stays free for
`stop_scan`, `set_integration`, table re-reads). Clock rules of AGENTS.md
"Time":

- Starter makes `Clock::Hold` on spectrometer's clock before `std::thread`;
  thread constructs `Clock::Participant` first, drops its hold copy; starter
  drops its own after thread created.
- Last act of thread, still participant: sets done flag under bridge mutex,
  `clock.notify_all(cv)`. Joiner waits flag with `clock.wait`, then `join()`.
  Never `Clock::Detached` around it.
- Cancel: `jobs::CancelToken` has no clock; plain token used, runner's hook
  ends an acquisition. Cancel takes effect at next step boundary: a settle
  or other clock wait runs to its end.
- Result posted to main thread through `Gate`, as other commands.

Destructor: cancels, waits as above, then existing teardown.

### 4.3 Live scan

Scan pause decided from scan operator last asked for (main-thread intent,
`scan_intent_`), not last reported status: a `start_scan` just issued has not
reported yet. If scan wanted at start: `ScanService` stopped first (on
executor, in order), then run. After run (any outcome): scan restarted with
integration it had, unless operator pressed Stop scan meanwhile (existing
`stop_requested_` rule) or run cancelled with `ScanAfter::Stopped`. Runner's
`integration` override (non-zero) changes integration for run only; restart
puts old one back.

Intent cleared (nothing to put back) by: `stop_scan`; a `start` whose result
is an error, unless a newer `start_scan` or `stop_scan` came after it
(generation counter); a `ScanStatus` with `running = false` (fault, other
program), but only when no `start_scan` is still unreported and no window
run goes (run's own stop is not that).

### 4.4 Save to set

`save_peak_center_set` on executor:

1. `format_peak_center_config(config)`.
2. `config::set_local_table(file, name, "[" + name + "]\n" + body)`
   (`libs/core` `config/local_file.hpp`): replaces that table (and
   `[name.*]`) in place; comments above it, its position, file's line ending
   (LF or CRLF), comments and every other set left byte for byte; table
   absent → appended after blank line; all or nothing; file made when
   absent. (`replace_local_table` drops comments above table and appends:
   not used here.)
3. Re-parse whole file with `parse_peak_center_configs`; error → reported
   (file already written; message says so).

Name rules: TOML bare key `[A-Za-z0-9_-]+`; else refused before write.
Existing name overwritten only after window asked (5.2).

## 5. UI

### 5.1 Peak Center window

- Own top-level window `PeakCenterWindow`
  (`apps/pychron-ui/src/peak_center_window.{hpp,cpp}`, `QMainWindow`), title
  "Peak Center" ("Peak Center (Simulation)" in simulation). Central widget:
  horizontal `QSplitter` `peakCenterSplitter`; left `PeakCenterPanel`
  (`peak_center_panel.{hpp,cpp}`, contents 5.2) inside a `QScrollArea`
  (form taller than a laptop screen; window not held up by it), right
  `PeakCenterView` (`peak_center_view.hpp`). Replaces the "Peak center" and
  "Peak center plot" docks the spectrometer window had (`peakCenterDock`,
  `peakCenterPlotDock`; gone, with their View-menu entries and default
  layout). Saved spectrometer dock state naming them still restores:
  `restoreState` skips unknown objects.
- Wiring: panel's `startRequested` clears plot; bridge's
  `peakCenterTryStarted`/`peakCenterPoint` feed it; `peakCenterFinished` with
  a result (ok or failed fit) → `on_done`, an error → nothing.
- Interface: `set_peak_center_file(path)`, `set_queue_running(fn)`,
  `queue_state_changed()`, `peak_center_running()`,
  `cancel_peak_center_and_wait()` (MainWindow teardown, `ScanAfter::Stopped`),
  `set_ask(fn)` (close question, 5.3), `peak_center_panel()`,
  `peak_center_view()`.
- Settings: `peak_center_window/<spectrometer name>/geometry` and
  `.../splitter`, written at close, restored at construction; a value Qt
  refuses leaves defaults.
- Plot axes: x fixed from a try's start to the union of the tries in hand
  (`PeakCenterTryStarted::start/end`; for a displayed result its
  `PeakCenterTry::start/end`), either order; a wider later try widens it once,
  at its start, never per point; no try with start ≠ end → x from what is
  drawn. y rescaled each redraw over every drawn graph (markers too), 5 %
  padding each side; 0..1 with no value. Accessors `x_range()`, `y_range()`
  (`QCPRange`). Same view in experiment window's Evolutions → Peak center
  tab: same axes there.
- Spectrometer window during a run (bridge's `peakCenterStarted` until
  `peakCenterFinished`, whichever window started it): magnet Apply, banner's
  Restart, rise rate Start disabled; `apply_position`, `restart_scan` do
  nothing; rise rate run going ended without result ("a peak center was
  started while it ran"); window shown meanwhile starts no scan. Graph,
  detectors, integration unchanged. Bridge refuses too (4.1).
  `peakCenterStarted()`: bridge signal, emitted on main thread from inside
  `peak_center()` when it accepts a run, never for a refused start; unlock on
  `peakCenterFinished` via `lock_for_peak_center(peak_center_running())`
  (refused second start keeps lock).

### 5.2 Peak center panel

- Set: combo of names in `peak_center.toml` (file read at window open and
  after each save); built-in `PeakCenterConfig{}` named `default` when file
  absent or has no `default`. File given by
  `PeakCenterWindow::set_peak_center_file(path)` (`<lab>/peak_center.toml`),
  set by `MainWindow` like `set_rise_rate_file`.
  Unparseable file: banner names error, combo holds built-in `default` only,
  Save disabled (would overwrite what operator must fix by hand).
- Form, one field per file key: detector (combo, spectrometer's detectors,
  empty = reference), isotope (combo, `isotopes_for(detector)`), additional
  detectors (checklist), center (optional), window, step, percent, min peak
  height, test peak flat, tries, direction, integration s, settle s,
  baseline timeout s, DAC offset, peak shift threshold, propagate.
  `update_table` not shown (window never writes on its own; 3.1); its file
  value kept on save.
- Picking set loads form. Edited field marked (bold label). "Revert to set"
  reloads.
- Form validated with runner's rules (`validate` in `peak_center.cpp`, made
  public as `validate_peak_center_config`): invalid → Start and Save
  disabled, reason under form.
- Start: disabled while running, while queue running (5.4), while form
  invalid. Cancel: enabled while running.
- Result area: status line (`message`, or error), initial centre, fitted
  centre, shift (fitted − initial), table value, additional detectors'
  centres, notes. Native units, axis name from config.
- "Apply to field table": enabled only when last result `ok`, has
  `table_value`, isotope non-empty, active table unchanged since result
  (no `fieldTableChanged` since), no run going. Disabled after Apply.
  Confirms first: "Write <table value> for <isotope> on <detector> to
  <table>? Propagate: yes/no." Result in status line.
- "Save to set…": asks name (default: set picked); overwrite of existing name
  asks first. On success combo re-read, form no longer marked edited.

### 5.3 Close

- Peak Center window closed with run going: asks exactly `Cancel peak
  center?`. Yes → `cancel_peak_center(ScanAfter::Resume)` (scan belongs to
  spectrometer window, which stays open: goes back as it was); this close
  ignored, window closes when `peakCenterFinished` arrives. No → stays open,
  run goes on. Second close while waiting ignored, not asked again.
- Spectrometer window close asks nothing. Its `stop_scan` makes a run going
  end with scan left stopped (`stop_requested_` rule, 4.3).

### 5.4 Exclusion with queue

- Window: Start refused while queue running; `PeakCenterWindow` given
  `std::function<bool()> queue_running` (pattern of `DataSourcePage`,
  `main_window.cpp:749`). Start disabled with tooltip "a queue is running";
  state re-checked on Start.
- Experiment window: `start()` refused while window's peak center runs,
  message "peak center running in the spectrometer window".
  `ExperimentWindow` given `std::function<bool()> peak_center_running`
  (reads `peak_center_window_`); `MainWindow` wires both.
- `MainWindow`: action "Peak Center…" contributed to `MenuHub::Menu::Spectrometer`
  after "Field Table…" (`peak_center_action()`); enabled only with a
  spectrometer bridge, tooltip "no spectrometer configured" otherwise; opens
  one window (`peak_center_window()`, `WA_DeleteOnClose` false), raised when
  opened again. Gives it `peak_center_file_` and queue-running lambda;
  forwards `queueStarted`/`queueEnded` as `queue_state_changed()`.
- `MainWindow::set_spectrometer` and quit cancel a window run and wait
  (`cancel_peak_center_and_wait`), no prompt (run cannot outlive window or
  bridge); `set_spectrometer` then closes and deletes Peak Center window
  before spectrometer window (both hold old bridge). Queue start invalidates
  panel's last result: Apply stays disabled for it even after queue ends.
- While queue runs: Apply disabled (queue's runs own table), tooltip "a
  queue is running". Save allowed: file read only at next queue start (3.4).

## 6. Tests

- `tests/systems/test_jobs_peak_center.cpp`: writer round-trip, every key and
  defaults; `center` absent stays absent; `direction` both ways; Apply calls
  give same table as `update_table = true` run on same sim sweep.
- `tests/experiment/test_lab.cpp` (or session test): edited file picked up at
  `start`; bad file refuses start naming file; plan ref to set added in file
  after load accepted; map not swapped while running. `check_queue`: set
  added after load counts; bad file named with `start`'s text; running
  queue's map while running (`tests/integration/test_lab_session.cpp`).
- `tests/ui/test_experiment_window.cpp`: set added to file after load,
  Revalidate, queue runnable, Start enabled. `test_run_factory_panel.cpp`:
  probe checked by model's checker.
- `tests/core/test_local_file.cpp` `set_local_table`: comment kept, position
  kept, CRLF kept, header with trailing comment, subtable replaced with it,
  new table appended, comment-only file, missing file made. Bridge case on a
  `peak_center.toml` with comments and two sets: other set and comments
  unchanged, saved set keeps its place.
- `tests/ui/test_spectrometer_bridge.cpp` (`VirtualTimeTest`, sim
  spectrometer): run completes, events relayed in order, result posted;
  cancel → Cancelled; second start refused; scan paused and resumed at old
  integration; stop during run keeps scan stopped; destructor during run
  returns; `start_scan`/`position` refused during run; failed start or scan
  stopped behind bridge not put back; `ScanAfter::Stopped` and
  `cancel_peak_center_and_wait` leave scan stopped.
- `tests/ui/test_peak_center_panel.cpp`: set loads form; edit marks; revert;
  invalid disables Start/Save; Apply enable states (ok, failed, table changed,
  queue running); Save round-trip through file; queue running disables Start.
  `test_experiment_window.cpp`: refuses start while peak center runs.
- `tests/ui/test_peak_center_window.cpp`: panel and plot in splitter; run
  drawn as it goes; finished run kept; close during run asks, no keeps open,
  yes cancels and closes after finish (second close not asked), yes puts scan
  back; no run → no question; `cancel_peak_center_and_wait` asks nothing;
  splitter and geometry restored; corrupt saved state ignored.
- `tests/ui/test_peak_center_view.cpp`: x = first try's [min, max] at its
  start, unchanged by points; descending sweep same; wider try widens to
  union; new peak center forgets old width; y sane with no point, covers
  points padded, follows larger point; shown result spans its tries' union.
- `tests/ui/test_spectrometer_window.cpp`: no peak center docks or panel;
  layout saved with them still restores; Controls dock locked from bridge
  start (`peakCenterStarted`), refused second start keeps lock, unlocked
  after; close during run asks nothing, scan left stopped.
- `tests/ui/test_spectrometer_bridge.cpp`: `peakCenterStarted` once per
  accepted start, not for refused.
- `tests/ui/test_docks.cpp`: Peak Center… disabled without spectrometer;
  beside Field Table…; one window, raised again, gone with spectrometer;
  gets peak center file; set_spectrometer and quit cancel run without
  asking.
- New thread: suites above run under `-DPYCHRON_SANITIZE=thread` before land.

## 7. Docs

`docs/user/03-spectrometer.md`, "Peak centring": opened from Spectrometer →
Peak Center…, layout, plot axes, running, Apply, Save to set, when plans see
saved set, close questions.

## 8. Out of scope

- `elctl` peak-center command (`spec job peak-center`, control spec 9.3).
- Magnet, HV, CDD sweep views.
- Deleting or renaming set from window.
- Editing what plan step overrides (`PeakCenterStep` fields).
- Peak center through `JobRunner` (session still passes `nullptr`).
- Sweep points in run record.

## 9. Decisions (developer, 2026-10-21)

1. Configure = one-run overrides plus "Save to set" writing back (C).
2. Field table never written automatically from window; Apply button after
   old/new/shift shown (B).
3. Own dock for controls plus own plot dock beside strip chart (B).
   Superseded 2026-10-21: own Peak Center window, panel and plot side by
   side (5.1).
4. Saved set reaches plans at next queue start; bad file refuses start (A).

## Open questions

None.
