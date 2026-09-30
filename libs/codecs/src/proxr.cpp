#include "pychron/codecs/proxr.hpp"

#include <string>

namespace pychron::codec::proxr {

namespace {

Result<void> check_relay(int relay) {
  if (relay < 0 || relay >= kRelaysPerBank) {
    return fail(ErrorKind::Config, "ProXR relay " + std::to_string(relay) + " outside 0.." +
                                       std::to_string(kRelaysPerBank - 1));
  }
  return {};
}

Result<Command> relay_command(std::uint8_t base, int relay) {
  if (auto ok = check_relay(relay); !ok) return fail(ok.error());
  return Command{Bytes{kPrefix, static_cast<std::uint8_t>(base + relay)}, ReadSpec::fixed(1)};
}

}  // namespace

Result<RelayAddress> relay_address(std::int64_t index) {
  if (index < 0 || index > kMaxIndex) {
    return fail(ErrorKind::Config,
                "ProXR relay index " + std::to_string(index) + " outside 0.." + std::to_string(kMaxIndex));
  }
  return RelayAddress{static_cast<int>(index / kRelaysPerBank) + 1, static_cast<int>(index % kRelaysPerBank)};
}

Result<Command> select_bank(int bank) {
  if (bank < 1 || bank > kBanks) {
    return fail(ErrorKind::Config, "ProXR bank " + std::to_string(bank) + " outside 1.." + std::to_string(kBanks));
  }
  return Command{Bytes{kPrefix, kSelectBank, static_cast<std::uint8_t>(bank)}, ReadSpec::fixed(1)};
}

Result<Command> relay_on(int relay) { return relay_command(kRelayOnBase, relay); }
Result<Command> relay_off(int relay) { return relay_command(kRelayOffBase, relay); }
Result<Command> read_relay(int relay) { return relay_command(kRelayStatusBase, relay); }

Result<void> decode_ack(const Bytes& reply) {
  if (reply.size() != 1 || reply[0] != kAck) return protocol_error("expected ack 'U'", reply);
  return {};
}

Result<bool> decode_relay_state(const Bytes& reply) {
  if (reply.size() != 1 || reply[0] > 1) return protocol_error("expected relay state 0 or 1", reply);
  return reply[0] == 1;
}

}  // namespace pychron::codec::proxr
