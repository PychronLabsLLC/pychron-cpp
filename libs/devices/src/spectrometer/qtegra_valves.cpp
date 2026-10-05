#include "pychron/devices/spectrometer/qtegra_valves.hpp"

namespace pychron::spectrometer {

namespace q = codec::qtegra;

namespace {

Result<q::Terminator> parse_terminator(const std::string& text) {
  if (text == "cr") return q::Terminator::CR;
  if (text == "lf") return q::Terminator::LF;
  if (text == "crlf") return q::Terminator::CRLF;
  return fail(ErrorKind::Config, "terminator must be cr, lf or crlf, not \"" + text + "\"");
}

}  // namespace

QtegraValves::QtegraValves(std::string name, QtegraLinkHandle link, DeviceOptions options)
    : Device(std::move(name), options), link_(std::move(link)) {}

DriverSchema QtegraValves::schema() {
  return {"",
          "valves through Thermo Qtegra RemoteControl (legacy QtegraGPActuator); valve address = the Qtegra valve "
          "name; on a kind = \"link\" transport it shares thermo_qtegra's connection",
          {{"link", KeyType::String, false,
            "on its own tcp/udp transport: the name its connection is registered under; default: the driver name"},
           {"terminator", KeyType::String, false,
            "on its own transport: write terminator cr (default), lf or crlf; a shared link uses the owner's"}}};
}

Result<std::unique_ptr<QtegraValves>> QtegraValves::create(const DriverArgs& args) {
  const auto& o = args.options;
  auto terminator = parse_terminator(o["terminator"].value_or(std::string("cr")));
  if (!terminator) return fail(std::move(terminator).error());
  static const SteadyClock steady;
  auto link = make_qtegra_link(args.transport, o["link"].value_or(args.name), *terminator,
                               args.clock != nullptr ? *args.clock : steady);
  if (!link) return fail(std::move(link).error());
  DeviceOptions options;
  options.clock = args.clock;
  return std::make_unique<QtegraValves>(args.name, std::move(*link), options);
}

Result<void> QtegraValves::open(const ValveAddress& address) { return observe(actuate(address, true)); }

Result<void> QtegraValves::close(const ValveAddress& address) { return observe(actuate(address, false)); }

Result<void> QtegraValves::actuate(const ValveAddress& address, bool open) {
  if (auto ok = q::validate_name(address.value); !ok) return fail(std::move(ok).error());  // nothing sent
  auto link = link_.get();
  if (!link) return fail(std::move(link).error());
  auto command = open ? q::open_valve(address.value, (*link)->terminator())
                      : q::close_valve(address.value, (*link)->terminator());
  if (!command) return fail(std::move(command).error());
  auto reply = (*link)->exchange(*command);
  if (!reply) return fail(std::move(reply).error());
  return q::decode_ok(*reply);
}

Result<ValveState> QtegraValves::read(const ValveAddress& address) {
  auto state = [&]() -> Result<ValveState> {
    if (auto ok = q::validate_name(address.value); !ok) return fail(std::move(ok).error());
    auto link = link_.get();
    if (!link) return fail(std::move(link).error());
    auto command = q::get_valve_state(address.value, (*link)->terminator());
    if (!command) return fail(std::move(command).error());
    auto reply = (*link)->exchange(*command);
    if (!reply) return fail(std::move(reply).error());
    auto open = q::decode_valve_state(*reply);
    if (!open) return fail(std::move(open).error());
    return *open ? ValveState::Open : ValveState::Closed;
  }();
  return observe(std::move(state));
}

}  // namespace pychron::spectrometer

REGISTER_DRIVER("qtegra_valves", pychron::spectrometer::QtegraValves);
