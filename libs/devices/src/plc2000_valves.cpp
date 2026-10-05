#include "pychron/devices/plc2000_valves.hpp"

#include <algorithm>
#include <map>
#include <optional>
#include <set>

#include "pychron/codecs/modbus.hpp"
#include "pychron/devices/modbus_ids.hpp"

namespace pychron {

namespace mb = codec::modbus;

Plc2000Valves::Plc2000Valves(std::string name, Transport& transport, Plc2000ValvesOptions options,
                             DeviceOptions device_options)
    : Device(std::move(name), device_options), transport_(transport), options_(options) {}

DriverSchema Plc2000Valves::schema() {
  return {"",
          "valves on an AutomationDirect PLC's coils over Modbus TCP (legacy PLC2000GPActuator); a valve "
          "address is a coil number",
          {{"unit", KeyType::Integer, false, "Modbus unit id, 0..255; default 1"},
           {"coil_offset", KeyType::Integer, false, "wire coil = address + this; default -1 (legacy, 1-based)"}}};
}

Result<std::unique_ptr<Plc2000Valves>> Plc2000Valves::create(const DriverArgs& args) {
  const auto& o = args.options;
  Plc2000ValvesOptions options;
  const auto unit = o["unit"].value_or(std::int64_t{1});
  if (unit < 0 || unit > 255) return fail(ErrorKind::Config, "unit: " + std::to_string(unit) + " is outside 0..255");
  options.unit = static_cast<std::uint8_t>(unit);
  const auto offset = o["coil_offset"].value_or(std::int64_t{-1});
  if (offset < -65535 || offset > 65535) {
    return fail(ErrorKind::Config, "coil_offset: " + std::to_string(offset) + " is outside -65535..65535");
  }
  options.coil_offset = static_cast<int>(offset);
  return std::make_unique<Plc2000Valves>(args.name, args.transport, options, DeviceOptions{args.clock});
}

Result<std::uint16_t> Plc2000Valves::coil(const ValveAddress& address) const {
  auto index = address.as_index();
  if (!index) return fail(Error{ErrorKind::Config, index.error().what, name()});
  const std::int64_t wire = *index + options_.coil_offset;
  if (wire < 0 || wire > 0xFFFF) {
    return fail(ErrorKind::Config,
                "valve address " + address.value + " is coil " + std::to_string(wire) + ", outside 0..65535", name());
  }
  return static_cast<std::uint16_t>(wire);
}

Result<void> Plc2000Valves::write(const ValveAddress& address, bool on) {
  auto c = coil(address);
  if (!c) return fail(std::move(c).error());
  auto done = [&]() -> Result<void> {
    const auto tid = next_modbus_transaction_id();
    const auto cmd = mb::write_single_coil(tid, options_.unit, *c, on);
    auto reply = transport_.exchange(cmd.tx, *cmd.reply);
    if (!reply) return fail(std::move(reply).error());
    return mb::decode_write_single_coil(tid, options_.unit, *c, on, *reply);
  }();
  return observe(std::move(done));
}

Result<void> Plc2000Valves::open(const ValveAddress& address) { return write(address, true); }
Result<void> Plc2000Valves::close(const ValveAddress& address) { return write(address, false); }

Result<std::vector<bool>> Plc2000Valves::read_coils(std::uint16_t start, std::uint16_t count) {
  auto coils = [&]() -> Result<std::vector<bool>> {
    const auto tid = next_modbus_transaction_id();
    auto cmd = mb::read_coils(tid, options_.unit, start, count);
    if (!cmd) return fail(std::move(cmd).error());
    auto reply = transport_.exchange(cmd->tx, *cmd->reply);
    if (!reply) return fail(std::move(reply).error());
    return mb::decode_coils(tid, options_.unit, count, *reply);
  }();
  return observe(std::move(coils));
}

Result<ValveState> Plc2000Valves::read(const ValveAddress& address) {
  auto c = coil(address);
  if (!c) return fail(std::move(c).error());
  auto coils = read_coils(*c, 1);
  if (!coils) return fail(std::move(coils).error());
  return coils->front() ? ValveState::Open : ValveState::Closed;
}

std::vector<Result<ValveState>> Plc2000Valves::read_many(const std::vector<ValveAddress>& addresses) {
  std::vector<Result<ValveState>> out(addresses.size(), Result<ValveState>(ValveState::Unknown));
  std::set<std::uint16_t> wanted;
  std::vector<std::optional<std::uint16_t>> coil_of(addresses.size());
  for (std::size_t i = 0; i < addresses.size(); ++i) {
    auto c = coil(addresses[i]);
    if (!c) {
      out[i] = fail(std::move(c).error());
      continue;
    }
    coil_of[i] = *c;
    wanted.insert(*c);
  }

  // One request per run of consecutive coils: a gap may be a coil the PLC
  // does not have, which would fail the whole read.
  std::map<std::uint16_t, Result<bool>> state;
  auto it = wanted.begin();
  while (it != wanted.end()) {
    const std::uint16_t start = *it;
    std::uint16_t count = 1;
    auto next = std::next(it);
    while (next != wanted.end() && *next == start + count && count < kMaxCoilsPerRead) {
      ++count;
      ++next;
    }
    auto coils = read_coils(start, count);
    for (std::uint16_t k = 0; k < count; ++k) {
      const auto c = static_cast<std::uint16_t>(start + k);
      if (coils) state.emplace(c, (*coils)[k]);
      else state.emplace(c, fail(coils.error()));
    }
    it = next;
  }

  for (std::size_t i = 0; i < addresses.size(); ++i) {
    if (!coil_of[i]) continue;
    const auto& s = state.at(*coil_of[i]);
    if (s) out[i] = *s ? ValveState::Open : ValveState::Closed;
    else out[i] = fail(s.error());
  }
  return out;
}

}  // namespace pychron

REGISTER_DRIVER("plc2000_valves", pychron::Plc2000Valves);
