#include "pychron/devices/proxr_board_sim.hpp"

namespace pychron {

namespace proxr = codec::proxr;

ProxrBoardSim::ProxrBoardSim(Listener on_actuate) : on_actuate_(std::move(on_actuate)) {}

Bytes ProxrBoardSim::respond(const Bytes& tx) {
  if (tx.size() < 2 || tx[0] != proxr::kPrefix) return {};
  const std::uint8_t op = tx[1];

  if (op == proxr::kSelectBank) {
    if (tx.size() != 3 || tx[2] < 1 || tx[2] > proxr::kBanks) return {};
    bank_ = tx[2];
    return Bytes{proxr::kAck};
  }
  if (tx.size() != 2) return {};

  const int bank = bank_;
  if (op >= proxr::kRelayStatusBase && op < proxr::kRelayStatusBase + proxr::kRelaysPerBank) {
    std::int64_t index = (bank - 1) * proxr::kRelaysPerBank + (op - proxr::kRelayStatusBase);
    return Bytes{static_cast<std::uint8_t>(energized(index) ? 1 : 0)};
  }
  if (op < proxr::kRelayOnBase + proxr::kRelaysPerBank) {
    const bool on = op >= proxr::kRelayOnBase;
    std::int64_t index = (bank - 1) * proxr::kRelaysPerBank + (op % proxr::kRelaysPerBank);
    set_energized(index, on);
    if (on_actuate_) on_actuate_(index, on);
    return Bytes{proxr::kAck};
  }
  return {};
}

SimTransport::Hook ProxrBoardSim::hook() {
  return [this](const Bytes& tx) { return respond(tx); };
}

bool ProxrBoardSim::energized(std::int64_t index) const {
  if (index < 0 || index >= static_cast<std::int64_t>(kRelays)) return false;
  return relays_[static_cast<std::size_t>(index)];
}

void ProxrBoardSim::set_energized(std::int64_t index, bool on) {
  if (index < 0 || index >= static_cast<std::int64_t>(kRelays)) return;
  relays_[static_cast<std::size_t>(index)] = on;
}

int ProxrBoardSim::selected_bank() const { return bank_; }

}  // namespace pychron
