#include "pychron/devices/spectrometer/ngx_link.hpp"

#include <algorithm>

#include "pychron/transport/link_transport.hpp"

namespace pychron::spectrometer {

namespace ngx = codec::ngx;

namespace {

using SteadyTime = std::chrono::steady_clock::time_point;

SteadyTime steady_now() { return std::chrono::steady_clock::now(); }

std::string trim(std::string s) {
  auto ws = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
  while (!s.empty() && ws(s.back())) s.pop_back();
  std::size_t i = 0;
  while (i < s.size() && ws(s[i])) ++i;
  return s.substr(i);
}

// "Exx" (xx != 00) at the start of a reply, up to an optional ",text".
std::optional<int> error_code(std::string_view reply) {
  const auto head = reply.substr(0, reply.find(','));
  if (head.size() != 3 || head[0] != 'E' || head[1] < '0' || head[1] > '9' || head[2] < '0' || head[2] > '9') {
    return std::nullopt;
  }
  const int code = (head[1] - '0') * 10 + (head[2] - '0');
  return code == 0 ? std::nullopt : std::optional<int>(code);
}

// The command as it may appear in an error or a log: never a password.
std::string shown(std::string_view command) {
  if (command.starts_with("Login ")) {
    const auto comma = command.find(',');
    return std::string(command.substr(0, comma)) + ",****";
  }
  return std::string(command);
}

}  // namespace

NgxLink::NgxLink(Transport& transport, NgxLinkOptions options, const Clock& clock)
    : transport_(transport), options_(std::move(options)), clock_(clock) {}

NgxLink::~NgxLink() {
  {
    std::lock_guard lock(mutex_);
    stop_ = true;
  }
  cv_.notify_all();
  if (thread_.joinable()) thread_.join();
}

bool NgxLink::up() const {
  std::lock_guard lock(mutex_);
  return up_;
}

std::uint64_t NgxLink::session() const {
  std::lock_guard lock(mutex_);
  return session_;
}

NgxLink::Stats NgxLink::stats() const {
  std::lock_guard lock(mutex_);
  return stats_;
}

void NgxLink::set_event_sink(EventSink sink) {
  std::unique_lock lock(mutex_);
  sink_ = std::move(sink);
  // The reader thread never waits here: it only delivers.
  if (thread_.get_id() != std::this_thread::get_id()) cv_.wait(lock, [&] { return sink_calls_ == 0; });
}

bool NgxLink::has_event_sink() const {
  std::lock_guard lock(mutex_);
  return static_cast<bool>(sink_);
}

Result<std::string> NgxLink::read_line(Duration timeout) {
  auto r = transport_.read(ReadSpec::until(ngx::kLineEnd), timeout);
  if (!r) return fail(std::move(r).error());
  return ::pychron::to_string(*r);
}

Result<void> NgxLink::send(std::string_view command) {
  std::string tx(command);
  tx += options_.send_terminator;
  return transport_.write(to_bytes(tx));
}

Result<void> NgxLink::handshake() {
  // One greeting line first; legacy logs it and never checks it.
  if (auto banner = read_line(options_.banner_timeout); !banner && banner.error().kind != ErrorKind::Timeout) {
    return fail(std::move(banner).error());
  }
  if (options_.user.empty()) return {};
  const std::string login = "Login " + options_.user + "," + options_.password;
  if (auto w = send(login); !w) return fail(std::move(w).error());
  const auto deadline = steady_now() + options_.command_timeout;
  for (;;) {
    const auto left = std::chrono::duration_cast<Duration>(deadline - steady_now());
    if (left <= Duration::zero()) return fail(ErrorKind::Timeout, "NGX: no reply to " + shown(login));
    auto line = read_line(left);
    if (!line) {
      if (line.error().kind == ErrorKind::Timeout) continue;
      return fail(std::move(line).error());
    }
    if (line->starts_with(ngx::kEventPrefix)) continue;  // not ours
    const std::string reply = trim(*line);
    if (reply.empty()) continue;
    if (auto code = error_code(reply)) {
      return fail(ngx::error_kind(*code), "NGX refused " + shown(login) + ": " + reply + " (" +
                                              std::string(ngx::error_text(*code)) + ")");
    }
    return {};
  }
}

Result<void> NgxLink::connect() {
  std::lock_guard connecting(connect_mutex_);
  {
    std::unique_lock lock(mutex_);
    if (started_) {
      if (up_) return {};
      // The reader is reconnecting; give it one command timeout.
      cv_.wait_for(lock, options_.command_timeout, [&] { return up_ || stop_; });
      if (up_) return {};
      return fail(ErrorKind::NotConnected,
                  "NGX link is down" + (down_reason_ ? ": " + down_reason_->what : std::string()));
    }
  }
  if (auto h = handshake(); !h) return h;
  {
    std::lock_guard lock(mutex_);
    started_ = true;
    up_ = true;
    ++session_;
  }
  thread_ = std::thread([this] { reader(); });
  return {};
}

void NgxLink::went_down(const Error& why) {
  {
    std::lock_guard lock(mutex_);
    up_ = false;
    down_reason_ = why;
  }
  cv_.notify_all();
}

void NgxLink::reader() {
  Duration backoff = options_.reconnect_min;
  for (;;) {
    {
      std::lock_guard lock(mutex_);
      if (stop_) return;
    }
    const auto t0 = steady_now();
    auto polled = transport_.poll(ReadSpec::until(ngx::kLineEnd), options_.read_timeout);
    if (polled && *polled) {
      route(::pychron::to_string(**polled));
      continue;
    }
    if (polled) {
      // Nothing yet. A transport that answers at once (virtual time) must not spin.
      if (steady_now() - t0 < options_.read_timeout / 2) {
        std::unique_lock lock(mutex_);
        cv_.wait_for(lock, options_.read_timeout, [&] { return stop_; });
      }
      continue;
    }
    went_down(polled.error());
    // Reconnect: reopen, repeat the session, then carry on reading.
    for (;;) {
      {
        std::unique_lock lock(mutex_);
        if (cv_.wait_for(lock, backoff, [&] { return stop_; })) return;
      }
      backoff = std::min(backoff * 2, options_.reconnect_max);
      transport_.close();
      if (auto opened = transport_.open(); !opened) {
        went_down(opened.error());
        continue;
      }
      if (auto h = handshake(); !h) {
        went_down(h.error());
        continue;
      }
      {
        std::lock_guard lock(mutex_);
        up_ = true;
        down_reason_.reset();
        ++session_;
        ++stats_.reconnects;
      }
      cv_.notify_all();
      backoff = options_.reconnect_min;
      break;
    }
  }
}

void NgxLink::route(std::string line) {
  if (std::string_view(line).starts_with(ngx::kEventPrefix)) {
    if (!std::string_view(line).starts_with("#EVENT:ACQ")) return;  // other events are not used
    auto frame = ngx::decode_acq_event(to_bytes(line));
    EventSink sink;
    std::uint64_t session = 0;
    {
      std::lock_guard lock(mutex_);
      if (!frame) {
        ++stats_.bad_events;
        return;
      }
      ++stats_.events;
      sink = sink_;
      session = session_;
      if (sink) ++sink_calls_;
    }
    if (sink) {
      sink(*frame, clock_.now(), session);
      {
        std::lock_guard lock(mutex_);
        --sink_calls_;
      }
      cv_.notify_all();
    }
    return;
  }
  std::string reply = trim(line);
  if (reply.empty()) return;
  {
    std::lock_guard lock(mutex_);
    if (armed_ != 0 && !reply_) {
      reply_ = std::move(reply);
    } else if (owed_until_ && steady_now() < *owed_until_) {
      owed_until_.reset();
      ++stats_.late_replies;
    } else {
      ++stats_.replies_dropped;
      return;
    }
  }
  cv_.notify_all();
}

Result<std::string> NgxLink::ask(std::string_view command, std::optional<Duration> timeout) {
  const Duration limit = timeout.value_or(options_.command_timeout);
  if (auto c = connect(); !c) return fail(std::move(c).error());
  std::lock_guard one_at_a_time(command_mutex_);
  std::unique_lock lock(mutex_);
  // A timed-out command's reply may still come: wait for it (or for the
  // window to pass) so it is never taken for this command's.
  if (owed_until_) {
    const auto until = *owed_until_;
    cv_.wait_until(lock, until, [&] { return !owed_until_ || stop_; });
    owed_until_.reset();
  }
  if (!up_) {
    return fail(ErrorKind::NotConnected, "NGX link is down" + (down_reason_ ? ": " + down_reason_->what : std::string()));
  }
  armed_ = ++next_command_;
  reply_.reset();
  lock.unlock();
  auto written = send(command);
  lock.lock();
  if (!written) {
    armed_ = 0;
    return fail(std::move(written).error());
  }
  cv_.wait_until(lock, steady_now() + limit, [&] { return reply_.has_value() || !up_ || stop_; });
  armed_ = 0;
  if (!reply_) {
    if (!up_) return fail(ErrorKind::NotConnected, "NGX link dropped during " + shown(command));
    owed_until_ = steady_now() + options_.late_reply_window;
    return fail(ErrorKind::Timeout, "NGX: no reply to " + shown(command));
  }
  std::string reply = std::move(*reply_);
  reply_.reset();
  lock.unlock();
  if (auto code = error_code(reply)) {
    return fail(ngx::error_kind(*code),
                "NGX " + reply + " (" + std::string(ngx::error_text(*code)) + ") to " + shown(command));
  }
  return reply;
}

// --- registry ---------------------------------------------------------------

NgxLinkRegistry& NgxLinkRegistry::global() {
  static NgxLinkRegistry registry;
  return registry;
}

Result<void> NgxLinkRegistry::add(const std::string& name, const std::shared_ptr<NgxLink>& link) {
  std::lock_guard lock(mutex_);
  auto it = links_.find(name);
  if (it != links_.end() && !it->second.expired()) {
    return fail(ErrorKind::Config, "NGX link '" + name +
                                       "' is already open; declare its transport once and use kind = \"link\" "
                                       "elsewhere");
  }
  links_[name] = link;
  return {};
}

void NgxLinkRegistry::remove(const std::string& name, const NgxLink* link) {
  std::lock_guard lock(mutex_);
  auto it = links_.find(name);
  if (it == links_.end()) return;
  auto live = it->second.lock();
  if (!live || live.get() == link) links_.erase(it);
}

Result<std::shared_ptr<NgxLink>> NgxLinkRegistry::find(const std::string& name) const {
  std::lock_guard lock(mutex_);
  auto it = links_.find(name);
  if (it != links_.end()) {
    if (auto live = it->second.lock()) return live;
  }
  return fail(ErrorKind::NotConnected, "NGX link '" + name + "' is not running (is the config that owns it loaded?)");
}

// --- handle -----------------------------------------------------------------

Result<NgxLinkHandle> NgxLinkHandle::make(Transport& transport, const std::string& name, NgxLinkOptions options,
                                          const Clock& clock) {
  NgxLinkHandle h;
  if (const auto* link = dynamic_cast<const LinkTransport*>(&transport)) {
    h.name_ = link->link();
    return h;
  }
  h.name_ = name;
  auto owned = std::make_shared<NgxLink>(transport, std::move(options), clock);
  if (auto added = NgxLinkRegistry::global().add(name, owned); !added) return fail(std::move(added).error());
  h.owned_ = std::move(owned);
  return h;
}

NgxLinkHandle::~NgxLinkHandle() {
  if (owned_) NgxLinkRegistry::global().remove(name_, owned_.get());
}

Result<std::shared_ptr<NgxLink>> NgxLinkHandle::get() const {
  if (owned_) return owned_;
  return NgxLinkRegistry::global().find(name_);
}

}  // namespace pychron::spectrometer
