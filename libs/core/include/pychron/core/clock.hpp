#pragma once

#include <chrono>
#include <condition_variable>
#include <mutex>

namespace pychron {

using Duration = std::chrono::steady_clock::duration;
using TimePoint = std::chrono::steady_clock::time_point;

// Monotonic time source, injected wherever time matters so tests can drive it.
class Clock {
 public:
  virtual ~Clock() = default;

  virtual TimePoint now() const = 0;

  // Block on `cv` (whose mutex `lock` holds) until notified or until this
  // clock reaches `deadline`. Spurious returns are allowed; callers re-check.
  virtual void wait_until(std::condition_variable& cv, std::unique_lock<std::mutex>& lock,
                          TimePoint deadline) const = 0;
};

// Real monotonic time (std::chrono::steady_clock).
class SteadyClock final : public Clock {
 public:
  TimePoint now() const override;
  void wait_until(std::condition_variable& cv, std::unique_lock<std::mutex>& lock,
                  TimePoint deadline) const override;
};

// Test clock: time only moves when advance()/set() is called.
class ManualClock final : public Clock {
 public:
  explicit ManualClock(TimePoint start = TimePoint{});

  TimePoint now() const override;
  // Waits a short real-time slice so waiters observe advance() promptly.
  void wait_until(std::condition_variable& cv, std::unique_lock<std::mutex>& lock,
                  TimePoint deadline) const override;

  void advance(Duration d);
  void set(TimePoint t);

 private:
  mutable std::mutex mutex_;
  TimePoint now_;
};

}  // namespace pychron
