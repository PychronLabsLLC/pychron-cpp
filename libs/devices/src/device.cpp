#include "pychron/devices/device.hpp"

#include <chrono>
#include <string>

#include "pychron/core/log_context.hpp"
#include "pychron/core/process_log.hpp"

namespace pychron {
namespace {

// The level of a record that says an operation went through.
LogLevel ok_level() noexcept { return LogContext::polling() ? LogLevel::Trace : LogLevel::Debug; }

std::string took(std::string_view what, TimePoint started, TimePoint now) {
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - started).count();
  std::string message(what);
  message.append(" ok in ").append(std::to_string(ms)).append(" ms");
  return message;
}

std::string failed(std::string_view what, const Error& error) {
  std::string message(what);
  message.append(" failed: ").append(to_string(error.kind)).append(": ").append(error.what);
  return message;
}

}  // namespace

std::string_view to_string(DeviceState state) noexcept {
  switch (state) {
    case DeviceState::Unknown: return "unknown";
    case DeviceState::Ok: return "ok";
    case DeviceState::Degraded: return "degraded";
    case DeviceState::Faulted: return "faulted";
  }
  return "unknown";
}

Device::Device(std::string name, DeviceOptions options)
    : name_(std::move(name)),
      clock_(options.clock),
      fault_after_(options.fault_after),
      link_(options.link),
      log_(process_logger("device." + name_)) {}

DeviceHealth Device::health() const {
  DeviceHealth h;
  h.consecutive_failures = failures_.load();
  h.last_ok = TimePoint(Duration(last_ok_.load()));
  if (int e = last_error_.load(); e >= 0) h.last_error = static_cast<ErrorKind>(e);

  if (h.consecutive_failures == 0) {
    h.state = ever_ok_.load() ? DeviceState::Ok : DeviceState::Unknown;
  } else {
    h.state = h.consecutive_failures >= fault_after_ ? DeviceState::Faulted : DeviceState::Degraded;
  }
  return h;
}

TimePoint Device::log_now() const { return clock_ ? clock_->now() : SteadyClock().now(); }

Device::Operation Device::begin(std::string_view what) const {
  if (!log_.enabled(ok_level())) return {what, std::nullopt};
  return {what, log_now()};
}

std::optional<TimePoint> Device::trace_start() const {
  if (!log_.enabled(LogLevel::Trace)) return std::nullopt;
  return log_now();
}

void Device::log_outcome(const Operation& op, const Error* error, bool failing) const {
  if (error == nullptr) {
    // No start: the record was off when the operation began.
    if (op.started && log_.enabled(ok_level())) log_.log(ok_level(), took(op.what, *op.started, log_now()));
    return;
  }
  // Not a device failure (a cancel), or a poll of a device already failing
  // (said at warn when the streak began): at the level of an ok.
  const LogLevel level = error->kind == ErrorKind::Cancelled || (failing && LogContext::polling())
                             ? ok_level()
                             : LogLevel::Warn;
  if (log_.enabled(level)) log_.log(level, failed(op.what, *error));
}

void Device::log_relay(std::string_view what, TimePoint started, const Error* error) const {
  if (!log_.enabled(LogLevel::Trace)) return;
  log_.trace(error == nullptr ? took(what, started, log_now()) : failed(what, *error));
}

void Device::record_ok() {
  const TimePoint at = log_now();
  last_ok_.store(at.time_since_epoch().count());
  failures_.store(0);
  ever_ok_.store(true);
}

void Device::record_failure(ErrorKind kind) {
  if (kind == ErrorKind::Cancelled) return;
  last_error_.store(static_cast<int>(kind));
  failures_.fetch_add(1);
}

}  // namespace pychron
