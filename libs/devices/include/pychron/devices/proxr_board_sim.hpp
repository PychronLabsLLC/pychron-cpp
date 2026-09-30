#pragma once

// Stateful model of an NCD ProXR relay board, for SimSystem.
//
// Hook contract: SimSystem owns one ProxrBoardSim per `proxr_relay` driver
// and builds that driver's transport with SimTransport::hooked(board.hook()).
// The board answers exactly as hardware would (see pychron/codecs/proxr.hpp);
// malformed or unsupported commands get no reply, so the driver sees a
// Timeout. The listener fires for every energize/de-energize command, on the
// transport's worker thread, with the flat relay index (the valve address)
// and the commanded state; SimSystem maps index -> valve and updates its
// network model from there. set_energized() models changes made outside the
// driver (manual override) and does not notify.
//
// Relay state and the selected bank are atomics, so energized() may be
// called from any thread while the transport worker drives the board.

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>

#include "pychron/codecs/proxr.hpp"
#include "pychron/transport/bytes.hpp"
#include "pychron/transport/sim_transport.hpp"

namespace pychron {

class ProxrBoardSim {
 public:
  using Listener = std::function<void(std::int64_t index, bool energized)>;

  explicit ProxrBoardSim(Listener on_actuate = {});

  ProxrBoardSim(const ProxrBoardSim&) = delete;
  ProxrBoardSim& operator=(const ProxrBoardSim&) = delete;

  // One command in, the board's reply out (empty = no reply).
  Bytes respond(const Bytes& tx);
  // respond() as a SimTransport hook. The board must outlive the transport.
  SimTransport::Hook hook();

  // False for indices outside 0..255.
  bool energized(std::int64_t index) const;
  // Ignored for indices outside 0..255.
  void set_energized(std::int64_t index, bool on);
  int selected_bank() const;

 private:
  static constexpr std::size_t kRelays = codec::proxr::kBanks * codec::proxr::kRelaysPerBank;

  Listener on_actuate_;
  std::atomic<int> bank_{1};
  std::array<std::atomic<bool>, kRelays> relays_{};
};

}  // namespace pychron
