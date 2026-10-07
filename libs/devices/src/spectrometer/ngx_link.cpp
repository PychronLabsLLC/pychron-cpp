#include "pychron/devices/spectrometer/ngx_link.hpp"

#include <algorithm>
#include <system_error>

#include "pychron/transport/link_transport.hpp"

namespace pychron::spectrometer {

namespace ngx = codec::ngx;

namespace {

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
    : transport_(transport),
      options_(std::move(options)),
      clock_(clock),
      connect_mutex_(clock),
      command_mutex_(clock),
      valve_mutex_(clock) {}

NgxLink::~NgxLink() {
  {
    std::unique_lock lock(mutex_);
    stop_ = true;
    clock_.notify_all(cv_);
    // The wait is in the clock, where the reader says it has finished: what
    // it is busy with (a read, a backoff) may take clock time, and a join
    // alone would stop that.
    while (started_ && !reader_done_) clock_.wait(cv_, lock);
  }
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
  if (thread_.get_id() != std::this_thread::get_id()) {
    while (sink_calls_ != 0) clock_.wait(cv_, lock);
  }
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
  const TimePoint deadline = clock_.now() + options_.command_timeout;
  for (;;) {
    const Duration left = deadline - clock_.now();
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
      const TimePoint until = clock_.now() + options_.command_timeout;
      while (!up_ && !stop_ && clock_.now() < until) clock_.wait_until(cv_, lock, until);
      if (up_) return {};
      return fail(ErrorKind::NotConnected,
                  "NGX link is down" + (down_reason_ ? ": " + down_reason_->what : std::string()));
    }
  }
  if (auto h = handshake(); !h) return h;
  // Time does not jump until the reader has entered the clock.
  auto hold = std::make_shared<Clock::Hold>(clock_);
  try {
    // The reader's first look at the link waits for mutex_, so it sees the
    // link started. A thread that could not be made leaves it not started:
    // the destructor waits for no reader, and a later connect() tries again.
    std::lock_guard lock(mutex_);
    thread_ = std::thread([this, hold]() mutable { reader(std::move(hold)); });
    started_ = true;
    up_ = true;
    ++session_;
  } catch (const std::system_error& e) {
    return fail(ErrorKind::Io, std::string("NGX: the reader thread could not be started: ") + e.what());
  }
  hold.reset();
  return {};
}

void NgxLink::went_down(const Error& why) {
  {
    std::lock_guard lock(mutex_);
    up_ = false;
    down_reason_ = why;
  }
  clock_.notify_all(cv_);
}

void NgxLink::reader(std::shared_ptr<Clock::Hold> started) {
  Clock::Participant participant(clock_, "ngx.reader");
  started.reset();
  // Said while this thread is still a participant: the destructor is runnable
  // again before the clock stops counting the reader.
  struct Done {
    NgxLink& link;
    ~Done() {
      {
        std::lock_guard lock(link.mutex_);
        link.reader_done_ = true;
      }
      link.clock_.notify_all(link.cv_);
    }
  } done{*this};
  Duration backoff = options_.reconnect_min;
  for (;;) {
    {
      std::lock_guard lock(mutex_);
      if (stop_) return;
    }
    const TimePoint t0 = clock_.now();
    auto polled = transport_.poll(ReadSpec::until(ngx::kLineEnd), options_.read_timeout);
    if (polled && *polled) {
      route(::pychron::to_string(**polled));
      continue;
    }
    if (polled) {
      // Nothing yet. A transport that answers at once (a simulated one with
      // no event source) must not spin.
      if (clock_.now() - t0 < options_.read_timeout / 2) {
        std::unique_lock lock(mutex_);
        const TimePoint until = clock_.now() + options_.read_timeout;
        while (!stop_ && clock_.now() < until) clock_.wait_until(cv_, lock, until);
      }
      continue;
    }
    went_down(polled.error());
    // Reconnect: reopen, repeat the session, then carry on reading.
    for (;;) {
      {
        std::unique_lock lock(mutex_);
        const TimePoint until = clock_.now() + backoff;
        while (!stop_ && clock_.now() < until) clock_.wait_until(cv_, lock, until);
        if (stop_) return;
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
      clock_.notify_all(cv_);
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
      clock_.notify_all(cv_);
    }
    return;
  }
  std::string reply = trim(line);
  if (reply.empty()) return;
  {
    std::lock_guard lock(mutex_);
    if (armed_ != 0 && !reply_) {
      reply_ = std::move(reply);
    } else if (owed_until_ && clock_.now() < *owed_until_) {
      owed_until_.reset();
      ++stats_.late_replies;
    } else {
      ++stats_.replies_dropped;
      return;
    }
  }
  clock_.notify_all(cv_);
}

Result<std::string> NgxLink::ask(std::string_view command, std::optional<Duration> timeout) {
  const Duration limit = timeout.value_or(options_.command_timeout);
  if (auto c = connect(); !c) return fail(std::move(c).error());
  std::lock_guard one_at_a_time(command_mutex_);
  std::unique_lock lock(mutex_);
  // A timed-out command's reply may still come: wait for it (or for the
  // window to pass) so it is never taken for this command's.
  if (owed_until_) {
    const TimePoint until = *owed_until_;
    while (owed_until_ && !stop_ && clock_.now() < until) clock_.wait_until(cv_, lock, until);
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
  const TimePoint until = clock_.now() + limit;
  while (!reply_ && up_ && !stop_ && clock_.now() < until) clock_.wait_until(cv_, lock, until);
  armed_ = 0;
  if (!reply_) {
    if (!up_) return fail(ErrorKind::NotConnected, "NGX link dropped during " + shown(command));
    owed_until_ = clock_.now() + options_.late_reply_window;
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

// --- handle -----------------------------------------------------------------

Result<NgxLinkHandle> make_ngx_link(Transport& transport, const std::string& name, NgxLinkOptions options,
                                    const Clock& clock) {
  return NgxLinkHandle::make(transport, name, [&](Transport& t) {
    return std::make_shared<NgxLink>(t, std::move(options), clock);
  });
}

}  // namespace pychron::spectrometer
