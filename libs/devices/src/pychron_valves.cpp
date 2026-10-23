#include "pychron/devices/pychron_valves.hpp"

namespace pychron {

namespace tx = codec::pychron_tx;

PychronValves::PychronValves(std::string name, Transport& transport, DeviceOptions options)
    : Device(std::move(name), options), transport_(transport) {}

DriverSchema PychronValves::schema() {
  return {"",
          "valves through another Pychron's valve service (legacy PychronGPActuator); valve address = the remote "
          "Pychron's valve name; one TCP connection per command",
          {}};
}

Result<std::unique_ptr<PychronValves>> PychronValves::create(const DriverArgs& args) {
  DeviceOptions options;
  options.clock = args.clock;
  options.link = args.link;
  return std::make_unique<PychronValves>(args.name, args.transport, options);
}

Result<Bytes> PychronValves::ask(const codec::Command& command) {
  // Every command is Safe: the connection and its one exchange are repeated
  // as a unit.
  return relay<Bytes>("ask",
      [&](Resend resend) {
        return transact(transport_, [&]() -> Result<Bytes> {
          transport_.close();  // the server hung up after the last reply
          if (auto opened = transport_.open(); !opened) return fail(std::move(opened).error());
          auto reply = transport_.exchange(command.tx, *command.reply, Transport::kDefaultTimeout, resend);
          transport_.close();
          return reply;
        });
      },
      Repeat::Safe);
}

Result<void> PychronValves::open(const ValveAddress& address) {
  const Operation op = begin("open");
  auto command = tx::open_valve(address.value);
  if (!command) return observe(op, Result<void>(fail(std::move(command).error())));
  auto reply = ask(*command);
  if (!reply) return observe(op, Result<void>(fail(std::move(reply).error())));
  return observe(op, tx::decode_actuation(*reply));
}

Result<void> PychronValves::close(const ValveAddress& address) {
  const Operation op = begin("close");
  auto command = tx::close_valve(address.value);
  if (!command) return observe(op, Result<void>(fail(std::move(command).error())));
  auto reply = ask(*command);
  if (!reply) return observe(op, Result<void>(fail(std::move(reply).error())));
  return observe(op, tx::decode_actuation(*reply));
}

Result<ValveState> PychronValves::read(const ValveAddress& address) {
  const Operation op = begin("read");
  auto state = [&]() -> Result<ValveState> {
    auto command = tx::get_valve_state(address.value);
    if (!command) return fail(std::move(command).error());
    auto reply = ask(*command);
    if (!reply) return fail(std::move(reply).error());
    auto open = tx::decode_state(*reply);
    if (!open) return fail(std::move(open).error());
    return *open ? ValveState::Open : ValveState::Closed;
  }();
  return observe(op, std::move(state));
}

}  // namespace pychron

REGISTER_DRIVER("pychron_valves", pychron::PychronValves);
