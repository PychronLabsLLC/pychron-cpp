# Executor hardening: unreliable links to instruments and services

Status: draft, not agreed. Nothing implemented.

Date: 2026-10-09. Builds on `2026-09-29-experiment-system-design.md`
(sections 3.1 to 3.3, 8.4), `2026-10-06-virtual-clock-design.md`. Tested
partly through `2026-10-09-sim-serve-design.md` (real sockets).

## 1. Intent

Queue runs unattended for days. Links drop: switch reboots, vendor PC
(Qtegra, Chromium) restarts its server, Wi-Fi serial bridge stalls, mail
server refuses. Today one dropped reply ends the queue and loses what the
run measured. Goal: short outage costs nothing, long outage costs at most the
run in flight and leaves lab safe, operator told either way, nothing measured
lost.

Not goal: surviving a crash or power loss of pychron itself beyond today's
`--resume`.

## 2. What happens today

Read from code, 2026-10-09.

- `QueuedTransport`: `retries` default 0, no example or profile config sets
  it. Retry resends `tx` blind after `Timeout` or `Io`, whatever the command.
  No reopen: after peer closes, `open` flag stays true, every call fails `Io`
  until someone calls `close()` then `open()`.
- Reconnect exists in two drivers only: Qtegra (`Reconnector`,
  `libs/devices` `reconnect.hpp`: reopen, connect step, retry once, rate
  limited) and NGX (`NgxLink` reader: backoff `reconnect_min` to
  `reconnect_max`). Valve, gauge, heater, cryostat and laser (`chromium`)
  drivers never reconnect.
- `Run::execute`: failure in Prepare, Extract or Measure -> `Failed`,
  `save()` not called: series already collected are dropped. Only
  PostMeasure failure still saves.
- `MeasurementEngine`: no reading within `integration x timeout_factor (3.0)
  + timeout_slack (5 s)` -> `Failed`.
- Executor: `Failed` run ends queue (`QueueEnd::Failed`) unless save error
  only or `continue_on_failure`, which no app sets. `IPreRunCheck` failure
  ends queue as `Cancelled`. No implementation of `IPreRunCheck` exists.
  No state in which executor waits for a link.
- After `Failed`: extraction device ended and disabled (`end_extraction`),
  engine tries inlet close and detector unprotect once each, logs on failure.
  Line otherwise left as script left it.
- `SavePipeline`: spool first, then persister. `flush()` called once, at
  queue start (`recover()`). Persister is `FilePersister` (local).
- `Notifier`: own thread, one delivery at a time through `curl`. One attempt.
- `GaugeScanner`: failed read logged once, flag cleared on next good read.
- `executor_state.json`: `next_row`; row consumed when its run starts.
- `SimTransport` faults: `drop_next`, `delay_next`, `garble_next`,
  `fail_open_next`. No "link down for a while".

## 3. Model

Three kinds of failure, by `ErrorKind`:

| Kind | Members | Treatment |
|---|---|---|
| transient | `Timeout`, `Io`, `NotConnected` | recover: reconnect, retry, hold |
| definite | `Protocol`, `Config`, `Interlock` | never retried; as today |
| control | `Cancelled` | as today |

Four layers, each recovers what it can and passes rest up:

```
transport   reopen on a dead connection                      (4.1)
device      reconnect + retry of calls safe to repeat        (4.2)
run         ride out a gap; fail the run but save and make safe   (4.4, 4.5)
executor    hold the queue until links return or a limit passes   (4.6)
```

Services off the control path (store, notifications, mirrors) never hold a
run: section 4.8.

## 4. Design

### 4.1 Transport

- `Io` from `do_write` / `do_read` marks channel closed (`do_close()`,
  `open = false`, health `Down`): next call fails `NotConnected` at once
  instead of timing out on dead socket.
- Blind retry removed from `write()`; `exchange()` retries only when call
  says it may (4.2). `TransportOptions::retries` stays, meaning unchanged for
  such calls.
- TCP keepalive on (`SO_KEEPALIVE`; idle 10 s, interval 5 s, count 3 where
  platform allows): half-open connection found between commands, not at next
  one. Live timeout: on real time, added to virtual clock spec section 4.4
  list.
- `SimTransport::set_down(bool)` and `down_for(Duration)` (transport's
  clock): while down, calls fail `Io`, `open()` fails `Io`.

### 4.2 Device: one reconnect policy for every driver

```cpp
enum class Repeat { Safe, Unsafe };   // may this call be sent twice?

struct LinkPolicy {                   // per transport; [transports.<name>.link]
  bool reconnect = true;
  Duration backoff_min = 1s, backoff_max = 30s;   // doubling
  Duration call_window = 20s;         // how long one call keeps trying
};
```

- `Reconnector` generalised and moved under `Device` base (`device.hpp`):
  every driver's wire call goes through `Device::call(op, Repeat)`. Qtegra
  keeps its connect step through same path; `NgxLink` keeps own reader loop,
  takes its backoff from `LinkPolicy`.
- `Repeat::Safe`: on transient error, reconnect (backoff, connect step) and
  repeat until success or `call_window` passes; then return last error.
- `Repeat::Unsafe`: reconnect, do not repeat; return error with
  `code = "outcome_unknown"`. Caller resolves by reading back.
- Each driver marks each command. Rule: command that sets absolute target
  (valve to open, setpoint to 450, stage to hole 7, read anything) is `Safe`;
  command whose second arrival does more (toggle, step, `StartAcq`, fire for
  duration, relative move) is `Unsafe`. Table per driver in plan; driver
  header carries it.
- `SwitchManager::actuate`: `outcome_unknown` from actuator -> read state
  back (already there when `verify`); matches target: success; else repeat
  once; `verify = false` valve: repeat (absolute target). State stays
  `Unknown` on bus until resolved, as today.
- Known `Reconnector` window (generation not tied to connection) closed:
  generation taken under transport transaction.
- Every reconnect publishes `LinkReconnected{transport, attempts, down_for}`;
  every call that needed repeat adds to counter (4.9).

Waits here are clock waits (`clock.wait*`), mutex held across them is
`ClockMutex`: Time rules of AGENTS.md, no exception.

### 4.3 Link roles

Hold and safe-state need to know which links a run needs. Derived, not
configured: for run `r`, required links = transports under (a) every valve,
switch and gauge its scripts' line serves (whole line: scripts are code, not
analysed), (b) its extraction device, (c) spectrometer, when run has plan.
`LabSession` builds `LinkSet` per run from hardware it already holds.
`optional = true` on a transport (`[transports.<name>.link]`) takes it out of
(a): camera, cryostat readout, monitoring gauges.

### 4.4 Run phases

Point of no return: start of Extract phase (first line of extraction script).
Before it, nothing of sample is spent.

| Where transient failure outlasts 4.2 | Result |
|---|---|
| Prepare, or waiting for a resource | run not started; row not consumed; executor holds (4.6), then starts run again |
| Extract (script host call) | script error -> run `Failed`; record saved (4.5); safe state (4.7); executor holds before next row |
| Equilibrate, Measure: spectrometer silent | gap rule below |
| Measure: valve call (inlet close, detector unprotect) | `Safe` calls with `call_window = critical_window` (default 10 min): keeps trying; alarm at 20 s; then as Extract row |
| PostMeasure script | as today: note, still saves; plus safe state |
| Save | as today: spool; never blocks |

Gap rule (`MeasurementEngine`): reading timeout no longer ends block. Engine
waits up to `max_gap` (default 120 s, agreed) for readings to resume, link
layer reconnecting meanwhile. Nothing else is done while it waits: a
spectrometer link that drops moves no valve, protects no detector and starts
no safe state by itself. Resumed: `MeasurementGap{block, from, to}` recorded
(event in record, `RunNote`), block continues to its planned end time, not
planned count: time zero and block end do not move. Not resumed, or
`Unsafe` acquisition start lost (`outcome_unknown`): acquisition restarted
once if link is back, else block ends; run is `Failed` if main block has
fewer than `min_counts` (default 20% of planned, at least fit's minimum),
else `Success` with `truncated = true`, reason `link`. Conditionals are not
evaluated across a gap: first evaluation after it uses data after it only
for window functions that need contiguous points (`slope`, `mean` over
window); plan lists functions.

### 4.5 Nothing measured is dropped

`Failed` run that passed point of no return calls `save()`: record with
whatever exists (extraction actuals, series, notes, gaps), through spool, as
`b.draft()` path already allows. Record carries `status`: `complete`,
`truncated`, `failed`, and `failure` text. Importers and reduction skip
`failed` unless asked; schema change to `record/types.hpp` and
`serialize.cpp`, version bumped; readers of old records read missing
`status` as `complete` or `truncated` from existing flag.

Aliquot of such run is spent, never reused.

### 4.6 Executor: Holding

New state.

```
Running -> Holding -> Running
Holding -> StoppingAtBoundary | Cancelling | Aborting | Finalizing
```

Enters when: required link of next run is `Down` at pre-run check; run
ended `Failed` with transient error; run not started for transient error
(4.4 row 1). In overlap, run still in flight carries on; only launch of next
is held.

While holding: publishes `ExecutorHolding{reason, links, since, until}`;
waits (clock) for every required link `Connected` and stable for
`settle` (default 30 s); reconnect attempts come from 4.2's backoff plus one
probe call per link each `probe_interval` (default 15 s): driver's connect
step, `Safe`.

Leaves when:
- links back: `Running`; the row that never started is started, or the next
  row, per 4.4.
- `hold_limit` (default 2 h) passes: queue ends `QueueEnd::Failed`, reason
  names links; safe state already applied on entry.
- operator: `resume()` (try now, skip `settle`), `skip()` (drop held row,
  only when it has not started), `stop/cancel/abort` as today.

Rules:
- A run that passed its point of no return is never run again by executor.
- After `Failed` run for transient error, next row runs only if
  `after_link_failure` allows: `"hold_then_continue"` (default), `"end"`.
- A failed run is flagged (`status = failed`, `RunSummary`, notification)
  and never rerun or re-inserted by executor, reference or not. Operator
  adds a row if one is wanted.
- `IPreRunCheck` result gains kind: `Refuse` (ends queue, as today) or
  `Wait` (hold). First implementations: `LinksUp` (4.3), `SpoolWritable`
  (free space >= `min_free_mb`, default 500; backlog <= `max_pending`,
  default 200 records).
- `executor_state.json` gains `holding` object (reason, since, row) and
  `last_failed_row`; `resume_row` unchanged in meaning.
- `continue_on_failure` keeps meaning for definite failures.

### 4.7 Safe state

Run on: `Failed` run past point of no return; entering `Holding` with gas
possibly in line; `hold_limit` ending queue. Not run on a measurement gap
(4.4): only when the run has ended.

1. Built in, always, each `Safe` with `critical_window`: extraction device
   end and disable (as today); inlet valve of plan closed; detectors
   unprotected state restored per plan.
2. Then lab's `safe_state` script when `<lab>/scripts/safe_state.py` exists
   (new `ScriptKind::SafeState`): lab's own isolate-and-pump sequence. Runs
   with line only, no spectrometer acquisition.

Each step that fails after its window: `Alarm{severity = critical}` on bus,
notification `unsafe`, queue ends `Failed` with `code = "unsafe"`; executor
does not hold or continue. Steps after failed one still run.

Safe state is idempotent by construction (absolute targets): applying twice
is allowed and happens.

### 4.8 Services

Rule: no run waits on network service. Each has local durable buffer.

- Persister: `SavePipeline::flush()` also after every run and every 5 min
  while pending > 0 (scheduler job, backoff to 30 min). `pending` and
  `last_error` published (`SpoolStatus`). Applies unchanged when database
  persister and git mirror arrive (experiment spec 8.4): they sit behind
  spool.
- Aliquots: allocator answers from local knowledge (`FilePersister` today).
  When store-backed allocation lands it must answer offline from last known
  highest and reconcile on flush; conflict is flagged, never renumbered
  silently. Stated here as constraint, built there.
- Notifications: outbox on disk (`<data>/outbox/`, one file per
  notification); delivery retried with backoff 1 min doubling to 1 h, given
  up after 24 h with log line; order kept per channel. New events:
  `queue_holding`, `queue_resumed`, `unsafe`. A notification about lost
  network goes out when network returns; `command` channel (local program)
  is how lab gets told meanwhile.
- Metrics endpoint: pull; no effect on runs.

### 4.9 Seen and counted

- Bus: `LinkReconnected`, `ExecutorHolding`, `MeasurementGap`,
  `SpoolStatus`; `TransportHealth` as now.
- UI: health bar shows hold with reason and countdown to `hold_limit`;
  Resume and Skip in Experiment > Executor. `elctl exp run` prints same
  lines; `SIGUSR1` not used: operator stops and `--resume`s.
- Metrics (`libs/metrics`, rules of AGENTS.md: label = configured name;
  counter born at zero; panel per metric):
  `pychron_link_reconnects_total{transport}`,
  `pychron_link_repeats_total{transport}`,
  `pychron_executor_holds_total{reason}` (reason: enumeration),
  `pychron_executor_holding` (0/1), `pychron_measurement_gaps_total`,
  `pychron_spool_pending`, `pychron_notifications_outbox`.

### 4.10 Configuration

`<lab>/executor.toml`, optional; defaults above.

```toml
[links]
call_window_s = 20
critical_window_s = 600
backoff_min_s = 1
backoff_max_s = 30

[hold]
settle_s = 30
probe_interval_s = 15
limit_s = 7200
after_link_failure = "hold_then_continue"   # | "end"

[measurement]
max_gap_s = 120
min_counts_fraction = 0.2

[spool]
min_free_mb = 500
max_pending = 200
```

Per transport, in line and spectrometer files:
`[transports.<name>.link]` `reconnect`, `optional`, `call_window_s`.

Loader range-checks every key; example file lists each commented out with
default; test holds example and defaults together (as `sim.toml`).

## 5. Rules that must hold

- A command marked `Unsafe` is never sent twice for one request.
- A run past its point of no return is never started again automatically.
- Whatever a run measured reaches spool before run's `RunFinished`.
- Safe state is attempted in full even when one of its steps fails.
- Every wait added here is a clock wait; every mutex held across one a
  `ClockMutex`. `--sim-speed` queue with injected outages gives same result
  every time.
- With no fault, behaviour and records are as before this spec, except
  `status` field.
- Outage shorter than `call_window` on any one link, at any instant of a
  run, changes nothing in queue's result but notes and counters. (Property
  test, section 6.)
- Executor never ends a queue for a transient failure without having held
  first, except `after_link_failure = "end"`.

## 6. Tests

Simulated time (`VirtualTimeTest`, `Crew`), faults from `set_down` /
`down_for`.

- `tests/transport`: `Io` closes channel; keepalive option set; `set_down`.
- `tests/devices/test_reconnect.cpp`: `Safe` repeats to window; `Unsafe`
  sent once and `outcome_unknown`; two threads, one reconnect; closed window
  case of old `Reconnector`.
- Per driver: its `Repeat` table (each command, one test line).
- `tests/systems`: valve `outcome_unknown` resolved by readback, both ways.
- `tests/experiment/measurement`: gap resumed (block ends on time, gap
  recorded); gap not resumed with enough counts (truncated, reason `link`);
  too few (failed, saved).
- `tests/experiment/run`: failure in each phase -> record in spool with
  `status`; safe state steps in order; step failure -> `unsafe`, later steps
  ran.
- `tests/experiment/executor`: hold and resume before start (unstarted row started once,
  aliquot not spent twice); hold after failed run; `hold_limit`; `resume()`,
  `skip()`; overlap with one run in flight; state file round trip.
- `tests/integration/test_outage.cpp`, `EveryCut`: example sim lab, queue of
  air, blank, unknown; for each link and each of N instants across the queue
  (phase boundaries plus seeded instants), outage of 5 s: queue result equals
  fault-free result, compared by records' series. Same for outage of 10 min:
  asserts section 5 rules, not equality.
- Notifications: outbox survives restart; order; give-up.
- `MetricsPackaging`: new metrics have panels.
- Real sockets: `ExampleLineSteady`-style test killing listener of
  `WireServer` (sim serve spec) once that lands; until then TCP loopback
  server in test.
- TSan run of the above before landing (new waits, shared state).

## 7. Order of work

1. Transport (4.1) and `SimTransport` faults.
2. `Device::call`, `Repeat` tables, valve resolution (4.2).
3. Save on failure, `status` (4.5).
4. Safe state (4.7).
5. Gap rule (4.4).
6. Holding, pre-run checks, `executor.toml` (4.3, 4.6, 4.10).
7. Services (4.8).
8. Events, UI, metrics, dashboards, docs (4.9).

1 to 4 each help alone. Docs: `docs/user` executor chapter, `docs/notifications.md`,
`docs/observability.md`; AGENTS.md entry (`Repeat` marking rule, point of no
return, safe-state idempotence).

## 8. Out of scope

- Crash or power loss of pychron mid-run beyond `--resume`.
- Hardware interlocks, UPS, redundant links.
- Vendor software faults that answer wrongly while connected (`Protocol`).
- Database persister, git mirror, store-backed aliquots: constrained by 4.8,
  built elsewhere.
- Static analysis of scripts for links they use.
- Serial line noise beyond what driver codecs already reject.
- Remote control of a held queue (phone, web).
- Automatic rerun of a failed run.
- Any action on the line or detectors when the spectrometer link drops
  mid-measurement.

Decided 2026-10-09: `max_gap` 120 s; no action on spectrometer link drop;
failed run flagged only, no rerun.

## 9. Open

- Qtegra and Chromium: which commands report completion separately from
  acceptance; decides `Safe`/`Unsafe` for moves and acquisitions. Read from
  drivers in plan.
- `status` field: new record version, or additive field old readers ignore?
  Depends on `serialize.cpp` version rules; check before plan.
