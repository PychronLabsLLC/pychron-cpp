#pragma once

// One host call of a script, as a record on `script.<name>`. Private to the
// embedded-CPython host.
//
// Built first thing in a host function. On destruction it writes one record:
//   debug  "<call>(<summary>) -> <result> in <ms> ms"   ("ok" when no result)
//   warn   "<call>(<summary>) -> error: <what> in <ms> ms"
// The argument pairs come from a callable, called only when the logger is
// enabled at debug, so a call site pays nothing for them when the log is off.
// A failure is still recorded at warn, with no summary, when only warn is on.
// Time is read from the session clock (`now()`, never a wait), so a simulated
// run reports simulated milliseconds. Nothing here takes the GIL or waits.

#include <array>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "pychron/core/clock.hpp"
#include "pychron/core/error.hpp"
#include "pychron/core/log_summary.hpp"
#include "pychron/core/logger.hpp"

namespace pychron::scripting::python {

class HostCallTrace {
 public:
  using Arg = std::pair<std::string, std::string>;
  using Args = std::vector<Arg>;

  // `slot`, when given, is the owner's "innermost call in progress" pointer:
  // the trace puts itself there and restores the previous one on destruction,
  // so the owner can hand it the Error it is about to raise.
  HostCallTrace(const Logger& logger, const Clock& clock, std::string_view call,
                std::span<const Arg> args, HostCallTrace** slot = nullptr)
      : HostCallTrace(logger, clock, call, slot) {
    if (debug_) summary_ = summarize(args);
  }

  template <class MakeArgs>
    requires std::invocable<MakeArgs&> && std::convertible_to<std::invoke_result_t<MakeArgs&>, Args>
  HostCallTrace(const Logger& logger, const Clock& clock, std::string_view call, MakeArgs&& make_args,
                HostCallTrace** slot = nullptr)
      : HostCallTrace(logger, clock, call, slot) {
    if (debug_) {
      const Args args = make_args();
      summary_ = summarize(args);
    }
  }

  ~HostCallTrace() {
    if (slot_) *slot_ = previous_;
    if (!active_) return;
    try {
      write();
    } catch (...) {  // NOLINT(bugprone-empty-catch): logging never stops a script
    }
  }
  HostCallTrace(const HostCallTrace&) = delete;
  HostCallTrace& operator=(const HostCallTrace&) = delete;

  // What the call answered; recorded only when debug is enabled.
  void result(std::string_view text) {
    if (debug_) result_ = truncate_for_log(text);
  }
  // The call failed with `error` (the first one wins).
  void failed(const Error& error) {
    if (active_ && !error_) error_ = error;
  }

  // Runs `f`, records its answer when it has a printable one, returns it.
  template <class F>
  decltype(auto) run(F&& f) {
    if constexpr (std::is_void_v<std::invoke_result_t<F>>) {
      std::forward<F>(f)();
    } else {
      auto value = std::forward<F>(f)();
      if (debug_) {
        if constexpr (std::is_same_v<decltype(value), bool> || std::is_same_v<decltype(value), double> ||
                      std::is_same_v<decltype(value), std::string>)
          result(text(value));
      }
      return value;
    }
  }

  static std::string text(bool v) { return v ? "true" : "false"; }
  static std::string text(double v) {
    std::array<char, 32> buf{};
    std::snprintf(buf.data(), buf.size(), "%g", v);
    return buf.data();
  }
  static std::string text(std::int64_t v) { return std::to_string(v); }
  static std::string text(int v) { return std::to_string(v); }
  static std::string text(const std::string& v) { return v; }

 private:
  HostCallTrace(const Logger& logger, const Clock& clock, std::string_view call, HostCallTrace** slot)
      : logger_(logger),
        clock_(clock),
        debug_(logger.enabled(LogLevel::Debug)),
        active_(logger.enabled(LogLevel::Warn)),
        slot_(slot),
        unwinding_(std::uncaught_exceptions()) {
    if (slot_) {
      previous_ = *slot_;
      *slot_ = this;
    }
    if (active_) {
      call_ = call;
      start_ = clock_.now();
    }
  }

  void write() {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(clock_.now() - start_).count();
    const std::string head = call_ + "(" + summary_ + ") -> ";
    const std::string tail = " in " + std::to_string(ms) + " ms";
    if (error_) {
      std::string what = std::string(to_string(error_->kind)) + ": " + error_->what;
      if (!error_->device.empty()) what += " (" + error_->device + ")";
      logger_.warn(head + "error: " + truncate_for_log(what) + tail);
    } else if (std::uncaught_exceptions() > unwinding_) {
      // Left by an exception that carried no Error: a cancel, an abort, or a
      // failure further in that the Python side is passing up.
      logger_.warn(head + "error: interrupted" + tail);
    } else if (debug_) {
      logger_.debug(head + (result_ ? *result_ : std::string("ok")) + tail);
    }
  }

  const Logger& logger_;
  const Clock& clock_;
  bool debug_;
  bool active_;
  HostCallTrace** slot_;
  HostCallTrace* previous_ = nullptr;
  int unwinding_;
  std::string call_;
  std::string summary_;
  TimePoint start_{};
  std::optional<std::string> result_;
  std::optional<Error> error_;
};

}  // namespace pychron::scripting::python
