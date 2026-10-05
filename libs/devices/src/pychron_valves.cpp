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
  return std::make_unique<PychronValves>(args.name, args.transport, options);
}

Result<Bytes> PychronValves::ask(const codec::Command& command) {
  return transact(transport_, [&]() -> Result<Bytes> {
    transport_.close();  // the server hung up after the last reply
    if (auto opened = transport_.open(); !opened) return fail(std::move(opened).error());
    auto reply = transport_.exchange(command.tx, *command.reply);
    transport_.close();
    return reply;
  });
}

Result<void> PychronValves::open(const ValveAddress& address) {
  auto command = tx::open_valve(address.value);
  if (!command) return observe(Result<void>(fail(std::move(command).error())));
  auto reply = ask(*command);
  if (!reply) return observe(Result<void>(fail(std::move(reply).error())));
  return observe(tx::decode_actuation(*reply));
}

Result<void> PychronValves::close(const ValveAddress& address) {
  auto command = tx::close_valve(address.value);
  if (!command) return observe(Result<void>(fail(std::move(command).error())));
  auto reply = ask(*command);
  if (!reply) return observe(Result<void>(fail(std::move(reply).error())));
  return observe(tx::decode_actuation(*reply));
}

Result<ValveState> PychronValves::read(const ValveAddress& address) {
  auto state = [&]() -> Result<ValveState> {
    auto command = tx::get_valve_state(address.value);
    if (!command) return fail(std::move(command).error());
    auto reply = ask(*command);
    if (!reply) return fail(std::move(reply).error());
    auto open = tx::decode_state(*reply);
    if (!open) return fail(std::move(open).error());
    return *open ? ValveState::Open : ValveState::Closed;
  }();
  return observe(std::move(state));
}

}  // namespace pychron

REGISTER_DRIVER("pychron_valves", pychron::PychronValves);
