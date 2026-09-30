#include "pychron/scripting/cancel_token.hpp"

namespace pychron::scripting {

void CancelToken::cancel() {
  {
    std::lock_guard lock(mutex_);
    int none = static_cast<int>(CancelMode::None);
    mode_.compare_exchange_strong(none, static_cast<int>(CancelMode::Cancel));
  }
  cv_.notify_all();
}

void CancelToken::abort() {
  {
    std::lock_guard lock(mutex_);
    mode_.store(static_cast<int>(CancelMode::Abort));
  }
  cv_.notify_all();
}

void CancelToken::wake() {
  {
    std::lock_guard lock(mutex_);
    ++wakes_;
  }
  cv_.notify_all();
}

void CancelToken::reset() {
  std::lock_guard lock(mutex_);
  mode_.store(static_cast<int>(CancelMode::None));
}

WaitResult CancelToken::wait_until(const Clock& clock, TimePoint deadline) {
  std::unique_lock lock(mutex_);
  const auto wakes = wakes_;
  while (true) {
    if (requested()) return WaitResult::Cancelled;
    if (wakes_ != wakes) return WaitResult::Woken;
    if (clock.now() >= deadline) return WaitResult::Elapsed;
    clock.wait_until(cv_, lock, deadline);
  }
}

}  // namespace pychron::scripting
