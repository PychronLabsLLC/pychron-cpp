#include "pychron/devices/reconnect.hpp"

namespace pychron {

Reconnector::Reconnector(Transport& transport, const Clock& clock, Duration min_interval)
    : transport_(transport), clock_(clock), min_interval_(min_interval), mutex_(clock) {}

Result<bool> Reconnector::reconnect_since(std::uint64_t seen, const std::function<Result<void>()>& on_connect) {
  std::lock_guard lock(mutex_);
  // Another caller reconnected after this one started its op: just retry.
  if (generation_.load() != seen) return true;

  const TimePoint now = clock_.now();
  if (last_attempt_ && now - *last_attempt_ < min_interval_) return false;
  last_attempt_ = now;

  transport_.close();
  if (auto opened = transport_.open(); !opened) return fail(std::move(opened).error());
  if (auto connected = on_connect(); !connected) return fail(std::move(connected).error());
  generation_.fetch_add(1);
  return true;
}

}  // namespace pychron
