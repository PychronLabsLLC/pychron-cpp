#pragma once

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string_view>

namespace pychron {

using Duration = std::chrono::steady_clock::duration;
using TimePoint = std::chrono::steady_clock::time_point;
using WallTime = std::chrono::system_clock::time_point;

// Monotonic time source, injected wherever time matters so tests can drive it.
class Clock {
 public:
  virtual ~Clock() = default;

  virtual TimePoint now() const = 0;

  // Block on `cv` (whose mutex `lock` holds) until notified or until this
  // clock reaches `deadline`. Spurious returns are allowed; callers re-check.
  virtual void wait_until(std::condition_variable& cv, std::unique_lock<std::mutex>& lock,
                          TimePoint deadline) const = 0;

  // Calendar time, for stamps that are written down (the monotonic now() is not).
  virtual WallTime wall_now() const = 0;

  // Block on `cv` (whose mutex `lock` holds) until notified, with no deadline.
  // Spurious returns are allowed; callers re-check.
  virtual void wait(std::condition_variable& cv, std::unique_lock<std::mutex>& lock) const = 0;

  // A condition variable that is waited on through a clock is notified through
  // that clock, after its state has been changed under the waiters' mutex.
  virtual void notify_one(std::condition_variable& cv) const = 0;
  virtual void notify_all(std::condition_variable& cv) const = 0;

  // Block the calling thread for `d` of this clock's time.
  void sleep_for(Duration d) const;

  // A thread that takes part in a clock's time keeps one of these for as long
  // as it does. Nothing happens on the clocks that need no bookkeeping.
  class Participant {
   public:
    Participant(const Clock& clock, std::string_view name);
    ~Participant();
    Participant(const Participant&) = delete;
    Participant& operator=(const Participant&) = delete;

   private:
    const Clock& clock_;
  };

  // A participant that is about to block on something outside the clock holds
  // one of these, so the clock does not wait for it.
  class Detached {
   public:
    explicit Detached(const Clock& clock);
    ~Detached();
    Detached(const Detached&) = delete;
    Detached& operator=(const Detached&) = delete;

   private:
    const Clock& clock_;
  };

  // While one is alive this clock does not jump. A thread that starts a
  // participant thread makes one (shared) before std::thread and gives the
  // child a copy; the child drops it once its Participant is constructed.
  // Otherwise a starter that blocks first lets time go past the child, which
  // the clock has not heard of yet.
  class Hold {
   public:
    explicit Hold(const Clock& clock);
    ~Hold();
    Hold(const Hold&) = delete;
    Hold& operator=(const Hold&) = delete;

   private:
    const Clock& clock_;
  };

 protected:
  virtual void enter(std::string_view name) const;
  virtual void leave() const;
  virtual void detach() const;
  virtual void reattach() const;
  virtual void hold() const;
  virtual void unhold() const;
};

// Real monotonic time (std::chrono::steady_clock).
class SteadyClock final : public Clock {
 public:
  TimePoint now() const override;
  void wait_until(std::condition_variable& cv, std::unique_lock<std::mutex>& lock,
                  TimePoint deadline) const override;
  WallTime wall_now() const override;
  void wait(std::condition_variable& cv, std::unique_lock<std::mutex>& lock) const override;
  void notify_one(std::condition_variable& cv) const override;
  void notify_all(std::condition_variable& cv) const override;
};

// Test clock: time only moves when advance()/set() is called.
class ManualClock final : public Clock {
 public:
  explicit ManualClock(TimePoint start = TimePoint{}, WallTime epoch = WallTime{});

  TimePoint now() const override;
  // Waits a short real-time slice so waiters observe advance() promptly.
  void wait_until(std::condition_variable& cv, std::unique_lock<std::mutex>& lock,
                  TimePoint deadline) const override;
  // `epoch` plus the time advanced since `start`.
  WallTime wall_now() const override;
  void wait(std::condition_variable& cv, std::unique_lock<std::mutex>& lock) const override;
  void notify_one(std::condition_variable& cv) const override;
  void notify_all(std::condition_variable& cv) const override;

  void advance(Duration d);
  void set(TimePoint t);

 private:
  mutable std::mutex mutex_;
  TimePoint now_;
  TimePoint start_;
  WallTime epoch_;
};

}  // namespace pychron
