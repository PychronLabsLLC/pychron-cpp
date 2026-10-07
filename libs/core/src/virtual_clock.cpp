#include "pychron/core/virtual_clock.hpp"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <utility>

namespace pychron {

namespace {

using Real = std::chrono::steady_clock;
using Seconds = std::chrono::duration<double>;

constexpr double kUnpaced = std::numeric_limits<double>::infinity();
// A pacing sleep longer than this (thirty years) has no end of its own.
constexpr double kLongestSleep = 1e9;

}  // namespace

VirtualClock::VirtualClock() : VirtualClock(Options{}) {}

VirtualClock::VirtualClock(Options options)
    : options_(std::move(options)), now_(options_.start), speed_(options_.speed) {
  if (options_.stall_report_after > Duration::zero()) {
    watchdog_ = std::thread([this] { watch(); });
  }
}

VirtualClock::~VirtualClock() {
  if (watchdog_.joinable()) {
    {
      std::lock_guard guard(m_);
      stop_ = true;
    }
    watchdog_cv_.notify_all();
    watchdog_.join();
  }
  // A clock outlives every thread that uses it.
  assert(waiters_.empty() && members_.empty() && holds_ == 0);
}

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
  // A jump being paced was priced at the old speed.
  interrupt_locked();
}

double VirtualClock::speed() const {
  std::lock_guard guard(m_);
  return speed_;
}

std::size_t VirtualClock::participants() const {
  std::lock_guard guard(m_);
  return members_.size();
}

std::size_t VirtualClock::waiters() const {
  std::lock_guard guard(m_);
  return static_cast<std::size_t>(std::count_if(
      waiters_.begin(), waiters_.end(), [](const auto& waiter) { return !waiter->runnable; }));
}

void VirtualClock::enter(std::string_view name) const {
  std::lock_guard guard(m_);
  auto [it, inserted] = members_.try_emplace(std::this_thread::get_id());
  if (inserted) {
    it->second.name = std::string(name);
  } else {
    ++it->second.depth;
  }
  interrupt_locked();
}

void VirtualClock::leave() const {
  std::unique_lock guard(m_);
  const auto it = members_.find(std::this_thread::get_id());
  if (it == members_.end()) return;
  if (--it->second.depth > 0) return;
  members_.erase(it);
  // This thread may have been the last runnable one.
  maybe_jump_locked(guard, nullptr);
}

void VirtualClock::detach() const {
  std::unique_lock guard(m_);
  const auto it = members_.find(std::this_thread::get_id());
  if (it == members_.end()) return;
  ++it->second.detached;
  maybe_jump_locked(guard, nullptr);
}

void VirtualClock::reattach() const {
  std::lock_guard guard(m_);
  const auto it = members_.find(std::this_thread::get_id());
  if (it == members_.end() || it->second.detached == 0) return;
  if (--it->second.detached == 0) interrupt_locked();
}

void VirtualClock::hold() const {
  std::lock_guard guard(m_);
  ++holds_;
  // A jump being paced stops where it has got to.
  interrupt_locked();
}

void VirtualClock::unhold() const {
  std::unique_lock guard(m_);
  assert(holds_ > 0);
  if (--holds_ == 0) maybe_jump_locked(guard, nullptr);
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

  // A jump being paced is to a later deadline than this one.
  if (pacing_ && deadline && *deadline < pace_target_) interrupt_locked();

  // The caller's mutex goes first: the pacing sleep below must not keep a
  // notifier out of it. The clock's mutex is held from the registration above
  // to the first sleep, so a notifier that changes the caller's state after
  // this unlock finds the waiter registered: no wake-up is lost.
  lock.unlock();

  // If that was the last runnable participant, time moves now; when this
  // waiter's own deadline is the earliest it comes back runnable.
  maybe_jump_locked(guard, waiter.get());
  while (!waiter->runnable) {
    waiter->wake.wait(guard, [&] { return waiter->runnable || waiter->nominated; });
    if (waiter->runnable) break;
    // A thread that could not stay has handed over the pacing sleep.
    waiter->nominated = false;
    maybe_jump_locked(guard, waiter.get());
  }
  waiters_.erase(std::find(waiters_.begin(), waiters_.end(), waiter));
  // Asked to pace and woken for another reason in the same instant: if this
  // thread holds nothing back, the request goes to the next waiter.
  if (waiter->nominated) maybe_jump_locked(guard, nullptr);
  guard.unlock();

  // Only now, with the clock's mutex released (lock order).
  lock.lock();
}

void VirtualClock::notify(std::condition_variable& cv) const {
  {
    std::lock_guard guard(m_);
    bool woke = false;
    for (const auto& waiter : waiters_) {
      if (waiter->key != &cv || waiter->runnable) continue;
      waiter->runnable = true;
      waiter->wake.notify_all();
      woke = true;
    }
    if (woke) interrupt_locked();
  }
  // For a thread that blocked on the condition variable directly.
  cv.notify_all();
}

void VirtualClock::interrupt_locked() const {
  if (!pacing_ || interrupt_) return;
  interrupt_ = true;
  pace_cv_.notify_all();
  // The jump is abandoned and time moves by the part that has been paid for,
  // here and not when the pacing thread gets to run: a thread woken by the
  // caller reads the time the interruption happened at.
  const Seconds delta = pace_target_ - now_;
  const Seconds paid = Seconds(Real::now() - pace_real_start_) * pace_speed_;
  advance_locked(paid < delta ? now_ + std::chrono::duration_cast<Duration>(paid) : pace_target_);
}

void VirtualClock::advance_locked(TimePoint to) const {
  // Never backwards: a waiter whose deadline had passed was not registered,
  // and every advance wakes all that are due.
  now_ = to;
  ++advances_;
  for (const auto& waiter : waiters_) {
    if (waiter->runnable || !waiter->deadline || *waiter->deadline > now_) continue;
    waiter->runnable = true;
    waiter->wake.notify_all();
  }
}

void VirtualClock::maybe_jump_locked(std::unique_lock<std::mutex>& guard, Waiter* self) const {
  // A waiter that is not a participant holds nothing back, so waking only
  // such waiters leaves nobody runnable and the next deadline is taken too.
  // An interrupted pacing sleep comes round again as well: whoever ended it
  // may have left nobody runnable either.
  for (;;) {
    // The pacing thread asks again when its sleep ends, the last Hold when
    // it goes.
    if (pacing_ || holds_ > 0) return;

    // A thread is inside at most one wait, so each non-runnable waiter owned
    // by a member is one more blocked member. A member that has been woken
    // but has not yet returned counts as runnable.
    std::size_t blocked = 0;
    for (const auto& [id, member] : members_) {
      if (member.detached > 0) ++blocked;
    }
    Waiter* first = nullptr;
    for (const auto& waiter : waiters_) {
      if (waiter->runnable) continue;
      const auto it = members_.find(waiter->owner);
      if (it != members_.end() && it->second.detached == 0) ++blocked;
      if (waiter->deadline && (first == nullptr || *waiter->deadline < *first->deadline)) {
        first = waiter.get();
      }
    }
    if (blocked < members_.size()) return;  // somebody has work to do
    if (first == nullptr) return;           // idle until a thread outside notifies
    const TimePoint target = *first->deadline;

    if (speed_ == kUnpaced) {
      advance_locked(target);
      continue;
    }

    // The sleep is for a thread that is asleep anyway. One that is leaving,
    // detaching or has itself been woken gives it to the waiter that is due
    // first, which asks this question again when it wakes.
    if (self == nullptr || self->runnable) {
      first->nominated = true;
      first->wake.notify_all();
      return;
    }

    pacing_ = true;
    interrupt_ = false;
    pace_target_ = target;
    pace_speed_ = speed_ > 0 ? speed_ : 0;  // zero stops time
    pace_real_start_ = Real::now();
    const double real_seconds = Seconds(target - now_).count() / pace_speed_;
    const auto interrupted = [this] { return interrupt_; };
    if (real_seconds < kLongestSleep) {
      pace_cv_.wait_until(
          guard,
          pace_real_start_ + std::chrono::duration_cast<Real::duration>(Seconds(real_seconds)),
          interrupted);
    } else {
      pace_cv_.wait(guard, interrupted);
    }
    pacing_ = false;
    // Paid in full, and nothing became runnable meanwhile or the sleep would
    // have been interrupted; then interrupt_locked() has moved time already.
    if (!interrupt_) advance_locked(target);
  }
}

void VirtualClock::watch() {
  const auto period =
      std::max<Duration>(options_.stall_report_after / 4, std::chrono::milliseconds(1));
  std::unique_lock guard(m_);
  std::uint64_t seen = advances_;
  auto since = Real::now();
  bool reported = false;
  while (!watchdog_cv_.wait_for(guard, period, [this] { return stop_; })) {
    const bool timed = std::any_of(waiters_.begin(), waiters_.end(), [](const auto& waiter) {
      return !waiter->runnable && waiter->deadline.has_value();
    });
    const auto real_now = Real::now();
    if (!timed || pacing_ || advances_ != seen) {
      seen = advances_;
      since = real_now;
      reported = false;
      continue;
    }
    if (reported || real_now - since < options_.stall_report_after) continue;
    reported = true;

    // Whoever is neither detached nor asleep in a wait is what time waits for.
    std::string report = "virtual clock stalled; runnable: ";
    bool any = false;
    for (const auto& [id, member] : members_) {
      if (member.detached > 0) continue;
      const bool waiting = std::any_of(
          waiters_.begin(), waiters_.end(),
          [&](const auto& waiter) { return !waiter->runnable && waiter->owner == id; });
      if (waiting) continue;
      if (any) report += ", ";
      report += member.name;
      any = true;
    }

    // Not under the clock's mutex: the callback may use the clock.
    guard.unlock();
    if (options_.on_stall) {
      options_.on_stall(std::move(report));
    } else {
      std::fprintf(stderr, "%s\n", report.c_str());
    }
    guard.lock();
  }
}

}  // namespace pychron
