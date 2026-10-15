#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <utility>

#include "pychron/core/clock.hpp"
#include "pychron/core/clock_mutex.hpp"
#include "pychron/core/config/recovery_config.hpp"
#include "pychron/core/error.hpp"
#include "pychron/transport/transport.hpp"

namespace pychron {

class SignalBus;

// May this call be sent twice? A command that sets an absolute target (open
// a valve, a setpoint of 450, read anything) is Safe; one whose second
// arrival does more (toggle, step, start an acquisition, fire for a
// duration, a relative move) is Unsafe.
enum class Repeat { Safe, Unsafe };

// How a link recovers. One per transport ([transports.<name>.recovery]);
// defaults are spec section 4.10's.
struct LinkPolicy {
  bool reconnect = true;
  bool optional = false;  // not needed for a run to start (spec 4.3)
  Duration backoff_min = std::chrono::seconds(1);
  Duration backoff_max = std::chrono::seconds(30);
  Duration call_window = std::chrono::seconds(20);  // how long one call keeps trying

  friend bool operator==(const LinkPolicy&, const LinkPolicy&) = default;
};

// The policy a `[transports.<name>.recovery]` table gives; the backoff is not
// configured per transport and keeps LinkPolicy's. No table: LinkPolicy{}.
// `defaults` is the lab's (executor.toml [links]): what the transport's table
// does not say.
[[nodiscard]] LinkPolicy link_policy(const config::RecoveryConfig& recovery, const LinkPolicy& defaults = {});

// The window of a call that must get through while the line is in a state
// that is not safe to leave it in: a stop after a fire whose outcome is
// unknown, a valve call during a measurement (spec 4.4).
inline constexpr Duration kCriticalWindow = std::chrono::minutes(10);

// The error code an Unsafe call returns when it failed in a way that leaves
// open whether the command reached the device: the caller reads back.
inline constexpr const char* kOutcomeUnknown = "outcome_unknown";

// A failure that may be the link's, not the device's: a reconnect may cure it.
[[nodiscard]] constexpr bool is_transient(ErrorKind kind) noexcept {
  return kind == ErrorKind::Timeout || kind == ErrorKind::Io || kind == ErrorKind::NotConnected;
}

// `error` with code kOutcomeUnknown when it is transient: for the Unsafe part
// of an op that is otherwise Safe (see Link).
[[nodiscard]] inline Error outcome_unknown_if_transient(Error error) {
  if (is_transient(error.kind)) error.code = kOutcomeUnknown;
  return error;
}

// One per transport, shared by the devices on it. Runs a device's wire call
// and, when it fails with a transient error (Timeout, Io, NotConnected),
// reconnects: close, open, the connect step, with a backoff that doubles from
// backoff_min to backoff_max between attempts. A Safe call is then repeated
// until it succeeds or its window passes, and returns the last transient
// error at the window. An Unsafe call is never sent again: once the link is
// back (or the window has passed) it returns its error with code
// kOutcomeUnknown and its kind unchanged. Definite errors (Protocol, Config,
// Interlock) and Cancelled pass through at once.
//
// An op that is one transaction of Safe and Unsafe commands (a route, then
// the read that takes the instrument's error off its queue) marks a transient
// failure of its Unsafe part with code kOutcomeUnknown: that failure is
// handled as an Unsafe call's, whatever `repeat` says.
//
// Thread-safe. Each call runs `op` inside one transport transaction and reads
// the reconnect generation there; a reconnect (close, open, connect step) is
// one transaction too and moves the generation inside it. So the generation
// a caller saw is the one of the connection its op used: a caller whose
// failure another thread has already repaired just repeats, and one that
// failed on the new connection reconnects again. Concurrent failures on one
// connection reconnect once.
//
// The reconnect, its backoff included, runs under a ClockMutex: callers that
// fail meanwhile wait for it in the clock. `op` and the connect step run on
// the transport's own thread when it has one (QueuedTransport), so their
// calls on this transport run inline. Neither may call this Link again: it
// would hold the transaction while waiting for the reconnect, which waits for
// the transaction.
class Link {
 public:
  using Connect = std::function<Result<void>(Transport&)>;

  Link(Transport& transport, const Clock& clock, LinkPolicy policy = {}, SignalBus* bus = nullptr);

  // The step run after every reopen (Qtegra's handshake). None by default.
  // Set before the first call; not to be changed while calls run.
  void set_connect(Connect step);

  // Runs `op` under the policy; `window` replaces the policy's call_window.
  template <class T>
  [[nodiscard]] Result<T> call(const std::function<Result<T>()>& op, Repeat repeat,
                               std::optional<Duration> window = std::nullopt) {
    const TimePoint deadline = clock_.now() + window.value_or(policy_.call_window);
    Episode episode(deadline);
    std::uint64_t seen = 0;
    Result<T> r = run_once(op, seen);
    while (!r && transient(r.error().kind)) {
      Error error = std::move(r).error();
      if (!episode.failed_at) episode.failed_at = clock_.now();
      if (repeat == Repeat::Unsafe || error.code == kOutcomeUnknown) {
        // Brought back for the caller's read-back, whatever that gives.
        if (policy_.reconnect) (void)reconnect_since(seen, episode, error);
        error.code = kOutcomeUnknown;
        return fail(std::move(error));
      }
      if (!policy_.reconnect) return fail(std::move(error));
      if (auto back = reconnect_since(seen, episode, std::move(error)); !back) return fail(std::move(back).error());
      repeats_.fetch_add(1);
      r = run_once(op, seen);
    }
    return r;
  }

  // The connect step as a Safe call with no window: one try and, if that
  // fails, one reconnect. Without a connect step it opens the transport.
  [[nodiscard]] Result<void> probe();

  // Reconnects that succeeded.
  [[nodiscard]] std::uint64_t reconnects() const noexcept { return reconnects_.load(); }
  // Times a Safe call was sent again after a reconnect.
  [[nodiscard]] std::uint64_t repeats() const noexcept { return repeats_.load(); }

  [[nodiscard]] const LinkPolicy& policy() const noexcept { return policy_; }
  // The transport it recovers: its name and health() say whether it is up.
  [[nodiscard]] Transport& transport() const noexcept { return transport_; }

 private:
  // One call's recovery: its deadline, the wait before the next reopen
  // attempt (zero for the first) and how many attempts it has made since the
  // link was last up. The wait is kept across a reconnect that the repeated
  // call then finds gone again: a peer that accepts and drops at once is
  // tried at the backoff, not in a loop that takes no time.
  struct Episode {
    explicit Episode(TimePoint until) : deadline(until) {}
    TimePoint deadline;
    Duration next_wait{};
    std::uint64_t attempts = 0;
    std::optional<TimePoint> failed_at;
  };

  static bool transient(ErrorKind kind) noexcept { return is_transient(kind); }

  template <class T>
  Result<T> run_once(const std::function<Result<T>()>& op, std::uint64_t& seen) {
    std::optional<Result<T>> out;
    seen = generation_.load();  // kept if the transport never runs the body
    auto ran = transport_.transaction([&]() -> Result<void> {
      seen = generation_.load();
      out.emplace(op());
      return {};
    });
    if (out) return std::move(*out);
    return fail(std::move(ran).error());
  }

  // A link that came back, to be published once the reconnect lock is free.
  struct Back {
    std::uint64_t attempts = 0;
    Duration down_for{};
    TimePoint at{};
  };

  // Ok: the link is back (this caller reconnected it, or another did since
  // `seen`). An error: the window passed (the last transient error), or the
  // reopen or connect step failed definitely.
  Result<void> reconnect_since(std::uint64_t seen, Episode& episode, Error last);
  Result<std::optional<Back>> reconnect_locked(std::uint64_t seen, Episode& episode, Error last);
  // close, open, connect step, generation + 1: one transaction.
  Result<void> attempt();

  Transport& transport_;
  const Clock& clock_;
  LinkPolicy policy_;
  SignalBus* bus_;
  Connect connect_;
  ClockMutex mutex_;  // the reconnect, across its backoff
  std::atomic<std::uint64_t> generation_{0};
  std::atomic<std::uint64_t> reconnects_{0};
  std::atomic<std::uint64_t> repeats_{0};
};

}  // namespace pychron
