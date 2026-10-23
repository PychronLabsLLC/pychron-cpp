#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "pychron/core/clock.hpp"
#include "pychron/core/error.hpp"
#include "pychron/core/logger.hpp"
#include "pychron/devices/link.hpp"
#include "pychron/transport/transport.hpp"

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
  Link* link = nullptr;               // the transport's recovery; none: no reconnect
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
  // The recovery of the transport this device was given (DeviceOptions::link);
  // null: none, or the driver keeps its own (Qtegra, NGX).
  Link* link() const noexcept { return link_; }

 protected:
  // One device operation as the log sees it: its name (a literal, snake_case:
  // "read_pressure", "open", "set_setpoint") and when it started. The start
  // is read only when the record it times would be written.
  struct Operation {
    std::string_view what;
    std::optional<TimePoint> started;
  };

  // Starts an operation: taken before its first wire call, handed to
  // observe() with its outcome.
  [[nodiscard]] Operation begin(std::string_view what) const;

  // Record the outcome of one device operation and pass it through. Errors
  // without a device are attributed to this one. Cancelled is not a device
  // failure and leaves health unchanged.
  //
  // One record on device.<name>: "<what> ok in <ms> ms" at debug (trace
  // while LogContext::polling()), or "<what> failed: <kind>: <message>" at
  // warn (a cancel at the level of an ok; a failure while polling of a device
  // already failing at trace, so a dead gauge is said once, not every tick).
  template <class T>
  Result<T> observe(const Operation& op, Result<T> result) {
    if (result) {
      record_ok();
      log_outcome(op, nullptr, false);
    } else {
      if (result.error().device.empty()) result.error().device = name_;
      const bool failing = failures_.load() > 0;
      record_failure(result.error().kind);
      log_outcome(op, &result.error(), failing);
    }
    return result;
  }

  // One wire call, observed. `op` makes the call's exchanges with the Resend
  // it is given: Allowed for a Safe call, Never for an Unsafe one. Through
  // the link when there is one (reconnect, and repeat when Safe; see Link),
  // else `op` once: an Unsafe command is resent by neither layer.
  // `window` replaces the link policy's call_window (a command that must get
  // through, such as a stop after a lost fire, is given kCriticalWindow).
  // `what` names the operation in the log (see observe()).
  template <class T>
  Result<T> call(std::string_view what, const std::function<Result<T>(Resend)>& op, Repeat repeat,
                 std::optional<Duration> window = std::nullopt) {
    return call(what, link_, op, repeat, window);
  }

  // The same through `link`, for a driver whose link is not known when the
  // device is made (a Qtegra borrower finds the owner's at each call).
  template <class T>
  Result<T> call(std::string_view what, Link* link, const std::function<Result<T>(Resend)>& op, Repeat repeat,
                 std::optional<Duration> window = std::nullopt) {
    const Operation started = begin(what);
    return observe(started, pass(link, op, repeat, window));
  }

  // One wire call through the link, not observed: for a driver whose one
  // operation is several wire calls (a check, then the command, then its
  // read-back) and is observed once, as a whole. Each wire call still goes
  // through the link on its own, so what is repeated is that call, never the
  // operation around it. Its record ("<what> ok in <ms> ms" or "<what>
  // failed: ...") is at trace: the operation around it has its own.
  template <class T>
  Result<T> relay(std::string_view what, const std::function<Result<T>(Resend)>& op, Repeat repeat,
                  std::optional<Duration> window = std::nullopt) {
    return relay(what, link_, op, repeat, window);
  }

  template <class T>
  Result<T> relay(std::string_view what, Link* link, const std::function<Result<T>(Resend)>& op, Repeat repeat,
                  std::optional<Duration> window = std::nullopt) {
    const auto started = trace_start();
    Result<T> result = pass(link, op, repeat, window);
    if (started) log_relay(what, *started, result ? nullptr : &result.error());
    return result;
  }

 private:
  template <class T>
  Result<T> pass(Link* link, const std::function<Result<T>(Resend)>& op, Repeat repeat,
                 std::optional<Duration> window) {
    const Resend resend = repeat == Repeat::Safe ? Resend::Allowed : Resend::Never;
    if (link == nullptr) return op(resend);
    const std::function<Result<T>()> bound = [&op, resend] { return op(resend); };
    return link->call(bound, repeat, window);
  }

  void record_ok();
  void record_failure(ErrorKind kind);
  // The device's clock, else steady_clock: a read, never a wait.
  [[nodiscard]] TimePoint log_now() const;
  // log_now() when a relay's trace record would be written.
  [[nodiscard]] std::optional<TimePoint> trace_start() const;
  // `error` null: ok. `failing`: the device had failed before this one.
  void log_outcome(const Operation& op, const Error* error, bool failing) const;
  void log_relay(std::string_view what, TimePoint started, const Error* error) const;

  std::string name_;
  const Clock* clock_;
  std::uint64_t fault_after_;
  Link* link_;
  std::atomic<bool> ever_ok_{false};
  std::atomic<TimePoint::rep> last_ok_{0};
  std::atomic<std::uint64_t> failures_{0};
  std::atomic<int> last_error_{-1};  // ErrorKind, or -1 for none
  Logger log_;                       // device.<name>, taken when the device is made
};

}  // namespace pychron
