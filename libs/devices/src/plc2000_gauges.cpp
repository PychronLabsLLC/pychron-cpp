#include "pychron/devices/plc2000_gauges.hpp"

#include <algorithm>
#include <cmath>

#include "pychron/devices/modbus_ids.hpp"

namespace pychron {

namespace mb = codec::modbus;

namespace {

const Clock& steady_clock() {
  static const SteadyClock clock;
  return clock;
}

}  // namespace

Plc2000Gauges::Plc2000Gauges(std::string name, Transport& transport, Plc2000GaugesOptions options,
                             const Clock* clock)
    : Device(std::move(name), DeviceOptions{clock}),
      transport_(transport),
      options_(std::move(options)),
      clock_(clock != nullptr ? clock : &steady_clock()) {}

DriverSchema Plc2000Gauges::schema() {
  return {"",
          "gauge values in an AutomationDirect PLC's holding registers over Modbus TCP (legacy "
          "PLC2000GaugeController); channel n reads a float from register n + register_offset",
          {{"channels", KeyType::IntegerArray, false, "gauge channels in use, each >= 1; default [1]"},
           {"unit", KeyType::Integer, false, "Modbus unit id, 0..255; default 1"},
           {"word_order", KeyType::String, false, "abcd, cdab (default; low word first, legacy), badc or dcba"},
           {"register_offset", KeyType::Integer, false, "register of channel n is n + this; default -1 (legacy)"}}};
}

Result<std::unique_ptr<Plc2000Gauges>> Plc2000Gauges::create(const DriverArgs& args) {
  const auto& o = args.options;
  Plc2000GaugesOptions options;
  const auto unit = o["unit"].value_or(std::int64_t{1});
  if (unit < 0 || unit > 255) return fail(ErrorKind::Config, "unit: " + std::to_string(unit) + " is outside 0..255");
  options.unit = static_cast<std::uint8_t>(unit);
  if (auto text = o["word_order"].value<std::string>()) {
    auto order = mb::word_order_from_string(*text);
    if (!order) return fail(ErrorKind::Config, "word_order: \"" + *text + "\" is not abcd, cdab, badc or dcba");
    options.word_order = *order;
  }
  options.register_offset = static_cast<int>(o["register_offset"].value_or(std::int64_t{-1}));
  if (const auto* array = o["channels"].as_array()) {
    options.channels.clear();
    for (const auto& element : *array) {
      const auto ch = element.value_or(std::int64_t{0});
      if (ch < 1) return fail(ErrorKind::Config, "channels: " + std::to_string(ch) + " is below 1");
      if (std::ranges::find(options.channels, ch) != options.channels.end())
        return fail(ErrorKind::Config, "channels: " + std::to_string(ch) + " listed twice");
      options.channels.push_back(static_cast<int>(ch));
    }
    if (options.channels.empty()) return fail(ErrorKind::Config, "channels: at least one channel is required");
  }
  for (int ch : options.channels) {
    const long reg = static_cast<long>(ch) + options.register_offset;
    if (reg < 0 || reg > 0xFFFE)
      return fail(ErrorKind::Config, "channel " + std::to_string(ch) + " maps to register " + std::to_string(reg) +
                                         ", outside 0..65534");
  }
  return std::make_unique<Plc2000Gauges>(args.name, args.transport, std::move(options), args.clock);
}

Result<double> Plc2000Gauges::read_pressure() { return read_pressure(options_.channels.front()); }

Result<double> Plc2000Gauges::read_pressure(int channel) {
  auto value = [&]() -> Result<double> {
    if (std::ranges::find(options_.channels, channel) == options_.channels.end())
      return fail(ErrorKind::Config, "channel " + std::to_string(channel) + " is not configured");
    const auto reg = static_cast<std::uint16_t>(channel + options_.register_offset);
    const auto tid = next_modbus_transaction_id();
    auto cmd = mb::read_holding_registers(tid, options_.unit, reg, 2);
    if (!cmd) return fail(std::move(cmd).error());
    auto reply = transport_.exchange(cmd->tx, *cmd->reply);
    if (!reply) return fail(std::move(reply).error());
    auto regs = mb::decode_registers(tid, options_.unit, mb::Function::ReadHoldingRegisters, 2, *reply);
    if (!regs) return fail(std::move(regs).error());
    const double v = mb::decode_float((*regs)[0], (*regs)[1], options_.word_order);
    if (!std::isfinite(v) || v < 0) return codec::protocol_error("plc2000: register " + std::to_string(reg) +
                                                                     " holds no pressure", *reply);
    return v;
  }();
  return observe(std::move(value));
}

Result<Sample> Plc2000Gauges::sample() {
  auto p = read_pressure();
  if (!p) return fail(std::move(p).error());
  return Sample{name(), clock_->now(), *p};
}

}  // namespace pychron

REGISTER_DRIVER("plc2000_gauges", pychron::Plc2000Gauges);
