#pragma once

// Notifications (experiment spec 10.4; executor hardening spec 4.8): a message
// when a run fails, when the queue holds and goes on again, when a failed run
// leaves the line unsafe and when a queue ends, by email, webhook or a local
// command, configured per lab in <lab>/notifications.toml:
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
//   on = ["run_failed", "queue_ended"]     # default: every event (run_failed, queue_ended,
//                                          # queue_holding, queue_resumed, unsafe)
//
//   [[email]]
//   provider = "brevo"                     # a mail service: brevo, resend or postmark, over HTTPS
//   api_key_env = "PYCHRON_MAIL_API_KEY"   # its key; url, username, password_env and tls are not used
//   from = "pychron@example.org"           # to, queue_user and on as above
//
//   [[webhook]]
//   url = "https://hooks.slack.com/services/..."
//   format = "slack"                       # {"text": ...}; "json" (default) posts every field
//
//   [[command]]
//   argv = ["/usr/local/bin/notify-lab"]   # the message on stdin, fields as PYCHRON_* variables
//
// Email and webhooks run curl with its options on stdin (`--config -`), so
// neither the password, the key nor the webhook URL appears on a command
// line. The lab manager's guide is docs/notifications.md.

#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "pychron/core/error.hpp"
#include "pychron/core/process.hpp"
#include "pychron/experiment/executor/executor.hpp"

namespace pychron::experiment::lab {

enum class NotifyEvent { RunFailed, QueueEnded, Test, QueueHolding, QueueResumed, Unsafe };
std::string_view to_string(NotifyEvent e) noexcept;
// What a channel's `on` takes: every event but Test (which goes to every channel).
std::optional<NotifyEvent> parse_notify_event(std::string_view s) noexcept;
// What a channel is sent when its `on` is left out.
inline const std::set<NotifyEvent> kDefaultNotifyEvents{NotifyEvent::RunFailed, NotifyEvent::QueueEnded,
                                                        NotifyEvent::QueueHolding, NotifyEvent::QueueResumed,
                                                        NotifyEvent::Unsafe};

struct EmailChannel {
  // Smtp: curl speaks SMTP to `url`. The others: one HTTPS request to that
  // mail service, signed with the key in `api_key_env`.
  enum class Provider { Smtp, Brevo, Resend, Postmark };
  std::string name = "email";
  Provider provider = Provider::Smtp;
  std::string url, from, username, password_env, api_key_env;
  std::vector<std::string> to;
  bool queue_user = true;  // also mail the queue's `email`
  bool tls = true;         // smtp:// must upgrade with STARTTLS
  std::set<NotifyEvent> on = kDefaultNotifyEvents;
};

struct WebhookChannel {
  enum class Format { Json, Slack };
  std::string name = "webhook";
  std::string url;
  Format format = Format::Json;
  std::set<NotifyEvent> on = kDefaultNotifyEvents;
};

struct CommandChannel {
  std::string name = "command";
  std::vector<std::string> argv;
  std::set<NotifyEvent> on = kDefaultNotifyEvents;
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
// The queue holds (spec 4.6): why, the links it waits for, and how long it
// may hold before the limit ends it.
Notification queue_holding_notification(const std::string& queue, const executor::ExecutorHolding& hold);
// The queue went on again after a hold.
Notification queue_resumed_notification(const std::string& queue, const std::string& reason);
// A failed run left the line unsafe and ended the queue (spec 4.7; result.code
// is executor::kUnsafe).
Notification unsafe_notification(const std::string& queue, const executor::QueueResult& result);

using ProcessRunner = std::function<Result<ProcessResult>(const ProcessSpec&)>;

struct Delivery {
  std::string channel;
  bool ok = false;
  std::string error;  // why it failed
};

// Sends `n` on every channel whose `on` has its event (every channel for a
// Test), in channel_names() order. Blocks until each delivery ends.
std::vector<Delivery> deliver(const NotificationConfig& config, const Notification& n, const ProcessRunner& run);

// The channels by their place in channel_names(): whether channel `index`
// wants `event`, and one delivery of `n` on it. The password or key is read
// from its variable here, at delivery, never before.
std::size_t channel_count(const NotificationConfig& config) noexcept;
bool channel_wants(const NotificationConfig& config, std::size_t index, NotifyEvent event);
Delivery deliver_to(const NotificationConfig& config, std::size_t index, const Notification& n, const ProcessRunner& run);
// Whether channel `index` is a [[command]]: a local program, tried before the
// channels that need the network.
bool is_command_channel(const NotificationConfig& config, std::size_t index) noexcept;

// {channel, variable} for each email channel whose password_env or
// api_key_env names a variable that is not set here, in channel order.
std::vector<std::pair<std::string, std::string>> unset_secrets(const NotificationConfig& config);

}  // namespace pychron::experiment::lab
