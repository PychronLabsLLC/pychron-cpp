#include "pychron/devices/proxr_relay.hpp"

namespace pychron {

namespace proxr = codec::proxr;

ProxrRelay::ProxrRelay(std::string name, Transport& transport, DeviceOptions options)
    : Device(std::move(name), options), transport_(transport) {}

DriverSchema ProxrRelay::schema() {
  return {"", "NCD ProXR relay board; valve address = relay index 0..255 (bank index/8+1, relay index%8)", {}};
}

Result<std::unique_ptr<ProxrRelay>> ProxrRelay::create(const DriverArgs& args) {
  DeviceOptions options;
  options.clock = args.clock;
  return std::make_unique<ProxrRelay>(args.name, args.transport, options);
}

Result<void> ProxrRelay::open(const ValveAddress& address) { return observe(actuate(address, true)); }

Result<void> ProxrRelay::close(const ValveAddress& address) { return observe(actuate(address, false)); }

Result<ValveState> ProxrRelay::read(const ValveAddress& address) { return observe(query(address)); }

Result<void> ProxrRelay::actuate(const ValveAddress& address, bool energize) {
  // Bank select and relay command are one transaction, repeated whole.
  return relay<void>(
      [&](Resend resend) {
        return transact(transport_, [&]() -> Result<void> {
          auto which = select(address, resend);
          if (!which) return fail(which.error());
          auto cmd = energize ? proxr::relay_on(*which) : proxr::relay_off(*which);
          if (!cmd) return fail(cmd.error());
          auto reply = send(*cmd, resend);
          if (!reply) return fail(reply.error());
          return proxr::decode_ack(*reply);
        });
      },
      Repeat::Safe);
}

Result<ValveState> ProxrRelay::query(const ValveAddress& address) {
  return relay<ValveState>(
      [&](Resend resend) {
        return transact(transport_, [&]() -> Result<ValveState> {
          auto which = select(address, resend);
          if (!which) return fail(which.error());
          auto cmd = proxr::read_relay(*which);
          if (!cmd) return fail(cmd.error());
          auto reply = send(*cmd, resend);
          if (!reply) return fail(reply.error());
          auto on = proxr::decode_relay_state(*reply);
          if (!on) return fail(on.error());
          return *on ? ValveState::Open : ValveState::Closed;
        });
      },
      Repeat::Safe);
}

Result<int> ProxrRelay::select(const ValveAddress& address, Resend resend) {
  auto index = address.as_index();
  if (!index) return fail(index.error());
  auto where = proxr::relay_address(*index);
  if (!where) return fail(where.error());
  auto cmd = proxr::select_bank(where->bank);
  if (!cmd) return fail(cmd.error());
  auto reply = send(*cmd, resend);
  if (!reply) return fail(reply.error());
  if (auto ok = proxr::decode_ack(*reply); !ok) return fail(ok.error());
  return where->relay;
}

Result<Bytes> ProxrRelay::send(const codec::Command& command, Resend resend) {
  // Every ProXR command sets an absolute state or reads one: it may arrive twice.
  return transport_.exchange(command.tx, *command.reply, Transport::kDefaultTimeout, resend);
}

}  // namespace pychron

REGISTER_DRIVER("proxr_relay", pychron::ProxrRelay);
