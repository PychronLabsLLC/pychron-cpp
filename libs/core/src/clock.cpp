#include "pychron/core/clock.hpp"

namespace pychron {

void Clock::sleep_for(Duration d) const {
  if (d <= Duration::zero()) return;
  const TimePoint deadline = now() + d;
  std::mutex m;
  std::condition_variable cv;
  std::unique_lock lock(m);
  while (now() < deadline) wait_until(cv, lock, deadline);
}

Clock::Participant::Participant(const Clock& clock, std::string_view name) : clock_(clock) {
  clock_.enter(name);
}
Clock::Participant::~Participant() { clock_.leave(); }

Clock::Detached::Detached(const Clock& clock) : clock_(clock) { clock_.detach(); }
Clock::Detached::~Detached() { clock_.reattach(); }

void Clock::enter(std::string_view) const {}
void Clock::leave() const {}
void Clock::detach() const {}
void Clock::reattach() const {}

TimePoint SteadyClock::now() const { return std::chrono::steady_clock::now(); }

void SteadyClock::wait_until(std::condition_variable& cv, std::unique_lock<std::mutex>& lock,
                             TimePoint deadline) const {
  cv.wait_until(lock, deadline);
}

WallTime SteadyClock::wall_now() const { return std::chrono::system_clock::now(); }

void SteadyClock::wait(std::condition_variable& cv, std::unique_lock<std::mutex>& lock) const {
  cv.wait(lock);
}

void SteadyClock::notify_one(std::condition_variable& cv) const { cv.notify_one(); }

void SteadyClock::notify_all(std::condition_variable& cv) const { cv.notify_all(); }

ManualClock::ManualClock(TimePoint start, WallTime epoch)
    : now_(start), start_(start), epoch_(epoch) {}

WallTime ManualClock::wall_now() const {
  return epoch_ + std::chrono::duration_cast<WallTime::duration>(now() - start_);
}

void ManualClock::wait(std::condition_variable& cv, std::unique_lock<std::mutex>& lock) const {
  cv.wait(lock);
}

void ManualClock::notify_one(std::condition_variable& cv) const { cv.notify_one(); }

void ManualClock::notify_all(std::condition_variable& cv) const { cv.notify_all(); }

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
