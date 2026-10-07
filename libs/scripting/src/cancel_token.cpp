#include "pychron/scripting/cancel_token.hpp"

#include <algorithm>
#include <cassert>
#include <utility>

namespace pychron::scripting {

void CancelToken::notify_locked() {
  if (waiter_clock_ != nullptr) {
    waiter_clock_->notify_all(cv_);
  } else {
    cv_.notify_all();
  }
}

void CancelToken::call(const std::vector<std::shared_ptr<Callback>>& callbacks) {
  const auto self = std::this_thread::get_id();
  for (const auto& callback : callbacks) {
    {
      std::lock_guard lock(mutex_);
      if (callback->removed) continue;  // since the request, perhaps by an earlier callback
      callback->callers.push_back(self);
    }
    // Unlocked: the callback may take other locks, or use the token.
    callback->call();
    {
      std::lock_guard lock(mutex_);
      callback->callers.erase(std::find(callback->callers.begin(), callback->callers.end(), self));
      // Under the lock: the remover may destroy the token once it has returned.
      called_.notify_all();
    }
  }
}

void CancelToken::cancel() {
  std::vector<std::shared_ptr<Callback>> callbacks;
  {
    std::lock_guard lock(mutex_);
    int none = static_cast<int>(CancelMode::None);
    if (mode_.compare_exchange_strong(none, static_cast<int>(CancelMode::Cancel))) callbacks = callbacks_;
    notify_locked();
  }
  call(callbacks);
}

void CancelToken::abort() {
  std::vector<std::shared_ptr<Callback>> callbacks;
  {
    std::lock_guard lock(mutex_);
    const int abort = static_cast<int>(CancelMode::Abort);
    if (mode_.exchange(abort) != abort) callbacks = callbacks_;
    notify_locked();
  }
  call(callbacks);
}

void CancelToken::wake() {
  std::lock_guard lock(mutex_);
  ++wakes_;
  notify_locked();
}

void CancelToken::reset() {
  std::lock_guard lock(mutex_);
  mode_.store(static_cast<int>(CancelMode::None));
}

WaitResult CancelToken::wait_until(const Clock& clock, TimePoint deadline) {
  std::unique_lock lock(mutex_);
  // A request is notified through the clock its waiters are on.
  assert(waiter_clock_ == nullptr || waiter_clock_ == &clock);
  waiter_clock_ = &clock;
  ++waiters_;
  struct Leave {
    CancelToken& token;
    // The lock is held again whenever the wait is left.
    ~Leave() {
      if (--token.waiters_ == 0) token.waiter_clock_ = nullptr;
    }
  } leave{*this};

  const auto wakes = wakes_;
  while (true) {
    if (requested()) return WaitResult::Cancelled;
    if (wakes_ != wakes) return WaitResult::Woken;
    if (clock.now() >= deadline) return WaitResult::Elapsed;
    clock.wait_until(cv_, lock, deadline);
  }
}

std::uint64_t CancelToken::add_on_cancel(std::function<void()> callback) {
  auto entry = std::make_shared<Callback>();
  entry->call = std::move(callback);
  bool now = false;
  {
    std::lock_guard lock(mutex_);
    entry->id = next_callback_++;
    callbacks_.push_back(entry);
    // A request made from here on finds the callback in the list; one made
    // before did not.
    now = requested();
  }
  if (now) call({entry});
  return entry->id;
}

void CancelToken::remove_on_cancel(std::uint64_t id) {
  std::unique_lock lock(mutex_);
  auto it = std::find_if(callbacks_.begin(), callbacks_.end(), [id](const auto& c) { return c->id == id; });
  if (it == callbacks_.end()) return;
  const auto entry = *it;
  callbacks_.erase(it);
  entry->removed = true;
  // A call on this thread is the caller's own and cannot be waited for.
  const auto self = std::this_thread::get_id();
  called_.wait(lock, [&] {
    return std::all_of(entry->callers.begin(), entry->callers.end(), [&](auto t) { return t == self; });
  });
}

}  // namespace pychron::scripting
