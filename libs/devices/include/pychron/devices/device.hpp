#pragma once

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "pychron/core/clock.hpp"
#include "pychron/core/error.hpp"

namespace pychron {

// Unknown: never operated. Ok: last operation succeeded. Degraded: recent
// operations failed, below the fault threshold. Faulted: failures reached it.
enum class DeviceState { Unknown, Ok, Degraded, Faulted };

std::string_view to_string(DeviceState state) noexcept;

struct DeviceHealth {
  DeviceState state = DeviceState::Unknown;
  TimePoint last_ok{};  // epoch if never ok
  std::uint64_t consecutive_failures = 0;
  std::optional<ErrorKind> last_error;  // kept after recovery, for diagnostics

  friend bool operator==(const DeviceHealth&, const DeviceHealth&) = default;
};

struct DeviceOptions {
  const Clock* clock = nullptr;       // stamps last_ok; SteadyClock if null
  std::uint64_t fault_after = 3;      // consecutive failures that mean Faulted
};

// Identity and health of one configured device. Behavior lives in the
// capability interfaces a driver implements, never here.
//
// Health is tracked lock-free so health() may be called from any thread
// while the driver runs on a scheduler worker.
class Device {
 public:
  explicit Device(std::string name, DeviceOptions options = {});
  virtual ~Device() = default;

  Device(const Device&) = delete;
  Device& operator=(const Device&) = delete;

  const std::string& name() const noexcept { return name_; }
  DeviceHealth health() const;

 protected:
  // Record the outcome of one device operation and pass it through. Errors
  // without a device are attributed to this one. Cancelled is not a device
  // failure and leaves health unchanged.
  template <class T>
  Result<T> observe(Result<T> result) {
    if (result) {
      record_ok();
    } else {
      if (result.error().device.empty()) result.error().device = name_;
      record_failure(result.error().kind);
    }
    return result;
  }

 private:
  void record_ok();
  void record_failure(ErrorKind kind);

  std::string name_;
  const Clock* clock_;
  std::uint64_t fault_after_;
  std::atomic<bool> ever_ok_{false};
  std::atomic<TimePoint::rep> last_ok_{0};
  std::atomic<std::uint64_t> failures_{0};
  std::atomic<int> last_error_{-1};  // ErrorKind, or -1 for none
};

}  // namespace pychron
