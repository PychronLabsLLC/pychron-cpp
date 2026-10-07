#pragma once

#include <condition_variable>
#include <mutex>

#include "pychron/core/clock.hpp"

namespace pychron {

// A mutex for state that is held across a wait in clock time (a command in
// flight, a connect). A contended lock() waits through the clock, so a
// VirtualClock sees the waiter as blocked: behind a std::mutex it would look
// runnable, time would stand, and the holder's wait would never end.
//
// Lockable, so std::lock_guard, std::unique_lock and std::scoped_lock take
// it. Not for use with std::condition_variable: a short critical section
// keeps std::mutex. Not recursive, and no order among contenders. The clock
// must outlive it.
class ClockMutex {
 public:
  explicit ClockMutex(const Clock& clock);
  ClockMutex(const ClockMutex&) = delete;
  ClockMutex& operator=(const ClockMutex&) = delete;

  void lock();
  bool try_lock();
  void unlock();

 private:
  const Clock& clock_;
  std::mutex inner_;                 // held_, and never across a wait
  std::condition_variable released_;  // waited on and notified through clock_
  bool held_ = false;
};

}  // namespace pychron
