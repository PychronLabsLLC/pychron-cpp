#pragma once

// The one connection to an Isotopx NGX controller (NGX driver spec section 3),
// shared by the spectrometer driver (isotopx_ngx) and the valve driver
// (ngx_valves). It owns the only reader of the socket: a thread frames lines,
// hands "#EVENT" lines to the event sink and every other line to the one
// command in flight. Clients never touch the transport.
//
// Commands are serialised by a short mutex that is never held across an
// integration, so valves, magnet and source work while an integration runs.
// A command that timed out still owes its reply: the next command waits (up
// to late_reply_window) until that late reply has arrived and been dropped,
// so a reply is never handed to the wrong command.
//
// connect() opens the session: one banner line (content not checked), then
// "Login user,password" when credentials are set. The reader repeats it after
// a dropped connection before any queued command runs, and bumps session().

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "pychron/codecs/isotopx_ngx.hpp"
#include "pychron/core/clock.hpp"
#include "pychron/core/error.hpp"
#include "pychron/transport/transport.hpp"

namespace pychron::spectrometer {

struct NgxLinkOptions {
  std::string send_terminator{codec::ngx::kDefaultSendTerminator};
  std::string user;      // empty: no Login
  std::string password;  // never logged
  Duration command_timeout = std::chrono::seconds(2);
  Duration banner_timeout = std::chrono::seconds(2);
  Duration late_reply_window = std::chrono::seconds(2);
  // How long one reader read waits; also bounds how long a write waits
  // behind a read on the transport's worker.
  Duration read_timeout = std::chrono::milliseconds(10);
  Duration reconnect_min = std::chrono::milliseconds(250);
  Duration reconnect_max = std::chrono::seconds(5);
};

class NgxLink {
 public:
  // Called on the reader thread for every ACQ / ACQ.B line: the frame, the
  // host clock when it was read, and the session it arrived in.
  using EventSink = std::function<void(const codec::ngx::AcqFrame&, TimePoint, std::uint64_t session)>;

  // `transport` must outlive the link and be opened by its owner.
  NgxLink(Transport& transport, NgxLinkOptions options, const Clock& clock);
  ~NgxLink();  // stops the reader
  NgxLink(const NgxLink&) = delete;
  NgxLink& operator=(const NgxLink&) = delete;

  // The session handshake, then the reader. Idempotent; ask() calls it.
  Result<void> connect();
  bool up() const;
  // Bumped by every (re)connect; events carry the session they arrived in.
  std::uint64_t session() const;

  // One command; the reply line without terminator. An "Exx" reply other
  // than E00 is the error it names (codec::ngx::error_kind).
  Result<std::string> ask(std::string_view command, std::optional<Duration> timeout = std::nullopt);

  // Replaces the sink; returns once no call to the old one is in progress.
  void set_event_sink(EventSink sink);
  bool has_event_sink() const;
  // Held by a valve actuation (SAB 1 .. SAB 0) so actuations never overlap.
  std::mutex& valve_mutex() noexcept { return valve_mutex_; }

  struct Stats {
    std::uint64_t replies_dropped = 0;  // no command waiting for them
    std::uint64_t late_replies = 0;     // owed by a timed-out command, dropped
    std::uint64_t events = 0;
    std::uint64_t bad_events = 0;       // "#EVENT:ACQ..." that did not decode
    std::uint64_t reconnects = 0;
  };
  Stats stats() const;

  const NgxLinkOptions& options() const noexcept { return options_; }

 private:
  // On the bare transport; only before the reader runs or on the reader.
  Result<void> handshake();
  Result<std::string> read_line(Duration timeout);
  Result<void> send(std::string_view command);
  void reader();
  void route(std::string line);
  void went_down(const Error& why);

  Transport& transport_;
  const NgxLinkOptions options_;
  const Clock& clock_;

  std::mutex connect_mutex_;  // connect() vs connect()
  std::mutex command_mutex_;  // one command in flight
  std::mutex valve_mutex_;

  mutable std::mutex mutex_;  // everything below
  std::condition_variable cv_;
  bool started_ = false, up_ = false, stop_ = false;
  std::optional<Error> down_reason_;
  std::uint64_t session_ = 0;
  std::uint64_t armed_ = 0, next_command_ = 0;  // armed_: the command awaiting a reply, 0 none
  std::optional<std::string> reply_;
  std::optional<std::chrono::steady_clock::time_point> owed_until_;  // a timed-out command's reply
  EventSink sink_;
  int sink_calls_ = 0;  // deliveries in progress (outside mutex_)
  Stats stats_;
  std::thread thread_;
};

// Process-wide NGX links by name, so a driver whose transport is a
// LinkTransport finds the link another config file's driver owns.
class NgxLinkRegistry {
 public:
  static NgxLinkRegistry& global();
  // Config when `name` is already registered to a live link.
  Result<void> add(const std::string& name, const std::shared_ptr<NgxLink>& link);
  void remove(const std::string& name, const NgxLink* link);
  // NotConnected when no live link has that name.
  Result<std::shared_ptr<NgxLink>> find(const std::string& name) const;

 private:
  mutable std::mutex mutex_;
  std::map<std::string, std::weak_ptr<NgxLink>> links_;
};

// The link a driver should use: its own on a real transport (created and
// registered under `name`), or the registered one when `transport` is a
// LinkTransport. Owned links unregister when the returned handle is released.
class NgxLinkHandle {
 public:
  static Result<NgxLinkHandle> make(Transport& transport, const std::string& name, NgxLinkOptions options,
                                    const Clock& clock);
  NgxLinkHandle(NgxLinkHandle&&) noexcept = default;
  NgxLinkHandle& operator=(NgxLinkHandle&&) noexcept = default;
  ~NgxLinkHandle();

  // The link to use now (a borrowed one is looked up on every call).
  Result<std::shared_ptr<NgxLink>> get() const;
  bool owner() const noexcept { return static_cast<bool>(owned_); }
  const std::string& name() const noexcept { return name_; }

 private:
  NgxLinkHandle() = default;
  std::string name_;
  std::shared_ptr<NgxLink> owned_;
};

}  // namespace pychron::spectrometer
