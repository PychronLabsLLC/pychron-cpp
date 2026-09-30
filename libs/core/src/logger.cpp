#include "pychron/core/logger.hpp"

#include <ostream>

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
      level_(options.level),
      echo_(options.echo),
      echo_mutex_(std::make_shared<std::mutex>()) {}

Logger::Logger(const Logger& other)
    : name_(other.name_),
      clock_(other.clock_),
      bus_(other.bus_),
      level_(other.level()),
      echo_(other.echo_),
      echo_mutex_(other.echo_mutex_) {}

Logger& Logger::operator=(const Logger& other) {
  if (this != &other) {
    name_ = other.name_;
    clock_ = other.clock_;
    bus_ = other.bus_;
    level_.store(other.level());
    echo_ = other.echo_;
    echo_mutex_ = other.echo_mutex_;
  }
  return *this;
}

void Logger::log(LogLevel level, std::string_view message) const {
  if (!enabled(level)) return;
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
  Logger out(std::move(child_name), *clock_, bus_, Options{level(), echo_});
  out.echo_mutex_ = echo_mutex_;
  return out;
}

}  // namespace pychron
