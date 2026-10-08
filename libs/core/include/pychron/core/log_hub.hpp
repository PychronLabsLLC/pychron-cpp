#pragma once

// Process-wide logging back end: owns the rotating file sink and its writer
// thread. No spdlog type appears here; everything lives behind Impl.

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "pychron/core/clock.hpp"
#include "pychron/core/config/logging_config.hpp"
#include "pychron/core/error.hpp"
#include "pychron/core/events.hpp"

namespace pychron {

class Logger;
class SignalBus;

// File line format: `2026-10-01T14:03:22.481Z [warn] transport.serial.ig1: msg`
// (UTC wall time, milliseconds). Records are written asynchronously by one
// writer thread (queue 8192, producers block when full, nothing is dropped).
// `error` records are on disk when write() returns; everything else within
// about one second, or on flush()/destruction.
//
// A `dir` that cannot be created is not an error: the hub runs without a file
// sink and reports one `error` Log on the bus (stderr if there is no bus).
// An empty `dir` means no file sink, silently.
//
// Levels: a logger's level is the level of the most specific matching rule
// (config `levels`, then set_level()); ties go to the longer pattern, then the
// lexicographically greater one. No match uses `default_level`.
//
// Every record written also goes, on the calling thread, to stderr (when
// `echo_stderr`) as `[level] name: message`, and to the bus (if any) as a
// `Log` event stamped with `clock.now()`.
class LogHub {
 public:
  // Fails (ErrorKind::Io) only if the logging back end itself cannot start.
  static Result<std::shared_ptr<LogHub>> create(const config::LoggingConfig& config,
                                                const Clock& clock, SignalBus* bus = nullptr);

  ~LogHub();  // flushes, stops the writer
  LogHub(const LogHub&) = delete;
  LogHub& operator=(const LogHub&) = delete;

  // A logger named `name` whose level follows the rules. It may outlive the
  // hub; once the hub is destroyed it writes nothing.
  Logger logger(std::string name);

  // Adds or replaces the rule for `pattern` and re-resolves every existing
  // hub logger whose level was not set explicitly. Thread-safe.
  void set_level(std::string_view pattern, LogLevel level);

  // Replaces the default level and every rule at once: a rule that is not in
  // `levels` no longer applies. Explicitly set loggers keep their level, as
  // with set_level. Thread-safe.
  void set_levels(LogLevel default_level, const std::vector<std::pair<std::string, LogLevel>>& levels);

  // Blocks until every record written so far is on disk.
  void flush();

  // The bus this hub publishes `Log` events on (null if none). Producers that
  // own a bus use it to avoid delivering a record twice.
  SignalBus* bus() const noexcept;

  // Used by Logger; not for callers. Never throws; `message` is written
  // verbatim (it is never interpreted as a format string).
  void write(LogLevel level, std::string_view logger, std::string_view message);

  // Best-effort crash flush (spec 4.5). Static and idempotent, so main() may
  // call it before any hub exists; it acts on whichever hub create() made
  // most recently (held weakly, so a destroyed hub is simply skipped).
  //
  // - std::terminate: logs the active exception text at `error` on logger
  //   `pychron`, flushes (bounded wait), then calls the previous handler.
  // - POSIX SIGSEGV/SIGABRT/SIGBUS/SIGFPE/SIGILL: writes `fatal signal N` with
  //   write(2) to stderr and to a descriptor on that hub's pychron.log (moved
  //   onto the live file within about 1 s of a rotation), then restores the
  //   default action and re-raises. No flush (signal-unsafe):
  //   records below `error` younger than about 1 s may be lost.
  // - Windows: an unhandled-exception filter writing `fatal exception 0x..`
  //   the same way, returning EXCEPTION_CONTINUE_SEARCH.
  static void install_crash_handlers();

  struct Impl;

 private:
  explicit LogHub(std::shared_ptr<Impl> impl);
  std::shared_ptr<Impl> impl_;  // shared with hub loggers, which may outlive the hub
};

}  // namespace pychron
