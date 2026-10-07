#pragma once

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "pychron/core/clock.hpp"

namespace pychron {

// Simulated time for a program of several threads: it stands still while any
// thread has work to do and jumps to the next deadline when none has.
//
// Model:
//   participant - a thread between Clock::Participant's construction and its
//                 destruction. Time waits for participants and for nobody else.
//   blocked     - a participant inside wait()/wait_until() that has not been
//                 woken, or one inside Clock::Detached. Every other
//                 participant is runnable.
//   jump        - when a thread blocks, leaves or detaches and that leaves no
//                 runnable participant, now() becomes the earliest deadline
//                 among the waiters and every waiter due by then is woken.
//                 With no timed waiter nothing happens: the program is idle
//                 until a thread outside the clock notifies.
//
// There is no pump thread: the thread that blocks last does the jump.
//
// A waiter does not sleep on the caller's condition variable, which is only a
// name here: it sleeps on one of its own, under the clock's mutex. So
// notify_*() marks a waiter runnable at the instant of the call, not when the
// kernel gets round to it, and time cannot jump past work that is about to be
// done. It also means a raw cv.notify_*() is not heard: a condition variable
// waited on through this clock is notified through it.
//
// Lock order: the caller's mutex, then the clock's. The clock never takes a
// caller's mutex while it holds its own.
//
// The rule for users: a participant never waits for another participant
// except through the clock. One blocked some other way (a raw cv.wait, a
// join, a socket read) looks runnable, so time stops; Clock::Detached is for
// the waits that really are on the outside world. Threads that are not
// participants may call everything; their waits are woken by a jump like any
// other but never hold time back.
class VirtualClock final : public Clock {
 public:
  struct Options {
    double speed = std::numeric_limits<double>::infinity();  // simulated seconds per real second
    TimePoint start = TimePoint{} + std::chrono::hours(1);
    WallTime epoch = WallTime{} + std::chrono::hours(24 * 365 * 56);
    Duration stall_report_after = std::chrono::seconds(10);  // real; zero disables
    std::function<void(std::string)> on_stall;               // default writes to stderr
  };

  VirtualClock();
  explicit VirtualClock(Options options);
  ~VirtualClock() override;

  TimePoint now() const override;
  // `epoch` plus the time jumped since `start`.
  WallTime wall_now() const override;
  void wait_until(std::condition_variable& cv, std::unique_lock<std::mutex>& lock,
                  TimePoint deadline) const override;
  void wait(std::condition_variable& cv, std::unique_lock<std::mutex>& lock) const override;
  // Both wake every waiter on `cv`: the clock cannot tell which one the caller
  // meant, and a spurious return is allowed.
  void notify_one(std::condition_variable& cv) const override;
  void notify_all(std::condition_variable& cv) const override;

  void set_speed(double speed);
  double speed() const;

  // Threads between enter and leave; a nested guard counts once. For tests.
  std::size_t participants() const;

 protected:
  void enter(std::string_view name) const override;
  void leave() const override;
  void detach() const override;
  void reattach() const override;

 private:
  struct Member {
    std::string name;
    std::size_t depth = 1;     // nested Participant guards
    std::size_t detached = 0;  // nested Detached guards
  };

  // One thread inside wait()/wait_until(). `key` is the address of the
  // caller's condition variable and is never dereferenced.
  struct Waiter {
    const void* key = nullptr;
    std::optional<TimePoint> deadline;
    bool runnable = false;
    std::thread::id owner;
    std::condition_variable wake;
  };

  void block(std::condition_variable& cv, std::unique_lock<std::mutex>& lock,
             std::optional<TimePoint> deadline) const;
  void notify(std::condition_variable& cv) const;
  void maybe_jump_locked() const;

  const Options options_;
  mutable std::mutex m_;
  mutable TimePoint now_;
  mutable double speed_;
  mutable std::map<std::thread::id, Member> members_;
  mutable std::vector<std::shared_ptr<Waiter>> waiters_;
};

}  // namespace pychron
