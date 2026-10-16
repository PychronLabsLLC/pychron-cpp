// Notifications: notifications.toml, the messages, each channel's curl or
// command invocation (through a fake process runner), and the Notifier.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>

#include "pychron/core/events.hpp"
#include "pychron/experiment/lab/notifier.hpp"
#include "temp_path.hpp"

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
  // Decides the exit code of a call instead of exit_code, when set.
  std::function<int(const ProcessSpec&)> exit_for;
  std::function<void()> on_call;  // runs as each call is made

  ProcessRunner runner() {
    return [this](const ProcessSpec& spec) -> Result<ProcessResult> {
      Call c{spec, {}};
      for (const std::string key : {"upload-file = \"", "data-binary = \"@"}) {
        if (auto at = spec.input.find(key); at != std::string::npos) {
          const auto start = at + key.size();
          c.payload = read_file(spec.input.substr(start, spec.input.find('"', start) - start));
        }
      }
      if (on_call) on_call();
      const int code = exit_for ? exit_for(spec) : exit_code;
      std::lock_guard lock(mutex);
      calls.push_back(std::move(c));
      return ProcessResult{code, output};
    };
  }
};

NotifierOptions options(ProcessRunner runner, std::filesystem::path outbox = {}, bool thread = true) {
  NotifierOptions o;
  o.runner = std::move(runner);
  o.outbox = std::move(outbox);
  o.thread = thread;
  return o;
}

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
  EXPECT_EQ(c->webhooks[0].on, kDefaultNotifyEvents);
  EXPECT_EQ(c->webhooks[0].on, (std::set<NotifyEvent>{NotifyEvent::RunFailed, NotifyEvent::QueueEnded,
                                                      NotifyEvent::QueueHolding, NotifyEvent::QueueResumed,
                                                      NotifyEvent::Unsafe}));
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

// A run that was measured and could not be saved is not a failed run: the
// message says what happened to it, and why.
TEST(Notifications, ARunThatWasNotSavedSaysSoAndWhy) {
  executor::RunSummary r;
  r.row = 2;
  r.identifier = "66001";
  r.aliquot = 3;
  r.state = run::RunState::Success;
  r.save_error = true;
  r.messages = {"hole 3: centered", "not saved to the data store, kept in the spool: the database is locked"};
  auto n = run_failed_notification("q1", r);
  EXPECT_EQ(n.subject, "pychron: run 66001-3 not saved (queue q1)");
  EXPECT_NE(n.body.find("Saving:  failed; the record is in the spool and will be saved again\n"), std::string::npos)
      << n.body;
  EXPECT_NE(n.body.find("Reason:  the database is locked\n"), std::string::npos) << n.body;
  EXPECT_EQ(n.fields.at("save_error"), "true");
  EXPECT_EQ(n.fields.at("state"), "success");
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
        R"(email[1].provider: must be "brevo", "resend" or "postmark")",
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
  expect_has(s.payload(), R"("sender": {"email": "pychron@example.org"})");
  expect_has(s.payload(), R"("to": [{"email": "a@example.org"}, {"email": "user@example.org"}])");
  expect_has(s.payload(), "\"subject\": \"pychron: run 66001-3 failed (queue q1)\"");
  expect_has(s.payload(), R"("textContent": "Run 66001-3 failed.\n\n)");
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
  expect_has(s.payload(), R"("from": "pychron@example.org")");
  expect_has(s.payload(), R"("to": ["a@example.org", "user@example.org"])");
  expect_has(s.payload(), "\"subject\": \"pychron: run 66001-3 failed (queue q1)\"");
  expect_has(s.payload(), R"("text": "Run 66001-3 failed.\n\n)");
}

TEST(Notifications, PostmarkGetsItsJsonAndAServerToken) {
  ProviderSend s("postmark");
  ASSERT_EQ(s.deliveries.size(), 1u);
  EXPECT_TRUE(s.deliveries[0].ok) << s.deliveries[0].error;
  expect_has(s.config(), "url = \"https://api.postmarkapp.com/email\"");
  expect_has(s.config(), "header = \"X-Postmark-Server-Token: k3y-s3cret\"");
  expect_has(s.config(), "header = \"Accept: application/json\"");
  expect_has(s.payload(), R"("From": "pychron@example.org")");
  expect_has(s.payload(), R"("To": "a@example.org, user@example.org")");
  expect_has(s.payload(), "\"Subject\": \"pychron: run 66001-3 failed (queue q1)\"");
  expect_has(s.payload(), R"("TextBody": "Run 66001-3 failed.\n\n)");
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
    Notifier notifier(bus, *c, options(fake.runner()));
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
  Notifier none(bus, NotificationConfig{}, options(fake.runner()));
  bus.publish(executor::RunFinished{failed_run()});
  none.send_test("/lab");
  none.wait_idle();
  ASSERT_EQ(sent.size(), 1u);
  EXPECT_FALSE(sent[0].ok);
  EXPECT_TRUE(sent[0].channel.empty());
  EXPECT_NE(sent[0].error.find("no notifications are configured"), std::string::npos);
  EXPECT_EQ(fake.calls.size(), 3u);
}

// --- The outbox (executor hardening spec 4.8) --------------------------------
//
// Without its thread (NotifierOptions::thread = false) a notifier sends only
// when deliver_due() is called, at the time the test gives: no test sleeps.

namespace {

using namespace std::chrono_literals;

// A directory of its own, removed at the end.
struct OutboxDir {
  std::filesystem::path dir = pychron::testing::unique_temp_path("pychron-outbox-");
  OutboxDir() = default;
  OutboxDir(const OutboxDir&) = delete;
  OutboxDir& operator=(const OutboxDir&) = delete;
  OutboxDir(OutboxDir&&) = delete;
  OutboxDir& operator=(OutboxDir&&) = delete;
  ~OutboxDir() {
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
  }
  std::vector<std::filesystem::path> files() const {
    std::vector<std::filesystem::path> out;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) out.push_back(e.path());
    std::sort(out.begin(), out.end());
    return out;
  }
};

// What a bus heard: notifications sent and log lines.
struct Heard {
  std::mutex m;
  std::vector<NotificationSent> sent;
  std::vector<Log> logs;
  SignalBus::Subscription a, b;
  explicit Heard(SignalBus& bus)
      : a(bus.subscribe<NotificationSent>([this](const NotificationSent& e) {
          std::lock_guard lock(m);
          sent.push_back(e);
        })),
        b(bus.subscribe<Log>([this](const Log& e) {
          std::lock_guard lock(m);
          logs.push_back(e);
        })) {}
};

NotificationConfig config_of(const std::string& text) {
  auto c = NotificationConfig::from_toml(text, "n.toml");
  EXPECT_TRUE(c) << c.error().what;
  return c ? *c : NotificationConfig{};
}

executor::RunSummary failed_run_of(const std::string& identifier) {
  auto r = failed_run();
  r.identifier = identifier;
  return r;
}

const WallTime kT0 = WallTime(std::chrono::hours(24 * 365 * 56));  // some day in 2026

}  // namespace

TEST(NotifierRetry, TheGapDoublesFromAMinuteToAnHourAndEndsAfterADay) {
  EXPECT_EQ(next_attempt(kT0, 0), kT0);
  EXPECT_EQ(next_attempt(kT0, 1), kT0 + 1min);
  EXPECT_EQ(next_attempt(kT0, 2), kT0 + 3min);
  EXPECT_EQ(next_attempt(kT0, 3), kT0 + 7min);
  EXPECT_EQ(next_attempt(kT0, 4), kT0 + 15min);
  EXPECT_EQ(next_attempt(kT0, 6), kT0 + 63min);
  EXPECT_EQ(next_attempt(kT0, 7), kT0 + 123min);  // the gap stays at an hour
  EXPECT_EQ(next_attempt(kT0, 8), kT0 + 183min);
  EXPECT_EQ(next_attempt(kT0, 28), kT0 + 1383min);  // the last one within 24 h
  EXPECT_FALSE(next_attempt(kT0, 29));
  EXPECT_FALSE(next_attempt(kT0, 1000));
}

TEST(NotifierOutbox, ANotificationIsOnDiskBeforeItIsTriedAndGoneOnceSent) {
  OutboxDir out;
  SignalBus bus;
  Heard heard(bus);
  FakeRunner fake;
  fake.exit_code = 1;
  std::size_t files_at_call = 0;
  fake.on_call = [&] { files_at_call = out.files().size(); };
  Notifier notifier(bus, config_of("[[command]]\nargv = [\"notify-lab\"]\n"), options(fake.runner(), out.dir, false));
  notifier.post(run_failed_notification("q1", failed_run()));
  ASSERT_EQ(out.files().size(), 1u);
  EXPECT_EQ(notifier.outbox_size(), 1u);
  EXPECT_TRUE(fake.calls.empty());  // without the thread nothing is tried until deliver_due

  // Fails: tried again a minute later, not before.
  EXPECT_EQ(notifier.deliver_due(kT0), kT0 + 1min);
  EXPECT_EQ(files_at_call, 1u);
  ASSERT_EQ(fake.calls.size(), 1u);
  ASSERT_EQ(heard.sent.size(), 1u);
  EXPECT_FALSE(heard.sent[0].ok);
  EXPECT_NE(heard.sent[0].error.find("tried again in 1 min"), std::string::npos) << heard.sent[0].error;
  EXPECT_EQ(notifier.deliver_due(kT0 + 59s), kT0 + 1min);
  EXPECT_EQ(fake.calls.size(), 1u);
  EXPECT_EQ(notifier.deliver_due(kT0 + 1min), kT0 + 3min);
  EXPECT_EQ(fake.calls.size(), 2u);
  EXPECT_EQ(out.files().size(), 1u);

  // Sent: the file is removed, nothing waits.
  fake.exit_code = 0;
  EXPECT_FALSE(notifier.deliver_due(kT0 + 3min));
  EXPECT_EQ(fake.calls.size(), 3u);
  EXPECT_TRUE(heard.sent.back().ok);
  EXPECT_TRUE(out.files().empty());
  EXPECT_EQ(notifier.outbox_size(), 0u);
}

TEST(NotifierOutbox, ANewNotifierSendsWhatTheLastOneLeftInOrder) {
  OutboxDir out;
  SignalBus bus;
  Heard heard(bus);
  FakeRunner fake;
  const auto config = config_of("[[command]]\nargv = [\"notify-lab\"]\n[[webhook]]\nurl = \"https://example.org/h\"\n");
  {
    Notifier first(bus, config, options(fake.runner(), out.dir, false));
    for (const char* id : {"66001", "66002", "66003"}) first.post(run_failed_notification("q1", failed_run_of(id)));
  }
  EXPECT_EQ(out.files().size(), 3u);
  EXPECT_TRUE(fake.calls.empty());

  Notifier second(bus, config, options(fake.runner(), out.dir, false));
  EXPECT_EQ(second.outbox_size(), 3u);
  second.post(run_failed_notification("q1", failed_run_of("66004")));  // after what was left
  EXPECT_FALSE(second.deliver_due(kT0));
  EXPECT_TRUE(out.files().empty());
  std::vector<std::string> order;
  order.reserve(heard.sent.size());
  for (const auto& s : heard.sent) order.push_back(s.channel + " " + s.subject.substr(13, 7));
  EXPECT_EQ(order, (std::vector<std::string>{"command 66001-3", "command 66002-3", "command 66003-3", "command 66004-3",
                                             "webhook 66001-3", "webhook 66002-3", "webhook 66003-3",
                                             "webhook 66004-3"}));
}

TEST(NotifierOutbox, ADeliveryIsGivenUpAfterADayWithOneLogLine) {
  OutboxDir out;
  SignalBus bus;
  Heard heard(bus);
  FakeRunner fake;
  fake.exit_code = 7;
  fake.output = "curl: (7) Failed to connect";
  Notifier notifier(bus, config_of("[[command]]\nargv = [\"notify-lab\"]\n"), options(fake.runner(), out.dir, false));
  notifier.post(run_failed_notification("q1", failed_run()));
  std::optional<WallTime> at = kT0;
  WallTime last = kT0;
  while (at) {
    last = *at;
    at = notifier.deliver_due(*at);
    ASSERT_LT(fake.calls.size(), 100u);
  }
  EXPECT_EQ(fake.calls.size(), 29u);  // the first try and 28 retries
  EXPECT_EQ(last, kT0 + 1383min);
  EXPECT_LE(last - kT0, std::chrono::hours(24));
  EXPECT_TRUE(out.files().empty());
  EXPECT_EQ(notifier.outbox_size(), 0u);
  EXPECT_NE(heard.sent.back().error.find("given up after 24 h"), std::string::npos) << heard.sent.back().error;
  std::size_t gave_up = 0;
  for (const auto& l : heard.logs) {
    if (l.message.find("given up") == std::string::npos) continue;
    ++gave_up;
    EXPECT_EQ(l.level, LogLevel::Warn);
    EXPECT_NE(l.message.find("pychron: run 66001-3 failed (queue q1)"), std::string::npos) << l.message;
  }
  EXPECT_EQ(gave_up, 1u);
}

TEST(NotifierOutbox, TheCommandChannelGoesFirstAndDoesNotWaitForAFailingEmail) {
  OutboxDir out;
  SignalBus bus;
  Heard heard(bus);
  FakeRunner fake;
  bool network = false;
  fake.exit_for = [&](const ProcessSpec& spec) { return spec.argv.front() == "curl" && !network ? 7 : 0; };
  Notifier notifier(bus, config_of(R"(
[[email]]
url = "smtp://relay.example.org"
from = "pychron@example.org"
to = ["a@example.org"]
[[command]]
argv = ["notify-lab"]
)"),
                    options(fake.runner(), out.dir, false));
  notifier.post(run_failed_notification("q1", failed_run_of("66001")));
  notifier.post(run_failed_notification("q1", failed_run_of("66002")));
  EXPECT_EQ(notifier.deliver_due(kT0), kT0 + 1min);
  // Both commands, then the email of the first; its second waits behind it.
  ASSERT_EQ(fake.calls.size(), 3u);
  EXPECT_EQ(fake.calls[0].spec.argv.front(), "notify-lab");
  EXPECT_EQ(fake.calls[1].spec.argv.front(), "notify-lab");
  EXPECT_EQ(fake.calls[2].spec.argv.front(), "curl");
  EXPECT_NE(fake.calls[2].payload.find("66001-3"), std::string::npos);
  EXPECT_EQ(out.files().size(), 2u);  // each still waits for the email

  // A third notification goes out on the command at once, whatever the email does.
  notifier.post(run_failed_notification("q1", failed_run_of("66003")));
  EXPECT_EQ(notifier.deliver_due(kT0 + 10s), kT0 + 1min);
  ASSERT_EQ(fake.calls.size(), 4u);
  EXPECT_EQ(fake.calls[3].spec.argv.front(), "notify-lab");

  // The network is back: the emails go, in order.
  network = true;
  EXPECT_FALSE(notifier.deliver_due(kT0 + 1min));
  ASSERT_EQ(fake.calls.size(), 7u);
  for (std::size_t i = 4; i < 7; ++i) EXPECT_EQ(fake.calls[i].spec.argv.front(), "curl");
  EXPECT_NE(fake.calls[4].payload.find("66001-3"), std::string::npos);
  EXPECT_NE(fake.calls[5].payload.find("66002-3"), std::string::npos);
  EXPECT_NE(fake.calls[6].payload.find("66003-3"), std::string::npos);
  EXPECT_TRUE(out.files().empty());
}

TEST(NotifierOutbox, AnOutboxFileHoldsNoSecretOfTheConfig) {
  set_env("PYCHRON_TEST_OUTBOX_KEY", "k3y-0utb0x-s3cret");
  OutboxDir out;
  SignalBus bus;
  FakeRunner fake;
  fake.exit_code = 22;
  const auto config = config_of(R"(
[[email]]
provider = "brevo"
api_key_env = "PYCHRON_TEST_OUTBOX_KEY"
from = "pychron@example.org"
to = ["a@example.org"]
[[webhook]]
url = "https://hooks.example.org/services/T000/B000/XXXX"
)");
  Notifier notifier(bus, config, options(fake.runner(), out.dir, false));
  notifier.post(run_failed_notification("q1", failed_run()));
  notifier.deliver_due(kT0);  // fails: the file stays, with its attempts
  ASSERT_EQ(fake.calls.size(), 2u);
  EXPECT_NE(fake.calls[0].spec.input.find("k3y-0utb0x-s3cret"), std::string::npos);  // read at delivery
  ASSERT_EQ(out.files().size(), 1u);
  const std::string text = read_file(out.files()[0].string());
  EXPECT_NE(text.find("pychron: run 66001-3 failed (queue q1)"), std::string::npos) << text;
  for (const char* secret : {"k3y-0utb0x-s3cret", "PYCHRON_TEST_OUTBOX_KEY", "hooks.example.org", "T000/B000",
                             "a@example.org", "pychron@example.org", "brevo"})
    EXPECT_EQ(text.find(secret), std::string::npos) << secret << " in\n" << text;
}

TEST(NotifierOutbox, ATestNotificationIsTriedOnceAndNeverWritten) {
  OutboxDir out;
  SignalBus bus;
  Heard heard(bus);
  FakeRunner fake;
  fake.exit_code = 1;
  Notifier notifier(bus, config_of("[[command]]\nargv = [\"notify-lab\"]\n"), options(fake.runner(), out.dir, false));
  notifier.send_test("/lab");
  EXPECT_TRUE(out.files().empty());
  EXPECT_FALSE(notifier.deliver_due(kT0));
  EXPECT_EQ(fake.calls.size(), 1u);
  ASSERT_EQ(heard.sent.size(), 1u);
  EXPECT_FALSE(heard.sent[0].ok);
  EXPECT_EQ(notifier.outbox_size(), 0u);
}

TEST(NotifierOutbox, HoldsResumesAndAnUnsafeEndAreSent) {
  SignalBus bus;
  Heard heard(bus);
  FakeRunner fake;
  Notifier notifier(bus, config_of(R"(
[[command]]
name = "all"
argv = ["notify-lab"]
[[command]]
name = "failures"
argv = ["notify-lab"]
on = ["run_failed", "unsafe"]
)"),
                    options(fake.runner(), {}, false));
  notifier.set_queue("q1", "user@example.org");
  executor::ExecutorHolding hold;
  hold.reason = "link spectrometer is down";
  hold.links = {"spectrometer"};
  hold.since = TimePoint{};
  hold.until = hold.since + std::chrono::minutes(30);
  hold.row = 3;
  bus.publish(hold);
  using executor::ExecutorState;
  bus.publish(executor::ExecutorStateChanged{ExecutorState::Running, ExecutorState::Holding, "held"});  // not a resume
  bus.publish(executor::ExecutorStateChanged{ExecutorState::Holding, ExecutorState::Running, "links are back"});
  bus.publish(executor::ExecutorStateChanged{ExecutorState::Holding, ExecutorState::StoppingAtBoundary, "stop"});
  executor::QueueResult result;
  result.end = executor::QueueEnd::Failed;
  result.reason = "run 66001 failed and the line is not safe";
  result.code = executor::kUnsafe;
  notifier.queue_ended(result);
  EXPECT_FALSE(notifier.deliver_due(kT0));

  std::vector<std::string> got;
  got.reserve(heard.sent.size());
  for (const auto& s : heard.sent) got.push_back(s.channel + " " + std::string(to_string(s.event)));
  EXPECT_EQ(got, (std::vector<std::string>{"all queue_holding", "all queue_resumed", "all unsafe", "all queue_ended",
                                           "failures unsafe"}));
  ASSERT_GE(fake.calls.size(), 3u);
  EXPECT_EQ(fake.calls[0].spec.input.rfind("pychron: queue q1 holding", 0), 0u) << fake.calls[0].spec.input;
  EXPECT_NE(fake.calls[0].spec.input.find("link spectrometer is down"), std::string::npos);
  EXPECT_NE(fake.calls[0].spec.input.find("30 min"), std::string::npos) << fake.calls[0].spec.input;
  EXPECT_EQ(fake.calls[1].spec.input.rfind("pychron: queue q1 running again", 0), 0u) << fake.calls[1].spec.input;
  EXPECT_EQ(fake.calls[2].spec.input.rfind("pychron: queue q1 ended, line unsafe", 0), 0u) << fake.calls[2].spec.input;
}

TEST(Notifications, TheNewEventsAreSwitchedPerChannel) {
  auto c = NotificationConfig::from_toml(R"(
[[webhook]]
url = "https://example.org/h"
on = ["queue_holding", "queue_resumed", "unsafe"]
)",
                                         "n.toml");
  ASSERT_TRUE(c) << c.error().what;
  EXPECT_EQ(c->webhooks[0].on,
            (std::set<NotifyEvent>{NotifyEvent::QueueHolding, NotifyEvent::QueueResumed, NotifyEvent::Unsafe}));
  EXPECT_TRUE(channel_wants(*c, 0, NotifyEvent::QueueHolding));
  EXPECT_FALSE(channel_wants(*c, 0, NotifyEvent::RunFailed));
  EXPECT_TRUE(channel_wants(*c, 0, NotifyEvent::Test));
  auto bad = NotificationConfig::from_toml("[[command]]\nargv = [\"x\"]\non = [\"test\"]\n", "n.toml");
  ASSERT_FALSE(bad);
  EXPECT_NE(bad.error().what.find("queue_holding, queue_resumed, unsafe"), std::string::npos) << bad.error().what;
}

// With its thread, a notifier sends what an earlier one left as it starts.
TEST(NotifierOutbox, TheThreadSendsWhatWasLeftAtOnce) {
  OutboxDir out;
  SignalBus bus;
  FakeRunner fake;
  const auto config = config_of("[[command]]\nargv = [\"notify-lab\"]\n");
  { Notifier(bus, config, options(fake.runner(), out.dir, false)).post(run_failed_notification("q1", failed_run())); }
  ASSERT_EQ(out.files().size(), 1u);
  Notifier notifier(bus, config, options(fake.runner(), out.dir));
  notifier.wait_idle();
  EXPECT_EQ(fake.calls.size(), 1u);
  EXPECT_TRUE(out.files().empty());
}
