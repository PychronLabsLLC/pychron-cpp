# Executor Hardening Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A queue that runs unattended survives links that drop: a short outage costs nothing, a long one costs at most the run in flight, leaves the line safe, tells the operator, and loses nothing that was measured.

**Architecture:** Four layers, each recovering what it can and passing the rest up (spec section 3). The transport closes a dead channel instead of timing out on it. `Device::call(op, Repeat)` reconnects and repeats the calls that may be sent twice. A run rides out a gap in the readings, and a failed run past its point of no return still saves and puts the line in a safe state. The executor gains a `Holding` state in which it waits for the links the next run needs. Services off the control path (spool flush, notifications) buffer locally and never hold a run.

**Tech Stack:** C++20, asio (TCP), toml++, GoogleTest; `VirtualClock`, `VirtualTimeTest` and `Crew` for every test with a wait in it; Qt 6 Widgets for the two UI tasks.

**Spec:** `docs/superpowers/specs/2026-10-09-executor-hardening-design.md`. Read it first; section numbers below refer to it. Its section 9 holds the `Repeat` marking of Qtegra and Chromium and the decision on the record's `status` key.

## Global Constraints

- Every wait added is a clock wait (`clock.wait`, `wait_until`, `sleep_for`); a condition variable waited on that way is notified with `clock.notify_all(cv)`; a mutex held across a clock wait or a transport call is a `ClockMutex`. A new thread follows the start and join rules of AGENTS.md, "Time". No `cv.wait_for`, `std::this_thread::sleep_for` or polling against `steady_clock`.
- The one exception is TCP keepalive, which the kernel times: it goes on the list of things left on real time in `docs/superpowers/specs/2026-10-06-virtual-clock-design.md`, section 4.4, in the task that adds it.
- A command marked `Repeat::Unsafe` is never sent twice for one request. A run past the start of its Extract phase is never started again by the executor.
- With no fault injected, every existing test passes unchanged and a complete record is byte for byte what it was: `tests/data/records/v2.json` and `v2.toml` still read with their sha.
- Errors are values (`pychron::Result<T>`). `outcome_unknown` is `Error::code`, not a new `ErrorKind`.
- Defaults are the spec's (section 4.10) and live in one struct each; the loader, the example file and the docs are held together by a test, as `sim.toml` is.
- Before changing anything under `libs/experiment`, `libs/sim`, `libs/metrics` or `libs/setup`, read that component's `AGENTS.md`.
- Each task is one commit, Conventional Commits with the component as scope. Before each: `python3 tools/quality_check.py` (exit 0), the target's own test binary with a filter, then `ctest --preset dev-quick`.
- Tasks 4, 11, 13, 17, 19 and 20 add a wait or share state with a thread: each is also run under `-DPYCHRON_SANITIZE=thread` before its commit. The whole branch is run under TSan and under ASan/UBSan before it lands (Task 23).
- Real-socket test against sim serve's `WireServer` is deferred (spec section 6, owner 2026-10-13); the loopback-server test of Task 3 stands in.

## Order

Phases follow spec section 7. Phases 1 to 4 each help alone and may land on `develop` one at a time; 5 to 8 need what is before them.

| Phase | Tasks | Spec |
|---|---|---|
| 1 Transport | 1 to 3 | 4.1 |
| 2 Device | 4 to 8 | 4.2 |
| 3 Save on failure | 9, 10 | 4.5 |
| 4 Safe state | 11, 12 | 4.7 |
| 5 Gap rule | 13, 14 | 4.4 |
| 6 Holding | 15 to 18 | 4.3, 4.6, 4.10 |
| 7 Services | 19, 20 | 4.8 |
| 8 Seen and counted | 21 to 23 | 4.9, 7 |

## File Structure

| File | Responsibility |
|---|---|
| `libs/transport/src/queued_transport.cpp`, `include/pychron/transport/transport.hpp` | `Io` closes the channel; no blind retry in `write()`; `exchange()` retries only when asked. |
| `libs/transport/src/tcp_transport.cpp` | `SO_KEEPALIVE` after `no_delay`. |
| `libs/transport/include/pychron/transport/sim_transport.hpp`, `src/sim_transport.cpp` | `set_down(bool)`, `down_for(Duration)`. |
| `libs/devices/include/pychron/devices/link.hpp`, `src/link.cpp` (new) | `Repeat`, `LinkPolicy`, `Link`: the generalised `Reconnector`. |
| `libs/devices/include/pychron/devices/device.hpp`, `src/device.cpp` | `Device::call(op, Repeat)`. |
| `libs/devices/include/pychron/devices/reconnect.hpp`, `src/reconnect.cpp` | Removed once `QtegraLink` is on `Link`. |
| every driver under `libs/devices/src` (list in Task 6) | Each wire call marked; the table in the driver's header. |
| `libs/core/include/pychron/core/config/system_config.hpp` and its loader; `libs/systems/src/spectrometer/config_loader.cpp` | `[transports.<name>.recovery]`. |
| `libs/core/include/pychron/core/events.hpp` | `LinkReconnected`. |
| `libs/systems/src/switch_manager.cpp` | `outcome_unknown` resolved by reading back. |
| `libs/experiment/include/pychron/experiment/record/types.hpp`, `src/record/serialize.cpp`, `src/record/builder.cpp` | `provenance.status`, `provenance.failure`. |
| `libs/experiment/src/run/run.cpp`, `include/pychron/experiment/run/run.hpp` | Save after failure; safe state; point of no return. |
| `libs/scripting/include/pychron/scripting/script.hpp`, `src/script.cpp`; `libs/experiment/src/lab/scripts.cpp` | `ScriptKind::SafeState`, `<lab>/scripts/safe_state.py`. |
| `libs/experiment/src/measurement/engine.cpp`, `include/.../measurement/engine.hpp` | Gap rule, `MeasurementGap`, `min_counts`. |
| `libs/experiment/include/pychron/experiment/executor/executor.hpp`, `src/executor/executor.cpp` | `Holding`, `IPreRunCheck` kinds, state file, `resume()`, `skip()`. |
| `libs/experiment/include/pychron/experiment/executor/links.hpp`, `src/executor/links.cpp` (new) | `LinkSet`, `LinksUp`, `SpoolWritable`. |
| `libs/experiment/include/pychron/experiment/lab/executor_config.hpp`, `src/lab/executor_config.cpp` (new); `configs/examples/executor.toml` (new) | `<lab>/executor.toml`. |
| `libs/experiment/src/persist/persister.cpp`, `include/.../persist/persister.hpp` | Flush after every run and on a timer; `SpoolStatus`. |
| `libs/experiment/src/lab/notifier.cpp`, `notifications.cpp`, their headers | Outbox on disk, retry, three new events. |
| `libs/metrics/src/core_exporter.cpp`, `libs/experiment/src/metrics/experiment_metrics.cpp`, dashboards | Seven metrics, a panel each. |
| `apps/elctl/src/exp.cpp`; `apps/pychron-ui/src/health_bar.*`, `experiment_window.cpp`, `core_bridge.*` | Hold shown; Resume and Skip. |
| `docs/user/04-experiments.md`, `docs/notifications.md`, `docs/observability.md`, `libs/experiment/AGENTS.md`, `libs/devices/AGENTS.md` (new, with `CLAUDE.md` and a row in the root table) | Docs and rules. |

Tests: `tests/transport/test_queued_transport.cpp`, `test_sim_transport.cpp`, `test_tcp_transport.cpp`; `tests/devices/test_link.cpp` (replaces `test_reconnect.cpp`), `test_repeat_tables.cpp` (new); `tests/systems/test_switch_manager.cpp`; `tests/experiment/test_analysis_record.cpp`, `test_record_ingest.cpp`, `test_run.cpp`, `test_measurement_engine.cpp`, `test_executor.cpp`, `test_executor_config.cpp` (new), `test_persist.cpp`, `test_notifications.cpp`, `test_experiment_metrics.cpp`; `tests/integration/test_outage.cpp` (new), `test_metrics_packaging.cpp`; `tests/ui/test_experiment_window.cpp`.

---

## Phase 1: Transport (spec 4.1)

### Task 1: An `Io` error closes the channel

**Files:** `libs/transport/src/queued_transport.cpp`, `libs/transport/include/pychron/transport/transport.hpp`, `tests/transport/test_queued_transport.cpp`, and every test that relied on the blind retry.

Today `open` stays true after the peer closes (`queued_transport.cpp:56`) and `exchange` and `write` resend after `Timeout` or `Io` when `retries > 0` (`:273-280`, `:291-293`).

- [x] Failing tests, in a fake `QueuedTransport` subclass whose `do_read` can be told to fail:
  - `QueuedTransport.AnIoErrorClosesTheChannel`: after one `Io` from `do_read`, `do_close` was called once, health is `Down`, and the next `exchange` fails `NotConnected` without `do_write` being called.
  - `QueuedTransport.ATimeoutLeavesTheChannelOpen`.
  - `QueuedTransport.WriteIsNeverSentTwice`: `retries = 3`, `do_write` fails `Timeout`: one call.
  - `QueuedTransport.ExchangeRepeatsOnlyWhenAsked`: `exchange(tx, spec)` with `retries = 3` sends once; `exchange(tx, spec, timeout, Resend::Allowed)` sends up to four times on `Timeout`, and stops at once on `Io` (the channel is closed).
- [x] Implement: in the worker, `Io` from `do_write` or `do_read` runs `do_close()`, sets `open = false` and calls `note_closed`. Remove the loop from `write()`. Add `enum class Resend { Never, Allowed }` as a defaulted fourth parameter of `Transport::exchange`, after `timeout` (`transport.hpp:40`; default `Never`); the loop stays for `Allowed`. `exchange` is virtual with default arguments: every override and every fake in `tests/` takes the parameter. The default is written on the base and, with the same value, on the overrides that already repeat the default of `timeout` (`QueuedTransport`, `TraceRecorder`, `LinkTransport`, one test fake): they are called through their own type, where the base's defaults do not apply. `retries` keeps its meaning for the calls that pass `Allowed`, which from Task 5 on are the `Safe` ones (spec 4.1): `Device::call` hands the driver's op the `Resend` to use.
- [x] `grep -rn 'retries' libs apps configs profiles tests`: no shipped config sets it (spec section 2); fix any test that counted on the old meaning by passing `Resend::Allowed`, never by deleting the case.
- [x] `build/dev/tests/transport/pychron_transport_tests --gtest_filter='QueuedTransport.*'`, then `ctest --preset dev-quick`.
- [x] Commit: `fix(transport): an Io error closes the channel and nothing is resent unasked`.

### Task 2: `SimTransport` can be down for a while

**Files:** `libs/transport/include/pychron/transport/sim_transport.hpp`, `libs/transport/src/sim_transport.cpp`, `tests/transport/test_sim_transport.cpp`, `docs/simulator.md`.

- [x] Failing tests (`VirtualTimeTest`):
  - `SimTransport.WhileDownEveryCallFailsIo`: `set_down(true)`; `exchange`, `write`, `read` fail `Io`; `open()` fails `Io`; `set_down(false)` then `open()` succeeds and a scripted exchange answers.
  - `SimTransport.DownForEndsOnTheTransportsClock`: `down_for(5s)`; at 4.9 s `open()` fails, at 5 s it succeeds. No real sleep.
  - `SimTransport.GoingDownDoesNotConsumeAScriptedStep`: an `expect` step queued before the outage is still there after it.
- [x] Implement beside `drop_next` (`sim_transport.cpp:147-162`): a `down_until_` time point and a `down_` flag under the existing mutex, tested in `do_open`, `do_write` and `do_read`. No thread: the deadline is compared with `clock.now()` at each call.
- [x] Commit: `feat(transport): a simulated link can be down, for good or for a time`.

### Task 3: TCP keepalive

**Files:** `libs/transport/src/tcp_transport.cpp`, `tests/transport/test_tcp_transport.cpp`, `docs/superpowers/specs/2026-10-06-virtual-clock-design.md` (section 4.4 list).

- [x] Failing test `TcpTransport.KeepaliveIsOnAfterOpen`: open against the loopback listener the file already has, read the option back from the socket through a test accessor. Then `TcpTransport.APeerThatClosesIsSeenAsDown`: the listener closes the accepted socket; the next `exchange` fails `Io`, the one after `NotConnected` (Task 1).
- [x] Implement after `no_delay(true)` (`tcp_transport.cpp:54`): `asio::socket_base::keep_alive(true)`, then idle 10 s, interval 5 s, count 3 through `setsockopt` on `native_handle()` behind `#if defined(TCP_KEEPIDLE)` (Linux), `TCP_KEEPALIVE` (macOS, idle only) and `SIO_KEEPALIVE_VALS` (Windows). A platform that offers none of them keeps the system defaults; a failed `setsockopt` is not an error of `open()` and is not logged (the transport has no logger): `TcpTransport::keepalive()` reads back what took (on, and the idle time on Linux and macOS).
- [x] Add "TCP keepalive (kernel)" to the virtual clock spec's list of what stays on real time.
- [x] Windows and Linux compile only in CI: note in the commit body that the branch needs a CI run before release.
- [x] Commit: `feat(transport): TCP keepalive finds a half-open connection between commands`.

---

## Phase 2: Device (spec 4.2)

### Task 4: `Link`: reconnect with backoff, repeat when safe

**Files:** `libs/devices/include/pychron/devices/link.hpp`, `libs/devices/src/link.cpp`, `libs/core/include/pychron/core/events.hpp`, `tests/devices/test_link.cpp`.

```cpp
enum class Repeat { Safe, Unsafe };

struct LinkPolicy {
  bool reconnect = true;
  bool optional = false;                  // spec 4.3
  Duration backoff_min = std::chrono::seconds(1);
  Duration backoff_max = std::chrono::seconds(30);
  Duration call_window = std::chrono::seconds(20);
};

class Link {  // one per transport, shared by the devices on it
 public:
  Link(Transport& transport, const Clock& clock, LinkPolicy policy, SignalBus* bus = nullptr);
  using Connect = std::function<Result<void>(Transport&)>;
  void set_connect(Connect step);         // Qtegra's handshake
  template <class T>
  [[nodiscard]] Result<T> call(const std::function<Result<T>()>& op, Repeat repeat,
                               std::optional<Duration> window = std::nullopt);
  [[nodiscard]] Result<void> probe();     // the connect step, Safe; Task 17
  [[nodiscard]] std::uint64_t reconnects() const noexcept;
  [[nodiscard]] std::uint64_t repeats() const noexcept;
};
```

- [x] Failing tests, `VirtualTimeTest` over a `SimTransport` with Task 2's faults:
  - `Link.ASafeCallIsRepeatedUntilTheLinkReturns`: down for 7 s; the call succeeds, sent twice in all, `reconnects() == 1`, `repeats() == 1`, elapsed is between 7 s and 7 s plus one backoff.
  - `Link.ASafeCallGivesUpAtItsWindow`: down for good; returns the last transient error at `call_window` (20 s), not before, not after 20 s plus `backoff_max`.
  - `Link.BackoffDoublesFromMinToMax`: the `open()` attempts are at 1, 2, 4, 8, 16, 30, 30 s apart.
  - `Link.AnUnsafeCallIsSentOnce`: the op fails `Io` once; `call(op, Unsafe)` reconnects, does not call `op` again, returns an error whose `code` is `"outcome_unknown"` and whose kind is the original.
  - `Link.ADefiniteErrorIsNeverRepeated`: `Protocol`, `Config`, `Interlock`, `Cancelled`.
  - `Link.TwoThreadsOneReconnect` (`Crew`): two callers fail at once; `open()` is called once.
  - `Link.AReconnectThatStartedAfterMyFailureIsNotSkipped`: the "known window" in `reconnect.hpp`'s comment: the generation is read inside the transport's `transaction`, so a caller that failed on the new connection reconnects again.
  - `Link.EachReconnectIsPublished`: `LinkReconnected{transport, attempts, down_for}` on the bus.
  - `Link.NoReconnectWhenPolicySaysSo`.
- [x] Implement from `reconnect.cpp`: `ClockMutex` around the reconnect, `clock.sleep_for` for the backoff, the generation counter taken under `transport.transaction`. `LinkReconnected` goes in `events.hpp` beside `TransportHealth`.
- [x] TSan: `cmake --preset dev -DPYCHRON_SANITIZE=thread` in a second build directory, `pychron_devices_tests --gtest_filter='Link.*'`.
- [x] Commit: `feat(devices): a link reconnects with backoff and repeats what may be sent twice`.

### Task 5: `Device::call`, and Qtegra moves onto it

**Files:** `libs/devices/include/pychron/devices/device.hpp`, `libs/devices/src/device.cpp`, `libs/devices/src/spectrometer/qtegra_link.cpp` and its header, `thermo_qtegra.cpp`, `qtegra_valves.cpp`, `qtegra_gauges.cpp`, `libs/devices/include/pychron/devices/reconnect.hpp` and `src/reconnect.cpp` (deleted), `tests/devices/test_link.cpp`, `tests/devices/test_reconnect.cpp` (its cases moved, none dropped), `tests/systems/test_qtegra_system.cpp`.

- [x] `DeviceOptions` gains `Link* link = nullptr`. `Device::call(op, Repeat)` is `observe(link ? link->call(op, repeat) : op())`, where `op` takes the `Resend` for its exchanges (`Allowed` when `Safe`, `Never` when `Unsafe`): a device without a link behaves as today, and an `Unsafe` command is resent by neither layer (test: `retries = 3`, an `Unsafe` call that times out is seen once by the `SimTransport`).
- [x] `QtegraLink` owns a `Link` in place of its `Reconnector`; `set_handshake` becomes `Link::set_connect`. Every Qtegra command is `Safe` (spec section 9).
- [x] Move each case of `test_reconnect.cpp` into `test_link.cpp` under its old name where the behaviour is kept; where the rate limit was asserted, assert the backoff instead and say so in the commit body.
- [x] `QtegraAcquireSteady` and `test_qtegra_system` pass unchanged.
- [x] Commit: `refactor(devices): Qtegra reconnects through the shared link`.

### Task 6: Every driver marks its commands

**Files:** each driver below, its header, `tests/devices/test_repeat_tables.cpp`; `libs/devices/AGENTS.md` and `CLAUDE.md` (new), the root `AGENTS.md` table.

One commit a driver family, in this order; each is small and lands alone.

| Driver | `Safe` | `Unsafe` |
|---|---|---|
| `QtegraSpectrometer`, `QtegraValves`, `QtegraGauges` (Task 5) | all | none |
| `ChromiumLaser` | every query; `Laser.Enable 0\|1`, `Laser.Output`, `Stage.MoveTo`, `Scans.MoveTo`, `Laser.Stop`, `Stage.Stop`, `Scans.Stop`, `Scans.Status_Verbosity` | `Laser.Fire` |
| `NgxLink` users (`NgxSpectrometer`, `NgxValves`) | `Login`, `SetAcqPeriod`, `SetMass`, `GETMASS`, `SSO`, `GSO`, `GetValveStatus`, `StopAcq`; a valve actuation as one unit: `SAB 1`, `OpenValve` or `CloseValve`, `SAB 0` (`ngx.cpp:526-541`; `SAB` is an absolute set) | `StartAcq` |
| `AgilentSwitch` | `*IDN?`, `*CLS` (sent in `connect()`, which a reconnect runs again anyway), `ROUT:OPEN`, `ROUT:CLOSE`, `ROUT:OPEN?`, `ROUT:CLOSE?` | `SYST:ERR?` (a read that takes the error off the instrument's queue: a repeat after a lost reply loses it) |
| `PychronValves`, `Plc2000Valves`, `ProxrRelay` | open, close, on, off, state reads, bank select | none |
| `Plc2000Heater`, `Lakeshore` | reads; `SETP`, `RANGE`, coil and register writes (absolute) | none |
| `GpMicroIon`, `PfeifferMaxiGauge`, `Plc2000Gauges`, `VarianXgs600` | reads; `IG1 ON\|OFF` | none |
| `AdcBank`, `PulseCounter`, `SerialHv`, `Map215Dac` | reads; `VSET`; `B`, `W` | `PulseCounter` `R` (the read clears the count, `pulse_counter.hpp:5`: an `outcome_unknown` poll costs that interval's counts and the acquirer does not ask again for it) |

- [x] For each driver: a test line per command in `test_repeat_tables.cpp` (`RepeatTable.<Driver>`): the link is taken down for 3 s around that one command; a `Safe` command is seen twice by the `SimTransport` and the call succeeds; an `Unsafe` one is seen once and the call returns `outcome_unknown`.
- [x] Each wire call goes through `Device::call`. A driver that sends several writes in one `transact` (Chromium's `act`, Agilent, Lakeshore, Pfeiffer, ProXR) repeats the whole transaction, never one write of it. The same holds for a sequence that is several calls under a driver's own mutex: an NGX valve actuation is repeated as the whole `SAB 1` ... `SAB 0` bracket, and `RepeatTable.NgxValves` fails if the simulator's `unbracketed_actuations` (`ngx_sim.hpp:59`) is not zero.
- [x] Tests here make their `Link` by hand and give it to the driver; the line and the spectrometer assembler hand out links in Task 7.
- [x] `ChromiumLaser::fire_laser`: on `outcome_unknown`, send `Laser.Stop` (its recovery path already does, `chromium.cpp:386`) with the critical window (10 min, spec 4.4), not the call window, and return the error. Test: the link is down for 30 s after the lost `Laser.Fire`; the `Stop` arrives when it returns and `Laser.Fire` was seen at most once.
- [x] The table goes in each driver's header as a comment above the class. `libs/devices/AGENTS.md` states the marking rule (spec 4.2: absolute target or read is `Safe`; a command whose second arrival does more is `Unsafe`) and that a new command without a row in `test_repeat_tables.cpp` is not done.
- [x] Commits: `feat(devices): <driver> marks which commands may be sent twice`. Landed as one commit, `feat(devices): every driver marks which commands may be sent twice` (the nightly run that did it commits once a task). An op mixing `Safe` and `Unsafe` commands in one transaction (Agilent's route, then `SYST:ERR?`) marks its `Unsafe` part's transient failure `outcome_unknown`, and `Link` does not repeat it (`Link.ASafeCallWhoseOpSaysOutcomeUnknownIsNotRepeated`; spec 4.2). The NGX rigs have no `Link`: `NgxLink::call` repeats, its reader reconnects.

### Task 7: `[transports.<name>.recovery]`

**Files:** `libs/core/include/pychron/core/config/system_config.hpp` (`TransportConfig`, `:73`) and its loader, `libs/systems/src/extraction_line.cpp` (`:206-229`, where transports are built), `libs/systems/src/spectrometer/assembler.cpp`, `configs/examples` line and spectrometer files, `tests/systems/test_extraction_line.cpp`, `tests/setup`.

The spec called the table `link`; a spectrometer transport already has a string key of that name (`config_loader.cpp:145-155`, kind `"link"`: the NGX's shared connection), so the table is `recovery` (spec 4.2, 4.3 and 4.10 changed in this branch).

- [x] Failing tests: the three keys `reconnect`, `optional`, `call_window_s` load and are range-checked (`call_window_s` 0 to 3600; an unknown key under `recovery` is a `Config` error naming it), in the line's loader and in the spectrometer's (`libs/systems/src/spectrometer/config_loader.cpp`); absent table gives `LinkPolicy{}`; the line makes one `Link` per transport and every driver on that transport is given the same one; a transport of kind `"link"` uses the `Link` of the transport it names and may not have a `recovery` table of its own (error naming it). Built: `ConfigLoader.*Recovery*`, `SpectrometerConfig.*Recovery*`, `LinkPolicyConfig.*`, `ExtractionLine.EveryDriverOnATransportIsGivenItsOneLink`, `...ATransportsLinkTakesItsRecoveryTable`, `...ALinkTransportHasNoLinkOfItsOwn`, `SpectrometerAssembler.EveryDriverOnATransportIsGivenItsOneLink`. The table is `config::RecoveryConfig` (`libs/core` `recovery_config.hpp`), turned into a policy by `link_policy()` (`link.hpp`). A kind `"link"` transport gets no `Link` from the line: its drivers (NGX, a Qtegra borrower) reach the recovery of the connection it names through that connection's registry, made where it is owned. Qtegra and NGX keep their own connection and take only the transport's policy (`policy_of(args)`); see spec 4.10, "As built".
- [x] `NgxLink` keeps its reader loop and takes `backoff_min` and `backoff_max` from `LinkPolicy` in place of `reconnect_min` and `reconnect_max`; `NgxLinkSteady` passes unchanged. `NgxLinkOptions::policy` defaults to the reader's own 250 ms to 5 s (`reader_policy()`); `reconnect = false` stops `call()` repeating, the reader still reopens.
- [x] Read `libs/setup/AGENTS.md` before touching `configs/examples`: the example line shows the table commented out with its defaults, and `optional = true` on its camera transport. The example line has no camera transport (cameras are in `cameras.toml`), so the table is shown on `valve_bus` and `optional = true` only in its comment; `spectrometer.qtegra.toml` shows it too. `ExtractionLine.TheExampleRecoveryTableIsTheDefaults` and `SpectrometerConfig.TheExampleRecoveryTableIsTheDefaults` hold the files to the defaults.
- [x] `LineHeaters.AFailedScanAlarmsOnceUntilTheNextGoodOne` sets `reconnect = false` on the transport it takes offline: its read is now `Safe` through the line's `Link`, and on the test's `ManualClock` the call window's backoff is never advanced, so the scan waited for good.
- [x] Commit: `feat(systems): a transport's reconnect policy is read from the line file`.

### Task 8: A valve whose command may or may not have arrived

**Files:** `libs/systems/src/switch_manager.cpp` (`drive`, `:299-338`), `tests/systems/test_switch_manager.cpp`.

No shipped valve driver has an `Unsafe` command after Task 6, so this is reached only through a fake actuator: it is the rule for the next driver that has one.

- [x] Failing tests: `SwitchManager.AnUnknownOutcomeThatReadsBackAtTargetIsSuccess`; `...ThatReadsBackElsewhereIsRepeatedOnce` (then success, or the read-back mismatch error as today); `...OnAValveWithoutVerifyIsRepeated`; state on the bus is `Unknown` from the failed command until resolved. Built as those, with the mismatch case its own test (`...ThatReadsBackElsewhereAfterItsRepeatIsAMismatch`) and one more: `...WhoseReadFailsIsTheLostCommandsError` (not sent again blind; the switch stays `Unknown`).
- [x] Implement in `drive` after the command at `:302`.
- [x] Commit: `feat(systems): a valve command of unknown outcome is settled by reading the valve`.

---

## Phase 3: Nothing measured is dropped (spec 4.5)

### Task 9: The record says how its run ended

**Files:** `libs/experiment/include/pychron/experiment/record/types.hpp` (`Provenance`, `:203`), `src/record/serialize.cpp` (write near `:276`, read near `:433`), `tests/experiment/test_analysis_record.cpp`, `tests/experiment/test_record_ingest.cpp`, `libs/experiment/AGENTS.md`, the record ingest spec's not-stored list if the store gets no column.

```cpp
enum class RecordStatus { Complete, Truncated, Failed };
struct Provenance { /* ... */ RecordStatus status = RecordStatus::Complete; std::string failure; };
```

- [x] Failing tests (`RecordBuilder::set_status(status, failure)` sets both):
  - `AnalysisRecord.StatusIsWrittenOnlyWhenNotComplete`: a complete record's TOML and JSON have no `status` key; truncated and failed have `provenance.status = "truncated" | "failed"`; `failure` only when not empty.
  - `AnalysisRecord.ARecordWrittenBeforeStatusStillReadsWithItsSha`: `tests/data/records/v2.json` and `v2.toml` read, `verify_sha` holds, `status == Complete`.
  - `AnalysisRecord.StatusRoundTrips` and `...AnUnknownStatusIsRejected` (`Config`).
  - `kRecordSchemaVersion` stays 2; `SchemaVersionBumpIsRejected` untouched.
- [x] `libs/experiment/AGENTS.md:24-26`: a field added to `AnalysisRecord` needs a row in `test_record_ingest.cpp` or a line in the not-stored list. Owner, 2026-10-13: `RecordIngest` stores a `failed` record like any other and gives it the tag `invalid` (its tags revision), with the `failure` text as the tag's note where the tag has one; so the browser and every figure leave it out by default, as they do any invalid analysis, and the recall window can still open it. `truncated` is stored as any run is today, tag `ok`. Test both; `AnOrdinaryRunConvertsAsItAlwaysDid` unchanged. Built: `RecordIngest.AFailedRecordIsStoredTaggedInvalid`, `...ATruncatedRecordIsStoredAsAnyRun` (stored items the same but the item's payload sha).
- [x] `libs/processing/adapters/records/src/record_source.cpp` (`:294`): a `failed` record is read as an analysis whose tag is `invalid`, the same as the store gives it. Test in `tests/processing/test_record_source.cpp` (`RecordSource.AFailedRecordIsAnInvalidAnalysis`).
- [x] Add the key to `libs/experiment/AGENTS.md` beside the `simulated` rule (`:64-69`).
- [x] Commit: `feat(experiment): a record says whether its run was complete, truncated or failed`.

### Task 10: A run that fails after extraction began is saved

**Files:** `libs/experiment/src/run/run.cpp` (`execute`, `:111-167`; `save`, `:445`), `include/pychron/experiment/run/run.hpp`, `tests/experiment/test_run.cpp`, `tests/experiment/run_fakes.hpp`.

- [x] Failing tests, one per row of the table in spec 4.4 that ends a run:
  - `Run.AFailureInPrepareSavesNothingAndSpendsNoAliquot` (as today; pins it). Built as `RunTest.AFailureInPrepareSavesNothing`: the aliquot number is issued before the plan loads, and `FilePersister` does not issue it again in that process, so "spends no aliquot" was not today's behaviour; nothing of the sample is spent and nothing is saved, which is what the test pins.
  - `Run.AFailureInExtractionIsSavedAsFailed`: the spool holds a record with `status = failed`, `failure` the error's text, the extraction actuals and notes; `RunResult::state` is `Failed`.
  - `Run.AFailureInMeasurementKeepsTheSeriesCollected`: the series up to the failure are in the record.
  - `Run.ATruncatedRunIsMarkedTruncated`.
  - `Run.ACancelledOrAbortedRunIsStillNotSaved` (pins `:131-146`: operator's choice, not a link failure).
  - `Run.TheRecordReachesTheSpoolBeforeRunFinished` (spec section 5): order of spool write and the `RunFinished` event.
- [x] Implement: `RunResult` gains `past_no_return` (set at the first line of `extract()`), and `execute` calls `save()` on the `Fail` paths at `:136-137` and `:147-150` when it is set. `save()` returns early without a plan (`:447`) as today. Built: `save(failure)` sets `status = failed` and `failure`, or `truncated` when the run was; the record is saved before the run is `Failed`. A failed record needs only its Identity phase to finalize (`RecordBuilder.AFailedRecordNeedsOnlyItsIdentity`): it lacks results by nature, and `to_ingest` refuses a record without a sha, which would have kept it in the spool for good. Also `RunTest.ACompleteRunIsMarkedComplete`. `past_no_return` is set where the extraction script starts, after the resource wait, not at the first line of `extract()`: a run without an extraction script spent no sample and saves nothing on failure (`RunTest.ARunWithoutAnExtractionScriptThatFailsSavesNothing`; review of the branch).
- [x] The aliquot of a saved failed run is spent: `Run.AFailedRunsAliquotIsNotGivenAgain` with `FilePersister`.
- [x] Commit: `feat(experiment): a run that fails after extraction began saves what it measured`.

---

## Phase 4: Safe state (spec 4.7)

### Task 11: The built-in steps

**Files:** `libs/experiment/src/run/run.cpp`, `include/pychron/experiment/run/run.hpp`, `libs/experiment/src/measurement/engine.cpp` (the inlet close and detector unprotect it tries once today), `tests/experiment/test_run.cpp`. (`AlarmSeverity::Critical` exists, `events.hpp:19`.)

```cpp
struct SafeStateResult { std::vector<std::string> failed_steps; };  // empty: safe
[[nodiscard]] SafeStateResult Run::make_safe();  // idempotent; spec 4.7
```

- [x] Failing tests (`VirtualTimeTest`):
  - `SafeState.StepsRunInOrder`: extraction device end and disable, inlet valve of the plan closed, detectors' protection restored.
  - `SafeState.EachStepKeepsTryingForTheCriticalWindow`: the valve's link down for 5 min: the step succeeds at 5 min; an `Alarm` is published at 20 s.
  - `SafeState.AStepThatFailsDoesNotStopTheLaterOnes`: link down for good: after 10 min the step is in `failed_steps`, a critical `Alarm` is on the bus, the next step ran.
  - `SafeState.ApplyingItTwiceChangesNothing`.
  - `Run.AFailedRunPastNoReturnIsMadeSafe`; `Run.AFailedPrepareIsNot`; `Run.APostMeasureFailureStillSavesAndIsMadeSafe` (spec 4.4, PostMeasure row).
- [x] Implement with `Link::call(op, Repeat::Safe, critical_window)` (default 10 min; from `executor.toml` after Task 15, a constant until then). `RunResult` gains `unsafe` (true when `failed_steps` is not empty) for Task 17. Built: `Run` reaches the line through its ports (`IValvePort`, `ISpectrometerPort`, the extraction device), not a `Link`, so `Run::keep_trying` repeats each step on the run's clock (backoff 1 s doubling to 30 s) until `kSafeStateWindow` (10 min, `run.hpp`); each port call still has its link's own window beneath. A definite error gives up at once (`SafeState.ADefiniteErrorIsNotTriedAgain`). `RunResult::unsafe_steps` names the failed steps. Also `RunTest.ASuccessfulRunIsNotMadeSafeAgain`. The detectors unprotected are those the plan's hops protect.
- [x] TSan run of `SafeState.*`.
- [x] Commit: `feat(experiment): a failed run leaves the line in a safe state`.

### Task 12: The lab's own `safe_state` script

**Files:** `libs/scripting/include/pychron/scripting/script.hpp` (`ScriptKind`, `:29`), `src/script.cpp` (`:48-56`), `src/vocabulary.cpp`, `libs/experiment/src/lab/scripts.cpp` (`:44-59`), `src/lab/lab.cpp`, `apps/pychron-ui/src/script_editor_window.*`, `tests/experiment/test_lab_scripts.cpp`, `tests/experiment/test_run.cpp`, `docs/user`.

- [ ] Failing tests: `<lab>/scripts/safe_state.py` is found and run after the built-in steps, with the line and no acquisition verbs (a call to one is a script error naming it); a lab without the file runs the built-in steps only; a script that raises is a failed step (`unsafe`); a `-DPYCHRON_SCRIPTING=OFF` build compiles and runs the built-in steps.
- [ ] `ScriptKind::SafeState`, `to_string` `"safe_state"`. It is one file, not a directory of names: `script_path` handles the kind apart.
- [ ] The script editor lists it; `tests/ui` only if the editor's kind list is tested there.
- [ ] Commit: `feat(scripting): a lab's safe_state script runs after a failed run`.

---

## Phase 5: Gap rule (spec 4.4)

### Task 13: A silence in the readings is waited out

**Files:** `libs/experiment/src/measurement/engine.cpp` (`collect`, `:506-530`; sniff, `:323`, `:402`), `include/pychron/experiment/measurement/engine.hpp` (`:144-146`), `include/pychron/experiment/record/types.hpp` (a `gap` event kind), `tests/experiment/test_measurement_engine.cpp`.

- [ ] Failing tests (`VirtualTimeTest`, fake acquirer that can go silent):
  - `MeasurementGap.AShortSilenceIsRecordedAndTheBlockEndsOnTime`: silent from 30 s to 50 s of a 100 s block: the block ends at 100 s, the record has one `MeasurementGap{block, from, to}` and a `RunNote`, time zero is unmoved, the count is what 80 s gives.
  - `MeasurementGap.NothingIsDoneToTheLineWhileWaiting`: no valve or protect call between the last reading and the first after the gap.
  - `MeasurementGap.ASilenceLongerThanMaxGapEndsTheBlock`: 120 s default.
  - `MeasurementGap.EnoughCountsIsTruncatedWithReasonLink`; `...TooFewCountsIsFailed` (`min_counts`: 20% of planned and never under the fit's minimum; both bounds tested).
  - `MeasurementGap.AnAcquisitionStartOfUnknownOutcomeIsRestartedOnce` (NGX `StartAcq`).
  - `MeasurementGap.EquilibrationFollowsTheSameRule`.
  - `MeasurementEngine.AValveCallInMeasurementKeepsTryingForTheCriticalWindow` (spec 4.4, valve row): the inlet valve's link is down for 5 min at inlet close: the call succeeds at 5 min, an `Alarm` went out at 20 s, the run goes on. Down for good: the run is `Failed` after 10 min, saved (Task 10) and made safe (Task 11). Same for detector unprotect.
  - `MeasurementEngine.WithNoGapNothingChanges`: an existing record fixture compares equal.
- [ ] Implement: `timeout_factor` and `timeout_slack` now mark the start of a gap, not the end of the block; the wait is `clock.wait_until(min(gap_start + max_gap, block_end))`.
- [ ] TSan run of `MeasurementGap.*`.
- [ ] Commit: `feat(experiment): a measurement rides out a silent spectrometer for up to two minutes`.

### Task 14: Conditionals do not look across a gap

**Files:** `libs/experiment/src/conditionals/evaluator.cpp`, `src/conditionals/expr.cpp` (`is_series_func`, `:53`), `src/measurement/engine.cpp` (`main_reading`, `:558`), `tests/experiment/test_conditionals_runtime.cpp`, `docs/user` conditionals chapter.

The functions that need contiguous points are the series functions that take `window=`: `min`, `max`, `average`, `slope`, `std`, `rsd`, `count`. `between`, `elapsed` and `abs` are not affected.

- [ ] Failing tests: after a gap, a windowed series function sees only readings after it (a `slope` over a window that would straddle the gap is computed from the points after it, and is not evaluated until it has its minimum of points); `elapsed` counts through the gap; a conditional that would have tripped on a straddling window does not.
- [ ] Implement: the engine gives the evaluator the index of the first reading after the last gap.
- [ ] Commit: `feat(experiment): a conditional's window does not reach across a gap in the readings`.

---

## Phase 6: Holding (spec 4.3, 4.6, 4.10)

### Task 15: `executor.toml`

**Files:** `libs/experiment/include/pychron/experiment/lab/executor_config.hpp`, `src/lab/executor_config.cpp`, `configs/examples/executor.toml`, `tests/experiment/test_executor_config.cpp`, `tests/setup` (shipped profiles), `docs/user/04-experiments.md`.

Model: `libs/sim/src/sim_config.cpp` (`number()` with a range, `:175-196`) and `SimConfig.TheExampleFileIsTheDefaults` (`tests/sim/test_sim_config.cpp:772`).

- [ ] Failing tests: every key of spec 4.10 loads; each is range-checked with a message naming key, value and range; an unknown key or table is an error; a missing file is the defaults; `ExecutorConfig.TheExampleFileIsTheDefaults` (every key present, commented out, equal to the default); `after_link_failure` takes only its two values.
- [ ] Read `libs/setup/AGENTS.md`: whether a profile ships the file, and which test must list it.
- [ ] `LabSession` loads it and hands the values to the executor, the engine (`max_gap`, `min_counts_fraction`), the run (`critical_window`) and each `Link` (`call_window`, backoff) not overridden per transport.
- [ ] Commit: `feat(experiment): executor.toml holds the limits of link recovery`.

### Task 16: Which links a run needs, and the pre-run checks

**Files:** `libs/experiment/include/pychron/experiment/executor/links.hpp`, `src/executor/links.cpp`, `include/pychron/experiment/executor/executor.hpp` (`IPreRunCheck`, `:79-83`), `libs/systems/include/pychron/systems/extraction_line.hpp` (an accessor for the links of its valves, switches and gauges), `libs/experiment/src/lab/session.cpp`, `tests/experiment/test_executor.cpp`, `tests/integration/test_lab_session.cpp`.

```cpp
enum class CheckKind { Refuse, Wait };
struct CheckFailure { CheckKind kind; Error error; std::vector<std::string> links; };
class IPreRunCheck { /* name(); */ virtual std::optional<CheckFailure> check(const RunSpec&) = 0; };
struct LinkSet { std::vector<Link*> required; };   // derived, never configured
```

- [ ] Failing tests:
  - `LinkSet.ARunNeedsTheWholeLineItsDeviceAndTheSpectrometer`: the example sim lab; an unknown on the laser needs the line's transports, the laser's and the spectrometer's.
  - `LinkSet.ARunWithoutAPlanDoesNotNeedTheSpectrometer`.
  - `LinkSet.AnOptionalTransportIsLeftOut` (`optional = true`, Task 7).
  - `LinksUp.WaitsWhenARequiredLinkIsDown` (kind `Wait`, names the links); `...PassesWhenOnlyAnOptionalOneIs`.
  - `SpoolWritable.WaitsUnderMinFreeOrOverMaxPending` (free space through an injected function, not the real disk).
  - An existing `IPreRunCheck` in the tests keeps ending the queue (`Refuse`).
- [ ] `LabSession` builds the `LinkSet` per run from `SessionHardware` (`session.hpp:51-65`) and registers `LinksUp` and `SpoolWritable`.
- [ ] Commit: `feat(experiment): a pre-run check can ask the queue to wait, and two do`.

### Task 17: The executor holds

**Files:** `libs/experiment/include/pychron/experiment/executor/executor.hpp` (`ExecutorState`, `:65-75`; events), `src/executor/executor.cpp` (`can_transition` `:62`, `finish` `:340-429`, pre-run check `:623`, `write_state` `:463`, `resume_row` `:485`, overlap `:645-661`), `tests/experiment/test_executor.cpp`.

```cpp
enum class ExecutorState { Idle, Preparing, Running, Holding, StoppingAtBoundary, Cancelling, Aborting, Finalizing };
struct ExecutorHolding { std::string reason; std::vector<std::string> links; TimePoint since, until; };
void Executor::resume();   // try now, skip settle
Result<void> Executor::skip();   // only a row that has not started
```

- [ ] Failing tests (`VirtualTimeTest`, `Crew`), each asserting the events published and the queue's end:
  - `ExecutorHolding.ALinkDownBeforeARunHoldsThenStartsThatRunOnce`: down 5 min from before row 2: row 2 starts once, its aliquot is allocated once, the queue completes.
  - `...WaitsForTheLinkToBeStableForSettle`: up at 60 s, down again at 70 s, up at 100 s: the run starts at 130 s.
  - `...ProbesEachLinkAtTheProbeInterval`.
  - `...ARunThatFailedForALinkHoldsThenGoesOnToTheNextRow`: the failed row is not run again.
  - `...AfterLinkFailureEndEndsTheQueue`.
  - `...TheHoldLimitEndsTheQueueAsFailedNamingTheLinks` (2 h).
  - `...ResumeSkipsSettle`; `...SkipDropsTheHeldRow`; `...SkipIsRefusedForARowThatStarted`.
  - `...StopCancelAndAbortLeaveAHold`.
  - `...InOverlapTheRunInFlightCarriesOn`.
  - `...AnUnsafeRunEndsTheQueueWithoutHolding` (`code = "unsafe"`, Task 11).
  - `...ADefiniteFailureEndsTheQueueAsToday`, and with `continue_on_failure` goes on as today.
  - `...ALinkThatDropsDuringPrepareHoldsAndStartsTheRowAgain` (spec 4.4, first row): `RunResult` gains `started` (false when the run failed before its point of no return for a transient error); the executor then does not consume the row: `next_row` in the state file still names it, its aliquot is given back or allocated once, and it runs once after the hold. A definite failure in Prepare ends the queue as today.
  - `...AFailedRunIsFlaggedInItsSummary`: `RunSummary` gains `status` (the record's `RecordStatus`) and `failure`; `truncated` stays for its readers.
  - `...TheStateFileCarriesTheHoldAndTheLastFailedRow`: round trip; a file written before this task still gives its `resume_row`; a file with the new `holding` object gives the same `resume_row` as one without.
  - `...AQueueNeverEndsForATransientFailureWithoutHavingHeld` (spec section 5).
- [ ] Implement. The hold's wait is a `clock.wait_until` on the executor's condition variable, woken through the clock by a `TransportHealth` subscription, `resume()`, `skip()` and the stop family. `write_state` writes with a JSON library if the file is still hand-built, and `resume_row` reads `next_row` as before.
- [ ] Safe state is applied on entering a hold when a run failed past no return (Task 11 did it at the run's end; here assert it is not skipped and applying twice is harmless).
- [ ] TSan run of `ExecutorHolding.*`.
- [ ] Commit: `feat(experiment): the executor holds the queue while a needed link is down`.

### Task 18: Every cut, across the whole queue

**Files:** `tests/integration/test_outage.cpp`, `tests/integration/CMakeLists.txt`.

- [ ] `Outage.EveryCut`: the example sim lab on a `VirtualClock`, a queue of air, blank and unknown. Run once without a fault and keep the records. Then for each required transport and each instant in a list (every phase boundary of every run, and 20 instants from a fixed seed), `down_for(5s)` at that instant: the queue's end and every record's series equal the fault-free ones; only notes and counters differ.
- [ ] `Outage.TenMinutes`: same instants, `down_for(600s)`: asserts the rules of spec section 5 (no `Unsafe` command seen twice by the `SimTransport`; no run past no return started twice; every run that measured anything has a record in the spool; the queue held before it ended), not equality.
- [ ] `Outage.TheSameSeedGivesTheSameResult`: two runs of one case compare equal.
- [ ] This is the test that finds what Tasks 1 to 17 missed: a failure here is fixed in the layer that caused it, with a unit test there, before this commit.
- [ ] Give the target a ctest `TIMEOUT` above the fixture's dead-man (AGENTS.md, "Tests"); report how long it takes, and if it is over a minute put it with the importer suites that `dev-quick` skips.
- [ ] Commit: `test(integration): an outage at any instant of a queue changes nothing or follows the rules`.

---

## Phase 7: Services (spec 4.8)

### Task 19: The spool is flushed while the queue runs

**Files:** `libs/experiment/src/persist/persister.cpp` (`SavePipeline`, `:164-212`), `include/pychron/experiment/persist/persister.hpp`, `src/executor/executor.cpp`, `tests/experiment/test_persist.cpp`, `tests/experiment/failing_store.hpp`.

- [ ] Failing tests (`VirtualTimeTest`): `flush()` runs after every run; while `pending > 0` it runs every 5 min, backing off (doubling) to 30 min while it keeps failing and returning to 5 min after a success; `SpoolStatus{pending, last_error}` is published on each change; a persister that fails for an hour holds no run (the queue's timing equals the fault-free one); with nothing pending no job is scheduled.
- [ ] Implement as a scheduler job, not a thread. The aliquot rule of spec 4.8 is a constraint on a later store-backed allocator: add it to `libs/experiment/AGENTS.md`, build nothing.
- [ ] TSan run.
- [ ] Commit: `feat(experiment): the spool is flushed after each run and while anything is pending`.

### Task 20: Notifications wait on disk

**Files:** `libs/experiment/src/lab/notifier.cpp`, `include/pychron/experiment/lab/notifier.hpp`, `src/lab/notifications.cpp`, `include/.../lab/notifications.hpp` (`NotifyEvent`, `:51`), `tests/experiment/test_notifications.cpp`, `docs/notifications.md`, `configs/examples/notifications.toml`.

- [ ] Failing tests, with the injected runner the tests already use in place of `curl`: a notification is a file in `<data>/outbox/` before delivery is tried and is removed on success; a new `Notifier` on the same directory delivers what the last one left, in order per channel; retry at 1, 2, 4 ... 60 min; given up after 24 h with one log line and the file removed; the `command` channel is tried first and does not wait for a failing email channel; `queue_holding`, `queue_resumed` and `unsafe` are sent for `ExecutorHolding`, the return to `Running`, and an `unsafe` queue end; each can be switched per channel as `run_failed` is.
- [ ] The notifier's thread and its real-time `curl` timeout stay as they are (virtual clock spec, 4.5), and so does the retry schedule: the thread is no participant of the clock, so it may not wait in a `VirtualClock`. Add "notification retry" to that spec's list. The schedule is a pure function (`next_attempt(first_tried, attempts)`), and delivery of what is due is one step, `deliver_due(now)`, which the thread calls with real time and the tests call with times they choose: no test sleeps.
- [ ] No secret is written to the outbox: a file holds the event and its text, and the channel's credentials are read from `notifications.toml` at delivery. Test that an outbox file has no key of the config in it.
- [ ] Commit: `feat(experiment): a notification that cannot be sent waits on disk and is sent later`.

---

## Phase 8: Seen and counted (spec 4.9)

### Task 21: Metrics

**Files:** `libs/metrics/src/core_exporter.cpp` (`:127-205`), `libs/experiment/src/metrics/experiment_metrics.cpp` (`:25-59`, `:112-170`), the dashboards under `packaging/` or `docs/` that `MetricsPackaging` reads, `tests/experiment/test_experiment_metrics.cpp`, `tests/integration/test_metrics_packaging.cpp`, `docs/observability.md`.

Read `libs/metrics/AGENTS.md` first: a label is a configured name, a counter is born at zero, every metric has a panel.

- [ ] `pychron_link_reconnects_total{transport}` and `pychron_link_repeats_total{transport}` (core exporter; both read at scrape from `Link::reconnects()` and `Link::repeats()` through the accessor Task 16 gives the line, so each is there at zero for every configured transport; no `LinkRepeated` event); `pychron_executor_holds_total{reason}` (reason an enumeration: `link`, `spool`, `run_failed`), `pychron_executor_holding`, `pychron_measurement_gaps_total`, `pychron_spool_pending`, `pychron_notifications_outbox` (experiment metrics).
- [ ] Tests: each counter exists at zero for every configured transport or reason before any event; each moves on its event; `MetricsPackaging` finds a panel for each.
- [ ] Commit: `feat(metrics): link recoveries, holds, gaps and backlogs are counted`.

### Task 22: The operator sees a hold and can end it

**Files:** `apps/elctl/src/exp.cpp` (`:373-414`), `apps/pychron-ui/src/core_bridge.*`, `health_bar.*`, `experiment_window.cpp` (`:276-287`), `executor_pane.cpp`, `tests/ui/test_experiment_window.cpp`, `tests/elctl` if `exp run`'s output is tested there.

- [ ] `elctl exp run` prints `holding: <reason> (<links>) until <time>`, `resumed`, `gap in <block>: <n> s`, `reconnected <transport> after <n> s`.
- [ ] UI: the health bar shows the hold's reason and a countdown to `hold_limit` read from `CoreBridge::now()` (never `QDateTime::currentDateTime()`); Experiment > Executor gains Resume and Skip Held Run, enabled only while holding (Skip only when the held row has not started); a run row shows `(failed, saved)` beside today's `(truncated)`.
- [ ] Tests in `tests/ui/test_experiment_window.cpp` on a `VirtualClock`: the actions' enabled state through a hold; Resume ends it; the countdown text at two instants.
- [ ] Build and test with the `dev-ui` preset; `python3 tools/compiler_sweep.py --build-dir build/dev-ui`.
- [ ] Commit: `feat(ui): a held queue is shown with its reason, and can be resumed or skipped`.

### Task 23: Docs, rules, and the whole branch under the sanitizers

**Files:** `docs/user/04-experiments.md`, `docs/notifications.md`, `docs/observability.md`, `docs/simulator.md`, root `AGENTS.md` (table row for `libs/devices`), `libs/experiment/AGENTS.md`, `libs/devices/AGENTS.md`.

- [ ] User guide: what happens when a link drops, by phase (the table of spec 4.4 in the operator's words), the hold and its limit, `executor.toml`, `safe_state.py`, what a `failed` record is and that such a run is never rerun.
- [ ] `libs/experiment/AGENTS.md`: the point of no return; safe state is idempotent and every step is attempted; a new wait in the executor is a clock wait; the store-backed aliquot constraint.
- [ ] Full `cmake --build --preset dev-ui` and `ctest --preset dev-ui`; the same tree with `-DPYCHRON_SANITIZE=thread` and with `-DPYCHRON_SANITIZE=address,undefined`; `python3 tools/quality_check.py`; `python3 tools/compiler_sweep.py` on `build/dev` and `build/dev-ui`. Report the counts.
- [ ] The spec's status line: implemented, with what is deferred (the real-socket test against `WireServer`).
- [ ] Commit: `docs: what a queue does when a link drops`.

## Deferred

- The real-socket test that kills `WireServer`'s listener: written when sim serve lands (spec section 6).
- Store-backed aliquot allocation offline: a constraint recorded in Task 19, built with the database persister.
- Keepalive on Windows and Linux is compiled only by CI: the first pull request into `main` after Task 3 is its test.

## Decided by the owner, 2026-10-13

1. The per-transport table is `recovery` (Task 7): `link` is taken.
2. Every truncated run is written `status = truncated`, whoever cut it short: the operator, a conditional or a link (Tasks 10 and 13). The record of such a run gains a key, and so a sha other than it would have had; a complete record keeps its bytes. Records truncated before Task 10 carry no key and read as `complete`.
3. A `failed` record is ingested and tagged `invalid` (Task 9).
