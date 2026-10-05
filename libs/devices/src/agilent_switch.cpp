#include "pychron/devices/agilent_switch.hpp"

namespace pychron {

namespace ag = codec::agilent;

AgilentSwitch::AgilentSwitch(std::string name, Transport& transport, bool invert, DeviceOptions options)
    : Device(std::move(name), options), transport_(transport), invert_(invert) {}

DriverSchema AgilentSwitch::schema() {
  return {"",
          "Agilent/Keysight 34970A-family unit with a 34903A switch card (legacy AgilentGPActuator); valve address "
          "= channel, e.g. \"101\"",
          {{"invert", KeyType::Boolean, false,
            "swap ROUT:OPEN and ROUT:CLOSE (commands and read-back) for the whole unit; default false"}}};
}

Result<std::unique_ptr<AgilentSwitch>> AgilentSwitch::create(const DriverArgs& args) {
  DeviceOptions options;
  options.clock = args.clock;
  const bool invert = args.options["invert"].value_or(false);
  return std::make_unique<AgilentSwitch>(args.name, args.transport, invert, options);
}

Result<void> AgilentSwitch::connect() {
  return observe(transact(transport_, [&]() -> Result<void> {
    auto reply = ask(ag::identify());
    if (!reply) return fail(std::move(reply).error());
    auto id = ag::decode_identity(*reply);
    if (!id) return fail(std::move(id).error());
    if (!ag::is_switch_unit(*id)) {
      return fail(ErrorKind::Config, "agilent_switch: \"" + id->manufacturer + "," + id->model +
                                         "\" answered, not a 34970A-family unit");
    }
    const auto clear = ag::clear_status();
    if (auto sent = transport_.write(clear.tx); !sent) return fail(std::move(sent).error());
    return drain_errors("*CLS");
  }));
}

Result<void> AgilentSwitch::open(const ValveAddress& address) { return observe(route(address, true)); }

Result<void> AgilentSwitch::close(const ValveAddress& address) { return observe(route(address, false)); }

Result<ValveState> AgilentSwitch::read(const ValveAddress& address) {
  auto query = invert_ ? ag::query_close(address.value) : ag::query_open(address.value);
  if (!query) return observe(Result<ValveState>(fail(std::move(query).error())));
  auto reply = ask(*query);
  if (!reply) return observe(Result<ValveState>(fail(std::move(reply).error())));
  auto yes = ag::decode_route_state(*reply);
  if (!yes) return observe(Result<ValveState>(fail(std::move(yes).error())));
  return observe(Result<ValveState>(*yes ? ValveState::Open : ValveState::Closed));
}

Result<void> AgilentSwitch::route(const ValveAddress& address, bool open_valve) {
  const bool relay_open = open_valve != invert_;
  auto command = relay_open ? ag::route_open(address.value) : ag::route_close(address.value);
  if (!command) return fail(std::move(command).error());  // nothing sent
  return transact(transport_, [&]() -> Result<void> {
    if (auto sent = transport_.write(command->tx); !sent) return fail(std::move(sent).error());
    auto text = to_string(command->tx);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
    return drain_errors(text);
  });
}

Result<void> AgilentSwitch::drain_errors(std::string_view after) {
  std::optional<ag::InstrumentError> first;
  for (int i = 0; i < kMaxErrorReads; ++i) {
    auto reply = ask(ag::next_error());
    if (!reply) return fail(std::move(reply).error());
    auto error = ag::decode_error(*reply);
    if (!error) return fail(std::move(error).error());
    if (!error->has_value()) {
      if (!first) return {};
      return fail(ErrorKind::Protocol, "agilent_switch: " + std::to_string(first->code) + " \"" + first->message +
                                           "\" after " + std::string(after));
    }
    if (!first) first = **error;
  }
  return fail(ErrorKind::Protocol, "agilent_switch: error queue still not empty after " +
                                       std::to_string(kMaxErrorReads) + " reads (after " + std::string(after) + ")");
}

Result<Bytes> AgilentSwitch::ask(const codec::Command& command) {
  return transport_.exchange(command.tx, *command.reply);
}

}  // namespace pychron

REGISTER_DRIVER("agilent_switch", pychron::AgilentSwitch);
