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
//
// A wait is made on the caller's clock, and the token remembers that clock
// for as long as somebody waits: cancel(), abort() and wake() change the
// token under its mutex and then notify through that clock, so a waiter on a
// VirtualClock is woken at the instant of the request and not at its
// deadline. One token is waited on through one clock at a time.
//
// What cannot wait on the token (a job with a token of its own) registers a
// callback with add_on_cancel(). Callbacks are called on the thread that
// requests, with the token unlocked, so one may take other locks and may use
// the token. A callback may be called on two threads at once (a cancel() and
// an abort(), or the call made at registration and an abort()), and one
// registered late may so be called twice for what its owner sees as one
// request: it is written to stand both. remove_on_cancel() waits for a call
// in progress outside any clock: a callback is brief and does not wait in
// clock time, and the remover holds no lock the callback takes.

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

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

  // `callback` is called once, on the requesting thread, for each request
  // made while it is registered: a cancel() of a token that was not
  // requested, an abort() of one that was not aborted. reset() and wake()
  // call nothing, and reset() leaves the callbacks registered. If the token
  // is already requested the callback is called before add_on_cancel returns.
  // Returns an id for remove_on_cancel. What the callback throws comes out
  // of cancel() or abort(), with the callbacks after it not called, or out of
  // add_on_cancel, which then has not registered it.
  std::uint64_t add_on_cancel(std::function<void()> callback);
  // The callback is not called after this returns: a call in progress on
  // another thread is waited for. Called from inside the callback itself it
  // returns at once. An id that is not registered is ignored. The caller
  // holds no lock the callback takes.
  void remove_on_cancel(std::uint64_t id);

 private:
  struct Callback {
    std::uint64_t id = 0;
    std::function<void()> call;
    bool removed = false;
    std::vector<std::thread::id> callers;  // threads inside call()
  };

  // Caller holds mutex_, and has changed what the waiters test.
  void notify_locked();
  void call(const std::vector<std::shared_ptr<Callback>>& callbacks);

  std::atomic<int> mode_{0};  // written under mutex_
  mutable std::mutex mutex_;
  std::condition_variable cv_;  // waited on and notified through waiter_clock_
  std::uint64_t wakes_ = 0;
  const Clock* waiter_clock_ = nullptr;  // the clock of the threads in wait_until()
  std::size_t waiters_ = 0;
  std::vector<std::shared_ptr<Callback>> callbacks_;
  std::uint64_t next_callback_ = 1;
  std::condition_variable called_;  // remove_on_cancel(): a call has returned
};

}  // namespace pychron::scripting
