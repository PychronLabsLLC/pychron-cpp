#include "pychron/core/virtual_clock.hpp"

#include <algorithm>
#include <utility>

namespace pychron {

VirtualClock::VirtualClock() : VirtualClock(Options{}) {}

VirtualClock::VirtualClock(Options options)
    : options_(std::move(options)), now_(options_.start), speed_(options_.speed) {}

VirtualClock::~VirtualClock() = default;

TimePoint VirtualClock::now() const {
  std::lock_guard guard(m_);
  return now_;
}

WallTime VirtualClock::wall_now() const {
  return options_.epoch + std::chrono::duration_cast<WallTime::duration>(now() - options_.start);
}

void VirtualClock::wait_until(std::condition_variable& cv, std::unique_lock<std::mutex>& lock,
                              TimePoint deadline) const {
  block(cv, lock, deadline);
}

void VirtualClock::wait(std::condition_variable& cv, std::unique_lock<std::mutex>& lock) const {
  block(cv, lock, std::nullopt);
}

void VirtualClock::notify_one(std::condition_variable& cv) const { notify(cv); }

void VirtualClock::notify_all(std::condition_variable& cv) const { notify(cv); }

void VirtualClock::set_speed(double speed) {
  std::lock_guard guard(m_);
  speed_ = speed;
}

double VirtualClock::speed() const {
  std::lock_guard guard(m_);
  return speed_;
}

std::size_t VirtualClock::participants() const {
  std::lock_guard guard(m_);
  return members_.size();
}

void VirtualClock::enter(std::string_view name) const {
  std::lock_guard guard(m_);
  auto [it, inserted] = members_.try_emplace(std::this_thread::get_id());
  if (inserted) {
    it->second.name = std::string(name);
  } else {
    ++it->second.depth;
  }
}

void VirtualClock::leave() const {
  std::lock_guard guard(m_);
  const auto it = members_.find(std::this_thread::get_id());
  if (it == members_.end()) return;
  if (--it->second.depth > 0) return;
  members_.erase(it);
  // This thread may have been the last runnable one.
  maybe_jump_locked();
}

void VirtualClock::detach() const {
  std::lock_guard guard(m_);
  const auto it = members_.find(std::this_thread::get_id());
  if (it == members_.end()) return;
  ++it->second.detached;
  maybe_jump_locked();
}

void VirtualClock::reattach() const {
  std::lock_guard guard(m_);
  const auto it = members_.find(std::this_thread::get_id());
  if (it == members_.end() || it->second.detached == 0) return;
  --it->second.detached;
}

void VirtualClock::block(std::condition_variable& cv, std::unique_lock<std::mutex>& lock,
                         std::optional<TimePoint> deadline) const {
  std::unique_lock guard(m_);
  if (deadline && now_ >= *deadline) return;

  auto waiter = std::make_shared<Waiter>();
  waiter->key = &cv;
  waiter->deadline = deadline;
  waiter->owner = std::this_thread::get_id();
  waiters_.push_back(waiter);

  // If that was the last runnable participant, time moves now; when this
  // waiter's own deadline is the earliest it comes back runnable.
  maybe_jump_locked();

  // The clock's mutex is held from the registration above until the sleep
  // below gives it up, so a notifier that changes the caller's state after
  // this unlock finds the waiter registered: no wake-up is lost.
  lock.unlock();
  waiter->wake.wait(guard, [&] { return waiter->runnable; });
  waiters_.erase(std::find(waiters_.begin(), waiters_.end(), waiter));
  guard.unlock();

  // Only now, with the clock's mutex released (lock order).
  lock.lock();
}

void VirtualClock::notify(std::condition_variable& cv) const {
  {
    std::lock_guard guard(m_);
    for (const auto& waiter : waiters_) {
      if (waiter->key != &cv || waiter->runnable) continue;
      waiter->runnable = true;
      waiter->wake.notify_all();
    }
  }
  // For a thread that blocked on the condition variable directly.
  cv.notify_all();
}

void VirtualClock::maybe_jump_locked() const {
  // A waiter that is not a participant holds nothing back, so waking only
  // such waiters leaves nobody runnable and the next deadline is taken too.
  for (;;) {
    // A thread is inside at most one wait, so each non-runnable waiter owned
    // by a member is one more blocked member. A member that has been woken
    // but has not yet returned counts as runnable.
    std::size_t blocked = 0;
    for (const auto& [id, member] : members_) {
      if (member.detached > 0) ++blocked;
    }
    std::optional<TimePoint> earliest;
    for (const auto& waiter : waiters_) {
      if (waiter->runnable) continue;
      const auto it = members_.find(waiter->owner);
      if (it != members_.end() && it->second.detached == 0) ++blocked;
      if (waiter->deadline && (!earliest || *waiter->deadline < *earliest)) {
        earliest = waiter->deadline;
      }
    }
    if (blocked < members_.size()) return;  // somebody has work to do
    if (!earliest) return;                  // idle until a thread outside notifies

    // Never backwards: a waiter whose deadline had passed was not registered,
    // and every jump wakes all that are due.
    now_ = *earliest;
    for (const auto& waiter : waiters_) {
      if (waiter->runnable || !waiter->deadline || *waiter->deadline > now_) continue;
      waiter->runnable = true;
      waiter->wake.notify_all();
    }
  }
}

}  // namespace pychron
