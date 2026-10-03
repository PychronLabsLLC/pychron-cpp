#include "pychron/experiment/lab/notifications.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <random>
#include <sstream>

#include <toml++/toml.hpp>

#include "pychron/core/env.hpp"

namespace pychron::experiment::lab {

namespace {

std::optional<NotifyEvent> parse_event(std::string_view s) {
  if (s == "run_failed") return NotifyEvent::RunFailed;
  if (s == "queue_ended") return NotifyEvent::QueueEnded;
  return std::nullopt;
}

class Parser {
 public:
  explicit Parser(std::string_view name) : name_(name) {}
  void add(const std::string& where, const std::string& what) { errors_.push_back(name_ + ": " + where + ": " + what); }
  bool ok() const { return errors_.empty(); }
  std::string errors() const {
    std::string out;
    for (const auto& e : errors_) out += (out.empty() ? "" : "\n") + e;
    return out;
  }

  // Keys a table may have; anything else is reported (a typo would otherwise be ignored).
  void keys(const toml::table& t, const std::string& where, std::initializer_list<std::string_view> allowed) {
    for (const auto& [k, v] : t)
      if (std::find(allowed.begin(), allowed.end(), k.str()) == allowed.end())
        add(where.empty() ? std::string(k.str()) : where + "." + std::string(k.str()), "unknown key");
  }
  std::string str(const toml::table& t, const std::string& where, std::string_view key, bool required) {
    const auto* node = t.get(key);
    if (node == nullptr) {
      if (required) add(where + "." + std::string(key), "missing");
      return {};
    }
    if (auto s = node->value<std::string>()) return *s;
    add(where + "." + std::string(key), "must be a string");
    return {};
  }
  std::vector<std::string> strings(const toml::table& t, const std::string& where, std::string_view key) {
    std::vector<std::string> out;
    const auto* node = t.get(key);
    if (node == nullptr) return out;
    const auto* arr = node->as_array();
    if (arr == nullptr) {
      add(where + "." + std::string(key), "must be an array of strings");
      return out;
    }
    for (const auto& e : *arr) {
      if (auto s = e.value<std::string>()) out.push_back(*s);
      else add(where + "." + std::string(key), "must be an array of strings");
    }
    return out;
  }
  bool boolean(const toml::table& t, const std::string& where, std::string_view key, bool fallback) {
    const auto* node = t.get(key);
    if (node == nullptr) return fallback;
    if (auto b = node->value<bool>()) return *b;
    add(where + "." + std::string(key), "must be true or false");
    return fallback;
  }
  std::set<NotifyEvent> on(const toml::table& t, const std::string& where, std::set<NotifyEvent> fallback) {
    if (t.get("on") == nullptr) return fallback;
    std::set<NotifyEvent> out;
    for (const auto& s : strings(t, where, "on")) {
      if (auto e = parse_event(s)) out.insert(*e);
      else add(where + ".on", "unknown event '" + s + "' (run_failed, queue_ended)");
    }
    return out;
  }

 private:
  std::string name_;
  std::vector<std::string> errors_;
};

// Each [[kind]] entry as a table; reports anything else.
std::vector<std::pair<std::string, const toml::table*>> entries(Parser& p, const toml::table& root, std::string_view kind) {
  std::vector<std::pair<std::string, const toml::table*>> out;
  const auto* node = root.get(kind);
  if (node == nullptr) return out;
  const auto* arr = node->as_array();
  if (arr == nullptr) {
    p.add(std::string(kind), "must be written [[" + std::string(kind) + "]]");
    return out;
  }
  for (std::size_t i = 0; i < arr->size(); ++i) {
    const std::string where = std::string(kind) + "[" + std::to_string(i) + "]";
    if (const auto* t = (*arr)[i].as_table()) out.emplace_back(where, t);
    else p.add(where, "must be a table");
  }
  return out;
}

bool starts_with(std::string_view s, std::string_view prefix) { return s.substr(0, prefix.size()) == prefix; }

std::string run_label(const executor::RunSummary& r) {
  std::string s = r.identifier;
  if (r.aliquot > 0) s += "-" + std::to_string(r.aliquot) + r.step;
  return s;
}

std::string json_escape(const std::string& s) {
  std::string out;
  for (const char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char b[8];
          std::snprintf(b, sizeof b, "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
          out += b;
        } else {
          out += c;
        }
    }
  }
  return out;
}

// A value in a curl config file: quoted, one line.
std::string curl_value(const std::string& s) {
  std::string out = "\"";
  for (const char c : s) {
    if (c == '\n' || c == '\r') continue;
    if (c == '"' || c == '\\') out += '\\';
    out += c;
  }
  return out + "\"";
}

// A file holding one delivery's payload, readable only by this user, removed
// when the delivery ends.
class TempFile {
 public:
  static Result<TempFile> write(const std::string& content) {
    std::random_device rd;
    char name[48];
    std::snprintf(name, sizeof name, "pychron-notify-%08x%08x", rd(), rd());
    std::error_code ec;
    auto path = std::filesystem::temp_directory_path(ec) / name;
    if (ec) return fail(ErrorKind::Io, "no temporary directory: " + ec.message());
    { std::ofstream create(path, std::ios::binary | std::ios::trunc); }
    std::filesystem::permissions(path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::replace, ec);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f << content;
    f.close();
    if (!f) {
      std::filesystem::remove(path, ec);
      return fail(ErrorKind::Io, "cannot write " + path.string());
    }
    return TempFile(std::move(path));
  }
  TempFile(TempFile&& o) noexcept : path_(std::move(o.path_)) { o.path_.clear(); }
  TempFile& operator=(TempFile&&) = delete;
  ~TempFile() {
    std::error_code ec;
    if (!path_.empty()) std::filesystem::remove(path_, ec);
  }
  std::string path() const { return path_.string(); }

 private:
  explicit TempFile(std::filesystem::path p) : path_(std::move(p)) {}
  std::filesystem::path path_;
};

std::string rfc5322_date() {
  const std::time_t now = std::time(nullptr);
  std::tm tm{};
#ifdef _WIN32
  gmtime_s(&tm, &now);
#else
  gmtime_r(&now, &tm);
#endif
  char buf[64];
  std::strftime(buf, sizeof buf, "%a, %d %b %Y %H:%M:%S +0000", &tm);
  return buf;
}

std::string crlf(const std::string& text) {
  std::string out;
  for (const char c : text) {
    if (c == '\n') out += '\r';
    if (c != '\r') out += c;
  }
  return out;
}

std::string one_line(std::string s) {
  std::replace(s.begin(), s.end(), '\n', ' ');
  std::erase(s, '\r');
  return s;
}

Delivery finish(const std::string& channel, const std::string& program, const Result<ProcessResult>& r) {
  if (!r) return {channel, false, r.error().what};
  if (r->exit_code == 0) return {channel, true, {}};
  std::string out = r->output;
  while (!out.empty() && std::isspace(static_cast<unsigned char>(out.back()))) out.pop_back();
  if (out.size() > 300) out = out.substr(0, 300) + "...";
  return {channel, false, program + " exited with " + std::to_string(r->exit_code) + (out.empty() ? "" : ": " + out)};
}

std::vector<std::string> curl_argv(const NotificationConfig& c, bool fail_on_http_error) {
  std::vector<std::string> argv{c.curl, "--silent", "--show-error"};
  if (fail_on_http_error) argv.push_back("--fail");
  argv.insert(argv.end(), {"--max-time", std::to_string(c.timeout.count()), "--config", "-"});
  return argv;
}

std::chrono::milliseconds process_timeout(const NotificationConfig& c) {
  // curl stops itself at --max-time; this only catches a program that hangs.
  return std::chrono::duration_cast<std::chrono::milliseconds>(c.timeout) + std::chrono::seconds(5);
}

Delivery send_email(const NotificationConfig& c, const EmailChannel& ch, const Notification& n, const ProcessRunner& run) {
  std::vector<std::string> to = ch.to;
  if (ch.queue_user && !n.queue_email.empty() && std::find(to.begin(), to.end(), n.queue_email) == to.end())
    to.push_back(n.queue_email);
  if (to.empty()) return {ch.name, false, "no recipients (the queue has no email)"};
  std::string password;
  if (!ch.password_env.empty()) {
    auto p = env_var(ch.password_env.c_str());
    if (!p) return {ch.name, false, ch.password_env + " is not set"};
    password = *p;
  }

  std::string to_header;
  for (const auto& t : to) to_header += (to_header.empty() ? "" : ", ") + t;
  std::string message = "From: " + ch.from + "\r\nTo: " + to_header + "\r\nSubject: " + one_line(n.subject) +
                        "\r\nDate: " + rfc5322_date() +
                        "\r\nMIME-Version: 1.0\r\nContent-Type: text/plain; charset=utf-8\r\n"
                        "Content-Transfer-Encoding: 8bit\r\n\r\n" +
                        crlf(n.body);
  if (message.size() < 2 || message.substr(message.size() - 2) != "\r\n") message += "\r\n";
  auto payload = TempFile::write(message);
  if (!payload) return {ch.name, false, payload.error().what};

  // curl greets the server (EHLO) with the URL's path, else the upload's
  // file name; give it the sender's domain.
  std::string url = ch.url;
  const auto authority = url.find("://") + 3;
  if (url.find('/', authority) == std::string::npos) {
    const auto at = ch.from.rfind('@');
    url += "/" + (at == std::string::npos ? std::string("localhost") : ch.from.substr(at + 1));
  }
  std::string config = "url = " + curl_value(url) + "\nmail-from = " + curl_value(ch.from) + "\n";
  for (const auto& t : to) config += "mail-rcpt = " + curl_value(t) + "\n";
  if (!ch.username.empty()) config += "user = " + curl_value(ch.username + ":" + password) + "\n";
  if (ch.tls && starts_with(ch.url, "smtp://")) config += "ssl-reqd\n";
  config += "upload-file = " + curl_value(payload->path()) + "\n";
  ProcessSpec spec{curl_argv(c, false), config, {}, process_timeout(c), std::nullopt};
  return finish(ch.name, c.curl, run(spec));
}

Delivery send_webhook(const NotificationConfig& c, const WebhookChannel& ch, const Notification& n,
                      const ProcessRunner& run) {
  std::string json;
  if (ch.format == WebhookChannel::Format::Slack) {
    json = "{\"text\": \"" + json_escape("*" + n.subject + "*\n" + n.body) + "\"}";
  } else {
    json = "{\"event\": \"" + std::string(to_string(n.event)) + "\", \"subject\": \"" + json_escape(n.subject) +
           "\", \"text\": \"" + json_escape(n.body) + "\"";
    for (const auto& [k, v] : n.fields) json += ", \"" + json_escape(k) + "\": \"" + json_escape(v) + "\"";
    json += "}";
  }
  auto payload = TempFile::write(json);
  if (!payload) return {ch.name, false, payload.error().what};
  const std::string config = "url = " + curl_value(ch.url) + "\nrequest = \"POST\"\nheader = " +
                             curl_value("Content-Type: application/json") + "\ndata-binary = " +
                             curl_value("@" + payload->path()) + "\n";
  ProcessSpec spec{curl_argv(c, true), config, {}, process_timeout(c), std::nullopt};
  return finish(ch.name, c.curl, run(spec));
}

Delivery send_command(const NotificationConfig& c, const CommandChannel& ch, const Notification& n,
                      const ProcessRunner& run) {
  ProcessSpec spec;
  spec.argv = ch.argv;
  spec.input = n.subject + "\n\n" + n.body;
  spec.timeout = process_timeout(c);
  spec.env = {{"PYCHRON_EVENT", std::string(to_string(n.event))}, {"PYCHRON_SUBJECT", one_line(n.subject)}};
  for (const auto& [k, v] : n.fields) {
    std::string name = "PYCHRON_";
    for (const char ch2 : k) name += static_cast<char>(std::toupper(static_cast<unsigned char>(ch2)));
    spec.env.emplace_back(name, one_line(v));
  }
  return finish(ch.name, ch.argv.front(), run(spec));
}

}  // namespace

std::string_view to_string(NotifyEvent e) noexcept {
  switch (e) {
    case NotifyEvent::RunFailed: return "run_failed";
    case NotifyEvent::QueueEnded: return "queue_ended";
    case NotifyEvent::Test: return "test";
  }
  return "?";
}

std::vector<std::string> NotificationConfig::channel_names() const {
  std::vector<std::string> out;
  for (const auto& e : email) out.push_back(e.name);
  for (const auto& w : webhooks) out.push_back(w.name);
  for (const auto& c : commands) out.push_back(c.name);
  return out;
}

Result<NotificationConfig> NotificationConfig::from_toml(std::string_view text, std::string_view name) {
  auto parsed = toml::parse(text, name);
  if (!parsed)
    return fail(ErrorKind::Config, std::string(name) + ": syntax error: " + std::string(parsed.error().description()));
  const toml::table& root = parsed.table();
  Parser p(name);
  NotificationConfig c;
  p.keys(root, "", {"curl", "timeout", "email", "webhook", "command"});
  if (const auto* node = root.get("curl")) {
    if (auto s = node->value<std::string>(); s && !s->empty()) c.curl = *s;
    else p.add("curl", "must be a program name or path");
  }
  if (const auto* node = root.get("timeout")) {
    auto t = node->value<double>();
    if (t && *t >= 1) c.timeout = std::chrono::seconds(static_cast<long long>(*t));
    else p.add("timeout", "must be a number of seconds, at least 1");
  }

  for (const auto& [where, t] : entries(p, root, "email")) {
    p.keys(*t, where, {"name", "url", "from", "to", "username", "password_env", "queue_user", "tls", "on"});
    EmailChannel e;
    if (auto nm = p.str(*t, where, "name", false); !nm.empty()) e.name = nm;
    e.url = p.str(*t, where, "url", true);
    e.from = p.str(*t, where, "from", true);
    e.to = p.strings(*t, where, "to");
    e.username = p.str(*t, where, "username", false);
    e.password_env = p.str(*t, where, "password_env", false);
    e.queue_user = p.boolean(*t, where, "queue_user", true);
    e.tls = p.boolean(*t, where, "tls", true);
    e.on = p.on(*t, where, e.on);
    if (!e.url.empty() && !starts_with(e.url, "smtp://") && !starts_with(e.url, "smtps://"))
      p.add(where + ".url", "must start with smtp:// or smtps://");
    if (e.to.empty() && !e.queue_user) p.add(where + ".to", "no recipients (set to, or queue_user = true)");
    c.email.push_back(std::move(e));
  }
  for (const auto& [where, t] : entries(p, root, "webhook")) {
    p.keys(*t, where, {"name", "url", "format", "on"});
    WebhookChannel w;
    if (auto nm = p.str(*t, where, "name", false); !nm.empty()) w.name = nm;
    w.url = p.str(*t, where, "url", true);
    if (!w.url.empty() && !starts_with(w.url, "https://") && !starts_with(w.url, "http://"))
      p.add(where + ".url", "must start with https:// or http://");
    const auto format = p.str(*t, where, "format", false);
    if (format == "slack") w.format = WebhookChannel::Format::Slack;
    else if (!format.empty() && format != "json") p.add(where + ".format", "must be \"json\" or \"slack\"");
    w.on = p.on(*t, where, w.on);
    c.webhooks.push_back(std::move(w));
  }
  for (const auto& [where, t] : entries(p, root, "command")) {
    p.keys(*t, where, {"name", "argv", "on"});
    CommandChannel cmd;
    if (auto nm = p.str(*t, where, "name", false); !nm.empty()) cmd.name = nm;
    cmd.argv = p.strings(*t, where, "argv");
    if (cmd.argv.empty() || cmd.argv.front().empty()) p.add(where + ".argv", "must name a program");
    cmd.on = p.on(*t, where, cmd.on);
    c.commands.push_back(std::move(cmd));
  }
  if (!p.ok()) return fail(ErrorKind::Config, p.errors());
  return c;
}

Result<NotificationConfig> NotificationConfig::load(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return fail(ErrorKind::Io, "cannot open " + path.string());
  std::ostringstream ss;
  ss << in.rdbuf();
  return from_toml(ss.str(), path.string());
}

Notification run_failed_notification(const std::string& queue, const executor::RunSummary& run) {
  Notification n;
  n.event = NotifyEvent::RunFailed;
  const std::string label = run_label(run);
  const std::string state(run::to_string(run.state));
  n.subject = "pychron: run " + label + " " + (run.state == run::RunState::Failed ? "failed" : state) + " (queue " +
              queue + ")";
  std::ostringstream b;
  b << "Run " << label << " " << state << ".\n\n";
  b << "Queue:   " << queue << "\n";
  b << "Row:     " << run.row << "\n";
  if (!run.run_id.empty()) b << "Run id:  " << run.run_id << "\n";
  if (run.error) b << "Error:   " << *run.error << "\n";
  if (run.save_error) b << "Saving:  failed; the record is in the spool and will be saved again\n";
  n.body = b.str();
  n.fields = {{"queue", queue},
              {"identifier", run.identifier},
              {"run", label},
              {"run_id", run.run_id},
              {"row", std::to_string(run.row)},
              {"state", state},
              {"error", run.error.value_or("")},
              {"save_error", run.save_error ? "true" : "false"}};
  return n;
}

Notification queue_ended_notification(const std::string& queue, const executor::QueueResult& result) {
  Notification n;
  n.event = NotifyEvent::QueueEnded;
  const std::string end(executor::to_string(result.end));
  std::map<std::string, int> by_state;
  int failed = 0;
  for (const auto& r : result.runs) {
    ++by_state[std::string(run::to_string(r.state))];
    failed += r.state == run::RunState::Failed || r.save_error ? 1 : 0;
  }
  n.subject = "pychron: queue " + queue + " " + end;
  if (failed > 0) n.subject += ", " + std::to_string(failed) + " run(s) failed";
  std::ostringstream b;
  b << "Queue " << queue << " " << end;
  if (!result.reason.empty()) b << ": " << result.reason;
  b << ".\n\n" << result.runs.size() << " run(s)";
  for (const auto& [s, count] : by_state) b << ", " << count << " " << s;
  b << "\n\n";
  for (const auto& r : result.runs) {
    b << "  " << r.row << "  " << run_label(r) << "  " << run::to_string(r.state);
    if (r.truncated) b << " (truncated)";
    if (r.save_error) b << " (not saved yet)";
    if (r.error) b << ": " << *r.error;
    b << "\n";
  }
  n.body = b.str();
  n.fields = {{"queue", queue},
              {"end", end},
              {"reason", result.reason},
              {"runs", std::to_string(result.runs.size())},
              {"failed", std::to_string(failed)}};
  return n;
}

Notification test_notification(const std::string& lab) {
  Notification n;
  n.event = NotifyEvent::Test;
  n.subject = "pychron: test notification";
  n.body = "A test of the notifications configured for the lab at " + lab + ".\n";
  n.fields = {{"lab", lab}};
  return n;
}

std::vector<Delivery> deliver(const NotificationConfig& config, const Notification& n, const ProcessRunner& run) {
  std::vector<Delivery> out;
  auto wants = [&](const std::set<NotifyEvent>& on) { return n.event == NotifyEvent::Test || on.contains(n.event); };
  for (const auto& e : config.email)
    if (wants(e.on)) out.push_back(send_email(config, e, n, run));
  for (const auto& w : config.webhooks)
    if (wants(w.on)) out.push_back(send_webhook(config, w, n, run));
  for (const auto& c : config.commands)
    if (wants(c.on)) out.push_back(send_command(config, c, n, run));
  return out;
}

}  // namespace pychron::experiment::lab
