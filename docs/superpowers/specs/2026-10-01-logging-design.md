# Robust logging

Date: 2026-10-01
Status: Draft
Owner: Jake Ross
Depends on: `2026-09-29-instrument-control-design.md` (Qt-free core, SignalBus,
TraceRecorder), `2026-09-30-implementation-priorities.md` (item 4).

## 1. Goal

Make a lab machine diagnosable after the fact and while it runs:

- Persistent, rotated log files.
- Per-subsystem verbosity from config (`*.wire=trace`) and at
  runtime.
- Wire bytes visible in the debug log.
- No lost tail on a crash or fatal error.
- A live, filterable log view in the UI.

## 2. Decisions

| Decision | Choice | Reason |
|---|---|---|
| Backend | spdlog behind the existing `Logger` API | Rotating sinks, async writer and flush control come for free. Callers keep clock injection and bus publishing. |
| Core stays Qt-free | Only `apps/pychron-ui` touches Qt | Instrument-control design section 10. |
| `TraceRecorder` format | Unchanged | `SimTransport::replay()` reads it. Logging mirrors the bytes, it does not replace the trace file. |
| `Log` bus event | Unchanged shape | The UI and tests depend on it. The dock subscribes to the bus, not to files. |
| Scope | Core plus UI log dock | Chosen by owner. |

Non-goals: structured/JSON logs, remote log shipping, log-based alarms,
reading rotated files back into the UI.

## 3. Current state

- `Logger` (`libs/core/src/logger.cpp`) filters by an atomic level, publishes
  `Log` events on the `SignalBus` and echoes to one `std::ostream`.
  Nothing outside `tests/core/test_logger.cpp` constructs one yet, so the
  interface can change with almost no ripple.
- spdlog is listed in `vcpkg.json` but is not in
  `cmake/PychronDependencies.cmake`; the `dev` preset has no vcpkg.
- `TraceRecorder` writes tx/rx/err records to its own sink.
- `LogDock` (`apps/pychron-ui/src/log_dock.*`) is a read-only
  `QPlainTextEdit` capped at 2000 lines, fed by `CoreBridge::logLine`. It
  stamps lines with wall-clock time at append and ignores `Log::ts`.

## 4. Core design (`libs/core`)

### 4.1 `LogHub`

New `pychron/core/log_hub.hpp`. Owns the spdlog sinks and thread pool. No
spdlog type appears in any public header. Created once at start-up from a
`LoggingConfig`; `Logger` instances get a handle from it.

```cpp
struct LoggingConfig {
  std::filesystem::path dir;            // empty = no file sink
  std::size_t max_size_mb = 10;
  std::size_t max_files = 5;
  LogLevel default_level = LogLevel::Info;
  std::vector<std::pair<std::string, LogLevel>> levels;  // glob -> level
  bool echo_stderr = false;
};

class LogHub {
 public:
  static Result<std::shared_ptr<LogHub>> create(LoggingConfig, const Clock&, SignalBus*);
  Logger logger(std::string name);
  void set_level(std::string_view pattern, LogLevel);   // runtime, thread-safe
  void flush();
  void install_crash_handlers();
  ~LogHub();   // flushes and stops the writer thread
};
```

`Logger` keeps its current public API (`log`, `trace`..`error`, `child`,
`set_level`, `enabled`). Its constructors that take a `Clock` and optional
bus keep working, backed by a private `LogHub`, so existing code and tests
compile unchanged. `LogHub::logger()` is the production path.

### 4.2 Sinks

- **File:** spdlog rotating file sink at `<dir>/pychron.log`, `max_size_mb` per
  file, `max_files` kept. Written asynchronously through spdlog's thread pool
  (queue 8192, overflow policy `block`, so records are never dropped).
- **Flush policy:** `error` and above flush synchronously. A periodic flush
  every 1 s covers the rest. `LogHub::flush()` and the destructor flush
  everything.
- **Stderr echo:** optional, replaces the `std::ostream* echo` for
  hub-created loggers.
- **Bus sink:** a custom sink publishes the existing `Log` event
  (`level`, `logger`, `message`, `ts` from the injected `Clock`).
  It runs on the writer thread, so bus subscribers must be thread-safe, which
  the `SignalBus` already requires.
- **Line format:** `2026-10-01T14:03:22.481Z [warn] transport.serial.ig1: msg`
  (UTC ISO-8601 with milliseconds).

### 4.3 Levels from TOML

```toml
[logging]
dir = "~/pychron/logs"        # optional; omitted = no file sink
max_size_mb = 10
max_files = 5
default_level = "info"
echo_stderr = false

[logging.levels]
"*.wire"             = "trace"   # every transport's bytes ("<name>.wire")
"scheduler"          = "debug"
```

- Keys are glob patterns over the dotted logger name; `*` matches any
  sequence, including dots. A bare name matches that logger and its
  children (`scheduler` matches `scheduler.jobs`).
- The most specific match wins (longest literal prefix; exact beats glob).
  No match uses `default_level`.
- A logger resolves its level when created. `LogHub::set_level` re-resolves
  every existing logger and updates the rule table, so loggers created later
  also see it.
- Parsing lives in `libs/core/src/config/` with the existing
  `config::Diagnostic` reporting: an unknown level string, a non-table
  `[logging]` or an unwritable `dir` is a diagnostic with file and line,
  never an abort. An unwritable `dir` degrades to no file sink and one
  `error` record.

### 4.4 `TraceRecorder` routing

`TraceRecorder` gets an optional `Logger` (constructor argument, default
none). When set it logs each tx/rx/err at `trace` under
`<transport name>.wire`: direction, byte count, hex, and printable ASCII.
The factory attaches this logger to every transport whenever a hub exists,
even with `trace = false` (then without a trace file; nothing is formatted
unless a rule enables `trace` on the logger).
The existing trace sink and record format are untouched, and the logger call
happens inside the same per-transport serialization, so log order matches
wire order. With a null logger, behaviour is identical to today.

### 4.5 Crash and fatal flush

`LogHub::install_crash_handlers()` registers:

- `std::set_terminate`: log the active exception text at `error`, flush,
  then call the previous handler.
- POSIX: `SIGSEGV`, `SIGABRT`, `SIGBUS`, `SIGFPE`, `SIGILL` handlers that
  write one fixed line ("fatal signal N") with `write(2)` to stderr and to a
  file descriptor opened at install time, restore the default action and
  re-raise.
- Windows: `SetUnhandledExceptionFilter` doing the equivalent, then
  returning `EXCEPTION_CONTINUE_SEARCH`.

Handlers are best-effort. Signal handlers must not allocate or take locks,
so they cannot call spdlog's `flush()`. The terminate route is a normal
call context and does flush. The signal route therefore relies on the 1 s
periodic flush, the synchronous flush on `error`, and the fixed final line:
at most about one second of sub-error records can be lost to a hard crash.
The tests in section 8 assert exactly this guarantee and no more.
`elctl` and `pychron-ui` call `install_crash_handlers()` after creating the
hub.

### 4.6 Dependency

`cmake/PychronDependencies.cmake` gains, before the tests block:

```cmake
FetchContent_Declare(spdlog
  URL https://github.com/gabime/spdlog/archive/refs/tags/v1.15.3.tar.gz
  URL_HASH SHA256=15a04e69c222eb6c01094b5c7ff8a249b36bb22788d72519646fb85feb267e67
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  FIND_PACKAGE_ARGS CONFIG)
set(SPDLOG_FMT_EXTERNAL OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(spdlog)
```

`pychron_core` links `spdlog::spdlog` **PRIVATE**; `fmt` is not exposed. The
vcpkg path uses the manifest's existing `spdlog` entry. The `fmt` entry in
`vcpkg.json` stays only if another target needs it; the implementer checks
and removes it otherwise.

## 5. UI design (`apps/pychron-ui`)

### 5.1 Structure

- `LogModel` (`QAbstractTableModel`): ring of 10 000 records
  `{TimePoint ts, LogLevel level, QString logger, QString message, bool
  history}`. Columns: Time, Level, Logger, Message. Uses `Log::ts`.
- `LogFilterProxy` (`QSortFilterProxyModel`): minimum level, logger-prefix
  glob (same matcher as section 4.3, shared from core), message substring.
  Filters hide rows; they never drop records.
- `LogDock`: toolbar plus `QTableView` over the proxy.

`LogDock::append_log(const Log&)` and `append_line(const QString&)` keep their
signatures, so `MainWindow` and `tests/ui/test_docks.cpp` need no changes to
compile. `append_line` parses a leading `LEVEL [logger] ` prefix; anything it
cannot parse is stored as an `info` record from logger `ui`.
`line_count()` and `text()` remain, reflecting visible rows.

### 5.2 Controls

- Minimum-level combo, logger-prefix box, message-substring box.
- **Pause:** freezes the view, keeps buffering, shows a "+N new" badge.
- **Clear:** empties the ring.
- **Autoscroll:** sticks to the bottom unless the user scrolled up.
- **Save:** writes the visible rows to a text file chosen by the user.
- **Set logger level...:** takes a logger pattern and level and calls an
  injected `std::function<void(std::string, LogLevel)>`; `pychron-ui`'s
  `main.cpp` binds it to `LogHub::set_level`. The dock does not depend on
  `LogHub`. With no callback injected the action is hidden.

### 5.3 Start-up history

On launch, if the configured log directory has a `pychron.log`, the dock
reads its last 1000 lines and loads them as `history` records (shown dimmed),
so boot-time errors from before the bus subscription are visible. Rotated
files are not read. A missing directory or file is not an error.

### 5.4 Threading and load

`CoreBridge` already marshals bus events to the GUI thread. The dock buffers
incoming records and inserts them in batches on a 30 Hz timer, so a
trace-level flood cannot stall the UI. When the ring is full the oldest
records are evicted.

## 6. Error handling

- Hub creation never aborts the program: a failed file sink yields a hub with
  bus and stderr sinks only, and a diagnostic.
- Logging calls never throw to callers: spdlog errors are routed to an error
  handler that writes one line to stderr, rate-limited to 1 per 10 s.
- If the writer queue is blocked (disk stall), producers wait, which is
  preferable to dropping instrument records. The 8192 queue and 1 s flush
  bound the memory involved.

## 7. Files

| Path | Change |
|---|---|
| `libs/core/include/pychron/core/log_hub.hpp`, `libs/core/src/log_hub.cpp` | New |
| `libs/core/include/pychron/core/log_match.hpp`, `libs/core/src/log_match.cpp` | New: glob matcher shared with the UI |
| `libs/core/include/pychron/core/config/logging_config.hpp`, `libs/core/src/config/logging_config.cpp` | New: `[logging]` parsing |
| `libs/core/src/logger.cpp`, `logger.hpp` | Backed by `LogHub`; API unchanged |
| `libs/core/CMakeLists.txt`, `cmake/PychronDependencies.cmake`, `vcpkg.json` | spdlog wiring |
| `libs/transport/.../trace_recorder.*` | Optional `Logger`, `<name>.wire` records |
| `apps/elctl/src/main.cpp`, `apps/pychron-ui/src/main.cpp` | Create hub, install crash handlers |
| `apps/pychron-ui/src/log_dock.*`, new `log_model.*`, `log_filter_proxy.*` | Dock rework |
| `configs/examples/*` | Add an example `[logging]` table |

## 8. Testing

Core (`tests/core`):

- Level resolution: exact beats glob, longer prefix beats shorter, bare-name
  children, default fallback, runtime `set_level` re-resolving live loggers.
- Rotation: write past `max_size_mb`, assert `max_files` limit and file names
  (small sizes via config for speed).
- Bus sink: `Log` events carry the injected clock's `ts`.
- Config: bad level string, non-table `[logging]`, unwritable `dir` each
  produce a diagnostic and a working hub.
- Flush: `error` is on disk without calling `flush()`; the destructor flushes
  pending records.
- Terminate path: a death test checks the exception text and queued records
  reach the file. The signal path is tested only for the fixed final line,
  since the handler is best-effort by design.
- Existing `test_logger.cpp` keeps passing unmodified.

Transport (`tests/transport`):

- `TraceRecorder` with a logger emits `<name>.wire` records in wire order;
  with no logger its output is byte-identical to today.

UI (`tests/ui`, headless):

- Filter by level, prefix glob and substring; filters are reversible.
- Pause buffers and releases; ring eviction at 10 000.
- `append_line` prefix parsing and the `ui` fallback.
- The level action calls the injected callback with the entered values.
- History load from a fixture file; missing file is a no-op.
- Existing `test_docks.cpp` cases pass unchanged.

Model and proxy logic is also covered by plain gtest cases that need no
`QApplication`.

## 9. Rollout

1. spdlog dependency, `LogHub`, `LoggingConfig`, level matcher, `Logger`
   backing, with tests.
2. `TraceRecorder` routing.
3. Crash handlers and the `elctl` / `pychron-ui` start-up wiring.
4. UI log dock rework and history load.

Each step builds and passes `ctest --preset dev` and `dev-ui` on its own.
Steps 1-3 touch only core and apps' `main.cpp`; step 4 touches only
`apps/pychron-ui`.
