#include "pychron/devices/link.hpp"

#include <algorithm>
#include <mutex>

#include "pychron/core/events.hpp"
#include "pychron/core/log_context.hpp"
#include "pychron/core/log_error.hpp"
#include "pychron/core/process_log.hpp"
#include "pychron/core/signal_bus.hpp"

namespace pychron {

LinkPolicy link_policy(const config::RecoveryConfig& recovery, const LinkPolicy& defaults) {
  LinkPolicy policy = defaults;
  policy.reconnect = recovery.reconnect;
  policy.optional = recovery.optional;
  if (recovery.call_window_s)
    policy.call_window =
        std::chrono::duration_cast<Duration>(std::chrono::duration<double>(*recovery.call_window_s));
  return policy;
}

Link::Link(Transport& transport, const Clock& clock, LinkPolicy policy, SignalBus* bus)
    : transport_(transport),
      clock_(clock),
      policy_(policy),
      bus_(bus),
      log_(process_logger("device." + transport.name())),
      mutex_(clock) {
  // A zero backoff would retry a dead link without letting time pass.
  policy_.backoff_min = std::max(policy_.backoff_min, Duration(std::chrono::milliseconds(1)));
  policy_.backoff_max = std::max(policy_.backoff_max, policy_.backoff_min);
}

void Link::set_connect(Connect step) { connect_ = std::move(step); }

Result<void> Link::probe() {
  return call<void>(
      [this]() -> Result<void> { return connect_ ? connect_(transport_) : transport_.open(); }, Repeat::Safe,
      Duration::zero());
}

Result<void> Link::attempt() {
  return transport_.transaction([this]() -> Result<void> {
    transport_.close();
    if (auto opened = transport_.open(); !opened) return opened;
    if (connect_) {
      if (auto connected = connect_(transport_); !connected) return connected;
    }
    generation_.fetch_add(1);
    return {};
  });
}

Result<void> Link::reconnect_since(std::uint64_t seen, Episode& episode, Error last) {
  auto back = reconnect_locked(seen, episode, std::move(last));
  if (!back) return fail(std::move(back).error());
  // Outside the lock: a subscriber may make a call on this link.
  if (*back && bus_ != nullptr) {
    bus_->publish(LinkReconnected{transport_.name(), (*back)->attempts, (*back)->down_for, (*back)->at});
  }
  return {};
}

Result<std::optional<Link::Back>> Link::reconnect_locked(std::uint64_t seen, Episode& episode, Error last) {
  std::lock_guard lock(mutex_);
  // Another caller reconnected after this one's op ran: just repeat.
  if (generation_.load() != seen) return std::optional<Back>{};

  for (;;) {
    if (episode.next_wait > Duration::zero()) {
      const TimePoint now = clock_.now();
      if (now >= episode.deadline) return fail(std::move(last));
      clock_.sleep_for(std::min(episode.next_wait, episode.deadline - now));
    }
    ++episode.attempts;
    episode.next_wait = episode.next_wait == Duration::zero()
                            ? policy_.backoff_min
                            : std::min(episode.next_wait * 2, policy_.backoff_max);
    auto r = attempt();
    if (r) {
      reconnects_.fetch_add(1);
      const TimePoint now = clock_.now();
      Back back{episode.attempts, now - episode.failed_at.value_or(now), now};
      episode.attempts = 0;
      episode.failed_at.reset();
      reconnect_failing_ = false;  // the outage is over
      return std::optional<Back>(back);
    }
    // Handled: the backoff tries again, or the caller's call fails with it.
    // A poll (a scan) tries a dead link every few seconds: only the first
    // failed attempt of the outage warns, the rest are trace.
    const LogLevel level = reconnect_failing_ && LogContext::polling() ? LogLevel::Trace : LogLevel::Warn;
    reconnect_failing_ = true;
    if (log_.enabled(level)) log_handled(log_, level, r.error(), "reconnect " + transport_.name());
    if (!transient(r.error().kind)) return fail(std::move(r).error());
    last = std::move(r).error();
  }
}

}  // namespace pychron
