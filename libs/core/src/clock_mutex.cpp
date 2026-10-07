#include "pychron/core/clock_mutex.hpp"

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

}  // namespace pychron
