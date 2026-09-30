#include "pychron/core/clock.hpp"

namespace pychron {

TimePoint SteadyClock::now() const { return std::chrono::steady_clock::now(); }

void SteadyClock::wait_until(std::condition_variable& cv, std::unique_lock<std::mutex>& lock,
                             TimePoint deadline) const {
  cv.wait_until(lock, deadline);
}

ManualClock::ManualClock(TimePoint start) : now_(start) {}

TimePoint ManualClock::now() const {
  std::lock_guard lock(mutex_);
  return now_;
}

void ManualClock::wait_until(std::condition_variable& cv, std::unique_lock<std::mutex>& lock,
                             TimePoint deadline) const {
  if (now() >= deadline) return;
  cv.wait_for(lock, std::chrono::milliseconds(1));
}

void ManualClock::advance(Duration d) {
  std::lock_guard lock(mutex_);
  now_ += d;
}

void ManualClock::set(TimePoint t) {
  std::lock_guard lock(mutex_);
  now_ = t;
}

}  // namespace pychron
