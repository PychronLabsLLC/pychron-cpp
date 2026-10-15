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
// A dropped connection is repaired by the link's `Link`: a transient error
// (Timeout, Io, NotConnected) reopens the transport with backoff, runs the
// owner's handshake and repeats the command until it succeeds or the call
// window passes. Every Qtegra command is a read or sets an absolute target,
// so every one is Repeat::Safe (executor hardening spec, section 9).
// Borrowers trigger the same repair.

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
  // Runs on the bare transport after a reconnect (and from connect()).
  using Handshake = std::function<Result<void>(Transport&)>;

  // `transport` must outlive the link and be opened by its owner.
  QtegraLink(Transport& transport, codec::qtegra::Terminator terminator, const Clock& clock,
             LinkPolicy policy = {});
  QtegraLink(const QtegraLink&) = delete;
  QtegraLink& operator=(const QtegraLink&) = delete;

  // The owner's connect step, run by the Link after every reopen (its
  // Link::set_connect is this link's, set once). Replacing it waits for a
  // running one to end, so an owner clears it in its destructor and nothing
  // calls into a dead owner afterwards. Empty: reconnect only reopens the
  // transport.
  void set_handshake(Handshake handshake);

  // The handshake on the bare transport, in one transaction, never through
  // the reconnect path.
  Result<void> handshake();

  // One command and its reply on the bare transport: what a driver's op
  // sends inside Device::call(&link(), ...), with the Resend it is given.
  Result<Bytes> exchange(const codec::Command& command, Resend resend);
  // `text` sent with the link's terminator as a Safe call through link();
  // the reply framed by CR or LF.
  Result<Bytes> ask(std::string_view text);

  // The recovery every driver on this connection calls through.
  Link& link() noexcept { return link_; }

  codec::qtegra::Terminator terminator() const noexcept { return terminator_; }
  // Successful reconnects since construction.
  std::uint64_t reconnects() const noexcept { return link_.reconnects(); }

 private:
  // The Link's connect step: the owner's handshake, if any, under its mutex.
  Result<void> run_handshake(Transport& transport);

  Transport& transport_;
  const codec::qtegra::Terminator terminator_;
  // Held while handshake_ runs or is replaced. The handshake talks to the
  // instrument, which waits in clock time: a ClockMutex. Always taken inside
  // a transport transaction, never the other way round.
  ClockMutex handshake_mutex_;
  Handshake handshake_;
  Link link_;  // last: its connect step uses the members above
};

using QtegraLinkRegistry = LinkRegistry<QtegraLink>;
using QtegraLinkHandle = LinkHandle<QtegraLink>;

// The link a driver should use: its own on a real transport, registered under
// `name`, or the registered one when `transport` is a LinkTransport.
Result<QtegraLinkHandle> make_qtegra_link(Transport& transport, const std::string& name,
                                          codec::qtegra::Terminator terminator, const Clock& clock,
                                          LinkPolicy policy = {});

}  // namespace pychron::spectrometer
