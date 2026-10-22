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
- `<root>/logging.local.toml`: this computer's override, same merge rules as
  other `*.local.toml` (key present overrides; `levels` table is whole
  list). File > Preferences, Logging page, writes here
  (`write_local_table` in `local_file.hpp`) instead of
  `extraction_line.local.toml`.
- Legacy: `[logging]` in `extraction_line.toml` (and its local file) still
  read. Precedence, lowest first: built-in defaults; legacy `[logging]`
  (shared, then local); `logging.toml`; `logging.local.toml`. When legacy
  table is present and `logging.toml` is too, one `warn` line on logger
  `logging` names legacy file as overridden. Legacy table alone: one `info`
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
2026-10-22T14:03:22.481Z [debug] [executor] run=12345-01A experiment.run: message
```

- `[<thread tag>]`: from thread-local tag, set by RAII
  `pychron::LogContext::Thread tag{"executor"}`; untagged thread shows
  `[t<n>]`, n = small id hub assigns per thread on first record.
- `run=<runid>`: present only while thread-local run scope is set
  (`LogContext::Run scope{runid}`, set by executor around a run, and by
  whatever thread works for that run). Absent otherwise; no empty field.
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

`EventRecorder` (`libs/core`, `event_recorder.hpp`): subscribes to bus given
to it, writes each event as one line on logger `trace.<event>` with event's
fields as `key=value`. Line constructs one on its bus once hub attached;
destroyed before bus.

| Event | Level |
|---|---|
| `ValveChanged`, `SwitchLockChanged`, `LinkReconnected`, `TransportHealth` (on change only), `ActuationFailed` (warn) | debug / as noted |
| `Alarm` | warn |
| Executor: `ExecutorStateChanged`, `RunStarted`, `RunFinished` (with `RunSummary`), `QueueEdited`, `QueueFrontier`, `ExecutorWaiting`, `ExecutorHolding`, `QueueResult` | debug (`QueueResult` and `RunFinished` with failure: info) |

Never logged by tap: `Sample`, `PressureSample`, `TemperatureSample`,
`HeaterSample`, `Snapshot` (high rate), `Log` (loop). Event types added later
must be listed in recorder or in its never-logged list; a test holds the list
of every event type in `events.hpp` and `executor.hpp`, so a new type fails it
until classified.

Executor publishes on `ExecutorContext::services.bus`, which app supplies.
App constructs one `EventRecorder` per bus it creates (line bus; executor's
bus when that is another), each destroyed before its bus.

### 2.2 Explicit records where no event exists

Each at `debug` unless noted. Every one runs inside run scope (1.6) when a
run is active.

- Scripts (`libs/scripting`): script start (name, path, sha) and end (exit
  reason, duration); each host call: name, argument summary (max 200 chars),
  result or error, duration. Logger `script.<name>`. Thread tag
  `script:<n>`.
- Run steps (`libs/experiment`): extraction start/end, equilibration,
  measurement phase boundaries, peak center start/result, baseline
  start/end, each conditional evaluated (`info` when it fires), truncation,
  abort reason (`warn`). Logger `experiment.run`.
- Devices (`libs/devices`): each driver command: device, command, short
  reply (max 200 chars), duration, error. One choke point per driver family;
  plan names them. Raw bytes stay on `*.wire` at trace. Logger
  `device.<name>`. Polling commands (gauge reads, temperature reads driven
  by sampler) at `trace`, not debug: they repeat every second.
- Store and spool (`libs/experiment` persist, `libs/persistence`): record
  write start/end with runid and duration, spool hand-off, spool retry and
  result. Failure: `error`. Logger `store`.
- UI actions (`apps/pychron-ui`): each user command that changes state or
  starts work (start/stop/abort queue, open/close valve, queue edit, save,
  run script, preference change) at `info`, logger `ui.action`. Thread tag
  `ui`.

### 2.3 Thread tags

Every thread pychron starts sets tag first thing (beside its
`Clock::Participant`): `executor`, `script:<n>`, `sampler.<device>`,
`spool`, `scheduler.<n>`, `ui`, `metrics`, `log-writer` (writer itself
never logs). Plan lists threads found.

### 2.4 Error rule

Every `pychron::Error` that reaches user — dialog, banner, Log dock line,
`elctl` stderr, run marked failed, spool hold — is also written at `error`
with error kind, message, context and what was attempted.

- Helper `log_error(const Logger&, const Error&, std::string_view attempted)`
  in `libs/core` (`log_error.hpp`); one line, `attempted: kind: what
  (context)`.
- In `pychron-ui`, error display goes through one function that calls
  helper (plan finds existing display sites and routes them).
- In `elctl`, the place that prints `error: ...` and sets exit status calls
  helper.
- Error not shown to user but swallowed (retry, fallback) logged at `warn`
  once per occurrence.

## Rules that must hold

- Library default: no file unless `dir` given. Apps resolve default dir.
- Logging never stops anything: hub creation failure, unwritable dir, lock
  failure each degrade (stderr, or no file) and say so once; app goes on.
- Writer thread and `LogHub::write` stay on real time (spec
  `2026-10-06-virtual-clock-design.md` 4.4): no record path waits on clock.
- Timestamps from `clock.wall_now()` of session clock.
- No secret in log: credentials, passwords, tokens never in argument
  summaries; script host call summary and device command summary pass
  through redaction of keys named `password`, `token`, `secret`, `key`.
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
  none; every event type is either logged or listed as never logged.
- Sequence (`tests/experiment`, `VirtualTimeTest`): one-run sim queue; file
  holds, in order, queue start, run started, script start, host calls, run
  steps, record written, run finished, queue result; executor lines tagged
  `[executor]`, run lines carry `run=`.
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
