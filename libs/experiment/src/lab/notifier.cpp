#include "pychron/experiment/lab/notifier.hpp"

namespace pychron::experiment::lab {

Notifier::Notifier(SignalBus& bus, NotificationConfig config, ProcessRunner runner)
    : bus_(bus), config_(std::move(config)), runner_(runner ? std::move(runner) : ProcessRunner(run_process)) {
  thread_ = std::thread([this] { work(); });
  run_finished_ = bus_.subscribe<executor::RunFinished>([this](const executor::RunFinished& e) {
    const auto& r = e.summary;
    if (r.state != run::RunState::Failed && !r.save_error) return;
    std::string queue, email;
    {
      std::lock_guard lock(mutex_);
      queue = queue_;
      email = email_;
    }
    auto n = run_failed_notification(queue, r);
    n.queue_email = email;
    post(std::move(n));
  });
}

Notifier::~Notifier() {
  run_finished_ = {};
  {
    std::lock_guard lock(mutex_);
    stop_ = true;
  }
  cv_.notify_all();
  if (thread_.joinable()) thread_.join();
}

void Notifier::set_queue(std::string name, std::string email) {
  std::lock_guard lock(mutex_);
  queue_ = std::move(name);
  email_ = std::move(email);
}

void Notifier::queue_ended(const executor::QueueResult& result) {
  std::string queue, email;
  {
    std::lock_guard lock(mutex_);
    queue = queue_;
    email = email_;
  }
  auto n = queue_ended_notification(queue, result);
  n.queue_email = email;
  post(std::move(n));
}

void Notifier::send_test(const std::string& lab) {
  auto n = test_notification(lab);
  if (config_.empty()) {
    bus_.publish(NotificationSent{{}, n.event, n.subject, false, "no notifications are configured (notifications.toml)"});
    return;
  }
  post(std::move(n));
}

void Notifier::post(Notification n) {
  if (config_.empty()) return;
  {
    std::lock_guard lock(mutex_);
    pending_.push_back(std::move(n));
  }
  cv_.notify_all();
}

void Notifier::wait_idle() {
  std::unique_lock lock(mutex_);
  cv_.wait(lock, [this] { return pending_.empty() && !busy_; });
}

void Notifier::work() {
  std::unique_lock lock(mutex_);
  for (;;) {
    cv_.wait(lock, [this] { return stop_ || !pending_.empty(); });
    if (pending_.empty()) return;  // stopping, and everything queued has gone out
    Notification n = std::move(pending_.front());
    pending_.pop_front();
    busy_ = true;
    lock.unlock();
    for (const auto& d : deliver(config_, n, runner_))
      bus_.publish(NotificationSent{d.channel, n.event, n.subject, d.ok, d.error});
    lock.lock();
    busy_ = false;
    cv_.notify_all();
  }
}

}  // namespace pychron::experiment::lab
