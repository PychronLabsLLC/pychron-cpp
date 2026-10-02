#pragma once

// Notifier: sends the lab's notifications (notifications.hpp) when a run
// fails (executor::RunFinished with a failed run or a save error, which it
// hears on the bus) and when the queue ends (queue_ended(), called by
// LabSession). Deliveries run on the notifier's own thread, one at a time,
// so a slow mail server never holds up the executor; each is published as
// NotificationSent.

#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

#include "pychron/core/signal_bus.hpp"
#include "pychron/experiment/lab/notifications.hpp"

namespace pychron::experiment::lab {

struct NotificationSent {
  std::string channel;  // empty when no channel is configured
  NotifyEvent event = NotifyEvent::Test;
  std::string subject;
  bool ok = false;
  std::string error;
};

class Notifier {
 public:
  // `bus` must outlive the notifier. An empty `runner` runs real programs.
  Notifier(SignalBus& bus, NotificationConfig config, ProcessRunner runner = {});
  ~Notifier();  // delivers what is already queued, then joins
  Notifier(const Notifier&) = delete;
  Notifier& operator=(const Notifier&) = delete;

  // The queue now running: named in messages, its email added for queue_user.
  void set_queue(std::string name, std::string email);
  void queue_ended(const executor::QueueResult& result);
  // Every channel, whatever its `on`; with no channels, one failed
  // NotificationSent says so.
  void send_test(const std::string& lab);
  void post(Notification n);
  // Blocks until nothing is queued or being delivered.
  void wait_idle();

  const NotificationConfig& config() const noexcept { return config_; }

 private:
  void work();

  SignalBus& bus_;
  const NotificationConfig config_;
  ProcessRunner runner_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<Notification> pending_;
  bool busy_ = false, stop_ = false;
  std::string queue_, email_;
  std::thread thread_;
  SignalBus::Subscription run_finished_;  // last: unsubscribed first
};

}  // namespace pychron::experiment::lab
