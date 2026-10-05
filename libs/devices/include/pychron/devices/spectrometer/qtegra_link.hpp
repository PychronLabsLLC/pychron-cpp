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
// A dropped connection (Io / NotConnected) is repaired once per command, as
// the spectrometer driver always did: reopen the transport, run the owner's
// handshake, retry. Borrowers trigger the same repair.

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include "pychron/codecs/codec.hpp"
#include "pychron/codecs/thermo_qtegra.hpp"
#include "pychron/core/clock.hpp"
#include "pychron/core/error.hpp"
#include "pychron/devices/link_registry.hpp"
#include "pychron/devices/reconnect.hpp"
#include "pychron/transport/transport.hpp"

namespace pychron::spectrometer {

class QtegraLink {
 public:
  static constexpr std::string_view kLabel = "Qtegra";
  // Runs on the bare transport after a reconnect (and from connect()).
  using Handshake = std::function<Result<void>(Transport&)>;

  // `transport` must outlive the link and be opened by its owner.
  QtegraLink(Transport& transport, codec::qtegra::Terminator terminator, const Clock& clock);
  QtegraLink(const QtegraLink&) = delete;
  QtegraLink& operator=(const QtegraLink&) = delete;

  // The owner's connect step. Replacing it waits for a running one to end,
  // so an owner clears it in its destructor and nothing calls into a dead
  // owner afterwards. Empty: reconnect only reopens the transport.
  void set_handshake(Handshake handshake);

  // The handshake on the bare transport, never through the reconnect path.
  Result<void> handshake();

  // One command and its reply, through the reconnect path.
  Result<Bytes> exchange(const codec::Command& command);
  // `text` sent with the link's terminator; the reply framed by CR or LF.
  Result<Bytes> ask(std::string_view text);

  codec::qtegra::Terminator terminator() const noexcept { return terminator_; }
  // Successful reconnects since construction.
  std::uint64_t reconnects() const noexcept { return reconnector_.reconnects(); }

 private:
  Transport& transport_;
  const codec::qtegra::Terminator terminator_;
  Reconnector reconnector_;
  std::mutex handshake_mutex_;  // held while handshake_ runs or is replaced
  Handshake handshake_;
};

using QtegraLinkRegistry = LinkRegistry<QtegraLink>;
using QtegraLinkHandle = LinkHandle<QtegraLink>;

// The link a driver should use: its own on a real transport, registered under
// `name`, or the registered one when `transport` is a LinkTransport.
Result<QtegraLinkHandle> make_qtegra_link(Transport& transport, const std::string& name,
                                          codec::qtegra::Terminator terminator, const Clock& clock);

}  // namespace pychron::spectrometer
