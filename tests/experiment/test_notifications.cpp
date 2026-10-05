// Notifications: notifications.toml, the messages, each channel's curl or
// command invocation (through a fake process runner), and the Notifier.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <sstream>

#include "pychron/experiment/lab/notifier.hpp"

using namespace pychron;
using namespace pychron::experiment;
using namespace pychron::experiment::lab;

namespace {

void set_env(const char* name, const char* value) {
#ifdef _WIN32
  _putenv_s(name, value);
#else
  setenv(name, value, 1);
#endif
}

std::string read_file(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// Records each invocation; the payload file curl would upload is read while
// it still exists.
struct FakeRunner {
  struct Call {
    ProcessSpec spec;
    std::string payload;
  };
  std::mutex mutex;
  std::vector<Call> calls;
  int exit_code = 0;
  std::string output;

  ProcessRunner runner() {
    return [this](const ProcessSpec& spec) -> Result<ProcessResult> {
      Call c{spec, {}};
      for (const std::string key : {"upload-file = \"", "data-binary = \"@"}) {
        if (auto at = spec.input.find(key); at != std::string::npos) {
          const auto start = at + key.size();
          c.payload = read_file(spec.input.substr(start, spec.input.find('"', start) - start));
        }
      }
      std::lock_guard lock(mutex);
      calls.push_back(std::move(c));
      return ProcessResult{exit_code, output};
    };
  }
};

executor::RunSummary failed_run() {
  executor::RunSummary r;
  r.row = 4;
  r.identifier = "66001";
  r.aliquot = 3;
  r.run_id = "uuid-1";
  r.state = run::RunState::Failed;
  r.error = "extraction script raised";
  return r;
}

}  // namespace

TEST(Notifications, ParsesEveryChannel) {
  auto c = NotificationConfig::from_toml(R"(
curl = "/usr/bin/curl"
timeout = 12

[[email]]
url = "smtp://mail.example.org:587"
from = "pychron@example.org"
to = ["a@example.org", "b@example.org"]
username = "pychron"
password_env = "PYCHRON_TEST_SMTP_PW"
on = ["run_failed"]

[[webhook]]
name = "slack"
url = "https://hooks.example.org/x"
format = "slack"

[[command]]
argv = ["notify-lab", "--urgent"]
on = ["queue_ended"]
)",
                                         "n.toml");
  ASSERT_TRUE(c) << c.error().what;
  EXPECT_EQ(c->curl, "/usr/bin/curl");
  EXPECT_EQ(c->timeout, std::chrono::seconds(12));
  ASSERT_EQ(c->email.size(), 1u);
  EXPECT_EQ(c->email[0].to.size(), 2u);
  EXPECT_TRUE(c->email[0].queue_user);
  EXPECT_EQ(c->email[0].on, std::set<NotifyEvent>{NotifyEvent::RunFailed});
  ASSERT_EQ(c->webhooks.size(), 1u);
  EXPECT_EQ(c->webhooks[0].format, WebhookChannel::Format::Slack);
  EXPECT_EQ(c->webhooks[0].on, (std::set<NotifyEvent>{NotifyEvent::RunFailed, NotifyEvent::QueueEnded}));
  ASSERT_EQ(c->commands.size(), 1u);
  EXPECT_EQ(c->channel_names(), (std::vector<std::string>{"email", "slack", "command"}));
  EXPECT_FALSE(c->empty());
  EXPECT_TRUE(NotificationConfig::from_toml("", "empty.toml")->empty());
}

TEST(Notifications, ReportsEveryMistake) {
  auto c = NotificationConfig::from_toml(R"(
timeuot = 5
[[email]]
url = "http://mail"
to = []
queue_user = false
on = ["run_failed", "sometimes"]
[[webhook]]
url = "ftp://x"
format = "xml"
[[command]]
argv = []
)",
                                         "n.toml");
  ASSERT_FALSE(c);
  const auto& e = c.error().what;
  for (const char* expected : {"n.toml: timeuot: unknown key", "email[0].from: missing", "email[0].url: must start with smtp",
                               "email[0].to: no recipients", "unknown event 'sometimes'", "webhook[0].url: must start with https",
                               "webhook[0].format", "command[0].argv: must name a program"})
    EXPECT_NE(e.find(expected), std::string::npos) << expected << "\n" << e;
  EXPECT_FALSE(NotificationConfig::from_toml("email = 3", "n.toml"));
}

TEST(Notifications, MessagesNameTheRunAndTheQueue) {
  auto n = run_failed_notification("q1", failed_run());
  EXPECT_EQ(n.event, NotifyEvent::RunFailed);
  EXPECT_EQ(n.subject, "pychron: run 66001-3 failed (queue q1)");
  EXPECT_NE(n.body.find("extraction script raised"), std::string::npos) << n.body;
  EXPECT_EQ(n.fields.at("row"), "4");

  executor::QueueResult result;
  result.end = executor::QueueEnd::Failed;
  result.reason = "run 66001 failed: extraction script raised";
  executor::RunSummary ok;
  ok.identifier = "bu";
  ok.aliquot = 1;
  ok.state = run::RunState::Success;
  result.runs = {ok, failed_run()};
  auto q = queue_ended_notification("q1", result);
  EXPECT_EQ(q.subject, "pychron: queue q1 failed, 1 run(s) failed");
  EXPECT_NE(q.body.find("2 run(s), 1 failed, 1 success"), std::string::npos) << q.body;
  EXPECT_NE(q.body.find("bu-1  success"), std::string::npos) << q.body;
  EXPECT_EQ(q.fields.at("failed"), "1");
}

TEST(Notifications, EmailGoesThroughCurlWithItsOptionsOnStdin) {
  set_env("PYCHRON_TEST_SMTP_PW", "s3cret");
  auto c = NotificationConfig::from_toml(R"(
[[email]]
url = "smtp://mail.example.org:587"
from = "pychron@example.org"
to = ["a@example.org"]
username = "pychron"
password_env = "PYCHRON_TEST_SMTP_PW"
)",
                                         "n.toml");
  ASSERT_TRUE(c) << c.error().what;
  FakeRunner fake;
  auto n = run_failed_notification("q1", failed_run());
  n.queue_email = "user@example.org";
  auto d = deliver(*c, n, fake.runner());
  ASSERT_EQ(d.size(), 1u);
  EXPECT_TRUE(d[0].ok) << d[0].error;
  ASSERT_EQ(fake.calls.size(), 1u);
  const auto& call = fake.calls[0];
  EXPECT_EQ(call.spec.argv.front(), "curl");
  EXPECT_EQ(call.spec.argv.back(), "-");  // --config -
  for (const auto& a : call.spec.argv) EXPECT_EQ(a.find("s3cret"), std::string::npos);
  const auto& cfg = call.spec.input;
  EXPECT_NE(cfg.find("url = \"smtp://mail.example.org:587/example.org\""), std::string::npos) << cfg;
  EXPECT_NE(cfg.find("mail-rcpt = \"a@example.org\""), std::string::npos) << cfg;
  EXPECT_NE(cfg.find("mail-rcpt = \"user@example.org\""), std::string::npos) << cfg;
  EXPECT_NE(cfg.find("user = \"pychron:s3cret\""), std::string::npos) << cfg;
  EXPECT_NE(cfg.find("ssl-reqd"), std::string::npos) << cfg;  // smtp:// must upgrade
  EXPECT_NE(call.payload.find("Subject: pychron: run 66001-3 failed (queue q1)\r\n"), std::string::npos)
      << call.payload;
  EXPECT_NE(call.payload.find("To: a@example.org, user@example.org\r\n"), std::string::npos) << call.payload;
  EXPECT_NE(call.payload.find("\r\n\r\nRun 66001-3 failed."), std::string::npos) << call.payload;

  // The payload is gone once the delivery ends.
  const auto at = cfg.find("upload-file = \"") + 15;
  EXPECT_FALSE(std::filesystem::exists(cfg.substr(at, cfg.find('"', at) - at)));

  // A missing password variable fails without running curl.
  c->email[0].password_env = "PYCHRON_TEST_UNSET_VARIABLE_XYZ";
  d = deliver(*c, n, fake.runner());
  ASSERT_EQ(d.size(), 1u);
  EXPECT_FALSE(d[0].ok);
  EXPECT_NE(d[0].error.find("PYCHRON_TEST_UNSET_VARIABLE_XYZ is not set"), std::string::npos);
  EXPECT_EQ(fake.calls.size(), 1u);
}

TEST(Notifications, ParsesAMailServiceChannel) {
  auto c = NotificationConfig::from_toml(R"(
[[email]]
provider = "brevo"
api_key_env = "PYCHRON_TEST_MAIL_KEY"
from = "pychron@example.org"
to = ["a@example.org"]
)",
                                         "n.toml");
  ASSERT_TRUE(c) << c.error().what;
  ASSERT_EQ(c->email.size(), 1u);
  EXPECT_EQ(c->email[0].provider, EmailChannel::Provider::Brevo);
  EXPECT_EQ(c->email[0].api_key_env, "PYCHRON_TEST_MAIL_KEY");
  EXPECT_TRUE(c->email[0].queue_user);
  EXPECT_EQ(c->channel_names(), std::vector<std::string>{"email"});
}

TEST(Notifications, AMailServiceChannelTakesNoSmtpSettings) {
  auto c = NotificationConfig::from_toml(R"(
[[email]]
provider = "resend"
url = "smtp://mail.example.org"
username = "pychron"
password_env = "PW"
tls = false
from = "pychron@example.org"
[[email]]
provider = "gmail"
api_key_env = "K"
from = "pychron@example.org"
[[email]]
url = "smtp://mail.example.org"
api_key_env = "K"
from = "pychron@example.org"
)",
                                         "n.toml");
  ASSERT_FALSE(c);
  const auto& e = c.error().what;
  for (const char* expected :
       {"email[0].api_key_env: missing", "email[0].url: not used with provider", "email[0].username: not used with provider",
        "email[0].password_env: not used with provider", "email[0].tls: not used with provider",
        "email[1].provider: must be \"brevo\", \"resend\" or \"postmark\"",
        "email[2].api_key_env: only used with provider"})
    EXPECT_NE(e.find(expected), std::string::npos) << expected << "\n" << e;
  // The missing url of a provider channel is not a mistake.
  EXPECT_EQ(e.find("email[1].url"), std::string::npos) << e;
}

namespace {

// One [[email]] channel on `provider`, sent a failed run with a queue user.
struct ProviderSend {
  FakeRunner fake;
  std::vector<Delivery> deliveries;

  explicit ProviderSend(const std::string& provider) {
    set_env("PYCHRON_TEST_MAIL_KEY", "k3y-s3cret");
    auto c = NotificationConfig::from_toml("[[email]]\nprovider = \"" + provider +
                                               "\"\napi_key_env = \"PYCHRON_TEST_MAIL_KEY\"\n"
                                               "from = \"pychron@example.org\"\nto = [\"a@example.org\"]\n",
                                           "n.toml");
    EXPECT_TRUE(c) << c.error().what;
    auto n = run_failed_notification("q1", failed_run());
    n.queue_email = "user@example.org";
    deliveries = deliver(*c, n, fake.runner());
  }
  const std::string& config() const { return fake.calls.at(0).spec.input; }
  const std::string& payload() const { return fake.calls.at(0).payload; }
};

void expect_has(const std::string& text, const std::string& part) {
  EXPECT_NE(text.find(part), std::string::npos) << "no " << part << " in\n" << text;
}

}  // namespace

TEST(Notifications, BrevoGetsItsJsonAndTheKeyInAHeader) {
  ProviderSend s("brevo");
  ASSERT_EQ(s.deliveries.size(), 1u);
  EXPECT_TRUE(s.deliveries[0].ok) << s.deliveries[0].error;
  ASSERT_EQ(s.fake.calls.size(), 1u);
  const auto& argv = s.fake.calls[0].spec.argv;
  EXPECT_EQ(argv.front(), "curl");
  EXPECT_EQ(argv.back(), "-");  // --config -: the key is never an argument
  for (const auto& a : argv) EXPECT_EQ(a.find("k3y-s3cret"), std::string::npos);
  expect_has(s.config(), "url = \"https://api.brevo.com/v3/smtp/email\"");
  expect_has(s.config(), "request = \"POST\"");
  expect_has(s.config(), "header = \"api-key: k3y-s3cret\"");
  expect_has(s.config(), "header = \"Content-Type: application/json\"");
  expect_has(s.config(), "fail-with-body");  // the service's own words on a refusal
  expect_has(s.payload(), "\"sender\": {\"email\": \"pychron@example.org\"}");
  expect_has(s.payload(), "\"to\": [{\"email\": \"a@example.org\"}, {\"email\": \"user@example.org\"}]");
  expect_has(s.payload(), "\"subject\": \"pychron: run 66001-3 failed (queue q1)\"");
  expect_has(s.payload(), "\"textContent\": \"Run 66001-3 failed.\\n\\n");
  EXPECT_EQ(s.payload().find("k3y-s3cret"), std::string::npos);

  // The payload is gone once the delivery ends.
  const auto at = s.config().find("data-binary = \"@") + 16;
  EXPECT_FALSE(std::filesystem::exists(s.config().substr(at, s.config().find('"', at) - at)));
}

TEST(Notifications, ResendGetsItsJsonAndABearerKey) {
  ProviderSend s("resend");
  ASSERT_EQ(s.deliveries.size(), 1u);
  EXPECT_TRUE(s.deliveries[0].ok) << s.deliveries[0].error;
  expect_has(s.config(), "url = \"https://api.resend.com/emails\"");
  expect_has(s.config(), "header = \"Authorization: Bearer k3y-s3cret\"");
  expect_has(s.payload(), "\"from\": \"pychron@example.org\"");
  expect_has(s.payload(), "\"to\": [\"a@example.org\", \"user@example.org\"]");
  expect_has(s.payload(), "\"subject\": \"pychron: run 66001-3 failed (queue q1)\"");
  expect_has(s.payload(), "\"text\": \"Run 66001-3 failed.\\n\\n");
}

TEST(Notifications, PostmarkGetsItsJsonAndAServerToken) {
  ProviderSend s("postmark");
  ASSERT_EQ(s.deliveries.size(), 1u);
  EXPECT_TRUE(s.deliveries[0].ok) << s.deliveries[0].error;
  expect_has(s.config(), "url = \"https://api.postmarkapp.com/email\"");
  expect_has(s.config(), "header = \"X-Postmark-Server-Token: k3y-s3cret\"");
  expect_has(s.config(), "header = \"Accept: application/json\"");
  expect_has(s.payload(), "\"From\": \"pychron@example.org\"");
  expect_has(s.payload(), "\"To\": \"a@example.org, user@example.org\"");
  expect_has(s.payload(), "\"Subject\": \"pychron: run 66001-3 failed (queue q1)\"");
  expect_has(s.payload(), "\"TextBody\": \"Run 66001-3 failed.\\n\\n");
}

TEST(Notifications, AMailServiceRefusalIsReportedInItsOwnWords) {
  set_env("PYCHRON_TEST_MAIL_KEY", "k3y-s3cret");
  auto c = NotificationConfig::from_toml(R"(
[[email]]
provider = "brevo"
api_key_env = "PYCHRON_TEST_MAIL_KEY"
from = "pychron@example.org"
to = ["a@example.org"]
)",
                                         "n.toml");
  ASSERT_TRUE(c) << c.error().what;
  FakeRunner fake;
  fake.exit_code = 22;
  fake.output = "{\"code\":\"unauthorized\",\"message\":\"Key not found\"}\ncurl: (22) The requested URL returned error: 401\n";
  auto d = deliver(*c, test_notification("/lab"), fake.runner());
  ASSERT_EQ(d.size(), 1u);
  EXPECT_FALSE(d[0].ok);
  EXPECT_NE(d[0].error.find("Key not found"), std::string::npos) << d[0].error;
  EXPECT_EQ(d[0].error.find("k3y-s3cret"), std::string::npos) << d[0].error;

  // An unset key variable fails without running curl.
  c->email[0].api_key_env = "PYCHRON_TEST_UNSET_VARIABLE_XYZ";
  d = deliver(*c, test_notification("/lab"), fake.runner());
  ASSERT_EQ(d.size(), 1u);
  EXPECT_FALSE(d[0].ok);
  EXPECT_NE(d[0].error.find("PYCHRON_TEST_UNSET_VARIABLE_XYZ is not set"), std::string::npos) << d[0].error;
  EXPECT_EQ(fake.calls.size(), 1u);
}

TEST(Notifications, UnsetSecretsNamesEachChannelWhoseVariableIsMissing) {
  set_env("PYCHRON_TEST_MAIL_KEY", "k3y-s3cret");
  auto c = NotificationConfig::from_toml(R"(
[[email]]
name = "service"
provider = "brevo"
api_key_env = "PYCHRON_TEST_UNSET_VARIABLE_XYZ"
from = "pychron@example.org"
[[email]]
name = "set"
provider = "resend"
api_key_env = "PYCHRON_TEST_MAIL_KEY"
from = "pychron@example.org"
[[email]]
name = "smtp"
url = "smtp://mail.example.org"
from = "pychron@example.org"
username = "pychron"
password_env = "PYCHRON_TEST_UNSET_VARIABLE_ABC"
[[email]]
name = "open relay"
url = "smtp://relay.example.org"
from = "pychron@example.org"
)",
                                         "n.toml");
  ASSERT_TRUE(c) << c.error().what;
  EXPECT_EQ(unset_secrets(*c), (std::vector<std::pair<std::string, std::string>>{
                                   {"service", "PYCHRON_TEST_UNSET_VARIABLE_XYZ"},
                                   {"smtp", "PYCHRON_TEST_UNSET_VARIABLE_ABC"}}));
}

TEST(Notifications, WebhooksAndCommandsAndFailures) {
  auto c = NotificationConfig::from_toml(R"(
[[webhook]]
url = "https://hooks.example.org/x"
format = "slack"
on = ["queue_ended"]
[[webhook]]
name = "json"
url = "https://example.org/hook"
[[command]]
argv = ["notify-lab"]
)",
                                         "n.toml");
  ASSERT_TRUE(c) << c.error().what;
  FakeRunner fake;
  auto d = deliver(*c, run_failed_notification("q1", failed_run()), fake.runner());
  // The slack hook only wants queue_ended.
  ASSERT_EQ(d.size(), 2u);
  EXPECT_EQ(d[0].channel, "json");
  EXPECT_EQ(d[1].channel, "command");
  ASSERT_EQ(fake.calls.size(), 2u);
  EXPECT_NE(std::find(fake.calls[0].spec.argv.begin(), fake.calls[0].spec.argv.end(), "--fail"),
            fake.calls[0].spec.argv.end());
  EXPECT_NE(fake.calls[0].spec.input.find("url = \"https://example.org/hook\""), std::string::npos);
  EXPECT_NE(fake.calls[0].payload.find("\"event\": \"run_failed\""), std::string::npos) << fake.calls[0].payload;
  EXPECT_NE(fake.calls[0].payload.find("\"identifier\": \"66001\""), std::string::npos) << fake.calls[0].payload;
  const auto& cmd = fake.calls[1].spec;
  EXPECT_EQ(cmd.argv, std::vector<std::string>{"notify-lab"});
  EXPECT_EQ(cmd.input.rfind("pychron: run 66001-3 failed", 0), 0u);
  auto env = [&](const std::string& k) {
    for (const auto& [n, v] : cmd.env)
      if (n == k) return v;
    return std::string("<unset>");
  };
  EXPECT_EQ(env("PYCHRON_EVENT"), "run_failed");
  EXPECT_EQ(env("PYCHRON_IDENTIFIER"), "66001");
  EXPECT_EQ(env("PYCHRON_ERROR"), "extraction script raised");

  // A test message goes to every channel; a failing program is reported.
  fake.exit_code = 22;
  fake.output = "curl: (22) The requested URL returned error: 404\n";
  d = deliver(*c, test_notification("/lab"), fake.runner());
  ASSERT_EQ(d.size(), 3u);
  EXPECT_FALSE(d[0].ok);
  EXPECT_EQ(d[0].error, "curl exited with 22: curl: (22) The requested URL returned error: 404");
  EXPECT_NE(fake.calls[2].payload.find("{\"text\": \"*pychron: test notification*\\n"), std::string::npos)
      << fake.calls[2].payload;
}

TEST(Notifier, SendsFailedRunsAndTheQueueEndOffTheBus) {
  auto c = NotificationConfig::from_toml("[[command]]\nargv = [\"notify-lab\"]\n", "n.toml");
  ASSERT_TRUE(c);
  SignalBus bus;
  std::mutex m;
  std::vector<NotificationSent> sent;
  auto sub = bus.subscribe<NotificationSent>([&](const NotificationSent& e) {
    std::lock_guard lock(m);
    sent.push_back(e);
  });
  FakeRunner fake;
  {
    Notifier notifier(bus, *c, fake.runner());
    notifier.set_queue("q1", "user@example.org");
    executor::RunSummary ok = failed_run();
    ok.state = run::RunState::Success;
    bus.publish(executor::RunFinished{ok});  // nothing to say
    bus.publish(executor::RunFinished{failed_run()});
    executor::QueueResult result;
    result.end = executor::QueueEnd::Completed;
    notifier.queue_ended(result);
    notifier.wait_idle();
    std::lock_guard lock(m);
    ASSERT_EQ(sent.size(), 2u);
    EXPECT_EQ(sent[0].event, NotifyEvent::RunFailed);
    EXPECT_EQ(sent[0].subject, "pychron: run 66001-3 failed (queue q1)");
    EXPECT_TRUE(sent[0].ok);
    EXPECT_EQ(sent[1].event, NotifyEvent::QueueEnded);
    EXPECT_EQ(sent[1].subject, "pychron: queue q1 completed");
    EXPECT_EQ(sent[1].channel, "command");
    notifier.queue_ended(result);  // still queued at destruction: sent before it returns
  }
  EXPECT_EQ(fake.calls.size(), 3u);

  // No channels: a test says so; nothing else is sent.
  sent.clear();
  Notifier none(bus, NotificationConfig{}, fake.runner());
  bus.publish(executor::RunFinished{failed_run()});
  none.send_test("/lab");
  none.wait_idle();
  ASSERT_EQ(sent.size(), 1u);
  EXPECT_FALSE(sent[0].ok);
  EXPECT_TRUE(sent[0].channel.empty());
  EXPECT_NE(sent[0].error.find("no notifications are configured"), std::string::npos);
  EXPECT_EQ(fake.calls.size(), 3u);
}
