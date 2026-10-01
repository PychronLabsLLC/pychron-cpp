#pragma once

#include <atomic>
#include <cstdint>
#include <iosfwd>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include "pychron/core/clock.hpp"
#include "pychron/core/events.hpp"
#include "pychron/core/log_hub.hpp"

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
//
// Loggers made by LogHub::logger() instead hand records to the hub (file,
// stderr, bus) and take their level from the hub's rules, following
// LogHub::set_level() until set_level() is called on the logger itself.
class Logger {
 public:
  using Options = LoggerOptions;

  Logger(std::string name, const Clock& clock, SignalBus* bus = nullptr, Options options = {});
  Logger(const Logger& other);
  Logger& operator=(const Logger& other);

  const std::string& name() const noexcept { return name_; }

  // On a hub logger this is an explicit override that hub rules no longer replace.
  void set_level(LogLevel level) noexcept;
  LogLevel level() const noexcept;
  bool enabled(LogLevel level) const noexcept { return level >= this->level(); }

  void log(LogLevel level, std::string_view message) const;
  void trace(std::string_view m) const { log(LogLevel::Trace, m); }
  void debug(std::string_view m) const { log(LogLevel::Debug, m); }
  void info(std::string_view m) const { log(LogLevel::Info, m); }
  void warn(std::string_view m) const { log(LogLevel::Warn, m); }
  void error(std::string_view m) const { log(LogLevel::Error, m); }

  // Logger named "<parent>.<suffix>" sharing clock, bus, level and echo stream.
  // A hub logger's child shares the hub and resolves its own level.
  Logger child(std::string_view suffix) const;

 private:
  friend class LogHub;
  Logger(std::string name, std::shared_ptr<LogHub::Impl> hub);

  // state_ packs (epoch << 9) | (explicit << 8) | level so a level and the
  // rule epoch it was resolved under are always read and written together.
  static constexpr std::uint64_t kLevelMask = 0xFF;
  static constexpr std::uint64_t kExplicitBit = 0x100;
  static constexpr int kEpochShift = 9;

  std::string name_;
  const Clock* clock_ = nullptr;
  SignalBus* bus_ = nullptr;
  mutable std::atomic<std::uint64_t> state_{0};
  std::ostream* echo_ = nullptr;
  std::shared_ptr<std::mutex> echo_mutex_;
  std::shared_ptr<LogHub::Impl> hub_;  // null for the legacy constructors
};

}  // namespace pychron
