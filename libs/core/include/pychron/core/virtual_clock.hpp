#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
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
// waited on through this clock is notified through it. notify_*() does call
// the real condition variable as well (for a thread that waits on it
// directly), so that must still be alive, as with any condition variable.
//
// Pacing: at a finite speed a jump of D is first paid for with D / speed of
// real time. One thread at a time sleeps that off, with the clock's mutex
// released, and it is always a thread that is asleep in a wait anyway: one
// that leaves, detaches or drops a Hold hands the sleep to the waiter with
// the earliest deadline and returns at once. Anything that may have made a
// thread runnable (a notify that woke a waiter, a new participant, the end
// of a Detached, a Hold) or changed the price (set_speed) ends the sleep, and
// the question whether to jump is asked again at once. Time is continuous
// meanwhile: during the sleep now() and wall_now() advance at `speed`, an
// interruption leaves them where they had got to, and a jump still pending
// goes on from there. A speed that is not positive stops time. At infinite
// speed there is no sleep and no real time at all.
//
// Clock::Hold: while one is alive there is no jump and no new pacing sleep;
// the last one to go asks the question again.
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
//
// The stall report is how a broken rule shows: a watchdog thread (none when
// stall_report_after is zero) calls on_stall, once per stall, when for that
// long in real time a timed waiter has been asleep, now() has not moved and
// nobody was pacing. The message names the runnable participants, or says
// that the clock is held when there is none; on_stall runs on the watchdog's
// thread with the clock's mutex released.
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

  // Takes effect at once, also on a jump being paced. Zero or less stops time.
  void set_speed(double speed);
  double speed() const;

  // Threads between enter and leave; a nested guard counts once. For tests.
  std::size_t participants() const;
  // Threads asleep in wait()/wait_until(): registered and not woken. For tests.
  std::size_t waiters() const;

 protected:
  void enter(std::string_view name) const override;
  void leave() const override;
  void detach() const override;
  void reattach() const override;
  void hold() const override;
  void unhold() const override;

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
    bool nominated = false;  // asked to do the pacing sleep
    std::thread::id owner;
    std::condition_variable wake;
  };

  void block(std::condition_variable& cv, std::unique_lock<std::mutex>& lock,
             std::optional<TimePoint> deadline) const;
  void notify(std::condition_variable& cv) const;
  // `self` is the calling thread's own record when it is inside block(), and
  // null otherwise. Only a thread whose record is not runnable may pace.
  void maybe_jump_locked(std::unique_lock<std::mutex>& guard, Waiter* self) const;
  TimePoint current_locked(std::chrono::steady_clock::time_point real_now) const;
  void advance_locked(TimePoint to) const;
  void interrupt_locked() const;
  void watch();

  const Options options_;
  mutable std::mutex m_;
  mutable TimePoint now_;
  double speed_;
  mutable std::map<std::thread::id, Member> members_;
  mutable std::vector<std::shared_ptr<Waiter>> waiters_;
  mutable std::size_t holds_ = 0;

  // The pacing sleep: from `pacing_` set until its thread has woken, with
  // `interrupt_` set once it has been abandoned. Until then the time is not
  // `now_` but current_locked().
  mutable bool pacing_ = false;
  mutable bool interrupt_ = false;
  mutable TimePoint pace_target_;
  mutable double pace_speed_ = 0;
  mutable std::chrono::steady_clock::time_point pace_real_start_;
  // The real instant `now_` was reached at, while the jump it was on the way
  // to may still be pending.
  mutable std::optional<std::chrono::steady_clock::time_point> resume_from_;
  mutable std::condition_variable pace_cv_;

  // Counts the times now() has moved; the watchdog compares it.
  mutable std::uint64_t advances_ = 0;
  bool stop_ = false;
  std::condition_variable watchdog_cv_;
  std::thread watchdog_;
};

}  // namespace pychron
