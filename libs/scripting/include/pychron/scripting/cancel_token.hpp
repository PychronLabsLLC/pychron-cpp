#pragma once

// CancelToken: the control a script run is checked against (spec section 6).
//
//   Cancel  every blocking command (sleep, waits, moves, patterns, resource
//           acquisition) raises ScriptCancelled in the script, so `finally:`
//           blocks run and may still make non-blocking hardware calls.
//   Abort   the host interrupts the script at the next line and refuses any
//           further hardware call; `finally:` blocks cannot touch hardware.
//
// wake() ends the current pause()/sleep() early without cancelling.
// Thread-safe: request from any thread while the script runs on another.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>

#include "pychron/core/clock.hpp"

namespace pychron::scripting {

enum class CancelMode { None, Cancel, Abort };

enum class WaitResult { Elapsed, Woken, Cancelled };

class CancelToken {
 public:
  void cancel();
  // Overrides Cancel.
  void abort();
  // Ends a pending pause()/sleep() early.
  void wake();
  // Back to None (for reuse between runs).
  void reset();

  CancelMode mode() const noexcept { return static_cast<CancelMode>(mode_.load()); }
  bool requested() const noexcept { return mode() != CancelMode::None; }

  // Block on `clock` until `deadline`, a cancel/abort request, or wake().
  // Cancelled wins over Woken and Elapsed.
  WaitResult wait_until(const Clock& clock, TimePoint deadline);

 private:
  std::atomic<int> mode_{0};
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::uint64_t wakes_ = 0;
};

}  // namespace pychron::scripting
