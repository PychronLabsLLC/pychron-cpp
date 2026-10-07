#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>

#include "pychron/core/clock.hpp"
#include "pychron/core/clock_mutex.hpp"
#include "pychron/core/error.hpp"
#include "pychron/transport/transport.hpp"

namespace pychron {

// Lets a driver recover from a dropped connection: when an operation fails
// with Io or NotConnected, reopen the transport, re-run the driver's connect
// step and retry the operation once. Other errors pass through untouched.
//
// Thread-safe. The mutex guards only the reconnect step and its bookkeeping,
// never `op`. The step reopens the transport and runs `on_connect`, both of
// which may wait in clock time, so it is a ClockMutex. Each run() notes the reconnect generation before calling `op`;
// a caller whose failure was already repaired by another thread's reconnect
// (generation moved on) just retries, so concurrent failures reconnect once.
//
// Known window: the generation is not tied to the connection `op` used. If a
// caller snapshots generation g, another thread then reconnects (g+1), and the
// caller's `op` runs on the new connection and fails with a genuine new drop,
// the caller sees g+1 != g, skips the reconnect and retries on the dead
// connection. It then returns the retry's error once; the next call
// reconnects (subject to min_interval).
//
// `on_connect` runs under the reconnect mutex but other threads' ops may
// interleave with it on the transport. A multi-step handshake should use
// transact() so its steps stay back to back.
class Reconnector {
 public:
  Reconnector(Transport& transport, const Clock& clock, Duration min_interval = std::chrono::seconds(1));

  // Runs `op`. On Io or NotConnected, and if `min_interval` has passed since
  // the last reconnect attempt (successful or not) or there was none, closes
  // and reopens the transport, runs `on_connect`, then runs `op` once more.
  // A failed reopen or `on_connect` returns that error; within `min_interval`
  // the original error is returned without touching the transport.
  template <class T>
  Result<T> run(const std::function<Result<T>()>& op, const std::function<Result<void>()>& on_connect) {
    const std::uint64_t seen = generation_.load();
    Result<T> first = op();
    if (first || !recoverable(first.error().kind)) return first;
    auto go = reconnect_since(seen, on_connect);
    if (!go) return fail(std::move(go).error());
    if (!*go) return first;
    return op();
  }

  // Successful reopen + on_connect.
  std::uint64_t reconnects() const noexcept { return generation_.load(); }

 private:
  static bool recoverable(ErrorKind kind) noexcept {
    return kind == ErrorKind::Io || kind == ErrorKind::NotConnected;
  }

  // true: retry the operation; false: rate limited; error: reconnect failed.
  Result<bool> reconnect_since(std::uint64_t seen, const std::function<Result<void>()>& on_connect);

  Transport& transport_;
  const Clock& clock_;
  Duration min_interval_;
  ClockMutex mutex_;
  std::optional<TimePoint> last_attempt_;
  std::atomic<std::uint64_t> generation_{0};
};

}  // namespace pychron
