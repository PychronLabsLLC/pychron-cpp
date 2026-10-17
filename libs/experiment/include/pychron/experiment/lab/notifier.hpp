#pragma once

// Notifier: sends the lab's notifications (notifications.hpp) when a run
// fails (executor::RunFinished with a failed run or a save error), when the
// queue holds (executor::ExecutorHolding) and goes back to Running, all heard
// on the bus, and when the queue ends (queue_ended(), called by LabSession;
// a queue ended unsafe is told as `unsafe` too). Each attempt is published
// as NotificationSent.
//
// Outbox (executor hardening spec 4.8): a notification is written to
// <data>/outbox/ before it is first tried, one file per notification, and
// the file is removed once every channel it is for has taken it or been given
// up. A channel that fails is tried again on the schedule of next_attempt()
// (1, 2, 4 ... 60 min apart) and given up after 24 h, with one log line. Each
// channel keeps the order notifications were made in: a later one waits for
// an earlier one still being retried on that channel, never on another.
// [[command]] channels (a local program) are tried before the ones that need
// the network. A new Notifier on the same directory sends what the last one
// left. A file holds the event and its text, never a password, a key or a
// URL: those are read from notifications.toml and its variables at delivery.
// Test notifications are tried once and never written.
//
// Deliveries run on the notifier's own thread, so a slow mail server never
// holds up the executor. That thread is no participant of the line's clock
// and waits on real time (virtual clock spec 4.4): a retry is due by the wall
// clock of this computer, whatever a simulated session's clock says.

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "pychron/core/clock.hpp"
#include "pychron/core/signal_bus.hpp"
#include "pychron/experiment/lab/notifications.hpp"

namespace pychron::experiment::lab {

struct NotificationSent {
  std::string channel;  // empty when no channel is configured
  NotifyEvent event = NotifyEvent::Test;
  std::string subject;
  bool ok = false;
  std::string error;  // why it failed, and when it is tried again or that it was given up
};

// The outbox's size (Notifier::outbox_size()), published when it changes:
// after a notification is posted, after one leaves (taken or given up by
// every channel it was for), and once when a notifier is made, for what the
// last one left. Two threads change it (the poster and the notifier's own):
// the size is read and published under one mutex, so the last event said is
// the size as it is. A handler of it must not post a notification.
struct NotificationOutbox {
  std::size_t size = 0;
};

using WallTime = std::chrono::system_clock::time_point;

inline constexpr std::chrono::minutes kNotifyRetryFirst{1};
inline constexpr std::chrono::minutes kNotifyRetryLongest{60};
inline constexpr std::chrono::hours kNotifyGiveUpAfter{24};

// When to try again a delivery first tried at `first_tried` that has failed
// `attempts` times: first_tried + 1 min, + 3 min, + 7 min ... the gap doubling
// from 1 min to at most 60 min. Nothing: give up (the next try would be more
// than 24 h after the first).
std::optional<WallTime> next_attempt(WallTime first_tried, int attempts) noexcept;

struct NotifierOptions {
  std::filesystem::path outbox;  // <data>/outbox; empty: notifications are kept in memory only
  ProcessRunner runner;          // empty: real programs (run_process)
  // false: no thread; nothing is sent unless deliver_due() is called (tests).
  bool thread = true;
  // Stamps the Log events the notifier publishes. Null: TimePoint{}.
  const Clock* clock = nullptr;
};

class Notifier {
 public:
  // `bus` (and options.clock) must outlive the notifier.
  Notifier(SignalBus& bus, NotificationConfig config, NotifierOptions options = {});
  // Tries once more what is due, then joins; what failed stays in the outbox.
  ~Notifier();
  Notifier(const Notifier&) = delete;
  Notifier& operator=(const Notifier&) = delete;
  Notifier(Notifier&&) = delete;
  Notifier& operator=(Notifier&&) = delete;

  // The queue now running: named in messages, its email added for queue_user.
  void set_queue(std::string name, std::string email);
  void queue_ended(const executor::QueueResult& result);
  // Every channel, whatever its `on`; with no channels, one failed
  // NotificationSent says so.
  void send_test(const std::string& lab);
  void post(Notification n);

  // Tries every delivery due at `now`, channel by channel ([[command]]
  // first), each channel in the order its notifications were made. Returns
  // when the next retry is due; nothing when nothing waits. The thread calls
  // it with the real time; a test without the thread calls it with times it
  // chooses.
  std::optional<WallTime> deliver_due(WallTime now);
  // Blocks until the thread has tried everything due (a retry not yet due
  // does not count). Returns at once without the thread.
  void wait_idle();
  // Notifications in the outbox: posted, not yet taken or given up by every
  // channel they are for.
  std::size_t outbox_size() const;

  const NotificationConfig& config() const noexcept { return config_; }

 private:
  struct Pending {
    std::size_t index = 0;  // the channel's place in channel_names()
    std::string name;
    int attempts = 0;
    std::optional<WallTime> first_tried, next;  // next: when a retry is due; none: at once
  };
  struct Entry {
    std::uint64_t seq = 0;
    std::filesystem::path file;  // empty: not written (a test, or no outbox)
    Notification n;
    std::vector<Pending> channels;
  };

  void load();
  std::string save(const Entry& e) const;  // under mutex_; what went wrong, empty when written
  static void drop(const Entry& e);  // under mutex_
  void log_warn(std::string message);
  void say_outbox();  // not under mutex_: publishes NotificationOutbox, under say_mutex_
  void work();

  SignalBus& bus_;
  const NotificationConfig config_;
  NotifierOptions options_;
  mutable std::mutex mutex_;
  std::mutex say_mutex_;  // one NotificationOutbox at a time, read and said together
  std::mutex delivery_mutex_;  // one deliver_due at a time; held across delivery (real programs)
  std::condition_variable cv_;
  std::vector<Entry> entries_;  // by seq
  std::uint64_t next_seq_ = 1;
  bool posted_ = true, busy_ = false, stop_ = false;  // posted_: the thread's first pass sends what was left
  std::string queue_, email_;
  std::thread thread_;
  // Last: unsubscribed first.
  SignalBus::Subscription run_finished_, holding_, state_changed_;
};

}  // namespace pychron::experiment::lab
