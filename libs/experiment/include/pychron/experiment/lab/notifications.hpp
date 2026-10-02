#pragma once

// Notifications (experiment spec 10.4): a message when a run fails and when a
// queue ends, by email, webhook or a local command, configured per lab in
// <lab>/notifications.toml:
//
//   curl = "curl"        # the curl program used for email and webhooks
//   timeout = 30         # seconds per delivery
//
//   [[email]]
//   url = "smtps://smtp.example.org:465"   # smtp:// upgrades with STARTTLS unless tls = false
//   from = "pychron@example.org"
//   to = ["manager@example.org"]
//   username = "pychron@example.org"       # optional
//   password_env = "PYCHRON_SMTP_PASSWORD" # the password is read from this variable, never the file
//   queue_user = true                      # also the queue's email (default true)
//   on = ["run_failed", "queue_ended"]     # default: both
//
//   [[webhook]]
//   url = "https://hooks.slack.com/services/..."
//   format = "slack"                       # {"text": ...}; "json" (default) posts every field
//
//   [[command]]
//   argv = ["/usr/local/bin/notify-lab"]   # the message on stdin, fields as PYCHRON_* variables
//
// Email and webhooks run curl with its options on stdin (`--config -`), so
// neither the password nor the webhook URL appears on a command line.

#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "pychron/core/error.hpp"
#include "pychron/core/process.hpp"
#include "pychron/experiment/executor/executor.hpp"

namespace pychron::experiment::lab {

enum class NotifyEvent { RunFailed, QueueEnded, Test };
std::string_view to_string(NotifyEvent e) noexcept;

struct EmailChannel {
  std::string name = "email";
  std::string url, from, username, password_env;
  std::vector<std::string> to;
  bool queue_user = true;  // also mail the queue's `email`
  bool tls = true;         // smtp:// must upgrade with STARTTLS
  std::set<NotifyEvent> on{NotifyEvent::RunFailed, NotifyEvent::QueueEnded};
};

struct WebhookChannel {
  enum class Format { Json, Slack };
  std::string name = "webhook";
  std::string url;
  Format format = Format::Json;
  std::set<NotifyEvent> on{NotifyEvent::RunFailed, NotifyEvent::QueueEnded};
};

struct CommandChannel {
  std::string name = "command";
  std::vector<std::string> argv;
  std::set<NotifyEvent> on{NotifyEvent::RunFailed, NotifyEvent::QueueEnded};
};

struct NotificationConfig {
  std::string curl = "curl";
  std::chrono::seconds timeout{30};
  std::vector<EmailChannel> email;
  std::vector<WebhookChannel> webhooks;
  std::vector<CommandChannel> commands;

  bool empty() const noexcept { return email.empty() && webhooks.empty() && commands.empty(); }
  std::vector<std::string> channel_names() const;  // in delivery order

  static Result<NotificationConfig> from_toml(std::string_view text, std::string_view name);
  static Result<NotificationConfig> load(const std::filesystem::path& path);
};

struct Notification {
  NotifyEvent event = NotifyEvent::Test;
  std::string subject, body;
  // queue, identifier, run_id, state, error, end, reason, ...: the JSON
  // webhook's fields and the command's PYCHRON_<FIELD> variables.
  std::map<std::string, std::string> fields;
  std::string queue_email;  // the queue's user, for email channels with queue_user
};

Notification run_failed_notification(const std::string& queue, const executor::RunSummary& run);
Notification queue_ended_notification(const std::string& queue, const executor::QueueResult& result);
Notification test_notification(const std::string& lab);

using ProcessRunner = std::function<Result<ProcessResult>(const ProcessSpec&)>;

struct Delivery {
  std::string channel;
  bool ok = false;
  std::string error;  // why it failed
};

// Sends `n` on every channel whose `on` has its event (every channel for a
// Test), in channel_names() order. Blocks until each delivery ends.
std::vector<Delivery> deliver(const NotificationConfig& config, const Notification& n, const ProcessRunner& run);

}  // namespace pychron::experiment::lab
