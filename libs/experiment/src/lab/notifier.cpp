#include "pychron/experiment/lab/notifier.hpp"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <set>
#include <sstream>
#include <system_error>

#include <toml++/toml.hpp>

#include "pychron/core/events.hpp"
#include "pychron/core/log_context.hpp"

namespace pychron::experiment::lab {

namespace {

namespace fs = std::filesystem;
using std::chrono::milliseconds;

std::int64_t to_ms(WallTime t) {
  return std::chrono::duration_cast<milliseconds>(t.time_since_epoch()).count();
}
WallTime from_ms(std::int64_t ms) {
  return WallTime(std::chrono::duration_cast<WallTime::duration>(milliseconds(ms)));
}

// <seq>.toml, the seq zero-padded so that the names sort as the notifications were made.
std::string file_name(std::uint64_t seq) {
  std::string digits = std::to_string(seq);
  return std::string(digits.size() < 12 ? 12 - digits.size() : 0, '0') + digits + ".toml";
}

std::optional<std::uint64_t> seq_of(const fs::path& p) {
  if (p.extension() != ".toml") return std::nullopt;
  const std::string stem = p.stem().string();
  std::uint64_t seq = 0;
  const auto [end, ec] = std::from_chars(stem.data(), stem.data() + stem.size(), seq);
  if (ec != std::errc{} || end != stem.data() + stem.size() || seq == 0) return std::nullopt;
  return seq;
}

std::string read_text(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// "in 2 min": how long until `next`, rounded up to the minute.
std::string minutes_until(WallTime now, WallTime next) {
  const auto ms = std::chrono::duration_cast<milliseconds>(next - now).count();
  return std::to_string(std::max<std::int64_t>(1, (ms + 59'999) / 60'000)) + " min";
}

}  // namespace

std::optional<WallTime> next_attempt(WallTime first_tried, int attempts) noexcept {
  if (attempts <= 0) return first_tried;
  auto offset = std::chrono::minutes::zero();
  auto gap = kNotifyRetryFirst;
  for (int i = 0; i < attempts; ++i) {
    offset += gap;
    if (offset > kNotifyGiveUpAfter) return std::nullopt;
    gap = std::min(gap * 2, kNotifyRetryLongest);
  }
  return first_tried + offset;
}

Notifier::Notifier(SignalBus& bus, NotificationConfig config, NotifierOptions options)
    : bus_(bus), config_(std::move(config)), options_(std::move(options)) {
  if (!options_.runner) options_.runner = ProcessRunner(run_process);
  load();
  say_outbox();
  if (options_.thread) thread_ = std::thread([this] {
      const LogContext::Thread log_thread("notifier");
      work();
    });
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
  holding_ = bus_.subscribe<executor::ExecutorHolding>([this](const executor::ExecutorHolding& e) {
    std::string queue, email;
    {
      std::lock_guard lock(mutex_);
      queue = queue_;
      email = email_;
    }
    auto n = queue_holding_notification(queue, e);
    n.queue_email = email;
    post(std::move(n));
  });
  state_changed_ = bus_.subscribe<executor::ExecutorStateChanged>([this](const executor::ExecutorStateChanged& e) {
    if (e.from != executor::ExecutorState::Holding || e.to != executor::ExecutorState::Running) return;
    std::string queue, email;
    {
      std::lock_guard lock(mutex_);
      queue = queue_;
      email = email_;
    }
    auto n = queue_resumed_notification(queue, e.reason);
    n.queue_email = email;
    post(std::move(n));
  });
}

Notifier::~Notifier() {
  state_changed_ = {};
  holding_ = {};
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
  if (result.code == executor::kUnsafe) {
    auto u = unsafe_notification(queue, result);
    u.queue_email = email;
    post(std::move(u));
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
  const auto names = config_.channel_names();
  std::string problem;
  {
    std::lock_guard lock(mutex_);
    Entry e;
    e.seq = next_seq_++;
    for (std::size_t i = 0; i < names.size(); ++i)
      if (channel_wants(config_, i, n.event)) e.channels.push_back(Pending{i, names[i], 0, std::nullopt, std::nullopt});
    if (e.channels.empty()) return;
    e.n = std::move(n);
    // A test is tried once: it is not written, and not retried.
    if (!options_.outbox.empty() && e.n.event != NotifyEvent::Test) {
      e.file = options_.outbox / file_name(e.seq);
      problem = save(e);
    }
    entries_.push_back(std::move(e));
    posted_ = true;
  }
  cv_.notify_all();
  say_outbox();
  if (!problem.empty()) log_warn(problem + "; the notification is kept in memory only");
}

void Notifier::say_outbox() {
  // Every change is followed by a call here; read and said together, the
  // last event is the size after the last change, whichever thread made it.
  const std::lock_guard lock(say_mutex_);
  bus_.publish(NotificationOutbox{outbox_size()});
}

std::size_t Notifier::outbox_size() const {
  std::lock_guard lock(mutex_);
  return entries_.size();
}

void Notifier::load() {
  if (options_.outbox.empty()) return;
  std::error_code ec;
  if (!fs::is_directory(options_.outbox, ec)) return;
  std::vector<std::pair<std::uint64_t, fs::path>> files;
  for (const auto& de : fs::directory_iterator(options_.outbox, ec))
    if (auto seq = seq_of(de.path()); seq && de.is_regular_file(ec)) files.emplace_back(*seq, de.path());
  std::sort(files.begin(), files.end());
  const auto names = config_.channel_names();
  std::vector<std::string> problems;
  for (const auto& [seq, path] : files) {
    next_seq_ = std::max(next_seq_, seq + 1);
    auto parsed = toml::parse(read_text(path), path.string());
    std::optional<NotifyEvent> event;
    if (parsed) event = parse_notify_event(parsed.table()["event"].value_or(std::string()));
    if (!event) {
      problems.push_back("outbox: " + path.string() + " is not a notification; left where it is");
      continue;
    }
    const toml::table& t = parsed.table();
    Entry e;
    e.seq = seq;
    e.file = path;
    e.n.event = *event;
    e.n.subject = t["subject"].value_or(std::string());
    e.n.body = t["body"].value_or(std::string());
    e.n.queue_email = t["queue_email"].value_or(std::string());
    if (const auto* fields = t["fields"].as_table())
      for (const auto& [k, v] : *fields) e.n.fields[std::string(k.str())] = v.value_or(std::string());
    if (const auto* channels = t["channel"].as_array()) {
      for (const auto& node : *channels) {
        const auto* c = node.as_table();
        if (c == nullptr) continue;
        Pending p;
        p.index = static_cast<std::size_t>((*c)["index"].value_or(std::int64_t{-1}));
        p.name = (*c)["name"].value_or(std::string());
        p.attempts = static_cast<int>((*c)["attempts"].value_or(std::int64_t{0}));
        if (auto ms = (*c)["first_tried"].value<std::int64_t>()) p.first_tried = from_ms(*ms);
        if (auto ms = (*c)["next"].value<std::int64_t>()) p.next = from_ms(*ms);
        if (p.index >= names.size() || names[p.index] != p.name) {
          problems.push_back("outbox: " + e.n.subject + ": channel " + p.name +
                             " is no longer in notifications.toml; not sent there");
          continue;
        }
        e.channels.push_back(std::move(p));
      }
    }
    if (e.channels.empty()) {
      std::error_code rm;
      fs::remove(path, rm);
      continue;
    }
    entries_.push_back(std::move(e));
  }
  for (auto& p : problems) log_warn(std::move(p));
}

std::string Notifier::save(const Entry& e) const {
  if (e.file.empty()) return {};
  toml::table t;
  t.insert("event", std::string(to_string(e.n.event)));
  t.insert("subject", e.n.subject);
  t.insert("body", e.n.body);
  t.insert("queue_email", e.n.queue_email);
  toml::table fields;
  for (const auto& [k, v] : e.n.fields) fields.insert(k, v);
  t.insert("fields", std::move(fields));
  toml::array channels;
  for (const auto& p : e.channels) {
    toml::table c;
    c.insert("index", static_cast<std::int64_t>(p.index));
    c.insert("name", p.name);
    c.insert("attempts", static_cast<std::int64_t>(p.attempts));
    if (p.first_tried) c.insert("first_tried", to_ms(*p.first_tried));
    if (p.next) c.insert("next", to_ms(*p.next));
    channels.push_back(std::move(c));
  }
  t.insert("channel", std::move(channels));

  std::error_code ec;
  fs::create_directories(e.file.parent_path(), ec);
  fs::path tmp = e.file;
  tmp += ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    out << t << "\n";
    out.close();
    if (!out) return "outbox: cannot write " + tmp.string();
  }
  fs::rename(tmp, e.file, ec);
  if (ec) {
    fs::remove(tmp, ec);
    return "outbox: cannot write " + e.file.string();
  }
  return {};
}

void Notifier::drop(const Entry& e) {
  if (e.file.empty()) return;
  std::error_code ec;
  fs::remove(e.file, ec);
}

void Notifier::log_warn(std::string message) {
  bus_.publish(Log{LogLevel::Warn, "notifications", std::move(message),
                   options_.clock != nullptr ? options_.clock->now() : TimePoint{}});
}

std::optional<WallTime> Notifier::deliver_due(WallTime now) {
  std::lock_guard serial(delivery_mutex_);
  // [[command]] first: a local program does not wait for the network.
  std::vector<std::size_t> order;
  for (std::size_t i = 0; i < channel_count(config_); ++i)
    if (is_command_channel(config_, i)) order.push_back(i);
  for (std::size_t i = 0; i < channel_count(config_); ++i)
    if (!is_command_channel(config_, i)) order.push_back(i);

  for (const std::size_t channel : order) {
    for (;;) {
      std::uint64_t seq = 0;
      Notification n;
      std::string name;
      {
        std::lock_guard lock(mutex_);
        const Pending* head = nullptr;
        for (const auto& e : entries_) {
          const auto it = std::find_if(e.channels.begin(), e.channels.end(),
                                       [&](const Pending& p) { return p.index == channel; });
          if (it == e.channels.end()) continue;
          head = &*it;
          seq = e.seq;
          n = e.n;
          break;
        }
        if (head == nullptr || (head->next && *head->next > now)) break;
        name = head->name;
      }

      const Delivery d = deliver_to(config_, channel, n, options_.runner);

      NotificationSent sent{d.channel.empty() ? name : d.channel, n.event, n.subject, d.ok, d.error};
      bool retrying = false;
      bool left = false;  // the notification has left the outbox
      std::string gave_up, problem;
      {
        std::lock_guard lock(mutex_);
        auto e = std::find_if(entries_.begin(), entries_.end(), [&](const Entry& x) { return x.seq == seq; });
        if (e == entries_.end()) break;  // cannot happen: only this function removes entries
        auto p = std::find_if(e->channels.begin(), e->channels.end(),
                              [&](const Pending& x) { return x.index == channel; });
        if (d.ok || n.event == NotifyEvent::Test) {
          e->channels.erase(p);
        } else {
          ++p->attempts;
          if (!p->first_tried) p->first_tried = now;
          p->next = next_attempt(*p->first_tried, p->attempts);
          if (p->next) {
            retrying = true;
            sent.error += "; tried again in " + minutes_until(now, *p->next);
          } else {
            sent.error += "; given up after 24 h";
            gave_up = "notification '" + n.subject + "' to " + sent.channel + " given up after " +
                      std::to_string(p->attempts) + " attempts in 24 h: " + d.error;
            e->channels.erase(p);
          }
        }
        if (e->channels.empty()) {
          drop(*e);
          entries_.erase(e);
          left = true;
        } else {
          problem = save(*e);
        }
      }
      bus_.publish(sent);
      if (left) say_outbox();
      if (!gave_up.empty()) log_warn(std::move(gave_up));
      if (!problem.empty()) log_warn(std::move(problem));
      if (retrying) break;  // later notifications on this channel wait for this one
    }
  }

  // When the head of each channel is due next.
  std::lock_guard lock(mutex_);
  std::optional<WallTime> next;
  std::set<std::size_t> seen;
  for (const auto& e : entries_) {
    for (const auto& p : e.channels) {
      if (!seen.insert(p.index).second) continue;
      const WallTime due = p.next.value_or(now);
      if (!next || due < *next) next = due;
    }
  }
  return next;
}

void Notifier::wait_idle() {
  if (!thread_.joinable()) return;
  std::unique_lock lock(mutex_);
  cv_.wait(lock, [this] { return !posted_ && !busy_; });
}

void Notifier::work() {
  std::optional<WallTime> next;
  for (;;) {
    bool stopping = false;
    {
      std::unique_lock lock(mutex_);
      const auto ready = [this] { return stop_ || posted_; };
      // Real time on purpose (virtual clock spec 4.4): this thread is no participant.
      if (next) cv_.wait_until(lock, *next, ready);
      else cv_.wait(lock, ready);
      posted_ = false;
      busy_ = true;
      stopping = stop_;
    }
    next = deliver_due(std::chrono::system_clock::now());
    {
      std::lock_guard lock(mutex_);
      busy_ = false;
    }
    cv_.notify_all();
    if (stopping) return;
  }
}

}  // namespace pychron::experiment::lab
