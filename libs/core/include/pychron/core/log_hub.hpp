#pragma once

// Process-wide logging back end: owns the rotating file sink and its writer
// thread. No spdlog type appears here; everything lives behind Impl.

#include <memory>
#include <string_view>

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
class LogHub {
 public:
  // Fails (ErrorKind::Io) only if the logging back end itself cannot start.
  static Result<std::shared_ptr<LogHub>> create(const config::LoggingConfig& config,
                                                const Clock& clock, SignalBus* bus = nullptr);

  ~LogHub();  // flushes, stops the writer
  LogHub(const LogHub&) = delete;
  LogHub& operator=(const LogHub&) = delete;

  // Blocks until every record written so far is on disk.
  void flush();

  // Used by Logger; not for callers. Never throws; `message` is written
  // verbatim (it is never interpreted as a format string).
  void write(LogLevel level, std::string_view logger, std::string_view message);

  struct Impl;

 private:
  explicit LogHub(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace pychron
