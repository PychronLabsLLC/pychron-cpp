#pragma once

#include <atomic>
#include <iosfwd>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include "pychron/core/clock.hpp"
#include "pychron/core/events.hpp"

namespace pychron {

class SignalBus;

std::string_view to_string(LogLevel level) noexcept;

struct LoggerOptions {
  LogLevel level = LogLevel::Info;
  std::ostream* echo = nullptr;  // e.g. &std::cerr; must outlive the logger
};

// Named logger. Records at or above the threshold are published as `Log`
// events on the bus (if any) and optionally echoed as text to a stream.
// Thread-safe; cheap to copy via child().
class Logger {
 public:
  using Options = LoggerOptions;

  Logger(std::string name, const Clock& clock, SignalBus* bus = nullptr, Options options = {});
  Logger(const Logger& other);
  Logger& operator=(const Logger& other);

  const std::string& name() const noexcept { return name_; }

  void set_level(LogLevel level) noexcept { level_.store(level); }
  LogLevel level() const noexcept { return level_.load(); }
  bool enabled(LogLevel level) const noexcept { return level >= level_.load(); }

  void log(LogLevel level, std::string_view message) const;
  void trace(std::string_view m) const { log(LogLevel::Trace, m); }
  void debug(std::string_view m) const { log(LogLevel::Debug, m); }
  void info(std::string_view m) const { log(LogLevel::Info, m); }
  void warn(std::string_view m) const { log(LogLevel::Warn, m); }
  void error(std::string_view m) const { log(LogLevel::Error, m); }

  // Logger named "<parent>.<suffix>" sharing clock, bus, level and echo stream.
  Logger child(std::string_view suffix) const;

 private:
  std::string name_;
  const Clock* clock_;
  SignalBus* bus_;
  std::atomic<LogLevel> level_;
  std::ostream* echo_;
  std::shared_ptr<std::mutex> echo_mutex_;
};

}  // namespace pychron
