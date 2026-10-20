# Script rehearsal: step through a script on its own canvas, nothing actuated

Status: design agreed with owner in session 2026-10-20 (step unit, window,
inputs, approach, scope). Nothing built.

## 1. Problem

Operator wants to walk an extraction (or post-measurement) script one
hardware step at a time, see on a canvas what each step would do to line,
and be sure no valve moves. Today:

- Scripts run only inside a queue run (`libs/experiment/src/run/run.cpp`);
  script editor has Check Now (F7) and Estimate, no run, no step.
- No step, pause-after-each, or no-actuate mode in UI or `libs/scripting`.
- Canvas is main window's central widget (`apps/pychron-ui/src/main_window.cpp:190`),
  cannot be shown elsewhere; `CanvasView` reads only `CoreBridge`, and a
  click actuates.
- `--sim` replaces transports but script still drives (simulated) valves.

## 2. What is built

1. `libs/scripting`: third host mode, Rehearse. Every hardware call and every
   wait becomes one call to `IRehearsal::step`; nothing else reaches
   hardware or clock.
2. `libs/systems`: switch access and interlock rule pulled out of
   `SwitchManager` into a pure function, used by manager and by rehearsal.
3. `libs/experiment`: `Rehearsal` model, implements `IRehearsal` over a copy
   of line state; blocks script thread between steps.
4. `apps/pychron-ui`: Rehearse Script window, own top-level window: run-field
   form, read-only canvas painted from rehearsal state, script source with
   current line, step log, Step / Run to End / Stop / Restart.
5. `CanvasView` reads valve, lock and pressure state through `ICanvasState`;
   `CoreBridge` adapter keeps main canvas as is.

Step unit: one hardware call (or wait). Plain Python (`if`, arithmetic,
`info`, string work) runs between steps without stopping.

Script kinds in scope: `Extraction`, `PostEquilibration`, `PostMeasurement`,
`SafeState`. `MeasurementHook` out (section 9).

## 3. Scripting: Rehearse mode

### 3.1 Interface

`libs/scripting/include/pychron/scripting/rehearsal.hpp` (new):

```cpp
struct StepCall {
  std::string command;            // vocabulary name: "open", "sleep", "extract", ...
  std::vector<Value> args;        // positional, as the script passed them
  ValueMap kwargs;                // keyword arguments
  std::string script;             // script (or gosub) the call is in
  int line = 0;                   // its line there
  std::vector<std::string> gosubs;// outermost first; empty in the top script
};

struct IRehearsal {
  virtual ~IRehearsal() = default;
  // Called on the script's thread with the GIL released. Blocks until the
  // step may go on. Returns the call's value (monostate for a void call),
  // an error the script sees as the real call's error, or Cancelled.
  virtual Result<Value> step(const StepCall& call) = 0;
};
```

`IScriptHost` gains:

```cpp
virtual Result<ScriptResult> rehearse(const Script& script, const ScriptEnvironment& env,
                                      IRehearsal& rehearsal, CancelToken& token) = 0;
```

### 3.2 Rules

- `rehearse()` runs `check()` first, as `run()` does; a failed check is
  returned, nothing stepped.
- `rehearse()` refuses (Config error, nothing executed) an env in which any
  of `line.device`, `line.valves`, `line.pressure`, `line.cryo`,
  `intensity`, `measurement`, `resources`, `on_pump_time_start` is set. The
  check is in `rehearse()` itself, so no caller can hand it live services.
- Capabilities for `check()` and `require()` come from
  `env.capabilities` / `env.valve_names` (as `editor_environment` sets them),
  since `line` is empty.
- `HostState::Mode::Rehearse`. At the one branch Estimate already takes
  (`PYCHRON_HW` macro, `host_state.cpp:307`, and the `if (estimating())`
  early returns of the other hardware commands), Rehearse builds a
  `StepCall` and calls `step()` without the GIL; its `Result<Value>` is
  unwrapped as a service result would be (error → `HardwareError` of that
  kind, Cancelled → `ScriptCancelled`/abort as today).
- Commands that step: every command in `vocabulary()` that Estimate skips
  or books time for: valves (`open close lock unlock is_open is_closed`),
  pressure (`get_pressure get_manometer_pressure`), device (`extract
  end_extract enable disable prepare`, laser, warmup, stage, furnace,
  pipette, motors, cryo, imaging), waits (`sleep pause` and any wait on a
  condition), post-measurement (`get_intensity signal_pump_time_start`).
  Test `Rehearse.EveryHardwareCommandSteps` walks `vocabulary()` and fails
  on a hardware command that does not reach `step()`.
- No `clock.wait*` in Rehearse mode. `sleep(20)` is one step, returns at
  once after operator steps.
- `info()`/`print()` go to `env.log` and `ScriptResult::messages`, no step.
- `now()` in Rehearse returns sum of stepped wait durations since start
  (as Estimate's accumulator does), so time-based script logic sees time
  pass.
- Line trace (`on_line`) keeps `current_script_`/`current_line_`; StepCall
  takes them from there. Limits (`max_lines`, `max_wall_time`) still apply;
  `max_wall_time` counts only time Python runs, not time blocked in
  `step()` (clock started again after each step).
- Return value of a read used as script expects: `is_open`/`is_closed`
  bool, `get_pressure`/`get_manometer_pressure`/`get_intensity` double. A
  value of wrong type from `step()` is a Config error naming the command.
- Stub host (`PYCHRON_SCRIPTING=OFF`): `rehearse()` returns NotSupported.

## 4. Systems: switch rule as pure function

`libs/systems/include/pychron/systems/switch_rules.hpp` (new):

```cpp
struct SwitchView {           // what the rule needs to know of one switch
  std::string name;
  ValveState state = ValveState::Unknown;
  bool locked = false;
  std::string owner;          // empty: nobody's
};
struct InterlockSet {
  std::vector<std::string> exclusive;  // negative interlocks, both directions, each once
  std::vector<std::string> positive;
};

// Lock, owner checks, then interlock checks of SwitchManager::command, in its
// order. ErrorKind::Interlock with the manager's text on refusal. Closing is
// never interlocked: caller applies interlocks to opens only.
[[nodiscard]] Result<void> check_switch_access(const SwitchView& self, std::string_view actor);
[[nodiscard]] Result<void> check_switch_interlocks(const SwitchView& self,
                                                   const std::vector<const SwitchView*>& exclusive,
                                                   const std::vector<const SwitchView*>& positive);
```

`SwitchManager::interlock_sets()` (new):
`std::map<std::string, InterlockSet, std::less<>>`, immutable topology, so
rehearsal resolves partners by name from a copy of line state.

- `SwitchManager::check_access` and `check_interlocks` become calls to the two functions;
  refusal text byte for byte as today (existing tests in `tests/systems`
  pin it; none edited).
- Negative interlocks enforced both ways, as manager's header says; Unknown
  partner blocks.

## 5. Experiment: Rehearsal model

`libs/experiment/include/pychron/experiment/rehearsal/rehearsal.hpp` (new),
namespace `pychron::experiment::rehearsal`. Pure C++, no Qt.

### 5.1 State

```cpp
struct LineSnapshot {
  std::map<std::string, SwitchView, std::less<>> switches;
  std::map<std::string, InterlockSet, std::less<>> interlocks;  // SwitchManager::interlock_sets()
  std::map<std::string, double, std::less<>> pressures;  // latest live readings, by gauge name
};
```

Taken once at start from live line (UI: `CoreBridge::state()` plus
`SwitchManager::interlock_sets()`). Partner named in `interlocks` but
missing from `switches`: counts Unknown. Rehearsal never reads live line again.

### 5.2 Steps

`Rehearsal : IRehearsal`. For each `StepCall`:

- `open`/`close`: `check_switch_access`, then for an open `check_switch_interlocks`, against rehearsal states, actor
  `"script"` (as `session.cpp`'s `script_valves`). Allowed: rehearsal state
  becomes Open/Closed, step outcome "would open". Refused: step outcome
  "would be refused: <text>", `step()` returns that Interlock error; script
  then fails as a real run would and rehearsal ends there.
- `lock`/`unlock`: rehearsal lock flag. Close also checked by
  `check_switch_access`, as manager does; refused close ends run as open does.
- `is_open`/`is_closed`: from rehearsal state.
- `get_pressure` (gauge = args[1]) / `get_manometer_pressure` (args[0]):
  live reading from snapshot when it has one for that gauge, else step waits
  for value typed by operator (`provide_value(double)`). Unknown gauge: same
  wait, not error (typed value covers it).
- `get_intensity`, DeviceHandle `output`/`is_enabled`, `get_value`,
  `get_cryo_temp`, `get_resource_value`: always wait for typed value (no
  live source in snapshot). Bool read (`is_enabled`) answers `v != 0`.
  Outcome "= <value>".
- Unknown valve name: Config error `unknown switch '<name>'`, as
  `SwitchManager` gives.
- Waits: recorded with duration; nothing else.
- Every other command: recorded, returns monostate.

### 5.3 Control

```cpp
enum class Advance { Step, RunToEnd };
void advance(Advance how);      // UI thread; RunToEnd: no further stops
void provide_value(double v);   // answers a pending read
void stop();                    // sets token Abort, wakes a blocked step
// Observer, called on script thread, before blocking and after outcome:
std::function<void(const StepRecord&)> on_step;
```

`StepRecord`: index, `StepCall`, outcome text, state of each changed
switch, whether waiting for operator and for what (step / value).

Run to End still passes each call through same rules (refusal stops it);
it does not block between them. Limit `Rehearsal::kRunToEndLimit = 1000`
steps: then mode falls back to Step, step 1001 blocks (`Waiting::Step`),
its records' outcome begins "run to end paused after 1000 steps", so a
script in loop does not run on for good. Read with no value still waits for
one under Run to End. `advance(Step)` banks one go-ahead for next stop
(ignored while read waits for value) and ends a Run to End. `step()` calls
`on_step` twice: once waiting (skipped under Run to End while under limit),
once with outcome; step woken by `stop()` reports outcome "stopped" and
returns Cancelled, as does every step after.

### 5.4 Threads and time

- `RehearsalRunner` owns one `std::thread` running `host.rehearse()`; one
  rehearsal at a time per runner. Destructor: `stop()`, then join.
- Thread is not on a simulated path: own `SteadyClock` in env, never a
  `Clock::Participant`, even when app runs on `VirtualClock`. Blocking in
  `step()` is a wait on operator: plain `std::mutex` +
  `std::condition_variable`, never across a device or clock call (there are
  none). Added to virtual-clock spec section 4.4 list in same branch.
- GIL released while blocked (section 3.2), so another script thread (a
  queue running at same time on a simulated line) is not stalled.
- Rehearsal may run while a queue runs: it touches nothing live.

## 6. UI

### 6.1 `ICanvasState`

`apps/pychron-ui/src/canvas_state.hpp` (new): QObject interface, what
`CanvasView::apply_state` reads now: valve states, switch locks, pending
flag, pressures and units, valve details text, `changed()` signal.
`CoreBridgeCanvasState` adapts `CoreBridge` (its signals → `changed()`).
`CanvasView` takes `ICanvasState&` plus optional `CoreBridge*`: with null
bridge view is read-only (click, lock, unlock do nothing; no
`actuate`/`request_lock` path exists). Main window passes both; behaviour
unchanged (existing `tests/ui` canvas tests pass unedited).

### 6.2 Rehearse Script window

`apps/pychron-ui/src/rehearsal_window.{hpp,cpp}` (new). Top-level
`QMainWindow`, registered with `MenuHub::add_window`, QSettings group
`rehearsal_window`, key `geometry`.

Opened from:
- Script Editor: action "Rehearse…" (Scripts menu while editor active),
  acts on current tab's text (unsaved text included), script kind from tab.
- Scripts menu: "Rehearse Script…" opens file picker over lab's scripts
  (`lab_scripts`), kinds in scope only.

Opening a second rehearsal brings window forward and asks before replacing
running one.

Layout:
- Top: run fields form: every name of `default_context()`;
  `analysis_type`, `run_identifier`, `extract_value`, `extract_units`,
  `duration` first, rest under "More".
  Defaults: first row of experiment window's open queue naming this script
  in its matching script column, else `make_context()` defaults. Edits kept
  until window closes.
- Left: `CanvasView` over rehearsal state (`RehearsalCanvasState`), read
  only. Valve changed by last step outlined. Title bar text "Rehearsal: no
  valve is actuated".
- Right: read-only `CodeEditor` with script source; current line
  highlighted; gosub opens its source with call stack shown above.
- Bottom: step log, one row per step: `#n  script:line  command(args)  →
  outcome`. Pending read: inline value field and Enter.
- Toolbar: Start (form → run), Step (F10), Run to End, Stop, Restart
  (new snapshot of live line, same form). Shortcuts registered in
  `shortcuts.cpp`.

`on_step` arrives on script thread; marshalled to UI with
`QMetaObject::invokeMethod(..., Qt::QueuedConnection)`.

### 6.3 End

Script ends: log row "finished", canvas keeps final rehearsal state.
Error (refusal, script bug, check failure): row in red with error text;
check failure lists diagnostics as Problems do in editor.

## 7. Rules that must hold

- No rehearsal path can actuate. Proven by construction (section 3.2
  refusal of non-null services) and by test: UI test runs a full
  rehearsal of `sim_extract.py` and `sim_pump.py` against live sim line and
  asserts `SwitchManager` stats (opens, closes, refusals) and every valve
  state unchanged, and no `ValveChanged` on bus.
- Interlock verdicts in rehearsal equal manager's for same state (one
  function, section 4).
- Rehearse mode never waits on a clock.

## 8. Testing

- `tests/scripting` (`test_rehearse.cpp`, new): `sim_extract.py` with
  `analysis_type='unknown'` steps `close C, open P2, sleep 2, close P2,
  open P1, sleep 2, close P1, sleep <duration>, sleep 2` with right line
  numbers; with `blank_unknown` steps `close C, sleep, sleep`;
  `sim_pump.py` steps `signal_pump_time_start, open C, open B, sleep 50,
  close B`; refusal of each non-null service field; error from `step()`
  raised as `HardwareError` of that kind; Stop while blocked aborts;
  gosub carries stack; `EveryHardwareCommandSteps`; stub returns
  NotSupported.
- `tests/systems`: `SwitchRules` cases (lock, owner, negative both
  ways, positive, Unknown partner, close never refused); existing
  `SwitchManager` tests unchanged and passing.
- `tests/experiment` (`test_rehearsal.cpp`, new): open refused when
  interlocked partner open in rehearsal state; `is_open` follows earlier
  step; pressure from snapshot, else waits for `provide_value`; Run to End
  stops at refusal; runner destructor while blocked joins.
- `tests/ui` (`test_rehearsal_window.cpp`, new): window steps
  `sim_extract.py`; after step 2 canvas `P2` Open in rehearsal canvas and
  Closed in main canvas; rule 7 check; read-only canvas click does nothing.
- Thread code: run new tests under `-DPYCHRON_SANITIZE=thread` before
  landing.

## 9. Out of scope

- Stepping by source line (debugger-style).
- Rehearsing whole queue or a queue row's full script chain.
- Measurement hooks (`MeasurementHook`, `call_hook`, `IMeasurementApi`).
- Simulated gas, pressure or intensity response to rehearsal steps.
- Changing a rehearsal valve by hand mid-rehearsal.
- Pop-out of live canvas to its own window.
- `elctl` front end for rehearsal.

## Open questions

None.
