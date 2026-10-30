#pragma once

// An emulator in the form a socket server needs (sim serve spec,
// docs/superpowers/specs/2026-10-09-sim-serve-design.md section 4.1): the
// hook a SimTransport would be given, and how one request is cut from the
// stream a driver writes. In-process, the transport hands the hook one whole
// write; on a socket, writes arrive in pieces and run together.

#include <cstddef>
#include <functional>
#include <optional>
#include <string>

#include "pychron/transport/bytes.hpp"
#include "pychron/transport/read_spec.hpp"
#include "pychron/transport/sim_transport.hpp"

namespace pychron::sim {

// The length of the first whole request at the start of a buffer; nullopt
// while it is incomplete. Taken from the driver's codec, never a second copy
// of its framing.
using RequestFramer = std::function<std::optional<std::size_t>(const Bytes&)>;

// frame_length(spec, buffer).
RequestFramer framer(ReadSpec spec);

struct WireSim {
  SimTransport::Hook hook;                // one request -> its reply (empty: none)
  SimTransport::Unsolicited unsolicited;  // may be empty
  RequestFramer request;                  // never empty
  std::function<void()> on_connect;       // may be empty
  // The device answers one command per connection and closes it
  // (pychron_valves: the driver reads its reply until the peer closes).
  bool close_after_reply = false;
  // The driver kind(s) answered as, `+` between the kinds of a shared PLC;
  // "" for a silent wire.
  std::string emulator;
};

}  // namespace pychron::sim
