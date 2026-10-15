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
  options.link = args.link;
  const bool invert = args.options["invert"].value_or(false);
  return std::make_unique<AgilentSwitch>(args.name, args.transport, invert, options);
}

Result<void> AgilentSwitch::connect() {
  // *IDN? and *CLS are Safe: the whole handshake is repeated, but not after
  // a lost SYST:ERR? reply (drain_errors).
  return call<void>(
      [&](Resend resend) {
        return transact(transport_, [&]() -> Result<void> {
          auto reply = ask(ag::identify(), resend);
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
        });
      },
      Repeat::Safe);
}

Result<void> AgilentSwitch::open(const ValveAddress& address) { return observe(route(address, true)); }

Result<void> AgilentSwitch::close(const ValveAddress& address) { return observe(route(address, false)); }

Result<ValveState> AgilentSwitch::read(const ValveAddress& address) {
  auto query = invert_ ? ag::query_close(address.value) : ag::query_open(address.value);
  if (!query) return observe(Result<ValveState>(fail(std::move(query).error())));
  auto reply = relay<Bytes>([&](Resend resend) { return ask(*query, resend); }, Repeat::Safe);
  if (!reply) return observe(Result<ValveState>(fail(std::move(reply).error())));
  auto yes = ag::decode_route_state(*reply);
  if (!yes) return observe(Result<ValveState>(fail(std::move(yes).error())));
  return observe(Result<ValveState>(*yes ? ValveState::Open : ValveState::Closed));
}

Result<void> AgilentSwitch::route(const ValveAddress& address, bool open_valve) {
  const bool relay_open = open_valve != invert_;
  auto command = relay_open ? ag::route_open(address.value) : ag::route_close(address.value);
  if (!command) return fail(std::move(command).error());  // nothing sent
  // The route and its error read are one transaction, repeated whole when
  // the route was lost; a lost SYST:ERR? reply ends it as outcome_unknown.
  return relay<void>(
      [&](Resend) {
        return transact(transport_, [&]() -> Result<void> {
          if (auto sent = transport_.write(command->tx); !sent) return fail(std::move(sent).error());
          auto text = to_string(command->tx);
          while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
          return drain_errors(text);
        });
      },
      Repeat::Safe);
}

Result<void> AgilentSwitch::drain_errors(std::string_view after) {
  std::optional<ag::InstrumentError> first;
  for (int i = 0; i < kMaxErrorReads; ++i) {
    // Unsafe: the read takes the error off the queue, so a lost reply loses
    // it. Sent once; a transient failure is marked outcome_unknown, which the
    // link does not repeat.
    auto reply = ask(ag::next_error(), Resend::Never);
    if (!reply) return fail(outcome_unknown_if_transient(std::move(reply).error()));
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

Result<Bytes> AgilentSwitch::ask(const codec::Command& command, Resend resend) {
  return transport_.exchange(command.tx, *command.reply, Transport::kDefaultTimeout, resend);
}

}  // namespace pychron

REGISTER_DRIVER("agilent_switch", pychron::AgilentSwitch);
