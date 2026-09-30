#include "pychron/devices/device.hpp"

namespace pychron {

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
    : name_(std::move(name)), clock_(options.clock), fault_after_(options.fault_after) {}

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

void Device::record_ok() {
  TimePoint now = clock_ ? clock_->now() : SteadyClock().now();
  last_ok_.store(now.time_since_epoch().count());
  failures_.store(0);
  ever_ok_.store(true);
}

void Device::record_failure(ErrorKind kind) {
  if (kind == ErrorKind::Cancelled) return;
  last_error_.store(static_cast<int>(kind));
  failures_.fetch_add(1);
}

}  // namespace pychron
