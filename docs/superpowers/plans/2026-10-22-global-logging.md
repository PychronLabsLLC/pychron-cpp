# Global Logging Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Every pychron process writes a verbose log by default, configured once per install, with enough context to rebuild the order of what ran on which thread and every error the user saw.

**Architecture:** Phase 1 turns the existing `LogHub` into a process-wide hub that `pychron-ui` and `elctl` build in `main()`, configured from a new `<root>/logging.toml` and written to `<root>/logs` (or the OS log folder) by default; the line and the clock attach to it later through RAII attachments, and every record carries a thread tag and a run id. Phase 2 adds an `EventRecorder` that writes bus events to the log, explicit debug records where no event exists (scripts, devices, run steps, store, UI actions), and a `log_error` helper at every place an error reaches the user.

**Tech Stack:** C++20, spdlog (behind `LogHub::Impl`), toml++, Qt 6 (UI only), GoogleTest.

**Spec:** `docs/superpowers/specs/2026-10-22-global-logging-design.md`

## Global Constraints

- Phase 1 (Tasks 1–9) lands on `develop` before phase 2 starts. Phase 2 (Tasks 10–17) is its own branch, `feat/execution-log`, cut from `develop` after phase 1 lands.
- Library default stays off: `LogHub::create` with `dir` unset or empty writes no file. Only `apps/` resolve the default folder.
- Defaults: `max_size_mb = 50`, `max_files = 10`, `default_level = debug`, `dock_level = info`, `echo_stderr = false`.
- Default folder: install → `<root>/logs`; else macOS `~/Library/Logs/pychron`, Linux `$XDG_STATE_HOME/pychron/logs` (`~/.local/state/pychron/logs` when unset), Windows `%LOCALAPPDATA%\pychron\logs`.
- File names: `pychron-ui` → `pychron.log`, `elctl` → `elctl.log`; lock held by another process → `<stem>-<pid>.log`.
- Precedence, lowest first: built-in defaults; legacy `[logging]` in `extraction_line.toml`, then its `.local.toml`; `<root>/logging.toml`; `<root>/logging.local.toml`.
- Line format: `2026-10-22T14:03:22.481Z [debug] [executor] run=12345-01A experiment.run: message`; untagged thread `[t<n>]`; no `run=` field outside a run scope.
- Logging never stops anything: every failure degrades (stderr or no file), is said once, and the app goes on.
- `LogHub::write` and the writer stay on real time; no record path waits on a `Clock`.
- Timestamps come from `wall_now()` of the clock attached to the hub (the session clock).
- No secret in a record: argument summaries redact values of keys named `password`, `token`, `secret`, `key`.
- Argument summaries are capped at 200 characters and built only when `logger.enabled(level)`.
- Code that adds shared state with the writer thread (Tasks 4, 5) is run under `-DPYCHRON_SANITIZE=thread` before it lands.
- After each task: `python3 tools/quality_check.py` and `python3 tools/compiler_sweep.py --changed` (add `--build-dir build/dev-ui` for `apps/pychron-ui` / `tests/ui`).

## Review Focus

- A user whose `extraction_line.toml` already sets `[logging] dir` must still find their log where it was when no `logging.toml` exists: the legacy `dir` wins over the default folder. Test added to Task 2 (`LegacyDirBeatsDefaultFolder`).
- `dir = "~/logs"` in `logging.toml` must expand `~` exactly as the legacy table does. Test added to Task 2 (`HomeIsExpandedInLoggingToml`).
- A log folder that cannot be created (read-only install root on a lab machine) must leave the app running with no file and one stderr line, not fall back silently to some other place. Test added to Task 7 (`UnwritableInstallLogsDegrades`).
- Destroying a line while another thread is logging must not publish onto the destroyed bus. Test added to Task 4 (`DetachWaitsForInFlightPublish`).
- A `VirtualClock` at `--sim-speed max` destroyed while the hub lives on must not leave the hub stamping with a dangling clock. Test added to Task 4 (`ClockDetachFallsBackToHubClock`).

---

## Phase 1: process hub, per-install config, on by default

### Task 1: `LoggingConfig` fields and defaults

**Files:**
- Modify: `libs/core/include/pychron/core/config/logging_config.hpp`
- Modify: `libs/core/src/config/loader.cpp:469-510` (`parse_logging`)
- Modify: `libs/core/src/config/local_file.cpp:145` (`logging_override_toml`)
- Modify: `libs/core/src/log_hub.cpp:579-600` (reads `dir`)
- Modify: `apps/pychron-ui/src/logging_page.cpp:122-140`, `apps/pychron-ui/src/main.cpp:665`
- Test: `tests/core/test_config_loader.cpp`, `tests/core/test_local_file.cpp`, `tests/core/test_log_hub.cpp`

**Interfaces:**
- Produces:
  ```cpp
  struct LoggingConfig : Located {
    std::optional<std::filesystem::path> dir;  // nullopt = app default; empty = no file
    std::string file_name = "pychron.log";     // set by the app, not a TOML key
    std::int64_t max_size_mb = 50;
    std::int64_t max_files = 10;
    LogLevel default_level = LogLevel::Debug;
    LogLevel dock_level = LogLevel::Info;
    std::vector<std::pair<std::string, LogLevel>> levels;
    bool echo_stderr = false;
  };
  ```

- [ ] **Step 1: Write failing tests**

  In `test_config_loader.cpp`:
  - `LoggingDefaults`: config with no `[logging]` → `dir == std::nullopt`, `max_size_mb == 50`, `max_files == 10`, `default_level == LogLevel::Debug`, `dock_level == LogLevel::Info`, `file_name == "pychron.log"`.
  - `LoggingEmptyDirMeansOff`: `[logging]\ndir = ""` → `dir.has_value()` and `dir->empty()`.
  - `LoggingDockLevelRead`: `dock_level = "warn"` → `LogLevel::Warn`; `dock_level = "loud"` → diagnostic on field `logging.dock_level`.
  - `LoggingFileNameIsNotAKey`: `file_name = "x.log"` → unknown-key diagnostic.

  In `test_local_file.cpp`: `LoggingOverrideWritesDockLevel`: wanted `dock_level = Warn`, main `Info` → override text contains `dock_level = "warn"` and nothing else.

  In `test_log_hub.cpp`: `FileNameIsUsed`: config `dir = tmp`, `file_name = "elctl.log"` → `tmp/"elctl.log"` exists after a write, `tmp/"pychron.log"` does not. Existing `EmptyDirMeansNoFileSink` also covers `dir = std::nullopt`.

- [ ] **Step 2: Run, expect failures**

  `cmake --build --preset dev --target pychron_core_tests && build/dev/tests/core/pychron_core_tests --gtest_filter='ConfigLoader*.Logging*:LocalFile*.Logging*:LogHub.FileNameIsUsed'` → compile errors / FAIL.

- [ ] **Step 3: Implement**

  Change the struct as above. `parse_logging`: `dir` key present → `l.dir = expand_home(dir)` (an empty string stays an empty path); add `dock_level` to the `Keys` list and read it with `read_enum` like `default_level`. `logging_override_toml`: write `dock_level` when it differs; write `dir` when the optionals differ (`dir = ""` for an empty path). `LogHub::create`: file sink only when `config.dir && !config.dir->empty()`, file `*config.dir / config.file_name`. Update the two UI readers (`logging_page.cpp` shows `dir.value_or({})`; `main.cpp:665` becomes Task 8's `hub->file_path()`, until then `value_or`). Update `configs/examples/extraction_line.toml` comments to the new defaults.

- [ ] **Step 4: Run the filter from Step 2 and the full core binary**, expect PASS.

- [ ] **Step 5: Commit** `feat(core): logging config gains dock level, file name and verbose defaults`

### Task 2: `logging.toml` and its precedence

**Files:**
- Create: `libs/core/include/pychron/core/config/logging_file.hpp`, `libs/core/src/config/logging_file.cpp`
- Modify: `libs/core/src/config/loader.cpp` (expose `parse_logging` through a free function), `libs/core/include/pychron/core/config/loader.hpp`, `libs/core/CMakeLists.txt`
- Create: `configs/examples/logging.toml`; modify `configs/examples/extraction_line.toml` (replace `[logging]` with a comment pointing to it)
- Test: `tests/core/test_logging_file.cpp` (add to `tests/core/CMakeLists.txt`)

**Interfaces:**
- Produces:
  ```cpp
  namespace pychron::config {
  // Applies the top-level keys of a logging.toml text over `into`. Diagnostics
  // name `name`; unknown key is an error, as in [logging].
  Result<void> apply_logging_toml(std::string_view toml, std::string_view name, LoggingConfig& into);

  struct LoggingSources {
    LoggingConfig config;
    std::vector<std::filesystem::path> files;  // read, lowest precedence first
    // One line each, for the hub to log once it exists:
    // legacy overridden → warn, legacy alone → info.
    std::vector<std::pair<LogLevel, std::string>> notes;
  };
  // install_root: <root>/logging.toml and <root>/logging.local.toml.
  // line_config: extraction_line.toml whose [logging] (and local) is the legacy table.
  Result<LoggingSources> load_logging_config(const std::optional<std::filesystem::path>& install_root,
                                             const std::optional<std::filesystem::path>& line_config);
  std::filesystem::path logging_file(const std::filesystem::path& install_root);        // <root>/logging.toml
  std::filesystem::path logging_local_file(const std::filesystem::path& install_root);  // <root>/logging.local.toml
  }
  ```
  A line config that does not load is not an error here: its table is skipped, and a `warn` note says why. A `logging.toml` that does not parse is an error.

- [ ] **Step 1: Write failing tests** in `test_logging_file.cpp` (temp dirs):
  - `NoFilesGivesDefaults`: both nullopt → `config.dir == std::nullopt`, `default_level == Debug`, `notes.empty()`.
  - `PrecedenceLowestFirst`: legacy `max_files = 3`, legacy local `max_files = 4`, `logging.toml` `max_files = 5` → 5; add `logging.local.toml` `max_files = 6` → 6; `files` lists the four in that order.
  - `LegacyDirBeatsDefaultFolder`: only legacy `dir = "<tmp>/old"` → `config.dir == "<tmp>/old"`, one note at `Info` that contains `logging.toml`.
  - `LegacyOverriddenIsWarned`: legacy and `logging.toml` both present → one note at `Warn` naming the legacy file's path.
  - `HomeIsExpandedInLoggingToml`: `dir = "~/logs"` with `HOME` set to a temp dir → `config.dir == <tmp>/logs`.
  - `UnknownKeyInLoggingTomlFails`: `colour = 1` → error whose text names `logging.toml` and `colour`.
  - `LevelsTableReplacesWholeList`: legacy `levels = {"a" = "trace"}`, `logging.toml` `[levels]\n"b" = "warn"` → `levels == {{"b", Warn}}`.

- [ ] **Step 2: Run** `build/dev/tests/core/pychron_core_tests --gtest_filter='LoggingFile.*'` → compile error.

- [ ] **Step 3: Implement.** `apply_logging_toml` parses the text and runs the existing `ConfigBuilder::parse_logging` on the root table (move `parse_logging` where both can call it; no second parser). `load_logging_config` loads the legacy table with `load_system_config` (which already merges its local file), then applies `logging.toml` and `logging.local.toml` when present. Write `configs/examples/logging.toml` with every key commented and its default.

- [ ] **Step 4: Run, expect PASS.**

- [ ] **Step 5: Commit** `feat(core): an install-wide logging.toml over the line's [logging]`

### Task 3: default log folder

**Files:**
- Create: `libs/core/include/pychron/core/log_dir.hpp`, `libs/core/src/log_dir.cpp`; modify `libs/core/CMakeLists.txt`
- Test: `tests/core/test_log_dir.cpp`

**Interfaces:**
- Produces:
  ```cpp
  namespace pychron {
  enum class HostOs { Mac, Linux, Windows };
  HostOs host_os() noexcept;
  using EnvLookup = std::function<std::optional<std::string>(std::string_view)>;
  EnvLookup process_env();  // std::getenv
  // <install_root>/logs, else the OS folder (constraints). Empty path when the
  // OS folder's variable (HOME / LOCALAPPDATA) is unset.
  std::filesystem::path default_log_dir(const std::optional<std::filesystem::path>& install_root,
                                        HostOs os, const EnvLookup& env);
  // Sets config.dir to default_log_dir(...) when it is nullopt; leaves a given dir (empty included) alone.
  void resolve_log_dir(config::LoggingConfig& config, const std::optional<std::filesystem::path>& install_root,
                       HostOs os, const EnvLookup& env);
  }
  ```

- [ ] **Step 1: Write failing tests:** `InstallRootWins` (`/r` → `/r/logs` for every `HostOs`); `MacFolder` (`HOME=/h` → `/h/Library/Logs/pychron`); `LinuxXdg` (`XDG_STATE_HOME=/s` → `/s/pychron/logs`); `LinuxNoXdg` (`HOME=/h` → `/h/.local/state/pychron/logs`); `WindowsLocalAppData` (`LOCALAPPDATA=C:/L` → `C:/L/pychron/logs`); `NoHomeGivesEmpty`; `ResolveKeepsGivenDir` (dir `""` stays `""`, dir `/x` stays `/x`, nullopt becomes the default).
- [ ] **Step 2: Run** `--gtest_filter='LogDir.*'` → compile error.
- [ ] **Step 3: Implement** as the signatures say; `host_os()` from `__APPLE__` / `_WIN32`.
- [ ] **Step 4: Run, expect PASS.**
- [ ] **Step 5: Commit** `feat(core): default log folder per install and OS`

### Task 4: hub attachments, dock level, file lock

**Files:**
- Modify: `libs/core/include/pychron/core/log_hub.hpp`, `libs/core/src/log_hub.cpp`
- Test: `tests/core/test_log_hub.cpp`

**Interfaces:**
- Produces (on `LogHub`):
  ```cpp
  // While the returned object lives, records are published on `bus` (dock_level and up)
  // and stamped by nothing new. Its destructor stops publishing and waits until no
  // write is publishing on `bus`. One bus at a time: attaching a second while one
  // is attached replaces it (the first attachment's destructor then does nothing).
  class BusAttachment;   // movable, not copyable
  [[nodiscard]] BusAttachment attach_bus(SignalBus& bus);
  // While it lives, records are stamped from clock.wall_now() and bus events with clock.now();
  // after, from the clock given to create(). Same wait-on-detach rule.
  class ClockAttachment;
  [[nodiscard]] ClockAttachment attach_clock(const Clock& clock);
  std::filesystem::path file_path() const;  // empty = no file
  ```
  `create(config, clock, bus)` keeps its signature; a non-null `bus` there is attached for the hub's life.

- [ ] **Step 1: Write failing tests:**
  - `DockLevelFiltersBusNotFile`: `default_level = Debug`, `dock_level = Info`; a debug and an info record → file has both, bus subscriber saw only the info.
  - `AttachBusLater`: hub created with no bus, write `a`; `attach_bus(bus)`, write `b`; reset attachment, write `c` → subscriber saw only `b`; file has `a b c`.
  - `DetachWaitsForInFlightPublish`: a bus handler blocks on a latch; thread writes (publish in progress); main thread destroys the attachment in another thread → that thread does not return until the latch is released (checked with an atomic flag, real-time bound 5 s).
  - `ClockDetachFallsBackToHubClock`: hub on `ManualClock` A (wall at T1), attach `ManualClock` B (T2), write → bus event `ts == B.now()`, file stamp T2; drop attachment, write → stamp T1.
  - `SecondProcessGetsPidFile`: two hubs on the same dir and `file_name` in one process (the lock is per open file description, so this exercises the fallback) → second's `file_path()` is `<dir>/pychron-<pid>.log`, and it logs one info line saying the first file is in use.
  - `LockReleasedOnDestruction`: create, destroy, create again → `file_path()` is `pychron.log`.
- [ ] **Step 2: Run** `--gtest_filter='LogHub.*'` → compile error.
- [ ] **Step 3: Implement.** `Impl::bus` and `Impl::clock` become atomics with a publish counter each; an attachment's destructor swaps the pointer back and waits on a `std::condition_variable` (real time, not a `Clock`) until the counter is zero. A handler that logs back from inside a publish increments the counter again; the wait is for zero, so do not hold a lock across `publish`. Lock: `flock(fd, LOCK_EX | LOCK_NB)` on `<dir>/<file_name>.lock` (POSIX), `LockFileEx` with `LOCKFILE_FAIL_IMMEDIATELY` (Windows); held for the hub's life; fallback name `<stem>-<pid><ext>`. Bus publish happens only when `level >= dock_level`.
- [ ] **Step 4: Run `LogHub.*`, then build with `-DPYCHRON_SANITIZE=thread` in a separate build dir and run `LogHub.*` there.** Both PASS.
- [ ] **Step 5: Commit** `feat(core): attach a bus and a clock to a running log hub`

### Task 5: thread tag and run scope on every record

**Files:**
- Create: `libs/core/include/pychron/core/log_context.hpp`, `libs/core/src/log_context.cpp`
- Modify: `libs/core/include/pychron/core/events.hpp` (`Log` gains `std::string thread; std::string run;`), `libs/core/src/log_hub.cpp` (format), `libs/core/src/logger.cpp` (legacy loggers fill the fields too)
- Test: `tests/core/test_log_context.cpp`, `tests/core/test_log_hub.cpp`

**Interfaces:**
- Produces:
  ```cpp
  namespace pychron::LogContext {
  class Thread { public: explicit Thread(std::string tag); ~Thread(); /* non-copyable, non-movable */ };
  class Run { public: explicit Run(std::string run_id); ~Run(); /* nests: inner replaces, dtor restores */ };
  std::string_view thread_tag();  // "" when unset
  std::string_view run_id();      // "" when unset
  std::uint32_t thread_number();  // small id, assigned on first call per thread, from 1
  }
  ```

- [ ] **Step 1: Write failing tests:**
  - `LogContext.ThreadTagScoped`, `LogContext.RunNestsAndRestores`, `LogContext.ThreadNumbersDiffer` (two threads → different numbers).
  - `LogHub.LineCarriesThreadAndRun`: under `Thread t{"executor"}; Run r{"12345-01A"};` a debug record on `experiment.run` → file line matches `^\S+Z \[debug\] \[executor\] run=12345-01A experiment\.run: msg$`; bus `Log` has `thread == "executor"`, `run == "12345-01A"`.
  - `LogHub.UntaggedThreadHasNumberNoRun`: line matches `\[t\d+\] logger: msg` and contains no `run=`.
  - Update existing format tests (`WritesFormattedLineToFile`, `LevelNamesInFile`) to the new format.
- [ ] **Step 2: Run** `--gtest_filter='LogContext.*:LogHub.*'` → FAIL.
- [ ] **Step 3: Implement** with `thread_local` state; capture tag and run id in `LogHub::write` on the calling thread and put them into the record before it is queued.
- [ ] **Step 4: Run, then the thread-sanitizer build as in Task 4.** PASS.
- [ ] **Step 5: Commit** `feat(core): thread tag and run id on every log record`

### Task 6: session marker

**Files:**
- Modify: `libs/core/include/pychron/core/log_hub.hpp`, `libs/core/src/log_hub.cpp`
- Test: `tests/core/test_log_hub.cpp`

**Interfaces:**
- Produces:
  ```cpp
  struct SessionInfo {
    std::string app;                                   // "pychron-ui", "elctl"
    std::string version;                               // pychron::setup::version()
    std::optional<std::filesystem::path> install_root;
    std::string profiles;                              // "" when none
    bool simulated = false;
    std::string clock;                                 // "steady", "virtual x10", "virtual max"
  };
  // Logs one info record on logger `pychron`: every field, the OS, and the level
  // rules in force. Marks the hub so its destructor logs `session end` first.
  void LogHub::begin_session(const SessionInfo& info);
  ```
- [ ] **Step 1: Write failing tests:** `SessionMarkerFields` (line contains `app=elctl`, `version=0.4.0`, `install=none`, `sim=no`, `clock=steady`, `default_level=debug`); `SessionEndOnDestruction` (last line ends `pychron: session end`); `NoSessionNoEndLine`.
- [ ] **Step 2: Run** → FAIL. **Step 3: Implement.** **Step 4: Run** → PASS.
- [ ] **Step 5: Commit** `feat(core): session start and end lines in the log`

### Task 7: `elctl` builds the process hub

**Files:**
- Create: `apps/elctl/src/process_log.hpp`, `apps/elctl/src/process_log.cpp` (add to `apps/elctl/CMakeLists.txt`)
- Create: `libs/core/include/pychron/core/process_log.hpp`, `libs/core/src/process_log.cpp`; modify `libs/core/include/pychron/core/logger.hpp` (`Logger::disabled`)
- Test: `tests/core/test_process_log.cpp` (`ProcessLoggerWithoutHubWritesNothing`, `ScopeRestoresPrevious`)
- Modify: `apps/elctl/src/main.cpp`, `apps/elctl/src/line.cpp:73` (use the process hub when there is one), the place `elctl exp run` makes its `VirtualClock` (attach it with `attach_clock`)
- Modify: `libs/core/src/virtual_clock.cpp` callers' `on_stall` in `elctl` (also write to logger `clock` at warn once a hub exists)
- Test: `apps/elctl/tests/test_process_log.cpp`

**Interfaces:**
- Consumes: `load_logging_config` (Task 2), `resolve_log_dir`, `process_env`, `host_os` (Task 3), `begin_session` (Task 6), `LogHub::create`.
- Produces:
  ```cpp
  namespace elctl {
  // Reads --install / --config / --sim from the global options before the command
  // (as run() does), resolves the install (resolve_install; none is fine), loads the
  // logging config, sets file_name = "elctl.log", resolves the folder, creates the hub,
  // logs the notes and begin_session. Never fails: on error, one stderr line, null hub.
  std::shared_ptr<pychron::LogHub> open_process_log(const std::vector<std::string>& args,
                                                    const pychron::Clock& clock, pychron::HostOs os,
                                                    const pychron::EnvLookup& env, std::ostream& err);
  }
  // libs/core/include/pychron/core/process_log.hpp (new, used by both apps and by phase 2):
  namespace pychron {
  // The hub main() made; null in tests that call run() directly (they must not write to the user's folder).
  std::shared_ptr<LogHub> process_log_hub();
  void set_process_log_hub(std::shared_ptr<LogHub> hub);  // main() and tests; thread-safe
  // A logger on the process hub, or one that writes nothing when there is none.
  Logger process_logger(std::string name);
  // Tests: sets the process hub for the scope, restores the previous one after.
  class ProcessLogScope { public: explicit ProcessLogScope(std::shared_ptr<LogHub> hub); ~ProcessLogScope(); };
  }
  ```
  `main()` calls `open_process_log` with a process-lifetime `SteadyClock` and `process_env()` and passes the result to `set_process_log_hub` before `run()`. `process_logger` needs a `Logger` that writes nothing: add `static Logger Logger::disabled(std::string name)` (no hub, no bus, level above `Error`).

- [ ] **Step 1: Write failing tests** (temp HOME through the injected env; site config pointed at a temp dir the way `elctl_fixture.hpp` does):
  - `NoInstallWritesToOsFolder`: args `{"version"}`, env `HOME=<tmp>`, `HostOs::Mac` → `<tmp>/Library/Logs/pychron/elctl.log` exists and contains `app=elctl`.
  - `InstallWritesToRootLogs`: args `{"--install", "lab"}` with a temp install → `<root>/logs/elctl.log`.
  - `UnwritableInstallLogsDegrades`: install root's `logs` is a regular file → hub non-null, `file_path()` empty, `err` has one line containing `file logging disabled`.
  - `LoggingTomlDirOff`: `<root>/logging.toml` with `dir = ""` → no `logs` folder made.
  - In `test_line_commands.cpp` the existing test at line 154 still passes (legacy `dir` used when no process hub).
- [ ] **Step 2: Run** `cmake --build --preset dev --target pychron_elctl_tests && build/dev/apps/elctl/tests/pychron_elctl_tests --gtest_filter='ProcessLog.*:LineCommands*'` → compile error.
- [ ] **Step 3: Implement.** In `Line::build`, use `process_log_hub()` when set and `attach_bus` to the line's bus, holding the attachment as a member declared after `bus_` so it is destroyed first; else the old `create` path. `exp run` attaches its `VirtualClock` for as long as it lives.
- [ ] **Step 4: Run, expect PASS.**
- [ ] **Step 5: Commit** `feat(elctl): every command logs to elctl.log by default`

### Task 8: `pychron-ui` builds the process hub; the line attaches to it

**Files:**
- Create: `apps/pychron-ui/src/process_log.hpp`, `apps/pychron-ui/src/process_log.cpp`, `apps/pychron-ui/src/qt_log_handler.hpp`, `apps/pychron-ui/src/qt_log_handler.cpp` (add to the app's CMake)
- Modify: `apps/pychron-ui/src/main.cpp` (after the clock is chosen, line ~455; and in `run_data_reduction`), `main.cpp:657-667` (level callback and history from the process hub)
- Modify: `libs/systems/src/extraction_line.cpp:181-195`, `libs/systems/include/pychron/systems/extraction_line.hpp` (an `Options::log_hub` given is attached to `bus_`; attachment member declared after `bus_`)
- Test: `tests/ui/test_process_log.cpp`, `tests/systems/test_extraction_line.cpp` (or the file that tests `Options`)

**Interfaces:**
- Consumes: Tasks 2, 3, 4, 6.
- Produces:
  ```cpp
  namespace pychron::ui {
  // As elctl's, with file_name "pychron.log" and the install's line config as legacy table.
  std::shared_ptr<LogHub> open_process_log(const std::optional<setup::SiteInstall>& install,
                                           const std::optional<std::filesystem::path>& line_config,
                                           const Clock& clock, bool simulated, HostOs os,
                                           const EnvLookup& env);
  // qInstallMessageHandler to logger `qt` (debug/info/warn/error as the spec maps them),
  // then the previous handler. Destructor restores the previous handler.
  class QtLogHandler { public: explicit QtLogHandler(LogHub& hub); ~QtLogHandler(); };
  }
  ```
  The setup wizard and the install chooser run before the hub exists and are not logged (they run before an install is known).

- [ ] **Step 1: Write failing tests:**
  - `ExtractionLine.GivenHubGetsTheLinesBus` (systems): hub created without bus, passed in `Options::log_hub`; a line logger record reaches a subscriber of `line.bus()`; after the line is destroyed the hub still writes without crashing.
  - `UiProcessLog.QtWarningReachesFile`: hub on temp dir, `QtLogHandler` installed, `qWarning("boom")` → file has a line `[warn] ... qt: boom`.
  - `UiProcessLog.DataReductionInstallLogs`: install of kind `data_reduction`, no line config → `<root>/logs/pychron.log` exists.
- [ ] **Step 2: Run** `cmake --build --preset dev-ui --target pychron_ui_tests` and the filter → FAIL.
- [ ] **Step 3: Implement.** In `main.cpp`, create the hub once the clock is known, `set_process_log_hub` it, set `options.log_hub`, install `QtLogHandler`, pass `begin_session`; `run_data_reduction` gets the hub as a parameter. The `on_stall` lambda also writes logger `clock` at warn once the hub exists. `load_history(hub->file_path())` replaces the `dir / "pychron.log"` line.
- [ ] **Step 4: Run** the filters and `ctest --preset dev-ui -R 'UiProcessLog|ExtractionLine'` → PASS.
- [ ] **Step 5: Commit** `feat(ui): pychron-ui logs from start-up, line or no line`

### Task 9: Preferences, Log dock columns, docs

**Files:**
- Modify: `apps/pychron-ui/src/line_settings.hpp/.cpp` (logging from `load_logging_config`; written to `logging.local.toml` when there is an install root, else to the line's local file as now), `apps/pychron-ui/src/logging_page.hpp/.cpp` (`dock_level` control), `apps/pychron-ui/src/log_model.hpp/.cpp` (`ThreadCol`, `RunCol`), `apps/pychron-ui/src/log_dock.cpp` (history parser reads the new format, and the old one)
- Modify: `docs/user/01-getting-started.md:230` table row, `docs/user/02-extraction-line.md:328-345` ("The log file"), `docs/user/10-troubleshooting.md` (new "Logs" section: where the file is per OS and install, what to send), `apps/pychron-ui/AGENTS.md` if it mentions the logging override file
- Test: `tests/ui/test_preferences.cpp`, `tests/ui/test_log_model.cpp`

**Interfaces:**
- Consumes: `load_logging_config`, `logging_local_file` (Task 2); `Log::thread`, `Log::run` (Task 5).

- [ ] **Step 1: Write failing tests:**
  - `Preferences.LoggingSavedToInstallLocalFile`: install root with `logging.toml`; change `dock_level` on the page and save → `<root>/logging.local.toml` holds `dock_level = "warn"`, `extraction_line.local.toml` unchanged.
  - `Preferences.RestoreDefaultsGoesToLoggingToml`: shared value comes from `logging.toml`, not from built-in defaults.
  - `LogModel.ThreadAndRunColumns`: a `Log` with `thread = "executor"`, `run = "1-01"` → column headers `Thread`, `Run`, cell values match.
  - `LogDock.HistoryParsesNewAndOldFormat`: both line shapes load with level, logger and message right.
- [ ] **Step 2: Run** → FAIL. **Step 3: Implement.** **Step 4: Run** → PASS.
- [ ] **Step 5: Full check before landing phase 1:** `cmake --build --preset dev-ui && ctest --preset dev-ui`, `python3 tools/quality_check.py`, `python3 tools/compiler_sweep.py` and `--build-dir build/dev-ui`. All clean.
- [ ] **Step 6: Commit** `feat(ui): logging preferences per install, thread and run in the Log dock`

Phase 1 lands (rebase on `origin/develop`, full run, merge, push, as `AGENTS.md` says).

---

## Phase 2: execution sequence and errors

Branch `feat/execution-log` from `develop` after phase 1 lands. Components reach the log through `process_logger(name)` (Task 7): nothing is threaded through `RunServices`, `ExecutorContext`, `ScriptEnvironment` or `Device`. A component takes its loggers when it is constructed (apps build components after `set_process_log_hub`), never in a static initializer. Without a process hub (most unit tests) every phase 2 record is dropped at the cost of one atomic load.

Departures from the spec, written into it in Task 17: event tables cover the systems and experiment events too; scripts carry the thread tag of the thread they run on (scripts have no thread of their own); a device record names the device and call, not the command text (that is on `*.wire` at trace); `elctl` logs what it writes to stderr through a tee instead of at each of its ~170 write sites.

### Task 10: core helpers for phase 2

**Files:**
- Create: `libs/core/include/pychron/core/log_error.hpp`, `libs/core/src/log_error.cpp`, `libs/core/include/pychron/core/log_summary.hpp`, `libs/core/src/log_summary.cpp`
- Modify: `libs/core/include/pychron/core/log_context.hpp` (`Polling` scope)
- Test: `tests/core/test_log_error.cpp`, `tests/core/test_log_summary.cpp`, `tests/core/test_log_context.cpp`

**Interfaces:**
- Produces:
  ```cpp
  namespace pychron {
  // One error record: "<attempted>: <kind>: <what> (<context>)"; "(...)" left out when no context.
  void log_error(const Logger& logger, const Error& error, std::string_view attempted);
  // Same at warn, for an error that was handled (retry, fallback) and not shown.
  void log_handled(const Logger& logger, const Error& error, std::string_view attempted);
  // `k=v` pairs joined by ", ", values of keys named password, token, secret or key
  // (case-insensitive, also as a suffix: api_key) replaced by ***, cut to 200
  // characters with a trailing "…".
  std::string summarize(std::span<const std::pair<std::string, std::string>> fields);
  std::string truncate_for_log(std::string_view text, std::size_t max = 200);
  }
  namespace pychron::LogContext {
  class Polling { public: Polling(); ~Polling(); };  // nests
  bool polling();
  }
  ```
- [ ] **Step 1: Write failing tests:** `LogError.FormatWithContext` (`open valve A: io: timeout (link ngx)`), `LogError.FormatWithoutContext`, `LogError.HandledIsWarn`; `LogSummary.RedactsSecrets` (`{"user","jake"},{"password","p"},{"api_key","k"}` → `user=jake, password=***, api_key=***`), `LogSummary.CutsAt200` (result size ≤ 200 + length of `…`), `LogContext.PollingNests`.
- [ ] **Step 2: Run** `--gtest_filter='LogError.*:LogSummary.*:LogContext.*'` → FAIL. **Step 3: Implement.** **Step 4: Run** → PASS.
- [ ] **Step 5: Commit** `feat(core): error, summary and polling helpers for the log`

### Task 11: event recorders

**Files:**
- Create: `libs/core/include/pychron/core/event_recorder.hpp`, `libs/core/src/event_recorder.cpp` (core events)
- Create: `libs/systems/include/pychron/systems/event_log.hpp`, `libs/systems/src/event_log.cpp` (job, peak center, acquisition events)
- Create: `libs/experiment/include/pychron/experiment/event_log.hpp`, `libs/experiment/src/event_log.cpp` (executor, run, session, engine, notifier events)
- Modify: `apps/elctl/src/line.cpp`, `apps/elctl/src/exp.cpp`, `apps/pychron-ui/src/main.cpp` (one recorder of each kind per bus the app makes, destroyed before the bus)
- Test: `tests/core/test_event_recorder.cpp`, `tests/systems/test_event_log.cpp`, `tests/experiment/test_event_log.cpp`

**Interfaces:**
- Consumes: `process_logger` (Task 7).
- Produces:
  ```cpp
  namespace pychron {
  // Subscriptions held for its life; each event one record on logger "trace.<EventName>".
  class EventRecorder { public: explicit EventRecorder(SignalBus& bus); /* move-only */ };
  // The names of every core event type, each marked logged or never logged; the test
  // compares this with the list it keeps by hand, so a new event fails until classified.
  std::span<const EventClass> core_event_classes();
  struct EventClass { std::string_view name; std::optional<LogLevel> level; };  // nullopt = never logged
  }
  namespace pychron::systems { class SystemsEventRecorder { /* same shape */ }; std::span<const EventClass> systems_event_classes(); }
  namespace pychron::experiment { class ExperimentEventRecorder { /* same shape */ }; std::span<const EventClass> experiment_event_classes(); }
  ```
  Levels:
  | Event | Level |
  |---|---|
  | `ValveChanged`, `SwitchLockChanged`, `LinkReconnected`, `TransportHealth` (only when its state differs from the last one of that transport) | debug |
  | `ActuationFailed`, `Alarm` | warn |
  | `ExecutorStateChanged`, `RunStarted`, `QueueEdited`, `QueueFrontier`, `ExecutorWaiting`, `ExecutorHolding`, `RunStateChanged`, `BlockStarted`, `BlockFinished`, `NotificationSent`, `JobStarted`, `JobFinished`, `PeakCenterTryStarted`, `PeakCenterDone` | debug |
  | `RunFinished`, `QueueResult` | debug; info when the run or queue failed |
  | `QueueStarted`, `QueueEnded`, `ConditionalTripped`, `RunNote` | info |
  | `Sample`, `PressureSample`, `TemperatureSample`, `HeaterSample`, `Snapshot`, `Log`, `HandlerFailed`, `CountsProgress`, `OverlapReady`, `PeakCenterPoint`, `IntensityReading`, `NotificationOutbox` | never |

  Message: the event's fields as `key=value`, in declaration order, time points as the record's own stamp (not repeated), strings through `truncate_for_log`.
- [ ] **Step 1: Write failing tests** (each lib): `<Lib>EventLog.EveryEventClassified` (hand-kept list of every event struct in that lib's headers equals the names in `*_event_classes()`); one test per logged event (publish → file line `[debug] ... trace.ValveChanged: name=A, open=true` or similar exact text); `NeverLoggedEventsWriteNothing` (publish one of each → file unchanged); `TransportHealthOnlyOnChange` (same state twice → one line).
- [ ] **Step 2: Run** each lib's filter → FAIL. **Step 3: Implement.** **Step 4: Run** → PASS.
- [ ] **Step 5: Commit** `feat(core): bus events written to the log`

### Task 12: thread tags and run scope

**Files (one `LogContext::Thread` first thing in each thread body, beside its `Clock::Participant` where it has one):**
- `libs/core/src/scheduler.cpp:300` (`scheduler`); `libs/transport/src/queued_transport.cpp:208` (`transport.<name>`); `libs/metrics/src/server.cpp:207` (`metrics`); `libs/vision/src/live_feed.cpp:56` (`camera`); `libs/devices/src/spectrometer/ngx_link.cpp:156` (`ngx.reader`); `libs/core/src/process.cpp:361` (`process.<program>`)
- `libs/experiment/src/executor/executor.cpp:366` (`run`, plus `LogContext::Run{run id}`); `libs/experiment/src/lab/session.cpp:236` (`queue`); `libs/experiment/src/run/run.cpp:547` (`run.post-eq`, plus `Run`), `run.cpp:602` (`run.phase`, plus `Run`); `libs/experiment/src/lab/notifier.cpp:75` (`notifier`); `libs/experiment/src/rehearsal/runner.cpp:16` (`rehearsal`); `libs/experiment/adapters/store/src/store_persister.cpp:498` (`store`)
- `apps/pychron-ui/src/main.cpp` (`ui`, main thread), `laser_bridge.cpp:90` (`laser`), `:95` (`laser.video`), `entry_bridge.cpp:16` (`entry`), `update_checker.cpp:106` (`update`), `spectrometer_bridge.cpp:531` (`peak-center`), `processing_bridge` worker (`processing`); `apps/elctl/src/main.cpp` (`elctl`)
- Polling: `libs/systems/src/gauge_scanner.cpp:80` and `libs/systems/src/spectrometer/acquisition.cpp:239` wrap their task body in `LogContext::Polling`.
- Not tagged: the log hub's own threads, `virtual_clock.cpp:26` watchdog, `secret_store_linux.cpp:76` (none log through the hub).
- Test: `tests/experiment/test_executor.cpp` (`RunThreadCarriesRunScope`), `tests/core/test_scheduler.cpp` (`JobsRunTaggedScheduler`)

- [ ] **Step 1: Write failing tests:** `RunThreadCarriesRunScope`: hub on temp dir via `ProcessLogScope`, a one-run queue on fakes whose script logs one record through `process_logger("t")` → its line has `[run] run=<id>`. `JobsRunTaggedScheduler`: a job logs → `[scheduler]`.
- [ ] **Step 2: Run** → FAIL. **Step 3: Implement.** **Step 4: Run, then the TSan build for `Executor*:Scheduler*`** → PASS.
- [ ] **Step 5: Commit** `feat: every pychron thread names itself in the log`

### Task 13: scripts

**Files:**
- Modify: `libs/scripting/src/python/python_host.cpp:184` (`execute`: start and end records), `libs/scripting/src/python/host_state.cpp` (`PYCHRON_HW` at :394 and every host function in :400-545 that does not pass through it: one `HostCallTrace` first thing; `step` at :309 logs at trace)
- Create: `libs/scripting/src/python/host_call_trace.hpp`
- Test: `tests/scripting/test_python_host.cpp` (or the file that runs scripts against fakes)

**Interfaces:**
- Produces (private to scripting):
  ```cpp
  // On construction nothing; on destruction one debug record on `script.<script name>`:
  // "<call>(<summary>) -> <result> in <ms> ms", or "-> error: <what>" at warn.
  class HostCallTrace {
   public:
    HostCallTrace(const Logger& logger, std::string_view call, std::span<const std::pair<std::string, std::string>> args);
    void result(std::string_view text);
    void failed(const Error& error);
  };
  ```
  Script start: `start <name> path=<path> sha=<sha> kind=<kind>`; end: `end <name> <exit reason> in <s> s`. Logger taken in `execute` from `process_logger("script." + name)`; the summary is built only when debug is enabled.
- [ ] **Step 1: Write failing tests:** `ScriptLog.StartCallsEnd` (script `open('A'); sleep(1); close('A')` on fakes → in order `start`, `open(name=A) -> ok`, `sleep(seconds=1)`, `close(name=A) -> ok`, `end ... completed`); `ScriptLog.FailedCallIsWarn`; `ScriptLog.SecretArgumentRedacted` (a host call taking a `password` keyword → `password=***`); `ScriptLog.NoHubNoCost` (no process hub: script runs, nothing written, no crash).
- [ ] **Step 2: Run** → FAIL. **Step 3: Implement.** **Step 4: Run** → PASS.
- [ ] **Step 5: Commit** `feat(scripting): every script and host call in the log`

### Task 14: run steps, store and spool

**Files:**
- Modify: `libs/experiment/src/run/run.cpp` (`run_script` :328 start/end; `make_safe` :389, `keep_trying` :454 at info), `libs/experiment/src/measurement/engine.cpp` (`peak_center` :706 start/result, `equilibrate` :765 start/end, `truncate` :1524 at info with ratio, `handle_trip` :1479 at info; conditional `evaluate` at trace), `libs/experiment/src/persist/persister.cpp` (`SavePipeline::save` :237, `hand_off` :219, `Spool::put` :169, `Spool::remove` :174, `SpoolFlusher::run` :328: retry result, failure through `log_handled`), `libs/experiment/adapters/store/src/store_persister.cpp:130` (`save_analysis` start/end/duration; failure `log_error`)
- Test: `tests/experiment/test_run.cpp`, `tests/experiment/test_persister.cpp` (existing files of these classes)

Loggers: `experiment.run`, `experiment.engine`, `store`, `spool`. State changes are already logged by Task 11 (`RunStateChanged`); do not log them again here.

- [ ] **Step 1: Write failing tests:** `RunLog.ScriptStartEndAroundExtraction`; `EngineLog.PeakCenterStartAndResult`; `EngineLog.TruncateIsInfo`; `PersisterLog.SaveWritesStartEndWithRunId`; `PersisterLog.StoreFailureIsErrorWithContext` (forced failing store → `[error] ... store: save <runid>: io: ...`); `SpoolLog.RetryResult`.
- [ ] **Step 2: Run** → FAIL. **Step 3: Implement.** **Step 4: Run** → PASS.
- [ ] **Step 5: Commit** `feat(experiment): run steps, saves and spool in the log`

### Task 15: devices

**Files:**
- Modify: `libs/devices/include/pychron/devices/device.hpp:56-110`, its `.cpp` (`call` and `relay` record each call), `libs/devices/src/link.cpp` (`reconnect_locked` :182 failure through `log_handled`)
- Test: `tests/devices/test_device.cpp` (or the file testing `Device::call`)

Record on `device.<device name>`: `<what> ok in <ms> ms` or `<what> failed: <kind>: <message>` (warn). Level debug, trace while `LogContext::polling()`. `call` and `relay` have no name for their op today, so all four overloads gain `std::string_view what` as their first parameter (`call(std::string_view what, const std::function<Result<T>(Resend)>& op, Repeat repeat, std::optional<Duration> window = std::nullopt)` and the `Link*` forms likewise), and every call site passes a literal naming the operation, e.g. `"read_pressure"`, `"open"`, `"set_setpoint"` (~140 sites; per family: gauges, valves, heaters/temperature, `extraction/chromium`, `ngx`, `thermo_qtegra`, `legacy/*`). `relay` logs at trace (its operation is observed, and logged, once by the driver's `call` or `observe` around it). Do not log the command bytes. No `Repeat` changes: `test_repeat_tables.cpp` must pass unchanged.

- [ ] **Step 1: Write failing tests:** `DeviceLog.CallOkIsDebug`, `DeviceLog.FailureIsWarn`, `DeviceLog.PollingIsTrace` (inside `LogContext::Polling` with `default_level = debug` → nothing in file).
- [ ] **Step 2: Run** → FAIL. **Step 3: Implement.** **Step 4: Run, then `ctest --preset dev-quick`** → PASS.
- [ ] **Step 5: Commit** `feat(devices): every driver call in the log`

### Task 16: UI actions and errors

**Files:**
- Create: `apps/pychron-ui/src/error_report.hpp`, `apps/pychron-ui/src/error_report.cpp`
- Modify: the 27 files with `QMessageBox::critical|warning` (55 sites) to call it; the `report_error_` / `show_error_` lambdas in `publish_actions.cpp:27`, `fit_actions.cpp:29`, `field_table_editor_window.cpp:231`, `publish_window.cpp:225`
- Modify (actions at info on `ui.action`): `experiment_bridge.cpp:94-98` (stop, cancel, abort, truncate, resume), `executor_pane` `request_start`, queue edit and save in `experiment_window.cpp`, the valve and switch commands in `core_bridge.cpp`, script run in `script_editor_window.cpp`, `PreferencesDialog` accept (which pages changed)
- Test: `tests/ui/test_error_report.cpp`, `tests/ui/test_experiment_bridge.cpp`

**Interfaces:**
- Produces:
  ```cpp
  namespace pychron::ui {
  // Logs at error on `ui.<area>` ("<title>: <text>"), then shows the box as before.
  // Returns what QMessageBox returned.
  QMessageBox::StandardButton show_error(QWidget* parent, std::string_view area, const QString& title,
                                         const QString& text, QMessageBox::StandardButtons buttons = QMessageBox::Ok);
  QMessageBox::StandardButton show_warning(QWidget* parent, std::string_view area, const QString& title,
                                           const QString& text, QMessageBox::StandardButtons buttons = QMessageBox::Ok);
  // For an Error object: logs through log_error (attempted = title), shows its text.
  void show_error(QWidget* parent, std::string_view area, const QString& title, const Error& error);
  }
  ```
  Tests run boxes non-modally: follow how existing UI tests dismiss a `QMessageBox` (`QTimer::singleShot` closing the active modal widget).
- [ ] **Step 1: Write failing tests:** `ErrorReport.ErrorIsLoggedThenShown`; `ErrorReport.ErrorObjectHasKindAndContext`; `ExperimentBridgeLog.StopIsLoggedAsAction`.
- [ ] **Step 2: Run** → FAIL. **Step 3: Implement**, then route all 55 sites.
- [ ] **Step 4: Verify:** `grep -rn "QMessageBox::critical\|QMessageBox::warning" apps/pychron-ui/src` prints only `error_report.cpp`; `ctest --preset dev-ui` → PASS.
- [ ] **Step 5: Commit** `feat(ui): every error shown and every action taken is in the log`

### Task 17: `elctl` stderr, end-to-end sequence, spec and docs

**Files:**
- Create: `apps/elctl/src/log_tee.hpp`, `apps/elctl/src/log_tee.cpp`
- Modify: `apps/elctl/src/main.cpp` (wrap `std::cerr`'s buffer while the process hub lives)
- Modify: `docs/superpowers/specs/2026-10-22-global-logging-design.md` (sections 2.1–2.4: the departures listed at the top of this phase), `docs/user/10-troubleshooting.md` ("Logs": what a sequence looks like, how to find a run with `run=`)
- Test: `apps/elctl/tests/test_log_tee.cpp`, `apps/elctl/tests/test_exp_commands.cpp`

**Interfaces:**
- Produces:
  ```cpp
  namespace elctl {
  // A streambuf that passes everything to `inner` and writes each complete line to `logger`:
  // lines starting "error" or "elctl: " + "error" at error, every other line at warn.
  class LogTeeBuf : public std::streambuf { public: LogTeeBuf(std::streambuf* inner, pychron::Logger logger); };
  }
  ```
- [ ] **Step 1: Write failing tests:**
  - `LogTee.ErrorLineIsErrorRecord`, `LogTee.PartialLineWaitsForNewline`, `LogTee.PassesBytesThrough`.
  - `ElctlExpTest.ExecutionSequenceIsLogged`: `ProcessLogScope` on a temp-dir hub, then the existing `RunTheExampleQueueOnTheSim` command line → `elctl.log` holds, in this order: `trace.QueueStarted`, `trace.RunStarted`, a `script.` `start` line, at least one host call, a `trace.RunStateChanged` to measured, a `store` or records-folder save line, `trace.RunFinished`, `trace.QueueEnded`; every line between `RunStarted` and `RunFinished` written by the run thread carries `[run] run=<that id>`.
- [ ] **Step 2: Run** → FAIL. **Step 3: Implement**; update the spec and docs.
- [ ] **Step 4: Full check before landing phase 2:** `cmake --build --preset dev-ui && ctest --preset dev-ui`, `python3 tools/quality_check.py`, `python3 tools/compiler_sweep.py` and `--build-dir build/dev-ui`, the TSan build for `LogHub*:Executor*:Scheduler*`. All clean.
- [ ] **Step 5: Commit** `feat(elctl): what elctl tells the user is in its log`

## Open questions

None.
