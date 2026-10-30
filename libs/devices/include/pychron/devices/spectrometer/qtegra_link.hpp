#pragma once

// The one connection to a Thermo Qtegra RemoteControl server, shared by the
// spectrometer driver (thermo_qtegra) and the extraction-line drivers that
// reach valves and gauge readbacks through Qtegra. Qtegra accepts one client,
// so at melbourne and ldeo they must all use one socket (legacy survey C.5).
//
// Qtegra is strict request/reply. Each exchange() is one Transport::exchange,
// which the transport's queue runs whole, so a valve command never lands
// between an acquisition's GetData and its reply.
//
// A connection is good for one exchange unless its first command is KeepAlive,
// and KeepAlive anywhere else is an error, so the link keeps the session: before
// the first command on a connection, whoever sends it, it opens one (KeepAlive,
// then Describe, then the owner's handshake), and a transient failure ends it.
// RemoteControlServer closes a session idle for 60 s; the next command then
// repairs it like any other drop.
//
// A dropped connection is repaired by the link's `Link`: a transient error
// (Timeout, Io, NotConnected) reopens the transport with backoff, opens a
// session and repeats the command until it succeeds or the call window passes.
// Every Qtegra command but ProtectDetector is a read or sets an absolute target
// and is Repeat::Safe (executor hardening spec, section 9; ProtectDetector:
// thermo_qtegra.hpp). Borrowers trigger the same repair.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "pychron/codecs/codec.hpp"
#include "pychron/codecs/thermo_qtegra.hpp"
#include "pychron/core/clock.hpp"
#include "pychron/core/clock_mutex.hpp"
#include "pychron/core/error.hpp"
#include "pychron/devices/link.hpp"
#include "pychron/devices/link_registry.hpp"
#include "pychron/transport/transport.hpp"

namespace pychron::spectrometer {

class QtegraLink {
 public:
  static constexpr std::string_view kLabel = "Qtegra";
  // Runs on the bare transport when a session opens, and from connect(), with
  // what the server said about itself.
  using Handshake = std::function<Result<void>(Transport&, const codec::qtegra::ServerInfo&)>;

  // `transport` must outlive the link and be opened by its owner.
  QtegraLink(Transport& transport, codec::qtegra::Terminator terminator, const Clock& clock,
             LinkPolicy policy = {});
  QtegraLink(const QtegraLink&) = delete;
  QtegraLink& operator=(const QtegraLink&) = delete;

  // The owner's connect step, run after KeepAlive and Describe (Link's connect
  // step is this link's, set once). Replacing it waits for a running one to
  // end, so an owner clears it in its destructor and nothing calls into a dead
  // owner afterwards. Empty: a session is KeepAlive and Describe alone.
  void set_handshake(Handshake handshake);

  // Opens the session on the bare transport, in one transaction, never through
  // the reconnect path. On a session already open it sends no KeepAlive: it
  // repeats Describe and the owner's handshake.
  Result<void> handshake();

  // One command and its reply on the bare transport: what a driver's op
  // sends inside Device::call(&link(), ...), with the Resend it is given.
  // Opens the session first when there is none.
  Result<Bytes> exchange(const codec::Command& command, Resend resend);
  // `text` sent with the link's terminator as a Safe call through link();
  // the reply framed by LF.
  Result<Bytes> ask(std::string_view text);

  // The recovery every driver on this connection calls through.
  Link& link() noexcept { return link_; }

  codec::qtegra::Terminator terminator() const noexcept { return terminator_; }
  // Successful reconnects since construction.
  std::uint64_t reconnects() const noexcept { return link_.reconnects(); }

 private:
  // The Link's connect step: opens the session, under handshake_mutex_.
  Result<void> run_handshake(Transport& transport);
  // handshake_mutex_ held: KeepAlive unless it has been said on this
  // connection, Describe and its checks, the owner's handshake.
  Result<void> open_session(Transport& transport);

  Transport& transport_;
  const codec::qtegra::Terminator terminator_;
  // Held while handshake_ runs or is replaced. The handshake talks to the
  // instrument, which waits in clock time: a ClockMutex. Always taken inside
  // a transport transaction, never the other way round.
  ClockMutex handshake_mutex_;
  Handshake handshake_;
  // Under handshake_mutex_. KeepAlive was answered on this connection, and it
  // must not be sent again until the connection is new. Kept through a definite
  // refusal (a server too new), so that the next try hears that refusal again
  // and not RemoteControlServer's complaint about a second KeepAlive.
  bool keep_alive_ = false;
  bool session_open_ = false;  // Describe and the owner's handshake passed too
  Link link_;  // last: its connect step uses the members above
};

using QtegraLinkRegistry = LinkRegistry<QtegraLink>;
using QtegraLinkHandle = LinkHandle<QtegraLink>;

// The link a driver should use: its own on a real transport, registered under
// `name`, or the registered one when `transport` is a LinkTransport. Config
// error on a UdpTransport: RemoteControlServer's UDP replies have no line
// ending, and KeepAlive is TCP only.
Result<QtegraLinkHandle> make_qtegra_link(Transport& transport, const std::string& name,
                                          codec::qtegra::Terminator terminator, const Clock& clock,
                                          LinkPolicy policy = {});

}  // namespace pychron::spectrometer
