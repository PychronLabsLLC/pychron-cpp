# Global logging, on by default

## Why

Users report problems that only execution sequence or logged error explains.
Today log file exists only when `extraction_line.toml` has `[logging] dir`;
fresh install sets none, so first report has no log. Hub built only by
`ExtractionLine` (and `elctl line`): data-reduction install, or UI whose line
failed to build, logs nothing. Few components write to hub (core, transport
`*.wire`, systems, experiment metrics, some UI); executor sequence, scripts,
spectrometer, store writes, most UI errors never reach file.

Goal: every process of pychron writes verbose log by default, configured once
per install, holding enough to rebuild order of what ran on which thread, and
every error user saw.

Two phases, one branch each, phase 1 lands first.

## Phase 1: process hub, configured per install, on by default

### 1.1 Config file

- New optional `<root>/logging.toml` (install root, beside `.pychron/`).
  Same keys as today's `[logging]` table, at top level, plus `dock_level`
  (1.3). Parsed by same `parse_logging` (`libs/core/src/config/loader.cpp`);
  unknown key is error, as now.
- `logging.toml` or `logging.local.toml` that does not load (bad TOML, unknown
  key, wrong type): apps fall back to built-in defaults (default folder for
  install/OS included), hub made anyway; load error is first `error` record,
  logger `logging`, and one stderr line. `pychron-ui` also shows it once in
  Log dock. Preferences Logging page then shown on built-in defaults (in force
  and beneath), with reason above fields; save never writes `logging.toml`,
  and leaves broken `logging.local.toml` as it is.
- `<root>/logging.local.toml`: this computer's override, same merge rules as
  other `*.local.toml` (key present overrides; `levels` table is whole
  list). File > Preferences, Logging page, writes here
  (`write_local_table` in `local_file.hpp`) instead of
  `extraction_line.local.toml`.
- Legacy: `[logging]` in `extraction_line.toml` (and its local file) still
  read. Precedence, lowest first: built-in defaults; legacy `[logging]`
  (shared, then local); `logging.toml`; `logging.local.toml`. When legacy
  table is present and `logging.toml` and/or `logging.local.toml` are too, one
  `warn` line on logger `logging` names legacy file as overridden, and by
  which of the two. All config notes on logger `logging`. Legacy table alone: one `info`
  line suggesting move. Nothing removed in this spec.
- No profile writes `logging.toml`: absent file means defaults, so every
  install, data-reduction included, logs without one. Documented in user
  guide (`docs/user/01-getting-started.md` table, new "Logs" section in
  `docs/user/10-troubleshooting.md`: where file is, what to send).
- `configs/examples/logging.toml` added; `[logging]` in
  `configs/examples/extraction_line.toml` replaced by comment pointing to it.

### 1.2 Defaults

| Key | Old default | New default |
|---|---|---|
| `dir` | none (no file) | see below |
| `max_size_mb` | 10 | 50 |
| `max_files` | 5 | 10 |
| `default_level` | `info` | `debug` |
| `dock_level` | (new) | `info` |
| `echo_stderr` | false | false |

Default `dir` is resolved by apps, not by `LoggingConfig`:

- Started from install: `<root>/logs`.
- Otherwise: OS log folder. macOS `~/Library/Logs/pychron`; Linux
  `$XDG_STATE_HOME/pychron/logs`, `~/.local/state/pychron/logs` when unset;
  Windows `%LOCALAPPDATA%\pychron\logs`.
- Variable the OS folder needs unset or empty (`HOME`; Windows
  `LOCALAPPDATA`) and `dir` not given: no file, one `warn` record on logger
  `logging` `no log folder: HOME is not set` (variable named per OS), one
  stderr line (left out when `echo_stderr`, which already prints record);
  `pychron-ui` also shows it once in Log dock.
- `dir = ""` written explicitly: no file. Loader must tell "key absent" from
  "empty string": `LoggingConfig::dir` becomes `std::optional<path>`
  (absent = default, empty path = off).

Rule: library default stays off. `LogHub::create` with `dir` unset writes no
file, as now; only `apps/` resolve default dir (one function,
`pychron::default_log_dir(const std::optional<path>& install_root)` in
`libs/core`, testable with injected environment). So tests that build
`ExtractionLine` or hub without config never write to user's log folder.

Old `default_level` default changes from `info` to `debug`: existing configs
without `default_level` get debug in file. Intended.

### 1.3 Dock level

`dock_level`: records below it are not published to bus as `Log` events
(file and stderr echo unaffected). Hub resolves logger level as now
(`default_level`, `levels`); bus publish checks `dock_level` after. Log
dock's own minimum-level control filters what bus gave, as now.
Preferences Logging page gains `dock_level` control.

Live, not fixed at creation: hub holds it atomic,
`LogHub::set_dock_level(LogLevel)` / `dock_level()`; change in force for next
write on any thread; session line says value in force. Preferences Apply sets
it when it changed (as levels). Log dock "Set logger level..." below hub's
dock level lowers dock level to it (and dock's own minimum-level filter when
above), so records asked for reach dock; dock's info record saying level was
set also says panel now shows that level and up.

Log dock history (tail of file loaded at start) takes minimum level: caller
passes hub's dock level; lines below it, and continuation lines of those,
left out; last N lines at level and up loaded (scan goes further back, up to
dock capacity). Save and Copy rows carry thread and run as file line does:
`HH:mm:ss LEVEL [thread] run=<id> [logger] message`, each left out when
empty; dock's text filter matches message, thread, run.

`*.wire` stays `trace` unless raised; 1.2 does not change it.

### 1.4 One hub per process

- `pychron-ui` and `elctl` build hub in `main()` after session clock is
  chosen (real or `VirtualClock`, so stamps follow `clock.wall_now()`) and
  before install, line, store or executor are opened. Crash handlers
  installed as now (`install_crash_handlers` already called first).
- Config hub is built from: install's `logging.toml` / local, plus legacy
  table if line config is readable at that point; if line config is read
  later (UI opens line after window), hub levels are updated with
  `set_levels` and a legacy `dir` differing from hub's is reported (`warn`)
  and not applied: file location fixed for process life.
- `ExtractionLine` receives hub through existing `Options::log_hub`; its own
  `LogHub::create` fallback stays for library users and tests.
- Hub has no bus at creation in apps. New `LogHub::attach_bus(SignalBus&)`
  returns RAII `LogHub::BusAttachment` (movable, not copyable): thread-safe;
  line attaches its bus when built and drops attachment before bus is
  destroyed. Attachment's destructor puts back `create()`'s bus and waits
  (real time) until no write still publishes on its bus. One bus at a time:
  later attach replaces earlier. Same for clock: `attach_clock(const Clock&)`
  → `ClockAttachment` (`elctl exp run` attaches its `VirtualClock`).
  Records written with no bus attached go to file only. Bus gets
  `dock_level` and up only. Existing `create(config, clock, bus)` unchanged.
  `LogHub::file_path()`: file written (`-<pid>` name on fallback), empty =
  none.
- File name per app: `pychron-ui` writes `pychron.log`, `elctl` writes
  `elctl.log`. New `LoggingConfig` field `file_name` (not a TOML key; set by
  app; default `pychron.log` keeps library behaviour).
- Two processes on one file: hub takes advisory exclusive lock on
  `<dir>/<file_name>.lock` (`flock` / `LockFileEx`). Lock held by other
  process: write to `<stem>-<pid>.log` instead, one `info` line saying so.
  Lock released at hub destruction.
- UI Installations switch (process restarts on other install) needs nothing:
  new process, new hub.

### 1.5 Capture of output that bypasses hub

- `pychron-ui`: `qInstallMessageHandler` sends Qt messages to logger `qt`
  (`QtDebugMsg` debug, `QtInfoMsg` info, `QtWarningMsg` warn,
  `QtCriticalMsg`/`QtFatalMsg` error, then previous handler). Installed after
  hub exists, removed before hub is destroyed.
- `fprintf(stderr, ...)` and `std::cerr` in `libs/` (non-test) moved to
  loggers where a hub or logger is reachable; ones that run before any hub
  exists (argument errors, `VirtualClock` stall report before hub) stay on
  stderr. Stall report: once hub exists, `on_stall` also writes to logger
  `clock` at `warn`.
- `elctl` user-facing stderr output unchanged; error lines also logged per
  2.4.

### 1.6 Line context

File line format becomes:

```
2026-10-23T10:21:20.317Z [debug] [run] run=bu-01 device.actuator1: close ok in 0 ms
```

- `[<thread tag>]`: from thread-local tag, set by RAII
  `pychron::LogContext::Thread tag{"run"}`; untagged thread shows
  `[t<n>]`, n = small id hub assigns per thread on first record.
- `run=<runid>`: present only while thread-local run scope is set
  (`LogContext::Run scope{runid}`, set by executor around a run, and by
  whatever thread works for that run: `run`, `run.post-eq`, `run.phase`).
  Absent otherwise; no empty field. Value is run name
  `format_runid(identifier, aliquot, step)` (`libs/experiment`
  `factory/increments.hpp`, `12345-01A`), `Run::name()`. Aliquot given at
  launch: executor calls `Run::assign_aliquot()` before publishing
  `RunStarted` (allocation error kept, reported by `prepare()`, which fails
  run as before); run executed directly (no executor) gets it in
  `prepare()`. Before an aliquot (allocation failed) name is identifier
  alone. `trace.RunStarted` carries name and uuid
  (`run=<name>, run_id=<uuid>`): map either way.
- One record, one file line: line breaks in message (`\r\n`, `\n`, `\r`)
  written ` / ` in file line and stderr echo, so every line has time, thread,
  `run=`. `Log` bus event (dock) keeps message as given.
- Context captured on calling thread in `LogHub::write` (before queue), so
  writer thread does not need it. `Log` bus event gains `std::string thread`
  and `std::string run` fields; Log dock shows them as two new columns,
  visible by default, hideable like the others.
- `LogContext` in `libs/core` (`log_context.hpp`), no hub dependency.

### 1.7 Session marker

First record of each hub, logger `pychron`, `info`: app name, version
(`version.txt` value compiled in), OS, install root (or "none"), profiles
from `.pychron/install.toml`, sim or real, clock kind and speed, log level
rules in force. Last record at clean exit: `session end`.

## Phase 2: execution sequence and errors

### 2.1 Bus tap

Three recorders, one per library that publishes events, each writing an
event as one line on logger `trace.<event>`, event's fields as `key=value`:
`EventRecorder` (`libs/core`, `event_recorder.hpp`), `systems::SystemsEventRecorder`
(`libs/systems`, `event_log.hpp`), `experiment::ExperimentEventRecorder`
(`libs/experiment`, `event_log.hpp`). Each library classifies every event
type it publishes in `core_event_classes()` / `systems_event_classes()` /
`experiment_event_classes()`: logged at a level, or never logged. A test holds
every event type, so new type fails it until classified.

| Recorder | Event | Level |
|---|---|---|
| core | `ValveChanged`, `SwitchLockChanged`, `LinkReconnected`, `TransportHealth` (on change of connected only) | debug |
| core | `ActuationFailed`, `Alarm` | warn |
| systems | `JobStarted`, `JobFinished`, `PeakCenterTryStarted`, `PeakCenterDone`, `DetectorState`, `FieldTableChanged` | debug |
| systems | `ScanStatus` | debug; warn when it carries error |
| experiment | `ExecutorStateChanged`, `RunStarted`, `QueueEdited`, `QueueFrontier`, `ExecutorWaiting`, `ExecutorHolding`, `RunStateChanged`, `BlockStarted`, `BlockFinished`, `NotificationSent` | debug |
| experiment | `RunFinished` (with `RunSummary`) | debug; info when run did not succeed or was not saved |
| experiment | `SpoolStatus` | debug; warn when record could not be sent |
| experiment | `QueueStarted`, `QueueEnded` (carries queue result: `end=`, `reason=`, `runs=`), `ConditionalTripped`, `RunNote` | info |
| experiment | `MeasurementGap` | info; warn when readings did not resume |

`QueueResult` is not bus event: logged inside `QueueEnded`, info.

Never logged. core: `Sample`, `PressureSample`, `TemperatureSample`,
`HeaterSample`, `Snapshot` (every scan), `Log` (loop), `HandlerFailed`.
systems: `JobProgress`, `PeakCenterPoint`, `IntensityReading`,
`MagnetMoved` (every step). experiment: `CountsProgress`, `OverlapReady`,
`NotificationOutbox`, `SeriesUpdated`, `FitsUpdated` (every reading).

Recorders built by apps, not by line: one of each kind per bus app makes
(line bus; executor's bus when another), each destroyed before its bus.
Executor publishes on `ExecutorContext::services.bus`, which app supplies.

Component that publishes `Log` on bus also writes it to file:
`log_and_publish` (`libs/core` `log_error.hpp`) writes through process logger,
and publishes on own bus only when that bus is not hub's (hub already
publishes it there). Every dock line in file once.

### 2.2 Explicit records where no event exists

Each at `debug` unless noted. Every one runs inside run scope (1.6) when a
run is active. Components take loggers with `process_logger(name)` when
constructed (apps build them after `set_process_log_hub`), never in static
initializer; nothing threaded through `RunServices`, `ExecutorContext`,
`ScriptEnvironment`, `Device`. No process hub (most unit tests): record
dropped at cost of one atomic load.

- Scripts (`libs/scripting`): script start (name, path, sha, kind) and end
  (exit reason, duration; `warn` when script failed); each host call: name,
  argument summary (max 200 chars), result or error, duration
  (`close(name=C) -> ok in 0 ms`). Logger `script.<name>`. No thread tag of
  own: script carries tag of thread it runs on (`run` in a run). Rehearsal:
  each step at `trace` (`step <cmd>(args)`), nothing else in rehearse or
  estimate.
- Run steps (`libs/experiment`): extraction start/end, equilibration,
  measurement phase boundaries, peak center start/result, baseline
  start/end, each conditional evaluated (`info` when it fires), truncation,
  abort reason (`warn`). Logger `experiment.run`; measurement engine
  `experiment.engine`, its dock lines `measurement`.
- Devices (`libs/devices`): each driver call, one line naming device and
  call: `<what> ok in <ms> ms`, `<what> failed: <kind>: <message>`. No
  command text, no reply: bytes stay on `*.wire` at trace. Logger
  `device.<name>`. Polling calls (gauge, temperature reads driven by
  sampler or scheduler) at `trace`. Cancelled call: `debug` (`trace` while
  polling), not warn. While polling only first failure of a streak warns;
  repeats at `trace`. Link reconnect failure: `warn` on
  `device.<transport name>`, one per attempt.
- Store and spool (`libs/experiment` persist, `libs/experiment/adapters/store`):
  logger `store` per save: `StorePersister` `save <runid>: start` /
  `save <runid>: done in <ms> ms`; records folder (`FilePersister`, `elctl
  exp run --data`) `save <run name>: wrote <path> in <ms> ms`. Save failure:
  `error` on each failed attempt (each attempt one occurrence; flush cadence
  5-30 min). Logger `spool`: `put`, `remove`, `hand-off <uuid>: sent`,
  `flush: sent <n>, <m> pending`; hand-off failure `warn` once per record per
  attempt; flush `warn` only for failures not from a hand-off.
- UI actions (`apps/pychron-ui`): each user command that changes state or
  starts work (start/stop/abort queue, open/close valve, queue edit, save,
  run script, preference change) at `info`, logger `ui.action`. Queue start
  logged by experiment window. Thread tag `ui`. UI loggers `ui.<area>`.

Redaction (host call and other argument summaries): value of key equal to
`password`, `token`, `secret` or `key`, or ending `_<one of them>` or
`-<one of them>`, case-insensitive, written `***`.

### 2.3 Thread tags

Every thread pychron starts sets tag first thing (beside its
`Clock::Participant`). Tags as built: `run` (executor run slot),
`run.post-eq` (post-equilibration), `run.phase` (phase thread), `queue`,
`scheduler`, `transport.<name>`, `metrics`, `camera`, `ngx.reader`,
`process.<program>`, `notifier`, `rehearsal`, `store`, `ui`, `laser`,
`laser.video`, `entry`, `update`, `peak-center`, `processing`, `elctl`.
Writer thread never logs. Threads with no body in pychron code (Qt thread
pools, store source pool) stay untagged (`[t<n>]`).

### 2.4 Error rule

Every `pychron::Error` that reaches user — dialog, banner, Log dock line,
`elctl` stderr, run marked failed, spool hold — is also written at `error`
with error kind, message, context and what was attempted.

- Helper `log_error(const Logger&, const Error&, std::string_view attempted)`
  in `libs/core` (`log_error.hpp`); one line, `attempted: kind: what
  (context)`.
- In `pychron-ui`: `show_error` / `show_warning` (`error_report.hpp`) log
  and show; banner or status line that shows error its own way calls
  `log_shown` first.
- In `elctl`: no call at each of ~170 write sites. `main()` tees `std::cerr`
  into log (`elctl::LogTeeBuf`, `apps/elctl/src/log_tee.hpp`, put on and
  taken off by RAII `StreamTee`), logger `elctl`: line starting `error` or
  `elctl: error` at `error`, every other line at `warn`; empty line no
  record. Tee unbuffered: every byte reaches stderr in call that wrote it;
  only unfinished line kept, logged at its newline or when tee goes. Tee
  made after process hub set; `std::cerr` gets its own buffer back before
  hub unset. Hub's stderr echo and writer's error report go to C `stderr`
  (`fwrite`), not `std::cerr`: through tee every echoed record would be
  logged again, and echo would re-enter hub's echo mutex. Line hub writes to
  stderr while tee logs on same thread passed through, not logged.
  `VirtualClock` stall report stays on `clock` (warn) too, so in file twice
  (`clock`, `elctl`).
- Error not shown to user but swallowed (retry, fallback) logged at `warn`
  once per occurrence.

## Rules that must hold

- Library default: no file unless `dir` given. Apps resolve default dir.
- Logging never stops anything: hub creation failure, unwritable dir, lock
  failure, logging file that does not load, no folder each degrade (stderr,
  defaults, or no file) and say so once; app goes on.
- Writer thread and `LogHub::write` stay on real time (spec
  `2026-10-06-virtual-clock-design.md` 4.4): no record path waits on clock.
- Timestamps from `clock.wall_now()` of session clock.
- No secret in log: credentials, passwords, tokens never in argument
  summaries; script host call summary passes through redaction (2.2). Device
  record holds no command text.
- Record that is `error` is on disk when `write` returns (unchanged).
- Debug logging cost: record below logger level costs one atomic load
  (unchanged). Argument summaries built only when `enabled(level)`.

## Testing

- Config (`tests/core`): precedence of four sources; absent vs empty `dir`;
  `dock_level` filters bus not file; `default_log_dir` per OS with injected
  env; unknown key error in `logging.toml`.
- Hub: `attach_bus`/attachment drop under concurrent writes (TSan run before
  landing: adds shared state with writer); lock fallback to `-<pid>` name;
  line format with thread tag and run scope; session marker fields.
- `elctl` (`apps/elctl/tests`): command run with temp HOME and no config
  writes `elctl.log` in OS default dir under temp HOME; with install, in
  `<root>/logs`.
- UI (`tests/ui`): `qWarning` reaches file; error shown in dialog reaches
  file at `error`.
- Recorder: each logged event type gives expected line; sample events give
  none; every event type of each library is either logged or listed as
  never logged.
- Sequence (`apps/elctl/tests`, `ElctlExpTest.ExecutionSequenceIsLogged`):
  example queue, from test's temporary copy of `configs/examples`, on sim at
  `--sim-speed max` under `ProcessLogScope`; file holds, in order,
  `trace.QueueStarted`, `trace.RunStarted`, `script.` `start`, a host call,
  `trace.RunStateChanged` `to=measuring`, `store` save line,
  `trace.RunFinished`, `trace.QueueEnded`; every line run thread wrote
  between `RunStarted` and `RunFinished` on a thread tagged `run*` carries
  `run=` name of `trace.RunStarted` (`bu-01`).
- `elctl` tee (`apps/elctl/tests/test_log_tee.cpp`): error line is `error`
  record, other `warn`; partial line waits for newline, logged when tee
  goes; bytes pass through unchanged and at once; with `echo_stderr` and
  `std::cerr` teed, each record in file once.
- Errors: forced store failure gives `error` line with context; `elctl`
  error exit gives `error` line.
- Redaction: host call with `password=` argument logs `password=***`.

## Out of scope

- Shipping logs off machine; "send log" or "collect logs" button.
- JSON or other machine-readable format; second trace file.
- Log dock features beyond `dock_level` and `thread`/`run` columns.
- Removing legacy `[logging]` table from `extraction_line.toml`.
- Logging inside `libs/ingest`, `libs/dvc` import walk beyond what
  `elctl import` already reports (separate spec if wanted).
- Metrics exporters (`libs/metrics`) unchanged.

## Open questions

None.
