# Script Rehearsal Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let an operator step through an extraction, post-equilibration, post-measurement or safe-state script one hardware call at a time, in its own window with a read-only canvas, with no valve or device ever actuated.

**Architecture:** A third mode of the Python script host (Rehearse) routes every hardware call and every wait through one interface, `IRehearsal::step`, and refuses any environment that carries a live service. A pure-C++ `Rehearsal` model in `libs/experiment` answers those steps from a copy of the line's state, judging opens with the same lock and interlock rule as `SwitchManager` (extracted to a pure function), and blocks the script thread until the operator advances. The UI shows it in a new top-level window whose `CanvasView` reads from an `ICanvasState` rather than from `CoreBridge` directly.

**Tech Stack:** C++20, embedded CPython via pybind11 (`libs/scripting`), Qt 6 Widgets (`apps/pychron-ui`), GoogleTest and Qt Test.

**Spec:** `docs/superpowers/specs/2026-10-20-script-rehearsal-design.md`

Work in the worktree `.claude/worktrees/script-rehearsal`, branch `feat/script-rehearsal`. Builds: `cmake --preset dev` once in the worktree, then `dev-ui` for Tasks 5 to 7.

## Global Constraints

- C++20; errors are `pychron::Result<T>`; no `new` outside Qt parent ownership; `#pragma once`.
- `rehearse()` refuses (Config error, nothing executed) an env in which any of `line.device`, `line.valves`, `line.pressure`, `line.cryo`, `intensity`, `measurement`, `resources`, `on_pump_time_start` is set.
- Rehearse mode never waits on a clock. `sleep(20)` is one step.
- Rehearsal actor is `"script"`, as `session.cpp`'s `script_valves`.
- Rehearsal thread: own `SteadyClock`, never a `Clock::Participant`; blocks on `std::mutex` + `std::condition_variable`; the GIL is released while it blocks.
- Refusal text from the extracted switch rule is byte for byte today's `SwitchManager` text; no existing `tests/systems` test is edited.
- Main canvas behaviour unchanged; existing `tests/ui/test_canvas_view.cpp` passes unedited.
- Window: QSettings group `rehearsal_window`, key `geometry`; registered with `MenuHub::add_window`.
- Script kinds in scope: `Extraction`, `PostEquilibration`, `PostMeasurement`, `SafeState`. `MeasurementHook` refused.
- New thread code runs under `-DPYCHRON_SANITIZE=thread` before landing.
- After each C++ task: `python3 tools/quality_check.py` exits 0 and `python3 tools/compiler_sweep.py --changed` (with `--build-dir build/dev-ui` for UI tasks) is clean.

## Review Focus

1. A script that loops on a read (`while get_pressure(...) > x: sleep(1)`) under Run to End: the reading never changes in a rehearsal, so it would step forever. Expected: Run to End drops back to Step after 1000 steps in one go, with a log row saying so. Test in Task 4 (`RunToEndFallsBackToStepAfterLimit`). The spec is extended to say this (Task 4, step 1).
2. Window closed, or Restart pressed, while the script thread is blocked waiting for a step or for a typed value. Expected: no hang, the thread is joined, and a new run starts clean. Tests in Task 4 (`DestroyWhileWaitingForValueJoins`) and Task 6 (`RestartWhileBlockedStartsOver`).
3. A live line whose valves are still `Unknown` (the bridge has had no snapshot, or a valve has never been read). Expected: an open whose interlock partner is Unknown is refused, with the manager's text, exactly as live. Test in Task 4 (`UnknownPartnerRefusesOpen`).
4. A script that names a valve the line does not have, or has a syntax error. Expected: the check fails before any step, the diagnostics are shown, and no thread is left blocked. Tests in Task 1 (`CheckFailureStepsNothing`) and Task 6 (`CheckFailureShowsDiagnostics`).
5. Unsaved edits in the editor tab. Expected: the rehearsal runs the text as shown, not the file on disk. Test in Task 6 (`RehearsesUnsavedText`).

---

### Task 1: Rehearse mode in the script host: interface, refusal, valves and waits

**Files:**
- Create: `libs/scripting/include/pychron/scripting/rehearsal.hpp`
- Modify: `libs/scripting/include/pychron/scripting/script_host.hpp` (add `rehearse`)
- Modify: `libs/scripting/src/python/internal.hpp` (`Mode::Rehearse`, `IRehearsal*` member, `rehearsing()`, `step()` helper)
- Modify: `libs/scripting/src/python/host_state.cpp` (valves, `sleep`, `pause`, `now`, trace bookkeeping)
- Modify: `libs/scripting/src/python/python_host.cpp` (`rehearse`)
- Modify: `libs/scripting/src/stub/stub_host.cpp` (`rehearse` → NotSupported)
- Modify: `tests/experiment/run_fakes.hpp:254` (`FakeScriptHost::rehearse` → NotSupported)
- Test: `tests/scripting/test_rehearse.cpp`, plus `tests/scripting/rehearse_fakes.hpp` (`RecordingRehearsal`)

**Interfaces:**
- Produces: `scripting::StepCall`, `scripting::IRehearsal` exactly as spec 3.1; `IScriptHost::rehearse(const Script&, const ScriptEnvironment&, IRehearsal&, CancelToken&) -> Result<ScriptResult>`; `scripting::rehearsal_refusal(const ScriptEnvironment&) -> std::optional<std::string>` (the name of the first non-null service, used for the error text `"rehearse: environment carries live service '<field>'"`).
- `RecordingRehearsal` (test helper): `std::vector<StepCall> calls`; `std::map<std::string, Value> answers` keyed by command; `std::function<Result<Value>(const StepCall&)> respond` override; formats each call as `"<command> <args joined by space>"` in `std::vector<std::string> lines()`.

- [ ] **Step 1: Write the failing tests** in `tests/scripting/test_rehearse.cpp`, suite `Rehearse`. Use `editor_environment`-style env: `capabilities` all set, `valve_names = {"A","B","C","P1","P2"}`, `context = make_context({{"analysis_type","unknown"},{"duration",5.0}})`, scripts loaded from `PYCHRON_EXAMPLE_CONFIGS_DIR "/scripts/extraction/sim_extract.py"`. Skip each test with `GTEST_SKIP()` when `!scripting_enabled()`.
  - `SimExtractStepsInOrder`: `rehearsal.lines() == {"close C","open P2","sleep 2","close P2","open P1","sleep 2","close P1","sleep 5","sleep 2"}`; `calls[0].line == 12`, `calls[1].line == 14`; every `calls[i].script == "extraction/sim_extract.py"`.
  - `BlankSkipsThePipette`: `analysis_type = "blank_unknown"` gives `{"close C","sleep 5","sleep 2"}`.
  - `InfoDoesNotStep`: result `messages.size() == 1`, no `info` call recorded.
  - `RefusesEveryLiveService`: for each of the eight fields in the Global Constraints, set it to a dummy (non-null pointer or non-empty function), `rehearse` fails with `ErrorKind::Config` whose `what` contains the field name, and `calls.empty()`.
  - `CheckFailureStepsNothing`: script `def main():\n    open('Z')\n` fails (Config), `calls.empty()`.
  - `StepErrorRaisesAsTheRealCallWould`: `respond` returns `fail(ErrorKind::Interlock, "interlocked", "P2")` for `open P2`; result error kind is Interlock, `what == "interlocked"`; no call after `open P2`.
  - `NowAdvancesBySteppedWaits`: script `t0 = now(); sleep(3); info(str(now() - t0))` → message `"3.0"`.
  - `MeasurementHookRefused`: a `ScriptKind::MeasurementHook` script fails with Config, nothing stepped.
  - `StubHostRefuses` (built only when `!scripting_enabled()`): `rehearse` error `extraction::is_not_supported`.

- [ ] **Step 2: Run them, expect a compile failure** (`rehearse` not declared):
  `cmake --build --preset dev --target pychron_scripting_tests`

- [ ] **Step 3: Add the header and the pure-virtual `rehearse`;** implement it in the stub host and `FakeScriptHost` as NotSupported (`extraction::not_supported("rehearse")`).

- [ ] **Step 4: Implement Rehearse in the Python host.** `PythonScriptHost::rehearse`: refuse `MeasurementHook`; refuse a live service (`rehearsal_refusal`); then go as `execute()` does with `HostState(env, token, Mode::Rehearse, kind, nullptr)` plus the `IRehearsal*`. In `HostState`, add a private `Value step(std::string command, std::vector<Value> args, ValueMap kwargs = {})`: builds the `StepCall` from `current_script_`, `current_line_` and `script_stack_` (outermost first, the top script dropped), calls `rehearsal_->step` inside `nogil`, and `unwrap`s the result (an error becomes a `HardwareError`, Cancelled goes through `check_requested`). Branch `if (rehearsing())` ahead of `estimating()` in `open close lock unlock is_open is_closed sleep pause`. `is_open`/`is_closed` need `bool` from the `Value`; anything else is Config `"rehearse: <command> answered with a <type>"`. Keep a `Duration rehearsed_` total of stepped waits, and have `now()` return the start time plus `rehearsed_` when rehearsing. Restart `started_` (the `max_wall_time` origin) after every `step()` returns.

- [ ] **Step 5: Run the suite and see it pass:**
  `build/dev/tests/scripting/pychron_scripting_tests --gtest_filter='Rehearse.*'`. Then run the full binary, to check `ScriptHost.*` is unchanged.

- [ ] **Step 6: Commit:** `feat(scripting): rehearse mode for valves and waits`

### Task 2: Every hardware command steps

**Files:**
- Modify: `libs/scripting/src/python/host_state.cpp` (every remaining `if (estimating())` hardware branch: device, laser, stage, furnace, pipette, motors, cryo, imaging, pressure, `get_intensity`, `signal_pump_time_start`, `wait_resource`, condition waits)
- Test: `tests/scripting/test_rehearse.cpp`

**Interfaces:**
- Consumes: `HostState::step` from Task 1.
- Produces: the read commands' answer types: `get_pressure`, `get_manometer_pressure`, `get_intensity` and `get_value` take a `double` (an `int64_t` is accepted and converted); a `DeviceHandle` from `get_device` is the handle Estimate returns.

- [ ] **Step 1: Write the failing tests:**
  - `EveryHardwareCommandSteps`: for each `cmd` in `vocabulary()` whose capability is not None, or whose name is in `{"sleep","pause","get_intensity","signal_pump_time_start","wait_resource"}`, generate a one-line script calling it with arguments built from the command's arity/arg spec (see `vocabulary.hpp`'s signature data) for a kind that allows it. Expect `calls.size() == 1 && calls[0].command == cmd.name`. On failure, list every command that did not step.
  - `SimPumpSteps`: `post_measurement/sim_pump.py` (kind PostMeasurement) gives `{"signal_pump_time_start","open C","open B","sleep 50","close B"}`.
  - `PressureReadAnswersTheScript`: `answers["get_pressure"] = 2.5e-9`; script `info(str(get_pressure('ig','ion')))` → message `"2.5e-09"`; `calls[0].args == {"ig","ion"}`.
  - `WrongAnswerTypeIsConfig`: `answers["is_open"] = 1.0` → Config error naming `is_open`.
  - `GosubCarriesStack`: a resolver with `lib/sub.py` (`def main():\n    open('A')`) and a script calling `gosub('lib/sub.py')` → `calls[0].script == "lib/sub.py"` and `calls[0].gosubs == {"extraction/top.py"}`.

- [ ] **Step 2: Run them, expect FAIL** (no step recorded for device commands; `NotSupported` raised because `device()` is null).

- [ ] **Step 3: Add the `rehearsing()` branch** to every remaining hardware command, before `guard()` and before any service lookup, passing positional args in script order and keyword args (`block`, `autocenter`, ...) in `kwargs`. Waits on a condition (`wait_while`, `wait_resource`) step once with their arguments and do not poll.

- [ ] **Step 4: Run the `Rehearse.*` tests and the full scripting binary: all pass.**

- [ ] **Step 5: Commit:** `feat(scripting): every hardware command steps in a rehearsal`

### Task 3: Switch access and interlock rule as pure functions

**Files:**
- Create: `libs/systems/include/pychron/systems/switch_rules.hpp`, `libs/systems/src/switch_rules.cpp`
- Modify: `libs/systems/src/switch_manager.cpp:262-292` (`check_access` and `check_interlocks` call the new functions)
- Modify: `libs/systems/include/pychron/systems/switch_manager.hpp` (add `interlock_sets()`)
- Modify: spec section 4 (the signature below replaces the spec's `SwitchLookup` sketch)
- Test: `tests/systems/test_switch_rules.cpp`

**Interfaces:**
- Produces:
```cpp
namespace pychron::systems {
struct SwitchView {
  std::string name;
  ValveState state = ValveState::Unknown;
  bool locked = false;
  std::string owner;
};
struct InterlockSet {
  std::vector<std::string> exclusive;  // negative interlocks, both directions, each once
  std::vector<std::string> positive;
};
[[nodiscard]] Result<void> check_switch_access(const SwitchView& self, std::string_view actor);
[[nodiscard]] Result<void> check_switch_interlocks(const SwitchView& self,
                                                   const std::vector<const SwitchView*>& exclusive,
                                                   const std::vector<const SwitchView*>& positive);
}
// SwitchManager:
std::map<std::string, InterlockSet, std::less<>> interlock_sets() const;  // immutable topology
```

- [ ] **Step 1: Write the failing tests,** suite `SwitchRules`: locked → Interlock `"'A' is software locked"`; owner `"ui"` with actor `"script"` → `"'A' is owned by 'ui', not 'script'"`; exclusive partner Open → `"cannot open 'A': interlocked with 'B' which is open"`; exclusive partner Unknown → refused (`UnknownPartnerRefuses`); positive partner Closed → `"cannot open 'A': requires 'B' open, it is closed"`; all clear → ok. `InterlockSetsAreBothWays`: a manager built from specs where A lists B → `interlock_sets()["B"].exclusive == {"A"}`. Take the exact strings from the manager's current code at `switch_manager.cpp:263-286`.

- [ ] **Step 2: Run, expect a compile failure.** `cmake --build --preset dev --target pychron_systems_tests`

- [ ] **Step 3: Implement both functions with the manager's text;** make `SwitchManager::check_access` and `check_interlocks` build `SwitchView`s from `Entry` and call them. Add `interlock_sets()`, built from `Entry::exclusive` and `positive`.

- [ ] **Step 4: Run** `build/dev/tests/systems/pychron_systems_tests` **in full: every existing `SwitchManager*` test passes unedited, and `SwitchRules.*` passes.**

- [ ] **Step 5: Edit spec section 4** to the signatures above (two functions plus `interlock_sets()`, in place of `check_switch_command`/`SwitchLookup`).

- [ ] **Step 6: Commit:** `refactor(systems): switch access and interlock rule as pure functions`

### Task 4: Rehearsal model and runner

**Files:**
- Create: `libs/experiment/include/pychron/experiment/rehearsal/rehearsal.hpp`, `libs/experiment/src/rehearsal/rehearsal.cpp`
- Create: `libs/experiment/include/pychron/experiment/rehearsal/runner.hpp`, `libs/experiment/src/rehearsal/runner.cpp`
- Modify: `libs/experiment/CMakeLists.txt` (if its sources are listed rather than globbed)
- Modify: spec section 5.3 (the Run to End limit) and `docs/superpowers/specs/2026-10-06-virtual-clock-design.md` section 4.4 (one entry: the rehearsal runner's thread waits on the operator, on real time, and is not a participant)
- Test: `tests/experiment/test_rehearsal.cpp`

**Interfaces:**
- Consumes: `scripting::IRehearsal`, `StepCall`, `IScriptHost::rehearse` (Task 1); `systems::SwitchView`, `InterlockSet`, `check_switch_access`, `check_switch_interlocks` (Task 3).
- Produces (namespace `pychron::experiment::rehearsal`):
```cpp
struct LineSnapshot {
  std::map<std::string, systems::SwitchView, std::less<>> switches;
  std::map<std::string, systems::InterlockSet, std::less<>> interlocks;
  std::map<std::string, double, std::less<>> pressures;  // keyed by gauge name
};
enum class Advance { Step, RunToEnd };
enum class Waiting { None, Step, Value };
struct StepRecord {
  std::size_t index = 0;               // from 1
  scripting::StepCall call;
  std::string outcome;                 // "would open", "would be refused: <text>", "sleep 2 s", "= 2.5e-09", "recorded"
  std::vector<std::string> changed;    // switches whose rehearsal state this step changed
  Waiting waiting = Waiting::None;
  bool refused = false;
};
class Rehearsal final : public scripting::IRehearsal {
 public:
  static constexpr std::size_t kRunToEndLimit = 1000;
  explicit Rehearsal(LineSnapshot snapshot, scripting::CancelToken& token);
  Result<scripting::Value> step(const scripting::StepCall& call) override;
  void advance(Advance how);
  void provide_value(double v);
  void stop();
  LineSnapshot state() const;          // copy, under the mutex
  std::function<void(const StepRecord&)> on_step;  // set before the run starts
};
class RehearsalRunner {
 public:
  RehearsalRunner(scripting::IScriptHost& host, scripting::Script script, scripting::ScriptEnvironment env,
                  LineSnapshot snapshot, std::function<void(const StepRecord&)> on_step,
                  std::function<void(const Result<scripting::ScriptResult>&)> on_finished);
  ~RehearsalRunner();                  // stop(), then join
  RehearsalRunner(const RehearsalRunner&) = delete;
  RehearsalRunner& operator=(const RehearsalRunner&) = delete;
  RehearsalRunner(RehearsalRunner&&) = delete;
  RehearsalRunner& operator=(RehearsalRunner&&) = delete;
  Rehearsal& rehearsal();
};
}
```
`step()` calls `on_step` twice: once with `waiting` set, before it blocks (skipped under Run to End while the limit is not reached), and once with the outcome. A refused open ends the run: `step` returns the Interlock error and the script fails.

- [ ] **Step 1: Write the failing tests,** suite `Rehearsal`. Drive a `Rehearsal` directly from a test thread (no Python), plus one runner test through `make_script_host()` that is skipped without scripting. Use `std::thread` and an `await`-with-limit helper in real time (5 s upper bound): this code is not on a simulated path.
  - `OpenChangesRehearsalStateOnly`: `open P2` after `advance(Step)` → `state().switches["P2"].state == Open`; record outcome `"would open"`, `changed == {"P2"}`.
  - `InterlockedOpenRefused`: P1 and P2 exclusive, P1 Open → `open P2` returns Interlock `"cannot open 'P2': interlocked with 'P1' which is open"`; `refused == true`.
  - `UnknownPartnerRefusesOpen`: partner Unknown → refused.
  - `LockedValveRefusedForScriptActor`: `locked = true` → `"'P2' is software locked"`.
  - `IsOpenFollowsEarlierStep`.
  - `PressureFromSnapshot`: `pressures["ion"] = 2e-9` → `get_pressure("ig","ion")` returns 2e-9 without `Waiting::Value`.
  - `PressureWithoutReadingWaitsForValue`: emits `Waiting::Value`; `provide_value(1.5)` → returns 1.5.
  - `UnknownValveIsConfig`.
  - `StopWakesABlockedStep`: `stop()` while blocked → `step` returns Cancelled and the token mode is Abort.
  - `RunToEndDoesNotBlock`: 5 sleeps after `advance(RunToEnd)` finish with no further `advance`.
  - `RunToEndFallsBackToStepAfterLimit`: 1001 steps under Run to End → step 1001 blocks with `Waiting::Step`, and its record's outcome begins `"run to end paused after 1000 steps"`.
  - `DestroyWhileWaitingForValueJoins`: a runner on `sim_pump`-like text whose `get_intensity` waits; destroy the runner → returns within 5 s, `on_finished` called once with Cancelled.
  - `RunnerRehearsesSimExtract`: the runner with `advance(RunToEnd)` → `on_finished` ok, and the final state has C, P1, P2 Closed.

- [ ] **Step 2: Run, expect a compile failure:** `cmake --build --preset dev --target pychron_experiment_tests`

- [ ] **Step 3: Implement `Rehearsal`.** One `std::mutex` guards state, the advance mode, the steps left in the Run to End run, and the pending value. `std::condition_variable cv_` is notified by `advance`, `provide_value` and `stop`. `stop()` calls `token.request(CancelMode::Abort)` (use the token's real method name) and notifies. `on_step` is called with the mutex released.

- [ ] **Step 4: Implement `RehearsalRunner`:** owns the `CancelToken`, the `Rehearsal`, and one `std::thread` running `host.rehearse(script, env, rehearsal, token)`; sets `env.clock` to a `SteadyClock` member; calls `on_finished` from that thread. The destructor calls `rehearsal().stop()` then `join()`.

- [ ] **Step 5: Run `Rehearsal.*`, then the experiment binary in full: all pass.**

- [ ] **Step 6: Run under TSan:** configure a `build/tsan` tree with `-DPYCHRON_SANITIZE=thread`, build `pychron_experiment_tests` and `pychron_scripting_tests`, and run `--gtest_filter='Rehearsal.*:Rehearse.*'`. Expected: pass, no ThreadSanitizer report.

- [ ] **Step 7: Make the two spec edits listed under Files.**

- [ ] **Step 8: Commit:** `feat(experiment): rehearsal model and runner`

### Task 5: CanvasView reads through ICanvasState; read-only canvas

**Files:**
- Create: `apps/pychron-ui/src/canvas_state.hpp`, `apps/pychron-ui/src/canvas_state.cpp`
- Modify: `apps/pychron-ui/src/canvas_view.hpp`, `apps/pychron-ui/src/canvas_view.cpp` (constructor, `apply_state`, `on_click`, `request_lock`)
- Modify: `apps/pychron-ui/src/main_window.cpp:174` (constructs with the adapter)
- Modify: `apps/pychron-ui/CMakeLists.txt` if sources are listed
- Test: `tests/ui/test_canvas_state.cpp`

**Interfaces:**
- Produces:
```cpp
namespace pychron::ui {
class ICanvasState : public QObject {
  Q_OBJECT
 public:
  using QObject::QObject;
  virtual const CoreBridge::State& state() const = 0;
  virtual bool pending(const std::string& name) const = 0;
  virtual QDateTime wall_now() const = 0;
  virtual const canvas::Canvas* canvas() const = 0;
 signals:
  void changed();
};
class CoreBridgeCanvasState final : public ICanvasState { /* explicit CoreBridgeCanvasState(CoreBridge&, QObject* parent) */ };
// CanvasView: new constructor; the old CanvasView(CoreBridge&, QWidget*) builds its own adapter and stays.
CanvasView(ICanvasState& state, CoreBridge* bridge, QWidget* parent = nullptr);
bool read_only() const noexcept;  // bridge == nullptr
}
```
`CoreBridge::State` is reused as the state shape on purpose: `valve_details` already takes it. Pressure-sample, alarm and actuation signals stay connected only when there is a bridge.

- [ ] **Step 1: Write the failing tests,** in the `test_canvas_view.cpp` style with the example line from `ui_fixture.hpp`, using a `FakeCanvasState` that holds a `CoreBridge::State`:
  - `ReadOnlyClickDoesNothing`: a click on `P2` → the fake's state is unchanged and `request_lock("P2", true)` returns false.
  - `PaintsFromState`: set `P2` Open in the fake, emit `changed()` → `view.valve("P2")->state() == Open`.
  - `BridgeAdapterForwardsChanges`: a `CoreBridgeCanvasState` over a bridge emits `changed()` on `valveChanged`.

- [ ] **Step 2: Run, expect a compile failure:** `cmake --build --preset dev-ui --target pychron_ui_test_canvas_state`

- [ ] **Step 3: Implement it.** `apply_state` reads `state_.state()`, `state_.pending()` and `state_.wall_now()`. `on_click` and `request_lock` return at once when `read_only()`.

- [ ] **Step 4: Run** `pychron_ui_test_canvas_state` **and the unedited** `pychron_ui_test_canvas_view`: **both pass.**

- [ ] **Step 5: Commit:** `refactor(ui): canvas reads through ICanvasState, read-only without a bridge`

### Task 6: Rehearse Script window and its entry points

**Files:**
- Create: `apps/pychron-ui/src/rehearsal_window.hpp`, `apps/pychron-ui/src/rehearsal_window.cpp` (`RehearsalWindow`, `RehearsalCanvasState`)
- Modify: `apps/pychron-ui/src/script_editor_window.{hpp,cpp}` (action "Rehearse…" in the Scripts group, acting on the current tab's text and kind)
- Modify: `apps/pychron-ui/src/main_window.{hpp,cpp}` (owns the one window, lazily, as `ensure_experiment_window` does; "Rehearse Script…" in the Scripts menu; passes the line's `interlock_sets()` and the bridge state)
- Modify: `apps/pychron-ui/src/shortcuts.{hpp,cpp}` (`Shortcut::RehearseStep` F10 and `RehearseRunToEnd` Shift+F10, new `ShortcutContext::RehearsalWindow`; `overlap` updated)
- Modify: `docs/user/09-scripting.md` (section "Rehearsing a script")
- Test: `tests/ui/test_rehearsal_window.cpp`

**Interfaces:**
- Consumes: `RehearsalRunner`, `StepRecord`, `LineSnapshot`, `Advance` (Task 4); `ICanvasState`, `CanvasView(ICanvasState&, CoreBridge*, QWidget*)` (Task 5); `SwitchManager::interlock_sets()` (Task 3).
- Produces:
```cpp
class RehearsalWindow : public QMainWindow {
 public:
  // `host`, `bridge` and `lab` must outlive the window.
  RehearsalWindow(scripting::IScriptHost& host, const CoreBridge& bridge,
                  std::map<std::string, systems::InterlockSet, std::less<>> interlocks,
                  const experiment::lab::Lab& lab, std::unique_ptr<QSettings> settings = nullptr,
                  QWidget* parent = nullptr);
  ~RehearsalWindow() override;  // runner destroyed first
  // Loads a script; asks (confirm_replace) before replacing a running one.
  void set_script(scripting::ScriptKind kind, QString name, QString text, scripting::ValueMap defaults = {});
  void start();          // snapshot of the bridge state + the form → RehearsalRunner
  void step();           // Advance::Step
  void run_to_end();
  void stop();
  void restart();        // stop, join, new snapshot, start with the same form values
  CanvasView* canvas() const;
  CodeEditor* source() const;
  QTableWidget* log() const;        // columns: #, where, call, outcome
  QFormLayout* fields() const;
  void set_confirm_replace(std::function<bool()> confirm);  // tests
};
```
Run-field defaults: `MainWindow` finds the first row of the open experiment window's queue whose script column for that kind names the script, and passes its fields; with none, `make_context()`'s defaults. Each `StepRecord` reaches the UI through `QMetaObject::invokeMethod(this, ..., Qt::QueuedConnection)`. A pending value shows a `QLineEdit` in the outcome cell; Enter calls `provide_value`. The window title is `"Rehearse <name>"`, and the status bar reads `"Rehearsal: no valve is actuated"`.

- [ ] **Step 1: Write the failing tests,** a QTest class with the example line, a `CoreBridge`, `make_script_host()` (skip without scripting) and a temporary lab holding `configs/examples/scripts`:
  - `StepsSimExtract`: `set_script(Extraction, "sim_extract.py", <file text>)`, `start()`, wait for the log to have row 1 waiting; `step()` twice → the rehearsal canvas `P2` is Open while the main bridge's `state().valves["P2"]` is unchanged; `source()`'s highlighted line is 14.
  - `RunToEndFinishes`: the last log row is `"finished"`.
  - `RehearsesUnsavedText`: text differing from the file (`close('A')` added first) → row 1's call is `close A`.
  - `CheckFailureShowsDiagnostics`: `open('Z')` → one red row naming `Z`; no runner thread left (`stop()` returns immediately).
  - `RestartWhileBlockedStartsOver`: `restart()` while waiting at step 3 → the log is cleared and row 1 is waiting again, within 5 s.
  - `PendingValueEntry`: a script with `get_pressure('x','nogauge')` → the outcome cell holds a line edit; type `1e-9` and Enter → the next row's outcome follows.
  - `GeometrySaved`: the `rehearsal_window/geometry` key is set after close (use `settings_guard.hpp`).
  - `ShortcutCatalogHasNoClash`: an existing shortcut test style, covering the new context.

- [ ] **Step 2: Run, expect a compile failure:** `cmake --build --preset dev-ui --target pychron_ui_test_rehearsal_window`

- [ ] **Step 3: Implement `RehearsalCanvasState`:** holds a `CoreBridge::State` copied from the bridge at start, with `valves` and `switches` replaced from `Rehearsal::state()` after each record; `pending` is always false; `wall_now` and `canvas` come from the bridge.

- [ ] **Step 4: Implement `RehearsalWindow`,** then wire the entry points in the script editor and the main window, register it with `MenuHub::add_window`, and add the shortcuts.

- [ ] **Step 5: Run** `pychron_ui_test_rehearsal_window`, `pychron_ui_test_script_editor`, `pychron_ui_test_shortcuts` **and** `pychron_ui_test_menu_hub`: **all pass.**

- [ ] **Step 6: Write the user guide section** (what rehearsal does, that nothing is actuated, the controls, and that reads with no live reading ask for a value).

- [ ] **Step 7: Commit:** `feat(ui): rehearse a script step by step in its own window`

### Task 7: No-actuation proof and full verification

**Files:**
- Test: `tests/ui/test_rehearsal_window.cpp` (one more case)

**Interfaces:**
- Consumes: everything above.

- [ ] **Step 1: Write the test** `NothingIsActuated`. Take `line.switches().list()` (states plus `SwitchStats` via `info`) before; subscribe to `ValveChanged` and `ActuationFailed` on the line's bus with counters; rehearse `extraction/sim_extract.py`, `extraction/sim_air.py` and `post_measurement/sim_pump.py` with Run to End to `"finished"` or a refusal. Expected: every switch's state and stats are equal to before, and both counters are 0.

- [ ] **Step 2: Run it: it passes** (or, if it fails, the leak it found is fixed at its cause in the task that owns it).

- [ ] **Step 3: Rebase on `origin/develop`,** then run `python3 tools/quality_check.py` (exit 0), `python3 tools/compiler_sweep.py --build-dir build/dev-ui` (no warnings), `cmake --build --preset dev-ui` and `ctest --preset dev-ui`. Expected: everything passes.

- [ ] **Step 4: Run the app by hand** with `build/dev-ui/apps/pychron-ui/Pychron.app/Contents/MacOS/Pychron --sim`, open Scripts > Rehearse Script… > `sim_extract.py`, step to the end, and take screenshots of the rehearsal window next to the main canvas.

- [ ] **Step 5: Commit:** `test(ui): a rehearsal actuates nothing`

## Open questions

None.
