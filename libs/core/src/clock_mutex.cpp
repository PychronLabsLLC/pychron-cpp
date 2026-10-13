#include "pychron/core/clock_mutex.hpp"

#include <cassert>

namespace pychron {

ClockMutex::ClockMutex(const Clock& clock) : clock_(clock) {}

void ClockMutex::lock() {
  std::unique_lock inner(inner_);
  while (held_) clock_.wait(released_, inner);
  held_ = true;
}

bool ClockMutex::try_lock() {
  std::lock_guard inner(inner_);
  if (held_) return false;
  held_ = true;
  return true;
}

void ClockMutex::unlock() {
  // Notified with inner_ held: the next holder may unlock and destroy this
  // mutex, and it cannot get that far before this call is done with it.
  std::lock_guard inner(inner_);
  held_ = false;
  clock_.notify_one(released_);
}

RecursiveClockMutex::RecursiveClockMutex(const Clock& clock) : clock_(clock) {}

void RecursiveClockMutex::lock() {
  const std::thread::id self = std::this_thread::get_id();
  std::unique_lock inner(inner_);
  // owner_ is read under inner_: it is this thread's only if this thread
  // wrote it, and nobody's id is no thread's.
  if (owner_ == self) {
    ++depth_;
    return;
  }
  while (depth_ != 0) clock_.wait(released_, inner);
  owner_ = self;
  depth_ = 1;
}

bool RecursiveClockMutex::try_lock() {
  const std::thread::id self = std::this_thread::get_id();
  std::lock_guard inner(inner_);
  if (owner_ == self) {
    ++depth_;
    return true;
  }
  if (depth_ != 0) return false;
  owner_ = self;
  depth_ = 1;
  return true;
}

void RecursiveClockMutex::unlock() {
  // Notified with inner_ held, as in ClockMutex::unlock.
  std::lock_guard inner(inner_);
  assert(depth_ != 0 && owner_ == std::this_thread::get_id());
  if (--depth_ != 0) return;
  owner_ = std::thread::id{};
  clock_.notify_one(released_);
}

}  // namespace pychron
