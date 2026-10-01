#include "pychron/core/logger.hpp"

#include <ostream>

#include "log_hub_internal.hpp"
#include "pychron/core/signal_bus.hpp"

namespace pychron {

std::string_view to_string(LogLevel level) noexcept {
  switch (level) {
    case LogLevel::Trace: return "trace";
    case LogLevel::Debug: return "debug";
    case LogLevel::Info: return "info";
    case LogLevel::Warn: return "warn";
    case LogLevel::Error: return "error";
  }
  return "unknown";
}

Logger::Logger(std::string name, const Clock& clock, SignalBus* bus, Options options)
    : name_(std::move(name)),
      clock_(&clock),
      bus_(bus),
      state_(static_cast<std::uint64_t>(options.level)),
      echo_(options.echo),
      echo_mutex_(std::make_shared<std::mutex>()) {}

// Epoch 0 is never a hub epoch, so the first level() call resolves.
Logger::Logger(std::string name, std::shared_ptr<LogHub::Impl> hub)
    : name_(std::move(name)), state_(0), hub_(std::move(hub)) {}

Logger::Logger(const Logger& other)
    : name_(other.name_),
      clock_(other.clock_),
      bus_(other.bus_),
      state_(other.state_.load()),
      echo_(other.echo_),
      echo_mutex_(other.echo_mutex_),
      hub_(other.hub_) {}

Logger& Logger::operator=(const Logger& other) {
  if (this != &other) {
    name_ = other.name_;
    clock_ = other.clock_;
    bus_ = other.bus_;
    state_.store(other.state_.load());
    echo_ = other.echo_;
    echo_mutex_ = other.echo_mutex_;
    hub_ = other.hub_;
  }
  return *this;
}

void Logger::set_level(LogLevel level) noexcept {
  // Keep the epoch bits: they are ignored once the explicit bit is set.
  std::uint64_t cur = state_.load();
  std::uint64_t next = 0;
  do {
    next = (cur & ~(kLevelMask | kExplicitBit)) | kExplicitBit | static_cast<std::uint64_t>(level);
  } while (!state_.compare_exchange_weak(cur, next));
}

LogLevel Logger::level() const noexcept {
  std::uint64_t cur = state_.load();
  if (hub_ && (cur & kExplicitBit) == 0) {
    const std::uint64_t epoch = detail::hub_rule_epoch(*hub_);
    while ((cur & kExplicitBit) == 0 && (cur >> kEpochShift) != epoch) {
      const auto resolved = detail::hub_resolve_level(*hub_, name_);
      const std::uint64_t next =
          (resolved.epoch << kEpochShift) | static_cast<std::uint64_t>(resolved.level);
      // Fails if another thread refreshed or set an explicit level meanwhile;
      // `cur` is then reloaded and re-checked.
      if (state_.compare_exchange_weak(cur, next)) {
        cur = next;
        break;
      }
    }
  }
  return static_cast<LogLevel>(cur & kLevelMask);
}

void Logger::log(LogLevel level, std::string_view message) const {
  if (!enabled(level)) return;
  if (hub_) {
    detail::hub_write(*hub_, level, name_, message);
    return;
  }
  Log event{level, name_, std::string(message), clock_->now()};
  if (echo_ != nullptr) {
    std::lock_guard lock(*echo_mutex_);
    *echo_ << '[' << to_string(level) << "] " << name_ << ": " << message << '\n';
  }
  if (bus_ != nullptr) bus_->publish(event);
}

Logger Logger::child(std::string_view suffix) const {
  std::string child_name = name_;
  child_name += '.';
  child_name += suffix;
  if (hub_) return Logger(std::move(child_name), hub_);
  Logger out(std::move(child_name), *clock_, bus_, Options{level(), echo_});
  out.echo_mutex_ = echo_mutex_;
  return out;
}

}  // namespace pychron
