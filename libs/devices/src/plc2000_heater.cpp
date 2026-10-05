#include "pychron/devices/plc2000_heater.hpp"

#include <cmath>
#include <limits>

#include "pychron/devices/extraction/capability.hpp"
#include "pychron/devices/modbus_device_sim.hpp"
#include "pychron/devices/modbus_ids.hpp"

namespace pychron {

namespace mb = codec::modbus;

namespace {

// 1-based address -> wire address.
std::uint16_t wire(std::uint16_t address) { return static_cast<std::uint16_t>(address - 1); }

}  // namespace

Plc2000Heater::Plc2000Heater(std::string name, Transport& transport, Plc2000HeaterOptions options,
                             const Clock* clock)
    : Device(std::move(name), DeviceOptions{clock}), transport_(transport), options_(std::move(options)) {}

DriverSchema Plc2000Heater::schema() {
  return {"",
          "a heater run by an AutomationDirect PLC over Modbus TCP (legacy PLC2000Heater); 1-based coil and "
          "register addresses as in the legacy device file",
          {{"unit", KeyType::Integer, false, "Modbus unit id, 0..255; default 1"},
           {"word_order", KeyType::String, false, "abcd, cdab (default; low word first, legacy), badc or dcba"},
           {"enable", KeyType::Integer, false, "enable coil, 1-based; absent: on/off not supported"},
           {"use_pid", KeyType::Integer, false, "use-PID coil, 1-based; absent: not supported"},
           {"setpoint", KeyType::Integer, false,
            "first of two setpoint registers, 1-based: read as input registers, written as holding registers"},
           {"readback", KeyType::Integer, false, "first of two readback input registers, 1-based"},
           {"setpoint_write_format", KeyType::String, false, "int32 (default, as legacy) or float32"}}};
}

Result<std::unique_ptr<Plc2000Heater>> Plc2000Heater::create(const DriverArgs& args) {
  auto options = parse_options(args.options);
  if (!options) return fail(std::move(options).error());
  return std::make_unique<Plc2000Heater>(args.name, args.transport, std::move(*options), args.clock);
}

Result<Plc2000HeaterOptions> Plc2000Heater::parse_options(const toml::table& o) {
  Plc2000HeaterOptions options;
  const auto unit = o["unit"].value_or(std::int64_t{1});
  if (unit < 0 || unit > 255) return fail(ErrorKind::Config, "unit: " + std::to_string(unit) + " is outside 0..255");
  options.unit = static_cast<std::uint8_t>(unit);
  if (auto text = o["word_order"].value<std::string>()) {
    auto order = mb::word_order_from_string(*text);
    if (!order) return fail(ErrorKind::Config, "word_order: \"" + *text + "\" is not abcd, cdab, badc or dcba");
    options.word_order = *order;
  }
  // Coils take one address, registers two.
  auto address = [&](const char* key, std::int64_t last, std::optional<std::uint16_t>& out) -> Result<void> {
    auto v = o[key].value<std::int64_t>();
    if (!v) return {};
    if (*v < 1 || *v > last) {
      return fail(ErrorKind::Config, std::string(key) + ": " + std::to_string(*v) + " is outside 1.." +
                                         std::to_string(last) + " (addresses are 1-based)");
    }
    out = static_cast<std::uint16_t>(*v);
    return {};
  };
  for (auto r : {address("enable", 65536, options.enable), address("use_pid", 65536, options.use_pid),
                 address("setpoint", 65535, options.setpoint), address("readback", 65535, options.readback)}) {
    if (!r) return fail(std::move(r).error());
  }
  if (auto text = o["setpoint_write_format"].value<std::string>()) {
    if (*text == "int32") options.setpoint_write_format = SetpointWriteFormat::Int32;
    else if (*text == "float32") options.setpoint_write_format = SetpointWriteFormat::Float32;
    else return fail(ErrorKind::Config, "setpoint_write_format: \"" + *text + "\" is not int32 or float32");
  }
  return options;
}

Result<bool> Plc2000Heater::read_coil(const std::optional<std::uint16_t>& address, const char* what) {
  if (!address) return fail(extraction::not_supported(std::string(what) + " (no address configured)", name()));
  auto value = [&]() -> Result<bool> {
    const auto tid = next_modbus_transaction_id();
    auto cmd = mb::read_coils(tid, options_.unit, wire(*address), 1);
    if (!cmd) return fail(std::move(cmd).error());
    auto reply = transport_.exchange(cmd->tx, *cmd->reply);
    if (!reply) return fail(std::move(reply).error());
    auto coils = mb::decode_coils(tid, options_.unit, 1, *reply);
    if (!coils) return fail(std::move(coils).error());
    return coils->front();
  }();
  return observe(std::move(value));
}

Result<void> Plc2000Heater::write_coil(const std::optional<std::uint16_t>& address, const char* what, bool on) {
  if (!address) return fail(extraction::not_supported(std::string(what) + " (no address configured)", name()));
  auto done = [&]() -> Result<void> {
    const auto tid = next_modbus_transaction_id();
    const auto cmd = mb::write_single_coil(tid, options_.unit, wire(*address), on);
    auto reply = transport_.exchange(cmd.tx, *cmd.reply);
    if (!reply) return fail(std::move(reply).error());
    return mb::decode_write_single_coil(tid, options_.unit, wire(*address), on, *reply);
  }();
  return observe(std::move(done));
}

Result<double> Plc2000Heater::read_float(const std::optional<std::uint16_t>& address, const char* what) {
  if (!address) return fail(extraction::not_supported(std::string(what) + " (no address configured)", name()));
  auto value = [&]() -> Result<double> {
    const auto tid = next_modbus_transaction_id();
    auto cmd = mb::read_input_registers(tid, options_.unit, wire(*address), 2);
    if (!cmd) return fail(std::move(cmd).error());
    auto reply = transport_.exchange(cmd->tx, *cmd->reply);
    if (!reply) return fail(std::move(reply).error());
    auto regs = mb::decode_registers(tid, options_.unit, mb::Function::ReadInputRegisters, 2, *reply);
    if (!regs) return fail(std::move(regs).error());
    const double v = mb::decode_float((*regs)[0], (*regs)[1], options_.word_order);
    if (!std::isfinite(v)) {
      return codec::protocol_error("plc2000_heater: " + std::string(what) + " register holds no number", *reply);
    }
    return v;
  }();
  return observe(std::move(value));
}

Result<void> Plc2000Heater::set_enabled(bool on) { return write_coil(options_.enable, "enable", on); }
Result<bool> Plc2000Heater::enabled() { return read_coil(options_.enable, "enable"); }
Result<void> Plc2000Heater::set_use_pid(bool on) { return write_coil(options_.use_pid, "use_pid", on); }
Result<bool> Plc2000Heater::use_pid() { return read_coil(options_.use_pid, "use_pid"); }
Result<double> Plc2000Heater::setpoint() { return read_float(options_.setpoint, "setpoint"); }
Result<double> Plc2000Heater::readback() { return read_float(options_.readback, "readback"); }

Result<void> Plc2000Heater::set_setpoint(double value) {
  if (!options_.setpoint) return fail(extraction::not_supported("setpoint (no address configured)", name()));
  std::array<std::uint16_t, 2> words{};
  if (options_.setpoint_write_format == SetpointWriteFormat::Int32) {
    if (!std::isfinite(value) || value != std::trunc(value) || value < std::numeric_limits<std::int32_t>::min() ||
        value > std::numeric_limits<std::int32_t>::max()) {
      return fail(ErrorKind::Config,
                  "setpoint " + std::to_string(value) + " is not a whole number; the PLC takes an int32", name());
    }
    words = mb::encode_int32(static_cast<std::int32_t>(value), options_.word_order);
  } else {
    if (!std::isfinite(value)) return fail(ErrorKind::Config, "setpoint is not a number", name());
    words = mb::encode_float(static_cast<float>(value), options_.word_order);
  }
  auto done = [&]() -> Result<void> {
    const auto tid = next_modbus_transaction_id();
    const auto start = wire(*options_.setpoint);
    auto cmd = mb::write_multiple_registers(tid, options_.unit, start, {words[0], words[1]});
    if (!cmd) return fail(std::move(cmd).error());
    auto reply = transport_.exchange(cmd->tx, *cmd->reply);
    if (!reply) return fail(std::move(reply).error());
    return mb::decode_write_multiple_registers(tid, options_.unit, start, 2, *reply);
  }();
  return observe(std::move(done));
}

// --- sim ------------------------------------------------------------------------

Plc2000HeaterSim::Plc2000HeaterSim(const Clock& clock, Plc2000HeaterOptions options, double ambient, Duration tau)
    : clock_(clock),
      options_(std::move(options)),
      ambient_(ambient),
      tau_(tau),
      last_(clock.now()),
      readback_(ambient) {}

void Plc2000HeaterSim::advance_locked() const {
  const auto now = clock_.now();
  const double dt = std::chrono::duration<double>(now - last_).count();
  last_ = now;
  if (dt <= 0) return;
  const double k = 1.0 - std::exp(-dt / std::chrono::duration<double>(tau_).count());
  readback_ += ((enabled_ ? setpoint_ : ambient_) - readback_) * k;
}

SimTransport::Hook Plc2000HeaterSim::hook() {
  ModbusDeviceSim plc;
  plc.unit = options_.unit;
  auto at = [](const std::optional<std::uint16_t>& address, std::uint16_t a) {
    return address && wire(*address) == a;
  };
  plc.read_coil = [this, at](std::uint16_t a) -> std::optional<bool> {
    std::lock_guard lock(mutex_);
    if (at(options_.enable, a)) return enabled_;
    if (at(options_.use_pid, a)) return use_pid_;
    return std::nullopt;
  };
  plc.write_coil = [this, at](std::uint16_t a, bool on) {
    std::lock_guard lock(mutex_);
    advance_locked();
    if (at(options_.enable, a)) enabled_ = on;
    else if (at(options_.use_pid, a)) use_pid_ = on;
    else return false;
    return true;
  };
  plc.read_input = [this](std::uint16_t a) -> std::optional<std::uint16_t> {
    std::lock_guard lock(mutex_);
    advance_locked();
    for (auto [address, value] : {std::pair{options_.setpoint, setpoint_}, std::pair{options_.readback, readback_}}) {
      if (!address) continue;
      const auto base = wire(*address);
      if (a != base && a != base + 1) continue;
      return mb::encode_float(static_cast<float>(value), options_.word_order)[a - base];
    }
    return std::nullopt;
  };
  plc.write_register = [this](std::uint16_t a, std::uint16_t value) {
    std::lock_guard lock(mutex_);
    if (!options_.setpoint) return false;
    const auto base = wire(*options_.setpoint);
    if (a != base && a != base + 1) return false;
    held_[a] = value;
    if (a == base + 1 && held_.count(base)) {
      advance_locked();
      const double v = options_.setpoint_write_format == SetpointWriteFormat::Int32
                           ? static_cast<double>(mb::decode_int32(held_[base], held_[a], options_.word_order))
                           : static_cast<double>(mb::decode_float(held_[base], held_[a], options_.word_order));
      setpoint_ = v + setpoint_error_;
    }
    return true;
  };
  return [this, inner = plc.hook()](const Bytes& tx) { return offline_ ? Bytes{} : inner(tx); };
}

bool Plc2000HeaterSim::enabled() const {
  std::lock_guard lock(mutex_);
  return enabled_;
}

bool Plc2000HeaterSim::use_pid() const {
  std::lock_guard lock(mutex_);
  return use_pid_;
}

double Plc2000HeaterSim::setpoint() const {
  std::lock_guard lock(mutex_);
  return setpoint_;
}

double Plc2000HeaterSim::readback() const {
  std::lock_guard lock(mutex_);
  advance_locked();
  return readback_;
}

void Plc2000HeaterSim::set_offline(bool offline) { offline_ = offline; }

void Plc2000HeaterSim::set_setpoint_error(double error) {
  std::lock_guard lock(mutex_);
  setpoint_error_ = error;
}

}  // namespace pychron

REGISTER_DRIVER("plc2000_heater", pychron::Plc2000Heater);
