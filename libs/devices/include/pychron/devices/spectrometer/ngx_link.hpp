#pragma once

// The one connection to an Isotopx NGX controller (NGX driver spec section 3),
// shared by the spectrometer driver (isotopx_ngx) and the valve driver
// (ngx_valves). It owns the only reader of the socket: a thread frames lines,
// hands "#EVENT" lines to the event sink and every other line to the one
// command in flight. Clients never touch the transport.
//
// Commands are serialised by a mutex that is held for one command and never
// across an integration, so valves, magnet and source work while an
// integration runs. It, the connect mutex and the valve mutex are held while
// their holder waits in clock time, so they are ClockMutex: a thread waiting
// for one is asleep in the clock, not in the kernel.
// A command that timed out still owes its reply: the next command waits (up
// to late_reply_window) until that late reply has arrived and been dropped,
// so a reply is never handed to the wrong command.
//
// connect() opens the session: one banner line (content not checked), then
// "Login user,password" when credentials are set. The reader repeats it after
// a dropped connection before any queued command runs, and bumps session().
//
// Every timeout, window and backoff is time on the link's clock, and the
// reader is a participant in it (Clock::Participant): the transport should
// keep the same clock.

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
#include <string_view>
#include <thread>

#include "pychron/codecs/isotopx_ngx.hpp"
#include "pychron/core/clock.hpp"
#include "pychron/core/clock_mutex.hpp"
#include "pychron/core/error.hpp"
#include "pychron/devices/link.hpp"
#include "pychron/devices/link_registry.hpp"
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
  // How long one call() keeps trying (LinkPolicy::call_window).
  Duration call_window = std::chrono::seconds(20);
};

class NgxLink {
 public:
  static constexpr std::string_view kLabel = "NGX";

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
  // than E00 is the error it names (codec::ngx::error_kind). Sent once.
  Result<std::string> ask(std::string_view command, std::optional<Duration> timeout = std::nullopt);

  // A request as one unit: one ask, or several made under the caller's own
  // mutex (a valve actuation's SAB 1 .. SAB 0). What Link::call (link.hpp)
  // does for a transport, with the reconnect left to the reader: when `op`
  // fails with a transient error (Timeout, Io, NotConnected), a Safe call
  // waits for the session after the one its op ran in and runs `op` again,
  // until it succeeds or `window` (default call_window) passes, and then
  // returns the last error. A Timeout on a link still up is run again at
  // once (ask has waited out the owed reply). An Unsafe call is never run
  // again: it waits for the link to come back, within the window, and
  // returns its error with code kOutcomeUnknown. Before connect() has
  // started the reader, the link is reopened here, at the reader's backoff.
  // Definite errors pass through at once.
  template <class T>
  [[nodiscard]] Result<T> call(const std::function<Result<T>()>& op, Repeat repeat,
                               std::optional<Duration> window = std::nullopt) {
    const TimePoint deadline = clock_.now() + window.value_or(options_.call_window);
    Duration backoff = options_.reconnect_min;
    for (;;) {
      const std::uint64_t seen = session();
      Result<T> r = op();
      if (r || !transient(r.error().kind)) return r;
      const bool back = await_back(seen, r.error().kind, deadline, backoff);
      if (repeat == Repeat::Unsafe) {
        Error error = std::move(r).error();
        error.code = kOutcomeUnknown;
        return fail(std::move(error));
      }
      if (!back) return r;
      note_repeat();
    }
  }

  // Replaces the sink; returns once no call to the old one is in progress.
  void set_event_sink(EventSink sink);
  bool has_event_sink() const;
  // Held by a valve actuation (SAB 1 .. SAB 0) so actuations never overlap.
  ClockMutex& valve_mutex() noexcept { return valve_mutex_; }

  struct Stats {
    std::uint64_t replies_dropped = 0;  // no command waiting for them
    std::uint64_t late_replies = 0;     // owed by a timed-out command, dropped
    std::uint64_t events = 0;
    std::uint64_t bad_events = 0;       // "#EVENT:ACQ..." that did not decode
    std::uint64_t reconnects = 0;
    std::uint64_t repeats = 0;          // a Safe call() run again
  };
  Stats stats() const;

  const NgxLinkOptions& options() const noexcept { return options_; }

 private:
  // On the bare transport; only before the reader runs or on the reader.
  Result<void> handshake();
  Result<std::string> read_line(Duration timeout);
  Result<void> send(std::string_view command);
  void reader(std::shared_ptr<Clock::Hold> started);
  void route(std::string line);
  void went_down(const Error& why);
  static bool transient(ErrorKind kind) noexcept {
    return kind == ErrorKind::Timeout || kind == ErrorKind::Io || kind == ErrorKind::NotConnected;
  }
  // After a call's op failed with `kind` in session `seen`: true once the
  // link is there to run it again, false when `deadline` passed first.
  bool await_back(std::uint64_t seen, ErrorKind kind, TimePoint deadline, Duration& backoff);
  void note_repeat();

  Transport& transport_;
  const NgxLinkOptions options_;
  const Clock& clock_;

  // Each is held across waits in clock time (a handshake, a reply, a whole
  // actuation). Order: valve, then connect or command, then mutex_.
  ClockMutex connect_mutex_;  // connect() vs connect()
  ClockMutex command_mutex_;  // one command in flight
  ClockMutex valve_mutex_;

  mutable std::mutex mutex_;    // everything below
  std::condition_variable cv_;  // waited on and notified through clock_
  bool started_ = false, up_ = false, stop_ = false;
  bool reader_done_ = false;  // the reader has left its loop
  std::optional<Error> down_reason_;
  std::uint64_t session_ = 0;
  std::uint64_t armed_ = 0, next_command_ = 0;  // armed_: the command awaiting a reply, 0 none
  std::optional<std::string> reply_;
  std::optional<TimePoint> owed_until_;  // a timed-out command's reply
  EventSink sink_;
  int sink_calls_ = 0;  // deliveries in progress (outside mutex_)
  Stats stats_;
  std::thread thread_;
};

// Process-wide NGX links by name (see link_registry.hpp).
using NgxLinkRegistry = LinkRegistry<NgxLink>;
using NgxLinkHandle = LinkHandle<NgxLink>;

// The link a driver should use: its own NgxLink on a real transport,
// registered under `name`, or the registered one when `transport` is a
// LinkTransport.
Result<NgxLinkHandle> make_ngx_link(Transport& transport, const std::string& name, NgxLinkOptions options,
                                    const Clock& clock);

}  // namespace pychron::spectrometer
